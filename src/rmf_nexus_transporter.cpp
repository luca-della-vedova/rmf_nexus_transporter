#include <nlohmann/json.hpp>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>

#include <nexus_endpoints.hpp>

#include <rmf_dispenser_msgs/msg/dispenser_result.hpp>
#include <rmf_dispenser_msgs/msg/dispenser_request.hpp>
#include <rmf_dispenser_msgs/msg/dispenser_state.hpp>
#include <rmf_ingestor_msgs/msg/ingestor_result.hpp>
#include <rmf_ingestor_msgs/msg/ingestor_request.hpp>
#include <rmf_ingestor_msgs/msg/ingestor_state.hpp>
#include <rmf_task_msgs/msg/api_request.hpp>
#include <rmf_task_msgs/msg/api_response.hpp>
#include <std_msgs/msg/string.hpp>

#include "rmf_nexus_transporter/rmf_nexus_transporter.hpp"

namespace rmf_nexus_transporter
{

using ApiRequest = rmf_task_msgs::msg::ApiRequest;
using ApiResponse = rmf_task_msgs::msg::ApiResponse;

using DispenserResult = rmf_dispenser_msgs::msg::DispenserResult;
using DispenserState = rmf_dispenser_msgs::msg::DispenserState;
using DispenserRequest = rmf_dispenser_msgs::msg::DispenserRequest;
using IngestorResult = rmf_ingestor_msgs::msg::IngestorResult;
using IngestorState = rmf_ingestor_msgs::msg::IngestorState;
using IngestorRequest = rmf_ingestor_msgs::msg::IngestorRequest;
// Json format
using TaskStateUpdate = std_msgs::msg::String;

class RmfNexusTransporter::Implementation {
public:
  struct WorkcellSession {
    std::string name;
    std::string task_id;
    rclcpp::Client<nexus::endpoints::SignalWorkcellService::ServiceType>::SharedPtr client;
  };

  rclcpp_lifecycle::LifecycleNode::WeakPtr _node;
  bool _ready = false;
  static std::unique_ptr<Implementation> make(rclcpp_lifecycle::LifecycleNode::WeakPtr node)
  {
    auto n = node.lock();
    if (!n)
    {
      return nullptr;
    }
    auto impl = std::make_unique<Implementation>();
    impl->_node = node;
    impl->init_subscriptions(n);
    impl->_ready = true;
    return impl;
  }

  void init_subscriptions(rclcpp_lifecycle::LifecycleNode::SharedPtr n)
  {
    const auto transient_qos =
      rclcpp::SystemDefaultsQoS().transient_local().keep_last(10).reliable();
    this->_api_request_pub = n->create_publisher<ApiRequest>("/task_api_requests", transient_qos);
    this->_api_response_sub = n->create_subscription<ApiResponse>("/task_api_responses", transient_qos,
        [&](ApiResponse::UniquePtr msg)
        {
          this->api_response_cb(*msg);
        });
    this->_task_state_sub = n->create_subscription<TaskStateUpdate>("/task_state_update", transient_qos,
        [&](TaskStateUpdate::UniquePtr msg)
        {
          this->task_state_cb(*msg);
        });
    // TODO(luca) check qos here
    this->_dispenser_result_pub = n->create_publisher<DispenserResult>("/dispenser_results", 10);
    this->_dispenser_state_pub = n->create_publisher<DispenserState>("/dispenser_states", 10);
    this->_dispenser_request_sub = n->create_subscription<DispenserRequest>("/dispenser_requests", 10,
        [&](DispenserRequest::UniquePtr msg)
        {
          this->dispenser_request_cb(*msg);
        });
    this->_ingestor_result_pub = n->create_publisher<IngestorResult>("/ingestor_results", 10);
    this->_ingestor_state_pub = n->create_publisher<IngestorState>("/ingestor_states", 10);
    this->_ingestor_request_sub = n->create_subscription<IngestorRequest>("/ingestor_requests", 10,
        [&](IngestorRequest::UniquePtr msg)
        {
          this->ingestor_request_cb(*msg);
        });
  }

