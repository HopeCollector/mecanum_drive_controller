#include "mecanum_drive_controller/mecanum_drive_controller.hpp"
#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "tf2/LinearMath/Quaternion.h"
#include "tf2/LinearMath/Matrix3x3.h"

namespace { // utility

using ControllerReferenceMsg =
    mecanum_drive_controller::MecanumDriveController::ControllerReferenceMsg;

// called from RT control loop
void reset_controller_reference_msg(
    const std::shared_ptr<ControllerReferenceMsg> &msg,
    const std::shared_ptr<rclcpp_lifecycle::LifecycleNode> &node) {
  msg->header.stamp = node->now();
  msg->twist.linear.x = std::numeric_limits<double>::quiet_NaN();
  msg->twist.linear.y = std::numeric_limits<double>::quiet_NaN();
  msg->twist.linear.z = std::numeric_limits<double>::quiet_NaN();
  msg->twist.angular.x = std::numeric_limits<double>::quiet_NaN();
  msg->twist.angular.y = std::numeric_limits<double>::quiet_NaN();
  msg->twist.angular.z = std::numeric_limits<double>::quiet_NaN();
}

// return True if vl:{x, y} && va:{z} not nan
bool is_msg_valid(const ControllerReferenceMsg::ConstSharedPtr &msg) {
  return !std::isnan(msg->twist.linear.x) && !std::isnan(msg->twist.linear.y) &&
         !std::isnan(msg->twist.angular.z);
}

} // namespace

