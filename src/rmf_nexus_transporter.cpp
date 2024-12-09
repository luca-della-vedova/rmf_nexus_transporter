#include <nlohmann/json.hpp>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>

#include <nexus_endpoints.hpp>

#include <rmf_dispenser_msgs/msg/dispenser_result.hpp>
#include <rmf_dispenser_msgs/msg/dispenser_request.hpp>
#include <rmf_dispenser_msgs/msg/dispenser_state.hpp>
#include <rmf_task_msgs/msg/api_request.hpp>
#include <rmf_task_msgs/msg/api_response.hpp>
#include <std_msgs/msg/string.hpp>

#include "rmf_nexus_transporter/rmf_nexus_transporter.hpp"

namespace rmf_nexus_transporter
{

static constexpr std::chrono::seconds REGISTER_TICK_RATE{1};

using CallbackReturn =
    rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

using TaskState = nexus_orchestrator_msgs::msg::TaskState;
using WorkcellRequest = nexus::endpoints::WorkcellRequestAction::ActionType;

RmfNexusTransporter::RmfNexusTransporter(const rclcpp::NodeOptions& options)
: rclcpp_lifecycle::LifecycleNode("rmf_nexus_transporter", options)
{
  // TODO(luca) declare param for navgraph here
  this->_register_workcell_client =
    this->create_client<nexus::endpoints::RegisterWorkcellService::ServiceType>(
    nexus::endpoints::RegisterWorkcellService::service_name());

  this->_register_timer = this->create_wall_timer(REGISTER_TICK_RATE,
      [this]()
      {
        this->register_workcell();
      });
}

void RmfNexusTransporter::init_subscriptions()
{
  const auto transient_qos =
    rclcpp::SystemDefaultsQoS().transient_local().keep_last(10).reliable();
  // RMF interfaces
  this->_api_request_pub = this->create_publisher<ApiRequest>("/task_api_requests", transient_qos);
  this->_api_response_sub = this->create_subscription<ApiResponse>("/task_api_responses", transient_qos,
      [&](ApiResponse::UniquePtr msg)
      {
        this->api_response_cb(*msg);
      });
  this->_task_state_sub = this->create_subscription<TaskStateUpdate>("/task_state_update", transient_qos,
      [&](TaskStateUpdate::UniquePtr msg)
      {
        this->task_state_cb(*msg);
      });
  // TODO(luca) check qos here
  this->_dispenser_result_pub = this->create_publisher<DispenserResult>("/dispenser_results", 10);
  this->_dispenser_state_pub = this->create_publisher<DispenserState>("/dispenser_states", 10);
  this->_dispenser_request_sub = this->create_subscription<DispenserRequest>("/dispenser_requests", 10,
      [&](DispenserRequest::UniquePtr msg)
      {
        this->dispenser_request_cb(*msg);
      });

  // Nexus interfaces
  this->_task_doable_srv =
    this->create_service<nexus::endpoints::IsTaskDoableService::ServiceType>(
    nexus::endpoints::IsTaskDoableService::service_name(this->get_name()),
    [this](nexus::endpoints::IsTaskDoableService::ServiceType::Request::ConstSharedPtr
    req,
    nexus::endpoints::IsTaskDoableService::ServiceType::Response::SharedPtr resp)
    {
      this->_handle_task_doable(req, resp);
    });

  this->_signal_client = this->create_client<nexus::endpoints::SignalWorkcellService::ServiceType>(
     nexus::endpoints::SignalWorkcellService::service_name("system_orchestrator"));

  this->_signal_srv = this->create_service<nexus::endpoints::SignalWorkcellService::ServiceType>(
     nexus::endpoints::SignalWorkcellService::service_name(this->get_name()),
     [this](nexus::endpoints::SignalWorkcellService::ServiceType::Request::ConstSharedPtr req,
       nexus::endpoints::SignalWorkcellService::ServiceType::Response::SharedPtr resp)
     {
        auto err = this->process_signal(req->task_id, req->signal);
        resp->success = !err.has_value();
        if (err.has_value())
        {
          resp->message = err.value();
        }
     });

  this->_cmd_server =
    rclcpp_action::create_server<WorkcellRequest>(
    this,
    nexus::endpoints::WorkcellRequestAction::action_name(this->get_name()),
    [this](const rclcpp_action::GoalUUID& /* uuid */,
    WorkcellRequest::Goal::ConstSharedPtr goal)
    {
      RCLCPP_INFO(this->get_logger(), "Got workcell task request");
      if (this->job_id_to_sessions.count(goal->task.id) > 0)
      {
        RCLCPP_ERROR(this->get_logger(),
        "A task with the same id is already executing");
        return rclcpp_action::GoalResponse::REJECT;
      }
      return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    },
    [this](std::shared_ptr<rclcpp_action::ServerGoalHandle<WorkcellRequest>>
    goal_handle)
    {
      RCLCPP_INFO(this->get_logger(), "Got cancel request");
      // TODO(luca) implement cancellation with RMF request
      /*
      const auto& goal = goal_handle->get_goal();
      auto it =
      std::find_if(this->_ctxs.begin(), this->_ctxs.end(),
      [&goal](const std::shared_ptr<Context>& ctx)
      {
        return goal->task.id == ctx->task.id;
      });

      if (it == this->_ctxs.end())
      {
        RCLCPP_WARN(this->get_logger(),
        "Fail to cancel task [%s]: task does not exist", goal->task.id.c_str());
      }
      else
      {
        // we can just remove a task that is not running
        if ((*it)->task_state.status != TaskState::STATUS_RUNNING)
        {
          this->_ctxs.erase(it);
        }
      }
      */
      return rclcpp_action::CancelResponse::ACCEPT;
    },
    [this](std::shared_ptr<rclcpp_action::ServerGoalHandle<WorkcellRequest>>
    goal_handle)
    {
      RCLCPP_INFO(this->get_logger(), "Starting");
      if (this->get_current_state().label() != "active")
      {
        RCLCPP_ERROR(this->get_logger(),
        "Can only process tasks when node is active!");
        goal_handle->abort(
          std::make_shared<WorkcellRequest::Result>());
        return;
      }

      const auto it = job_id_to_sessions.find(goal_handle->get_goal()->task.id);
      if (it != this->job_id_to_sessions.end())
      {
        RCLCPP_ERROR(this->get_logger(),
        "Job id [%s] is already being processed", goal_handle->get_goal()->task.id.c_str());
        goal_handle->abort(
          std::make_shared<WorkcellRequest::Result>());
        return;
      }
      const auto& task = goal_handle->get_goal()->task;
      if (!this->can_perform_task(task))
      {
        auto result =
        std::make_shared<WorkcellRequest::Result>();
        result->message = "Transporter cannot perform task " + task.type;
        result->success = false;
        RCLCPP_ERROR_STREAM(this->get_logger(), result->message);
        goal_handle->abort(
          result);
        return;
      }
      if (!submit_itinerary(goal_handle))
      {
        auto result =
        std::make_shared<WorkcellRequest::Result>();
        result->message = "Transporter failed generating itinerary for task " + task.id;
        result->success = false;
        RCLCPP_ERROR_STREAM(this->get_logger(), result->message);
        goal_handle->abort(
          result);
        return;
      }
      RCLCPP_INFO(this->get_logger(), "Executing task [%s]", task.id.c_str());
      auto fb = std::make_shared<WorkcellRequest::Feedback>();
      fb->state.status = TaskState::STATUS_RUNNING;
      goal_handle->publish_feedback(fb);
    });
}

void RmfNexusTransporter::register_workcell()
{
  if (this->_ongoing_register)
  {
    RCLCPP_ERROR(
      this->get_logger(),
      "Failed to register: No response from system orchestrator.");
    if (!this->_register_workcell_client->remove_pending_request(*this->
      _ongoing_register))
    {
      RCLCPP_WARN(this->get_logger(),
        "Unable to remove pending request during workcell registration.");
    }
  }

  RCLCPP_INFO(this->get_logger(), "Registering with system orchestrator");
  auto register_cb =
    [this](rclcpp::Client<nexus::endpoints::RegisterWorkcellService::ServiceType>::
    SharedFuture future)
    {
      this->_ongoing_register = std::nullopt;
      auto resp = future.get();
      if (!resp->success)
      {
        switch (resp->error_code)
        {
          case nexus::endpoints::RegisterWorkcellService::ServiceType::Response::
            ERROR_NOT_READY:
            RCLCPP_ERROR(
              this->get_logger(),
              "Error while registering with system orchestrator, retrying again... [%s]",
              resp->message.c_str());
            break;
          default:
            RCLCPP_FATAL(this->get_logger(),
              "Failed to register with system orchestrator! [%s]",
              resp->message.c_str());
            //throw RegistrationError(resp->message, resp->error_code);
        }
        return;
      }
      RCLCPP_INFO(this->get_logger(),
        "Successfully registered with system orchestrator");
      this->_register_timer->cancel();
      // TODO(luca) reintroduce once https://github.com/ros2/rclcpp/issues/2652 is fixed and released
      // this->_register_timer.reset();
    };

  if (!this->_register_workcell_client->wait_for_service(
      std::chrono::seconds{0}))
  {
    std::string msg = "Could not find system orchestrator";
    auto secs = std::chrono::seconds(REGISTER_TICK_RATE).count();
    RCLCPP_ERROR(
      this->get_logger(), "Failed to register [%s], retrying in %ld secs",
      msg.c_str(), secs);
    // timer is not canceled so it will run again.
    return;
  }

  auto req =
    std::make_shared<nexus::endpoints::RegisterWorkcellService::ServiceType::Request>();
  // TODO(luca) check if we need to advertise capabilities
  /*
  std::vector<std::string> caps;
  caps.reserve(this->_capabilities.size());
  for (const auto& [k, _] : this->_capabilities)
  {
    caps.emplace_back(k);
  }
  req->description.capabilities = caps;
  */
  req->description.workcell_id = this->get_name();
  this->_ongoing_register = this->_register_workcell_client->async_send_request(
    req,
    register_cb);
}

// void RmfNexusTransporter::submit_itinerary(const std::string& job_id, const YAML::Node& order)
bool RmfNexusTransporter::submit_itinerary(GoalHandlePtr goal)
{
  const auto& job_id = goal->get_goal()->task.id;
  try
  {
    YAML::Node order = YAML::Load(goal->get_goal()->task.payload);
    nlohmann::json j;
    j["type"] = "dispatch_task_request";
    nlohmann::json r;
    r["unix_millis_request_time"] = 0;
    r["unix_millis_earliest_start_time"] = 0;
    r["requester"] = this->get_name();
    r["category"] = "compose";
    nlohmann::json d;
    d["category"] = "multi_delivery";
    d["phases"] = nlohmann::json::array();
    nlohmann::json activity;
    std::deque<std::string> places;
    activity["category"] = "sequence";
    activity["description"]["activities"] = nlohmann::json::array();
    for (const auto& node : order)
    {
      if (node["type"] && node["destination"])
      {
        auto type = node["type"].as<std::string>();
        auto destination = node["destination"].as<std::string>();
        nlohmann::json a;
        a["category"] = type;
        nlohmann::json p;
        // TODO(luca) exception safety for wrong types? Checking for pickup only since we don't do ingestors yet?
        p["place"] = destination;
        // TODO(luca) We should assign a handler that is related to the workcell.
        // For now the assumption is that a location has only one handler
        p["handler"] = destination;
        p["payload"] = nlohmann::json::array();;
        a["description"] = p;
        activity["description"]["activities"].push_back(a);
        places.push_back(destination);
      }
      else
      {
        // Error!
        return false;
      }
    }
    nlohmann::json act_obj;
    act_obj["activity"] = activity;
    d["phases"].push_back(act_obj);
    r["description"] = d;
    j["request"] = r;
    std::cout << j.dump(4) << std::endl;
    ApiRequest msg;
    msg.json_msg = j.dump();
    msg.request_id = job_id;
    _api_request_pub->publish(msg);
    job_id_to_sessions.insert({job_id, WorkcellSession {std::nullopt, goal, places}});
    return true;
  }
  catch (const YAML::Exception& e)
  {
    return false;
  }
}

std::optional<std::string> RmfNexusTransporter::process_signal(const std::string& job_id, const std::string& signal)
{
  auto it = job_id_to_sessions.find(job_id);
  if (it == job_id_to_sessions.end())
  {
    return "Job [" + job_id + "] not found";
  }
  // TODO(luca) should we create a custom message with defined constants to
  // emulate an enum type for signals?
  auto signals_it = job_id_to_sessions.find(job_id);
  if (signals_it == job_id_to_sessions.end() || signals_it->second.signals.empty())
  {
    return "Job [" + job_id + "] does not have any pending signals";
  }

  if (signal == signals_it->second.signals.front())
  {
    if (!it->second.rmf_task_id.has_value())
    {
      return "Job [" + job_id + "] is not being executed by RMF, this should not happen!";
    }
    DispenserResult msg;
    msg.request_guid = it->second.rmf_task_id.value();
    msg.source_guid = signal;
    msg.status = DispenserResult::SUCCESS;
    this->_dispenser_result_pub->publish(msg);
    signals_it->second.signals.pop_front();
  }
  else
  {
    return "Signal [" + signal + "] not being awaited right now";
  }

  return std::nullopt;
}

void RmfNexusTransporter::dispenser_request_cb(const DispenserRequest& msg)
{
  // Send a message to the signal_source port
  // TODO(luca) dispensers _actually_ have a target for their request so we
  // could change the signaling primitives to just be a boolean, or just always signal?
  const auto rmf_id_it = rmf_task_id_to_job_id.find(msg.request_guid);
  if (rmf_id_it == rmf_task_id_to_job_id.end())
  {
    return;
  }
  const auto it = job_id_to_sessions.find(rmf_id_it->second);
  if (it == job_id_to_sessions.end())
  {
    return;
  }
  if (sent_source_signals.find(msg.request_guid) != sent_source_signals.end())
  {
    // TODO(luca) Send success here?
    return;
  }
  std::cout << "Sending signal" << std::endl;
  // Now send the signal
  auto req = std::make_shared<nexus::endpoints::SignalWorkcellService::ServiceType::Request>();
  req->task_id = rmf_id_it->second;
  // TODO(luca) this should probably be an enum constant
  req->signal = msg.target_guid;
  // TODO(luca) provide a callback here
  // TODO(luca) cleanup periodically pending requests at the end of tasks to avoid leaking if
  // the target workcell is not listening to signals
  this->_signal_client->async_send_request(req);
  // For now always acknowledge
  DispenserResult res;
  res.request_guid = msg.request_guid;
  res.source_guid = msg.target_guid;
  res.status = DispenserResult::ACKNOWLEDGED;
  this->_dispenser_result_pub->publish(res);
  // sent_source_signals.insert(msg.request_guid);
}

void RmfNexusTransporter::api_response_cb(const ApiResponse& msg)
{
  std::cout << "Received API response" << std::endl;
  // Receive response, populate hashmaps
  if (msg.type != msg.TYPE_RESPONDING)
  {
    std::cout << "Request was not responded to!" << std::endl;
    return;
  }
  auto j = nlohmann::json::parse(msg.json_msg, nullptr, false);
  if (j.is_discarded())
  {
    std::cout << "Invalid json in api response" << std::endl;
    return;
  }
  // TODO(luca) exception safety for missing fields
  if (j["success"] == false)
  {
    std::cout << "Task submission failed" << std::endl;
    return;
  }
  std::cout << j.dump(4) << std::endl;
  std::string rmf_id = j["state"]["booking"]["id"];
  std::string job_id = msg.request_id;
  auto session_it = job_id_to_sessions.find(job_id);
  if (session_it == job_id_to_sessions.end())
  {
    std::cout << "Job id not found, this should not happen!" << std::endl;
    return;
  }
  session_it->second.rmf_task_id = rmf_id;
  rmf_task_id_to_job_id.insert({rmf_id, job_id});
}

void RmfNexusTransporter::task_state_cb(const TaskStateUpdate& msg)
{
  std::cout << "Received Task state" << std::endl;
  auto j = nlohmann::json::parse(msg.data, nullptr, false);
  std::cout << j.dump(4) << std::endl;
  if (j.is_discarded())
  {
    std::cout << "Invalid json in task state" << std::endl;
    return;
  }
  if (j["data"]["status"] == "completed")
  {
    // Finished!
    std::string rmf_id = j["data"]["booking"]["id"];
    auto job_id_it = rmf_task_id_to_job_id.find(rmf_id);
    if (job_id_it == rmf_task_id_to_job_id.end())
    {
      std::cout << "Job id not found" << std::endl;
      return;
    }
    auto job_id = job_id_it->second;
    auto session_it = job_id_to_sessions.find(job_id);
    if (session_it == job_id_to_sessions.end())
    {
      RCLCPP_ERROR(this->get_logger(), "Session not found for job [%s], this should not happen", job_id.c_str());
      return;
    }
    auto result = std::make_shared<nexus::endpoints::WorkcellRequestAction::ActionType::Result>();
    result->success = true;
    session_it->second.goal->succeed(result);
    // Bookkeeping
    // TODO(luca) also cleanup on cancellation successful, and when the above statements fail
    job_id_to_sessions.erase(session_it);
    rmf_task_id_to_job_id.erase(job_id_it);
  }
}

bool RmfNexusTransporter::can_perform_task(const WorkcellTask& task)
{
  // TODO(luca) Do simple navgraph parsing and figure out whether the task is doable
  return task.type == "transportation";

}

void RmfNexusTransporter::_handle_task_doable(
    nexus::endpoints::IsTaskDoableService::ServiceType::Request::ConstSharedPtr req,
    nexus::endpoints::IsTaskDoableService::ServiceType::Response::SharedPtr resp)
{
  resp->success = can_perform_task(req->task);
}

CallbackReturn RmfNexusTransporter::on_configure(
    const rclcpp_lifecycle::State& previous_state)
{
  this->init_subscriptions();
  return CallbackReturn::SUCCESS;
}

CallbackReturn RmfNexusTransporter::on_activate(const rclcpp_lifecycle::State& previous_state)
{
  return CallbackReturn::SUCCESS;
}

CallbackReturn RmfNexusTransporter::on_deactivate(const rclcpp_lifecycle::State& previous_state)
{
  return CallbackReturn::SUCCESS;
}

CallbackReturn RmfNexusTransporter::on_cleanup(const rclcpp_lifecycle::State& previous_state)
{
  return CallbackReturn::SUCCESS;
}

/*
// TODO(luca) In theory Nexus could send the same job_id since it's just the high level id.
// For example if a job has more than one transportation request.
std::optional<Itinerary> RmfNexusTransporter::get_itinerary(
  const std::string& job_id,
  const std::string& destination,
  const std::string& source)
{
  // TODO(luca) Interface with a node that computes a simple itinerary with
  // feasibility / time estimate based on the nav graph
  if (!_pimpl)
  {
    return std::nullopt;
  }
  auto n = _pimpl->_node.lock();
  if (!n)
  {
    return std::nullopt;
  }
  RCLCPP_INFO(
    n->get_logger(),
    "Received itinerary request with id [%s] for destination [%s]",
    job_id.c_str(),
    destination.c_str()
  );

  const auto now = n->get_clock()->now();
  const rclcpp::Time finish_time = now + rclcpp::Duration::from_seconds(60.0);
  const rclcpp::Time expiration_time = now + rclcpp::Duration::from_seconds(3600.0);
  return Itinerary(
    job_id,
    destination,
    "rmf",
    finish_time,
    expiration_time,
    source
  );
}

void RmfNexusTransporter::transport_to_destination(
  const Itinerary& itinerary,
  TransportFeedback feedback_cb,
  TransportCompleted completed_cb,
  const std::string& signal_destination,
  const std::string& signal_source)
{
  if (!_pimpl)
  {
    return;
  }
  auto n = _pimpl->_node.lock();
  if (!n)
  {
    return;
  }
  RCLCPP_INFO(
    n->get_logger(),
    "Received request to travel to destination [%s]",
    itinerary.destination().c_str()
  );

  _pimpl->submit_itinerary(itinerary, completed_cb, signal_destination, signal_source);
}

bool RmfNexusTransporter::cancel(const Itinerary& itinerary)
{
  if (!_pimpl)
  {
    return false;
  }
  auto n = _pimpl->_node.lock();
  if (!n)
  {
    return false;
  }
  RCLCPP_INFO(
    n->get_logger(),
    "Received request to cancel travel to destination [%s]",
    itinerary.destination().c_str()
  );
  // TODO(luca) interface for task cancellation
  return false;
}
*/

}  // namespace rmf_nexus_transporter

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(
  rmf_nexus_transporter::RmfNexusTransporter)
