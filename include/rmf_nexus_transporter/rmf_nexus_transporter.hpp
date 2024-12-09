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

  bool can_perform_task(const WorkcellTask& task);

  void _handle_task_doable(
      nexus::endpoints::IsTaskDoableService::ServiceType::Request::ConstSharedPtr req,
      nexus::endpoints::IsTaskDoableService::ServiceType::Response::SharedPtr resp);

  struct WorkcellSession {
    /*
    std::string name;
    std::string task_id;
    */
    std::optional<std::string> rmf_task_id;
    GoalHandlePtr goal;
    // Queue of signals that the the workcell / RMF will need to synchronize on,
    // in order of occurrence
    std::deque<std::string> signals;
  };

  void init_subscriptions();

  void register_workcell();

  bool submit_itinerary(GoalHandlePtr goal);

  std::optional<std::string> process_signal(const std::string& job_id, const std::string& signal);
private:
  // Hashmap requested job_id -> actual rmf task id (i.e. for cancellation, tracking)
  // TODO(luca) remember to cleanup all the maps / sets when the task is finished or cancelled
  std::unordered_map<std::string, std::string> rmf_task_id_to_job_id;
  // TODO(luca) submit progress through feedback callback
  // Store the signals
  std::unordered_map<std::string, WorkcellSession> job_id_to_sessions;

  // TODO(luca) actually use this
  std::unordered_set<std::string> sent_source_signals;
  std::unordered_set<std::string> sent_destination_signals;

  // Task interface
  rclcpp::Publisher<ApiRequest>::SharedPtr _api_request_pub;
  rclcpp::Subscription<ApiResponse>::SharedPtr _api_response_sub;
  rclcpp::Subscription<TaskStateUpdate>::SharedPtr _task_state_sub;

  // Dispenser interface
  rclcpp::Publisher<DispenserResult>::SharedPtr _dispenser_result_pub;
  rclcpp::Publisher<DispenserState>::SharedPtr _dispenser_state_pub;
  rclcpp::Subscription<DispenserRequest>::SharedPtr _dispenser_request_sub;

  void dispenser_request_cb(const DispenserRequest& msg);

  void api_response_cb(const ApiResponse& msg);

  void task_state_cb(const TaskStateUpdate& msg);

  /*
  /// Return true if the transporter is configured and ready.
  virtual bool ready() const override;

  /// Receive an itinerary for a destination
  ///
  /// \param[in] job_id
  /// An id for this request.
  ///
  /// \param[in] destination
  /// The name of the destination
  /// \return A nullopt is returned if the destination is not valid.
  // TODO(YV): Consider creating a separate class for destination
  virtual std::optional<Itinerary> get_itinerary(
    const std::string& job_id,
    const std::string& destination,
    const std::string& source) override;

  /// Request the transporter to go to a destination. This call should be
  /// non-blocking.
  /// \param[in] itinerary
  ///   The itinerary previously generated by the transporter.
  ///   Note: The itinerary's validity should not have expired.
  ///
  /// \param[in] feedback_cb
  ///   A callback to execute to submit feedback on the transporter's progress
  ///
  /// \param[in] completed_cb
  ///   A callback to execute to when the transportation has failed or succeeded
  virtual void transport_to_destination(
    const Itinerary& itinerary,
    TransportFeedback feedback_cb,
    TransportCompleted completed_cb,
    const std::string& signal_destination,
    const std::string& signal_source) override;

  /// Process a signal sent to this transporter, not all transporter might need
  /// this interface so it is not mandatory to implement it.
  /// \param[in] job_id
  ///   The id to send the signal to
  ///
  /// \param[in] signal
  ///   The signal to send
  ///
  /// \return true if the signal was accepted, false if it was not.
  virtual std::optional<std::string> process_signal(
    const std::string& task_id,
    const std::string& signal) override;

  /// Cancel the presently assigned task
  /// \param[in] itinerary
  ///   The itinerary of the task to cancel
  /// \return True if the cancellation was successful.
  virtual bool cancel(const Itinerary& itinerary) override;
  */

};

}  // namespace rmf_nexus_transporter

#endif  // RMF_NEXUS_TRANSPORTER__RMF_NEXUS_TRANSPORTER_HPP_