  void submit_itinerary(const Itinerary& itinerary, TransportCompleted cb, const std::string& signal_destination, const std::string& signal_source)
  {
    nlohmann::json j;
    j["type"] = "dispatch_task_request";
    nlohmann::json r;
    r["unix_millis_request_time"] = 0;
    r["unix_millis_earliest_start_time"] = 0;
    r["requester"] = "rmf_nexus_transporter";
    r["category"] = "delivery";
    nlohmann::json p;
    p["place"] = itinerary.source();
    // TODO(luca) We should assign a handler that is related to the workcell.
    // For now the assumption is that a location has only one handler
    p["handler"] = itinerary.source();
    p["payload"] = nlohmann::json::array();;
    nlohmann::json d;
    d["place"] = itinerary.destination();
    d["payload"] = nlohmann::json::array();;
    d["handler"] = itinerary.destination();
    r["description"]["pickup"] = p;
    r["description"]["dropoff"] = d;
    j["request"] = r;
    std::cout << j.dump(4) << std::endl;
    ApiRequest msg;
    msg.json_msg = j.dump();
    msg.request_id = itinerary.id();
    _api_request_pub->publish(msg);
    job_id_to_completed.insert({itinerary.id(), cb});
    // TODO(luca) have a single hash map?
    auto n = this->_node.lock();
    if (!n)
    {
      // Error!
      std::cout << "Node not valid!" << std::endl;
      return;
    }
    if (!signal_destination.empty())
    {
      auto client = n->create_client<nexus::endpoints::SignalWorkcellService::ServiceType>(
         nexus::endpoints::SignalWorkcellService::service_name(itinerary.destination()));
      job_id_to_signal_destination.insert({itinerary.id(), WorkcellSession {itinerary.destination(), signal_destination, client}});
    }
    if (!signal_source.empty())
    {
      auto client = n->create_client<nexus::endpoints::SignalWorkcellService::ServiceType>(
         nexus::endpoints::SignalWorkcellService::service_name(itinerary.source()));
      job_id_to_signal_source.insert({itinerary.id(), WorkcellSession {itinerary.source(), signal_source, client}});
    }
  }

  std::optional<std::string> process_signal(const std::string& job_id, const std::string& signal)
  {
    auto it = job_id_to_rmf_task_id.find(job_id);
    if (it == job_id_to_rmf_task_id.end())
    {
      return "Job [" + job_id + "] not found";
    }
    // TODO(luca) should we create a custom message with defined constants to
    // emulate an enum type for signals?
    if (signal == "place_done")
    {
      auto source_it = job_id_to_signal_source.find(job_id);
      if (source_it == job_id_to_signal_source.end())
      {
        return "Job [" + job_id + "] does not have a source signal";
      }
      DispenserResult msg;
      msg.request_guid = it->second;
      msg.source_guid = source_it->second.name;
      msg.status = DispenserResult::SUCCESS;
      this->_dispenser_result_pub->publish(msg);
    }
    else if (signal == "pick_done")
    {
      auto source_it = job_id_to_signal_destination.find(job_id);
      if (source_it == job_id_to_signal_destination.end())
      {
        return "Job [" + job_id + "] does not have a source signal";
      }
      IngestorResult msg;
      msg.request_guid = it->second;
      msg.source_guid = source_it->second.name;
      msg.status = DispenserResult::SUCCESS;
      this->_ingestor_result_pub->publish(msg);
    }
    else
    {
      return "Signal [" + signal + "] not supported";
    }

    return std::nullopt;
  }
private:
  // Hashmap requested job_id -> actual rmf task id (i.e. for cancellation, tracking)
  // TODO(luca) remember to cleanup all the maps / sets when the task is finished or cancelled
  std::unordered_map<std::string, std::string> job_id_to_rmf_task_id;
  std::unordered_map<std::string, std::string> rmf_task_id_to_job_id;
  // TODO(luca) submit progress through feedback callback
  // Store the completed callback
  std::unordered_map<std::string, TransportCompleted> job_id_to_completed;
  // Store the signals
  std::unordered_map<std::string, WorkcellSession> job_id_to_signal_source;
  std::unordered_map<std::string, WorkcellSession> job_id_to_signal_destination;

  // TODO(luca) actually use this
  std::unordered_set<std::string> sent_source_signals;
  std::unordered_set<std::string> sent_destination_signals;

  // Task interface
  rclcpp::Publisher<ApiRequest>::SharedPtr _api_request_pub = nullptr;
  rclcpp::Subscription<ApiResponse>::SharedPtr _api_response_sub = nullptr;
  rclcpp::Subscription<TaskStateUpdate>::SharedPtr _task_state_sub = nullptr;

  // Dispenser interface
  rclcpp::Publisher<DispenserResult>::SharedPtr _dispenser_result_pub = nullptr;
  rclcpp::Publisher<DispenserState>::SharedPtr _dispenser_state_pub = nullptr;
  rclcpp::Subscription<DispenserRequest>::SharedPtr _dispenser_request_sub = nullptr;
  // Ingestor interface
  rclcpp::Publisher<IngestorResult>::SharedPtr _ingestor_result_pub = nullptr;
  rclcpp::Publisher<IngestorState>::SharedPtr _ingestor_state_pub = nullptr;
  rclcpp::Subscription<IngestorRequest>::SharedPtr _ingestor_request_sub = nullptr;

