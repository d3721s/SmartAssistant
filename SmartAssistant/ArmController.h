#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace arm {

enum class CommandStatus {
    kAccepted   = 0,
    kRejected   = 1,
    kCompleted  = 2,
    kPreempted  = 3,
    kAborted    = 4,
};

enum class ControllerError {
    kOk              = 0,
    kNotInitialized  = 1,
    kRos2Unavailable = 2,
    kInvalidCommand  = 3,
    kInternalError   = 99,
};

// End-effector Twist command.  Linear axes are metres/second and metres;
// angular axes are radians/second and radians.  Positive X/Y/Z directions
// follow the receiving ROS2 controller's end-effector frame convention.
struct Command {
    double        linear_x          = 0.0;
    double        linear_y          = 0.0;
    double        linear_z          = 0.0;
    double        angular_x         = 0.0;
    double        angular_y         = 0.0;
    double        angular_z         = 0.0;
    double        translation_x     = 0.0;
    double        translation_y     = 0.0;
    double        translation_z     = 0.0;
    double        rotation_x        = 0.0;
    double        rotation_y        = 0.0;
    double        rotation_z        = 0.0;
    int           duration_ms       = 0;
    bool          is_emergency_stop = false;
    std::string   label;
    std::uint64_t id                = 0;
};

struct State {
    double        linear_x              = 0.0;
    double        linear_y              = 0.0;
    double        linear_z              = 0.0;
    double        angular_x             = 0.0;
    double        angular_y             = 0.0;
    double        angular_z             = 0.0;
    double        commanded_linear_x    = 0.0;
    double        commanded_linear_y    = 0.0;
    double        commanded_linear_z    = 0.0;
    double        commanded_angular_x   = 0.0;
    double        commanded_angular_y   = 0.0;
    double        commanded_angular_z   = 0.0;
    double        traveled_x            = 0.0;
    double        traveled_y            = 0.0;
    double        traveled_z            = 0.0;
    double        rotated_x             = 0.0;
    double        rotated_y             = 0.0;
    double        rotated_z             = 0.0;
    bool          is_executing          = false;
    bool          is_emergency          = false;
    std::uint64_t current_command_id    = 0;
    std::string   current_label;
    std::int64_t  timestamp_us          = 0;
};

struct Limits {
    double max_linear_velocity      = 0.08;  // m/s
    double max_angular_velocity     = 0.6;   // rad/s
    double max_linear_accel         = 0.12;  // m/s^2
    double max_angular_accel        = 1.0;   // rad/s^2
    double max_linear_decel         = 0.18;  // m/s^2
    double max_angular_decel        = 1.4;   // rad/s^2
    double emergency_linear_decel   = 0.35;  // m/s^2
    double emergency_angular_decel  = 2.8;   // rad/s^2
};

struct Defaults {
    double linear_velocity   = 0.04;       // end-effector jog speed
    double angular_velocity  = 0.3;        // end-effector angular jog speed
    double step_distance     = 0.05;       // default jog distance
    double rotation_angle    = 0.2617994;  // default jog angle, 15 deg
    double speed_step_ratio  = 0.25;
};

struct SafetyConfig {
    int  command_timeout_ms        = 5000;
    int  heartbeat_timeout_ms      = 1000;
    bool zero_velocity_on_init     = true;
    bool zero_velocity_on_shutdown = true;
};

struct Ros2Config {
    bool        enabled         = true;
    std::string node_name       = "arm_end_effector_controller";
    std::string ns              = "";
    std::string cmd_vel_topic   = "/arm_end_effector_controller/cmd_vel";
    int         publish_rate_hz = 50;
    std::string qos_reliability = "reliable";
    std::string qos_history     = "keep_last";
    int         qos_depth       = 10;
};

struct LoggingConfig {
    std::string log_dir            = "logs";
    std::string log_file           = "arm_controller.log";
    bool        console            = true;
    std::size_t rotation_max_bytes = 10 * 1024 * 1024;
    int         rotation_max_files = 7;
};

struct ArmConfig {
    bool          enabled = true;
    Limits        limits;
    Defaults      defaults;
    SafetyConfig  safety;
    Ros2Config    ros2;
    LoggingConfig logging;
};

using StateCallback         = std::function<void(const State&)>;
using CommandStatusCallback = std::function<void(const Command&, CommandStatus)>;

class ArmController {
public:
    ArmController();
    ~ArmController();

    ArmController(const ArmController&)            = delete;
    ArmController& operator=(const ArmController&) = delete;

    bool init(const std::string& config_path = "Config/config.toml");
    bool init(const ArmConfig& cfg);
    void shutdown();
    bool isInitialized() const;

    std::uint64_t sendCommand(Command cmd);

    std::uint64_t raise  (double distance_m = 0.0, double velocity_mps = 0.0);
    std::uint64_t lower  (double distance_m = 0.0, double velocity_mps = 0.0);
    std::uint64_t extend (double distance_m = 0.0, double velocity_mps = 0.0);
    std::uint64_t retract(double distance_m = 0.0, double velocity_mps = 0.0);

    std::uint64_t moveAtVelocity(double linear_x, double linear_y, double linear_z,
                                 double angular_x = 0.0, double angular_y = 0.0,
                                 double angular_z = 0.0, int duration_ms = 0);
    bool adjustSpeed(double ratio);
    std::uint64_t stop();
    std::uint64_t emergencyStop();

    State state() const;

    void setStateCallback(StateCallback cb);
    void setCommandStatusCallback(CommandStatusCallback cb);

    const ArmConfig& config() const;

    static ArmConfig loadConfig(const std::string& config_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arm
