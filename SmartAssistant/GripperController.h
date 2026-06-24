#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gripper {

enum class CommandStatus {
    kAccepted  = 0,
    kRejected  = 1,
    kCompleted = 2,
    kAborted   = 3,
};

enum class Action {
    kGrab    = 0,
    kRelease = 1,
    kStop    = 2,
    kReset   = 3,
};

struct Command {
    Action        action = Action::kGrab;
    std::string   target;
    std::string   payload;
    std::uint64_t id     = 0;
};

struct State {
    Action        last_action       = Action::kRelease;
    std::string   last_target;
    std::string   last_payload;
    std::uint64_t last_command_id   = 0;
    bool          last_publish_ok   = false;
    std::int64_t  timestamp_us      = 0;
};

struct Ros2Config {
    bool        enabled            = true;
    std::string node_name          = "gripper_controller_client";
    std::string ns                 = "";
    std::string command_topic      = "/gripper_controller/command";
    std::string grab_payload       = "grab";
    std::string release_payload    = "release";
    std::string stop_payload       = "stop";
    std::string reset_payload      = "reset";
    int         repeat_count       = 1;
    int         repeat_interval_ms = 50;
    std::string qos_reliability    = "reliable";
    std::string qos_history        = "keep_last";
    int         qos_depth          = 10;
};

struct LoggingConfig {
    std::string log_dir            = "logs";
    std::string log_file           = "gripper_controller.log";
    bool        console            = true;
    std::size_t rotation_max_bytes = 10 * 1024 * 1024;
    int         rotation_max_files = 7;
};

struct GripperConfig {
    bool          enabled = true;
    Ros2Config    ros2;
    LoggingConfig logging;
};

using StateCallback         = std::function<void(const State&)>;
using CommandStatusCallback = std::function<void(const Command&, CommandStatus)>;

class GripperController {
public:
    GripperController();
    ~GripperController();

    GripperController(const GripperController&)            = delete;
    GripperController& operator=(const GripperController&) = delete;

    bool init(const std::string& config_path = "Config/config.toml");
    bool init(const GripperConfig& cfg);
    void shutdown();
    bool isInitialized() const;

    std::uint64_t sendCommand(Command cmd);
    std::uint64_t grab(const std::string& target = "");
    std::uint64_t release(const std::string& target = "");
    std::uint64_t stop();
    std::uint64_t reset();

    State state() const;

    void setStateCallback(StateCallback cb);
    void setCommandStatusCallback(CommandStatusCallback cb);

    const GripperConfig& config() const;

    static GripperConfig loadConfig(const std::string& config_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gripper