namespace mecanum_drive_controller
{

MecanumDriveController::MecanumDriveController()
: controller_interface::ChainableControllerInterface()
{
}

controller_interface::CallbackReturn MecanumDriveController::on_init() {
  try {
    param_listener_ = std::make_shared<ParamListener>(get_node());
    params_ = param_listener_->get_params();
  } catch (const std::exception &e) {
    fprintf(stderr,
            "Exception thrown during controller's init with message: %s \n",
            e.what());
    return controller_interface::CallbackReturn::ERROR;
  }
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::InterfaceConfiguration
MecanumDriveController::command_interface_configuration() const {
  controller_interface::InterfaceConfiguration command_interfaces_config;
  command_interfaces_config.type =
      controller_interface::interface_configuration_type::INDIVIDUAL;

  command_interfaces_config.names.reserve(command_joint_names_.size());
  for (const auto &joint : command_joint_names_) {
    command_interfaces_config.names.push_back(
        joint + "/" + hardware_interface::HW_IF_VELOCITY);
  }

  return command_interfaces_config;
}

controller_interface::InterfaceConfiguration
MecanumDriveController::state_interface_configuration() const {
  controller_interface::InterfaceConfiguration state_interfaces_config;
  state_interfaces_config.type =
      controller_interface::interface_configuration_type::INDIVIDUAL;

  state_interfaces_config.names.reserve(state_joint_names_.size());

  for (const auto &joint : state_joint_names_) {
    state_interfaces_config.names.push_back(joint + "/" +
                                            hardware_interface::HW_IF_VELOCITY);
  }

  return state_interfaces_config;
}

controller_interface::CallbackReturn MecanumDriveController::on_configure(
    const rclcpp_lifecycle::State &previous_state) {
  params_ = param_listener_->get_params();

  auto prepare_lists_with_joint_names =
      [&command_joints = this->command_joint_names_,
       &state_joints = this->state_joint_names_](
          const std::size_t index, const std::string &command_joint_name,
          const std::string &state_joint_name) {
        command_joints[index] = command_joint_name;
        if (state_joint_name.empty()) {
          state_joints[index] = command_joint_name;
        } else {
          state_joints[index] = state_joint_name;
        }
      };

  command_joint_names_.resize(4);
  state_joint_names_.resize(4);

  // The joint names are sorted according to the order documented in the header
  // file!
  prepare_lists_with_joint_names(FRONT_LEFT,
                                 params_.front_left_wheel_command_joint_name,
                                 params_.front_left_wheel_state_joint_name);
  prepare_lists_with_joint_names(FRONT_RIGHT,
                                 params_.front_right_wheel_command_joint_name,
                                 params_.front_right_wheel_state_joint_name);
  prepare_lists_with_joint_names(REAR_RIGHT,
                                 params_.rear_right_wheel_command_joint_name,
                                 params_.rear_right_wheel_state_joint_name);
  prepare_lists_with_joint_names(REAR_LEFT,
                                 params_.rear_left_wheel_command_joint_name,
                                 params_.rear_left_wheel_state_joint_name);

  // topics QoS
  auto subscribers_qos = rclcpp::SystemDefaultsQoS();
  subscribers_qos.keep_last(1);
  subscribers_qos.best_effort();

  // Reference Subscriber
  ref_timeout_ = rclcpp::Duration::from_seconds(params_.reference_timeout);
  ref_subscriber_ = get_node()->create_subscription<ControllerReferenceMsg>(
      "~/reference", subscribers_qos,
      std::bind(&MecanumDriveController::reference_callback, this,
                std::placeholders::_1));

  // send a STOP command(all Nan in msg)
  std::shared_ptr<ControllerReferenceMsg> msg =
      std::make_shared<ControllerReferenceMsg>();
  reset_controller_reference_msg(msg, get_node());
  input_ref_.writeFromNonRT(msg);

  try {
    // controller State publisher
    controller_s_publisher_ = get_node()->create_publisher<ControllerStateMsg>(
        "~/controller_state", rclcpp::SystemDefaultsQoS());
    controller_state_publisher_ =
        std::make_unique<ControllerStatePublisher>(controller_s_publisher_);
  } catch (const std::exception &e) {
    fprintf(stderr,
            "Exception thrown during publisher creation at configure stage "
            "with message : %s \n",
            e.what());
    return controller_interface::CallbackReturn::ERROR;
  }

  controller_state_publisher_->lock();
  controller_state_publisher_->msg_.header.stamp = get_node()->now();
  controller_state_publisher_->msg_.header.frame_id = params_.odom_frame_id;
  controller_state_publisher_->unlock();

  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn MecanumDriveController::on_activate(
    const rclcpp_lifecycle::State &previous_state) {
  // Set default value in command
  reset_controller_reference_msg(*(input_ref_.readFromRT()), get_node());

  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn MecanumDriveController::on_deactivate(
    const rclcpp_lifecycle::State &previous_state) {
  for (size_t i = 0; i < NR_CMD_ITFS; ++i) {
    command_interfaces_[i].set_value(std::numeric_limits<double>::quiet_NaN());
  }
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::return_type
MecanumDriveController::update_reference_from_subscribers(
    const rclcpp::Time &time, const rclcpp::Duration &period) {
  auto current_ref = *(input_ref_.readFromRT());
  const auto age_of_last_command = time - (current_ref)->header.stamp;
  bool is_msg_ok = is_msg_valid(current_ref);

  // returen if message not ok
  if (!is_msg_ok) {
    return controller_interface::return_type::OK;
  }

  // send only if msg valid and real in-time
  if (age_of_last_command <= ref_timeout_) {
    reference_interfaces_[0] = current_ref->twist.linear.x;
    reference_interfaces_[1] = current_ref->twist.linear.y;
    reference_interfaces_[2] = current_ref->twist.angular.z;
  } else if (ref_timeout_ == rclcpp::Duration::from_seconds(0)) {
    // always send STOP if ref_timeout_ is 0.0
    reference_interfaces_[0] = current_ref->twist.linear.x;
    reference_interfaces_[1] = current_ref->twist.linear.y;
    reference_interfaces_[2] = current_ref->twist.angular.z;
    current_ref->twist.linear.x = std::numeric_limits<double>::quiet_NaN();
    current_ref->twist.linear.y = std::numeric_limits<double>::quiet_NaN();
    current_ref->twist.angular.z = std::numeric_limits<double>::quiet_NaN();
  } else{
    // if command is ok, but timeout, send STOP
    reference_interfaces_[0] = 0.0;
    reference_interfaces_[1] = 0.0;
    reference_interfaces_[2] = 0.0;

    current_ref->twist.linear.x = std::numeric_limits<double>::quiet_NaN();
    current_ref->twist.linear.y = std::numeric_limits<double>::quiet_NaN();
    current_ref->twist.angular.z = std::numeric_limits<double>::quiet_NaN();
  }

  return controller_interface::return_type::OK;
}

controller_interface::return_type
MecanumDriveController::update_and_write_commands(
    const rclcpp::Time &time, const rclcpp::Duration &period) {
  // INVERSE KINEMATICS (move robot).
  // Compute wheels velocities (this is the actual ik):
  // NOTE: the input desired twist (from topic `~/reference`) is a body twist.
  if (!std::isnan(reference_interfaces_[0]) &&
      !std::isnan(reference_interfaces_[1]) &&
      !std::isnan(reference_interfaces_[2])) {
    tf2::Quaternion quaternion;
    quaternion.setRPY(0.0, 0.0, params_.kinematics.base_frame_offset.theta);
    /// \note The variables meaning:
    /// rotation_from_base_to_center: Rotation transformation matrix, to
    /// transform from base frame to center frame
    /// linear_trans_from_base_to_center: offset/linear transformation matrix,
    /// to transform from base frame to center frame

    tf2::Matrix3x3 rotation_from_base_to_center = tf2::Matrix3x3((quaternion));
    tf2::Vector3 velocity_in_base_frame_w_r_t_center_frame_ =
        rotation_from_base_to_center *
        tf2::Vector3(reference_interfaces_[0], reference_interfaces_[1], 0.0);
    tf2::Vector3 linear_trans_from_base_to_center =
        tf2::Vector3(params_.kinematics.base_frame_offset.x,
                     params_.kinematics.base_frame_offset.y, 0.0);

    velocity_in_center_frame_linear_x_ =
        velocity_in_base_frame_w_r_t_center_frame_.x() +
        linear_trans_from_base_to_center.y() * reference_interfaces_[2];
    velocity_in_center_frame_linear_y_ =
        velocity_in_base_frame_w_r_t_center_frame_.y() -
        linear_trans_from_base_to_center.x() * reference_interfaces_[2];
    velocity_in_center_frame_angular_z_ = reference_interfaces_[2];

    const double wheel_front_left_vel =
        1.0 / params_.kinematics.wheels_radius *
        (velocity_in_center_frame_linear_x_ -
         velocity_in_center_frame_linear_y_ -
         params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis *
             velocity_in_center_frame_angular_z_);
    const double wheel_front_right_vel =
        1.0 / params_.kinematics.wheels_radius *
        (velocity_in_center_frame_linear_x_ +
         velocity_in_center_frame_linear_y_ +
         params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis *
             velocity_in_center_frame_angular_z_);
    const double wheel_rear_right_vel =
        1.0 / params_.kinematics.wheels_radius *
        (velocity_in_center_frame_linear_x_ -
         velocity_in_center_frame_linear_y_ +
         params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis *
             velocity_in_center_frame_angular_z_);
    const double wheel_rear_left_vel =
        1.0 / params_.kinematics.wheels_radius *
        (velocity_in_center_frame_linear_x_ +
         velocity_in_center_frame_linear_y_ -
         params_.kinematics.sum_of_robot_center_projection_on_X_Y_axis *
             velocity_in_center_frame_angular_z_);

    // Set wheels velocities - The joint names are sorted accoring to the order
    // documented in the header file!
    command_interfaces_[FRONT_LEFT].set_value(wheel_front_left_vel);
    command_interfaces_[FRONT_RIGHT].set_value(wheel_front_right_vel);
    command_interfaces_[REAR_RIGHT].set_value(wheel_rear_right_vel);
    command_interfaces_[REAR_LEFT].set_value(wheel_rear_left_vel);
  } else {
    command_interfaces_[FRONT_LEFT].set_value(0.0);
    command_interfaces_[FRONT_RIGHT].set_value(0.0);
    command_interfaces_[REAR_RIGHT].set_value(0.0);
    command_interfaces_[REAR_LEFT].set_value(0.0);
  }

  if (controller_state_publisher_->trylock()) {
    controller_state_publisher_->msg_.header.stamp = get_node()->now();
    controller_state_publisher_->msg_.front_left_wheel_velocity =
        state_interfaces_[FRONT_LEFT].get_value();
    controller_state_publisher_->msg_.front_right_wheel_velocity =
        state_interfaces_[FRONT_RIGHT].get_value();
    controller_state_publisher_->msg_.back_right_wheel_velocity =
        state_interfaces_[REAR_RIGHT].get_value();
    controller_state_publisher_->msg_.back_left_wheel_velocity =
        state_interfaces_[REAR_LEFT].get_value();
    controller_state_publisher_->msg_.reference_velocity.linear.x =
        reference_interfaces_[0];
    controller_state_publisher_->msg_.reference_velocity.linear.y =
        reference_interfaces_[1];
    controller_state_publisher_->msg_.reference_velocity.angular.z =
        reference_interfaces_[2];
    controller_state_publisher_->unlockAndPublish();
  }

  reference_interfaces_[0] = std::numeric_limits<double>::quiet_NaN();
  reference_interfaces_[1] = std::numeric_limits<double>::quiet_NaN();
  reference_interfaces_[2] = std::numeric_limits<double>::quiet_NaN();

  return controller_interface::return_type::OK;
}

void MecanumDriveController::reference_callback(
    const std::shared_ptr<ControllerReferenceMsg> msg) {
  // if no timestamp provided use current time for command timestamp
  if (msg->header.stamp.sec == 0 && msg->header.stamp.nanosec == 0u) {
    RCLCPP_WARN(get_node()->get_logger(),
                "Timestamp in header is missing, using current time as command "
                "timestamp.");
    msg->header.stamp = get_node()->now();
  }
  const auto age_of_last_command = get_node()->now() - msg->header.stamp;

  if (ref_timeout_ == rclcpp::Duration::from_seconds(0) ||
      age_of_last_command <= ref_timeout_) {
    input_ref_.writeFromNonRT(msg);
  } else {
    RCLCPP_ERROR(get_node()->get_logger(),
                 "Received message has timestamp %.10f older for %.10f which "
                 "is more then allowed timeout "
                 "(%.4f).",
                 rclcpp::Time(msg->header.stamp).seconds(),
                 age_of_last_command.seconds(), ref_timeout_.seconds());
    reset_controller_reference_msg(msg, get_node());
  }
}

std::vector<hardware_interface::CommandInterface>
MecanumDriveController::on_export_reference_interfaces() {
  reference_interfaces_.resize(NR_REF_ITFS,
                               std::numeric_limits<double>::quiet_NaN());

  std::vector<hardware_interface::CommandInterface> reference_interfaces;

  reference_interfaces.reserve(reference_interfaces_.size());

  std::vector<std::string> reference_interface_names = {
      "linear/x/velocity", "linear/y/velocity", "angular/z/velocity"};

  for (size_t i = 0; i < reference_interfaces_.size(); ++i) {
    reference_interfaces.push_back(hardware_interface::CommandInterface(
        get_node()->get_name(), reference_interface_names[i],
        &reference_interfaces_[i]));
  }

  return reference_interfaces;
}

bool MecanumDriveController::on_set_chained_mode(bool chained_mode) {
  // Always accept switch to/from chained mode
  return true || chained_mode;
}

}  // namespace mecanum_drive_controller

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(mecanum_drive_controller::MecanumDriveController,
                       controller_interface::ChainableControllerInterface)