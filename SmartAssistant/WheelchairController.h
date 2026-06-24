#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace wheelchair {

// ============================================================================
// Public enums
// ============================================================================

enum class CommandStatus {
    kAccepted   = 0,
    kRejected   = 1,    // controller not ready / queue rejected
    kCompleted  = 2,    // ran to natural completion
    kPreempted  = 3,    // superseded by a newer command
    kAborted    = 4,    // emergency stop or shutdown
};

enum class ControllerError {
    kOk              = 0,
    kNotInitialized  = 1,
    kRos2Unavailable = 2,
    kInvalidCommand  = 3,
    kInternalError   = 99,
};

// ============================================================================
// Command / State
// ============================================================================

// Command is a single motion request executed by the worker thread.  Both
// linear and angular axes share the same command so users can request, e.g.,
// an arc by setting both velocities.  All units are SI and signed.
//
//   linear_velocity  > 0  : forward,  < 0 backward    [m/s]
//   angular_velocity > 0  : turn-left (CCW), < 0 right [rad/s]
//   distance         != 0 : limit linear motion to |distance| metres
//                           (sign must match linear_velocity)
//   rotation         != 0 : limit angular motion to |rotation| radians
//                           (sign must match angular_velocity)
//   duration_ms      >  0 : wall-clock cap on the entire command
//
// `id` is assigned by the controller; callers receive it from sendCommand().
struct Command {
    double        linear_velocity   = 0.0;
    double        angular_velocity  = 0.0;
    double        distance          = 0.0;
    double        rotation          = 0.0;
    int           duration_ms       = 0;
    bool          is_emergency_stop = false;
    std::string   label;
    std::uint64_t id                = 0;
};

struct State {
    double        linear_velocity            = 0.0;
    double        angular_velocity           = 0.0;
    double        commanded_linear_velocity  = 0.0;
    double        commanded_angular_velocity = 0.0;
    double        traveled_distance          = 0.0;   // since current command started
    double        traveled_rotation          = 0.0;   // since current command started
    bool          is_executing               = false;
    bool          is_emergency               = false;
    std::uint64_t current_command_id         = 0;
    std::string   current_label;
    std::int64_t  timestamp_us               = 0;
};

// ============================================================================
// Configuration (parsed from TOML [wheelchair.*])
// ============================================================================

struct Limits {
    double max_linear_velocity      = 0.45;  // m/s
    double max_angular_velocity     = 0.7;   // rad/s
    double max_linear_accel         = 0.25;  // m/s^2
    double max_angular_accel        = 0.8;   // rad/s^2
    double max_linear_decel         = 0.45;  // m/s^2 — normal stop
    double max_angular_decel        = 1.0;   // rad/s^2
    double emergency_linear_decel   = 1.6;   // m/s^2 — emergency stop
    double emergency_angular_decel  = 4.0;   // rad/s^2
};

struct Defaults {
    double linear_velocity   = 0.22;         // default cruise for forward/back
    double angular_velocity  = 0.35;         // default cruise for turning
    double forward_distance  = 0.6;          // default move distance
    double turn_angle        = 0.7853982;    // default turn (~45°)
    double turn_entry_linear_velocity = 0.12; // forward creep during turn entry
    double turn_entry_distance        = 0.18; // short arc before pure turning
    double speed_step_ratio  = 0.15;         // ±15% per faster/slower call
};

struct SafetyConfig {
    int  command_timeout_ms        = 10000;  // hard ceiling on a single command
    int  heartbeat_timeout_ms      = 2000;   // velocity-mode auto-stop window
    bool zero_velocity_on_init     = true;
    bool zero_velocity_on_shutdown = true;
};

struct Ros2Config {
    bool        enabled         = true;
    std::string node_name       = "wheelchair_controller";
    std::string ns              = "";
    std::string cmd_vel_topic   = "/wheelchair_controller/cmd_vel";
    int         publish_rate_hz = 50;
    std::string qos_reliability = "reliable";   // reliable | best_effort
    std::string qos_history     = "keep_last";  // keep_last | keep_all
    int         qos_depth       = 10;
};

struct LoggingConfig {
    std::string log_dir            = "logs";
    std::string log_file           = "wheelchair.log";
    bool        console            = true;
    std::size_t rotation_max_bytes = 10 * 1024 * 1024;
    int         rotation_max_files = 7;
};

struct WheelchairConfig {
    bool          enabled = true;
    Limits        limits;
    Defaults      defaults;
    SafetyConfig  safety;
    Ros2Config    ros2;
    LoggingConfig logging;
};

using StateCallback         = std::function<void(const State&)>;
using CommandStatusCallback = std::function<void(const Command&, CommandStatus)>;

// ============================================================================
// WheelchairController
// ============================================================================

class WheelchairController {
public:
    WheelchairController();
    ~WheelchairController();

    WheelchairController(const WheelchairController&)            = delete;
    WheelchairController& operator=(const WheelchairController&) = delete;

    // Two-phase init.  Calling init() while already initialized is a no-op
    // that returns true.  Any in-flight command is aborted by shutdown().
    bool init(const std::string& config_path = "Config/config.toml");
    bool init(const WheelchairConfig& cfg);
    void shutdown();
    bool isInitialized() const;

    // Primitive: any motion expressible as linear/angular velocities with
    // optional distance / rotation / duration limits.  Returns the command id
    // (0 on rejection).  Callers may inspect status via setCommandStatusCallback.
    std::uint64_t sendCommand(Command cmd);

    // High-level helpers — all dispatch through sendCommand().
    // Passing 0 picks the default from [wheelchair.defaults].
    std::uint64_t moveForward (double distance_m   = 0.0, double velocity_mps = 0.0);
    std::uint64_t moveBackward(double distance_m   = 0.0, double velocity_mps = 0.0);
    std::uint64_t turnLeft    (double angle_rad    = 0.0, double angular_rps  = 0.0);
    std::uint64_t turnRight   (double angle_rad    = 0.0, double angular_rps  = 0.0);

    // Continuous velocity (no distance / rotation cap; auto-stop after
    // heartbeat_timeout_ms unless refreshed).
    std::uint64_t moveAtVelocity(double linear_mps, double angular_rps,
                                 int duration_ms = 0);

    // Adjust the active command's cruise velocities by (1 + ratio).  Distance
    // tracking is preserved.  Returns true iff there was an active command.
    bool adjustSpeed(double ratio);

    // Controlled deceleration to zero (max_*_decel).
    std::uint64_t stop();

    // Immediate maximum-effort deceleration (emergency_*_decel).  Bypasses any
    // queued command and reports it with kAborted.
    std::uint64_t emergencyStop();

    State state() const;

    void setStateCallback(StateCallback cb);
    void setCommandStatusCallback(CommandStatusCallback cb);

    const WheelchairConfig& config() const;

    static WheelchairConfig loadConfig(const std::string& config_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelchair
