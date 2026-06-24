#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace navigation {

// ============================================================================
// Public enums
// ============================================================================

enum class CommandStatus {
    kAccepted  = 0,
    kRejected  = 1,
    kCompleted = 2,
    kAborted   = 3,
};

// ============================================================================
// Public types
// ============================================================================

struct Command {
    std::string   destination;     // user-supplied target name (e.g. "桌子")
    std::uint64_t id          = 0;
};

struct State {
    std::string   last_destination;
    std::uint64_t last_command_id = 0;
    bool          last_publish_ok = false;
    std::int64_t  timestamp_us    = 0;
};

// ============================================================================
// Configuration (parsed from TOML [navigation])
// ============================================================================

struct LoggingConfig {
    std::string log_dir            = "logs";
    std::string log_file           = "navigation.log";
    bool        console            = true;
    std::size_t rotation_max_bytes = 10 * 1024 * 1024;
    int         rotation_max_files = 7;
};

// ROS2 publishing config (parsed from [navigation.ros2]).
//
// The controller speaks the nav-2026 (Finav) "voice navigation" contract:
//   - navigateTo(name) -> std_msgs/String on `voice_command_topic`
//       Consumed by nav_bridge.py, which resolves the location name from the
//       active map's <map>.locations.yaml and republishes a /goal_pose.
//       Requires nav.launch.py to run with `use_nav_bridge:=true`.
//   - stop()           -> std_msgs/Empty  on `nav_clear_topic`
//       Consumed by nav_control.py, which halts and resets navigation.
struct Ros2Config {
    bool        enabled             = true;
    std::string node_name           = "navigation_voice_bridge";
    std::string ns                  = "";
    std::string voice_command_topic = "/nav_bridge/voice_command";
    std::string nav_clear_topic     = "/nav_clear";
    std::string qos_reliability     = "reliable";   // reliable | best_effort
    std::string qos_history         = "keep_last";  // keep_last | keep_all
    int         qos_depth           = 10;
};

struct NavigationConfig {
    bool          enabled            = true;
    // Simulated handling delay in milliseconds; only used when ROS2 is
    // unavailable (stub fallback).  0 = answer immediately.
    int           simulated_delay_ms = 0;
    Ros2Config    ros2;
    LoggingConfig logging;
};

using StateCallback         = std::function<void(const State&)>;
using CommandStatusCallback = std::function<void(const Command&, CommandStatus)>;

// ============================================================================
// NavigationController
//
// Bridges voice "take me to <place>" intents onto the nav-2026 (Finav) ROS2
// navigation stack.  navigateTo() publishes the destination name as a
// std_msgs/String on the nav_bridge voice-command topic; nav_bridge.py turns
// it into a /goal_pose using the active map's saved locations.  stop()
// publishes a std_msgs/Empty on the nav-clear topic to halt navigation.
//
// When the binary is built without ROS2 (or [navigation.ros2].enabled=false,
// or the node fails to come up) the controller degrades to a log-only stub:
// it still accepts commands and reports fake success so the upper layers
// (IntentManager -> Orchestrator -> navigateTo) behave identically.
// ============================================================================
class NavigationController {
public:
    NavigationController();
    ~NavigationController();

    NavigationController(const NavigationController&)            = delete;
    NavigationController& operator=(const NavigationController&) = delete;

    bool init(const std::string& config_path = "Config/config.toml");
    bool init(const NavigationConfig& cfg);
    void shutdown();
    bool isInitialized() const;

    // Returns a non-zero monotonically increasing fake command id on success
    // and 0 only when the controller has not been initialized or `enabled`
    // is false.  No actual motion is produced.
    std::uint64_t navigateTo(const std::string& destination);
    // Marks the most recently issued command as aborted (logs only).
    void          stop();

    State state() const;

    void setStateCallback(StateCallback cb);
    void setCommandStatusCallback(CommandStatusCallback cb);

    const NavigationConfig& config() const;

    static NavigationConfig loadConfig(const std::string& config_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace navigation
