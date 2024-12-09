#ifndef RMF_NEXUS_TRANSPORTER__RMF_NEXUS_TRANSPORTER_HPP_
#define RMF_NEXUS_TRANSPORTER__RMF_NEXUS_TRANSPORTER_HPP_

#include <deque>

#include <rclcpp_lifecycle/lifecycle_node.hpp>

#include <nexus_endpoints.hpp>
#include <nexus_orchestrator_msgs/msg/workcell_task.hpp>
#include <nexus_orchestrator_msgs/srv/is_task_doable.hpp>

#include <yaml-cpp/yaml.h>

namespace rmf_nexus_transporter
{

class RmfNexusTransporter : public rclcpp_lifecycle::LifecycleNode
{
public:
  using CallbackReturn =
      rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;
  using IsTaskDoable = nexus_orchestrator_msgs::srv::IsTaskDoable;
  using GoalHandlePtr =
      std::shared_ptr<rclcpp_action::ServerGoalHandle<nexus::endpoints::WorkcellRequestAction::ActionType>>;
  using WorkcellTask = nexus_orchestrator_msgs::msg::WorkcellTask;

  // RMF interfaces
  using ApiRequest = rmf_task_msgs::msg::ApiRequest;
  using ApiResponse = rmf_task_msgs::msg::ApiResponse;

  using DispenserResult = rmf_dispenser_msgs::msg::DispenserResult;
  using DispenserState = rmf_dispenser_msgs::msg::DispenserState;
  using DispenserRequest = rmf_dispenser_msgs::msg::DispenserRequest;
  // Json format
  using TaskStateUpdate = std_msgs::msg::String;

  RmfNexusTransporter(
    const rclcpp::NodeOptions& options = rclcpp::NodeOptions{});

  CallbackReturn on_configure(
      const rclcpp_lifecycle::State& previous_state) override;

  CallbackReturn on_activate(
      const rclcpp_lifecycle::State& previous_state) override;

  CallbackReturn on_deactivate(
      const rclcpp_lifecycle::State& previous_state) override;

  CallbackReturn on_cleanup(const rclcpp_lifecycle::State& previous_state)
    override;

private:
  struct WorkcellSession {
    std::optional<std::string> rmf_task_id;
    GoalHandlePtr goal;
    // Queue of signals that the the workcell / RMF will need to synchronize on,
    // in order of occurrence
    std::deque<std::string> signals;
  };

  // Nexus interfaces
  rclcpp::Service<nexus::endpoints::IsTaskDoableService::ServiceType>::SharedPtr
      _task_doable_srv;

  rclcpp::Client<nexus::endpoints::RegisterWorkcellService::ServiceType>::SharedPtr
      _register_workcell_client;

  rclcpp::Service<nexus::endpoints::SignalWorkcellService::ServiceType>::SharedPtr
      _signal_srv;
  rclcpp::Client<nexus::endpoints::SignalWorkcellService::ServiceType>::SharedPtr _signal_client;

  rclcpp_action::Server<nexus::endpoints::WorkcellRequestAction::ActionType>::SharedPtr _cmd_server;

  rclcpp::TimerBase::SharedPtr _register_timer;
  std::optional<rclcpp::Client<nexus::endpoints::RegisterWorkcellService::ServiceType>::
      SharedFutureAndRequestId> _ongoing_register;

  // RMF interfaces
  // Task interface
  rclcpp::Publisher<ApiRequest>::SharedPtr _api_request_pub;
  rclcpp::Subscription<ApiResponse>::SharedPtr _api_response_sub;
  rclcpp::Subscription<TaskStateUpdate>::SharedPtr _task_state_sub;

  // Dispenser interface
  rclcpp::Publisher<DispenserResult>::SharedPtr _dispenser_result_pub;
  rclcpp::Publisher<DispenserState>::SharedPtr _dispenser_state_pub;
  rclcpp::Subscription<DispenserRequest>::SharedPtr _dispenser_request_sub;

  // Hashmap requested job_id -> actual rmf task id (i.e. for cancellation, tracking)
  std::unordered_map<std::string, std::string> rmf_task_id_to_job_id;
  // TODO(luca) submit progress through feedback callback
  std::unordered_map<std::string, WorkcellSession> job_id_to_sessions;

  bool can_perform_task(const WorkcellTask& task);

  void cancel_task(const std::string& task_id);

  void handle_task_doable(
      nexus::endpoints::IsTaskDoableService::ServiceType::Request::ConstSharedPtr req,
      nexus::endpoints::IsTaskDoableService::ServiceType::Response::SharedPtr resp);

  void init_subscriptions();

  void register_workcell();

  std::optional<std::string> submit_itinerary(GoalHandlePtr goal);

  std::optional<std::string> process_signal(const std::string& job_id, const std::string& signal);

  void dispenser_request_cb(const DispenserRequest& msg);

  void api_response_cb(const ApiResponse& msg);

  void task_state_cb(const TaskStateUpdate& msg);
};

}  // namespace rmf_nexus_transporter

#endif  // RMF_NEXUS_TRANSPORTER__RMF_NEXUS_TRANSPORTER_HPP_