  void dispenser_request_cb(const DispenserRequest& msg)
  {
    // Send a message to the signal_source port
    // TODO(luca) dispensers _actually_ have a target for their request so we
    // could change the signaling primitives to just be a boolean, or just always signal?
    const auto rmf_id_it = rmf_task_id_to_job_id.find(msg.request_guid);
    if (rmf_id_it == rmf_task_id_to_job_id.end())
    {
      return;
    }
    const auto it = job_id_to_signal_source.find(rmf_id_it->second);
    if (it == job_id_to_signal_source.end())
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
    req->task_id = it->second.task_id;
    // TODO(luca) this should probably be an enum constant
    req->signal = "transporter_done";
    // TODO(luca) provide a callback here
    // TODO(luca) cleanup periodically pending requests at the end of tasks to avoid leaking if
    // the target workcell is not listening to signals
    it->second.client->async_send_request(req);
    // For now always acknowledge
    DispenserResult res;
    res.request_guid = msg.request_guid;
    res.source_guid = msg.target_guid;
    res.status = DispenserResult::ACKNOWLEDGED;
    this->_dispenser_result_pub->publish(res);
    // sent_source_signals.insert(msg.request_guid);
  }

  void ingestor_request_cb(const IngestorRequest& msg)
  {
    // Send a message to the signal_destination port
    // TODO(luca) dispensers _actually_ have a target for their request so we
    // could change the signaling primitives to just be a boolean, or just always signal?
    const auto rmf_id_it = rmf_task_id_to_job_id.find(msg.request_guid);
    if (rmf_id_it == rmf_task_id_to_job_id.end())
    {
      return;
    }
    const auto it = job_id_to_signal_destination.find(rmf_id_it->second);
    if (it == job_id_to_signal_source.end())
    {
      return;
    }
    if (sent_destination_signals.find(msg.request_guid) != sent_destination_signals.end())
    {
      // TODO(luca) Send success here?
      return;
    }
    std::cout << "Sending signal" << std::endl;
    // Now send the signal
    auto req = std::make_shared<nexus::endpoints::SignalWorkcellService::ServiceType::Request>();
    req->task_id = it->second.task_id;
    // TODO(luca) this should probably be an enum constant
    req->signal = "transporter_done";
    // TODO(luca) provide a callback here
    // TODO(luca) cleanup periodically pending requests at the end of tasks to avoid leaking if
    // the target workcell is not listening to signals
    it->second.client->async_send_request(req);
    // For now always acknowledge
    IngestorResult res;
    res.request_guid = msg.request_guid;
    res.source_guid = msg.target_guid;
    res.status = DispenserResult::ACKNOWLEDGED;
    this->_ingestor_result_pub->publish(res);
    // sent_destination_signals.insert(msg.request_guid);
  }

  void api_response_cb(const ApiResponse& msg)
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
    job_id_to_rmf_task_id.insert({job_id, rmf_id});
    rmf_task_id_to_job_id.insert({rmf_id, job_id});
  }

  void task_state_cb(const TaskStateUpdate& msg)
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
      auto completed_cb = job_id_to_completed.find(job_id);
      if (completed_cb == job_id_to_completed.end())
      {
        std::cout << "Completed callback not found" << std::endl;
        return;
      }
      // Signal completion
      (completed_cb->second)(true);
      // Bookkeeping
      // TODO(luca) also cleanup on cancellation successful, and when the above statements fail
      job_id_to_completed.erase(job_id);
      job_id_to_rmf_task_id.erase(job_id);
      job_id_to_signal_source.erase(job_id);
      job_id_to_signal_destination.erase(job_id);
      rmf_task_id_to_job_id.erase(rmf_id);
    }
  }
};

bool RmfNexusTransporter::configure(const rclcpp_lifecycle::LifecycleNode::WeakPtr& node)
{
  auto n = node.lock();
  if (!n)
  {
    return false;
  }
  this->_pimpl = Implementation::make(node);
  // TODO(luca) get RMF parameters here
  return true;
}

bool RmfNexusTransporter::ready() const
{
  return _pimpl->_ready;
}

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

std::optional<std::string> RmfNexusTransporter::process_signal(
  const std::string& task_id,
  const std::string& signal)
{
  return _pimpl->process_signal(task_id, signal);
}

}  // namespace rmf_nexus_transporter

#include <pluginlib/class_list_macros.hpp>

PLUGINLIB_EXPORT_CLASS(
  rmf_nexus_transporter::RmfNexusTransporter, nexus_transporter::Transporter)
