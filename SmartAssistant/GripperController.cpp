#include "GripperController.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

namespace gripper {

namespace {

inline std::int64_t nowMicros()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

inline bool fileExists(const std::string& path)
{
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

const char* statusLabel(CommandStatus s)
{
    switch (s) {
        case CommandStatus::kAccepted:  return "accepted";
        case CommandStatus::kRejected:  return "rejected";
        case CommandStatus::kCompleted: return "completed";
        case CommandStatus::kAborted:   return "aborted";
    }
    return "?";
}

const char* actionLabel(Action a)
{
    switch (a) {
        case Action::kGrab:    return "grab";
        case Action::kRelease: return "release";
        case Action::kStop:    return "stop";
        case Action::kReset:   return "reset";
    }
    return "?";
}

std::mutex       g_rclcpp_mtx;
std::atomic<int> g_rclcpp_users{0};

void rclcppAcquire()
{
    std::lock_guard<std::mutex> lk(g_rclcpp_mtx);
    if (g_rclcpp_users.fetch_add(1, std::memory_order_acq_rel) == 0) {
        if (!rclcpp::ok()) {
            rclcpp::init(0, nullptr);
        }
    }
}

void rclcppRelease()
{
    std::lock_guard<std::mutex> lk(g_rclcpp_mtx);
    g_rclcpp_users.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace

struct GripperController::Impl {
    GripperConfig cfg;
    quill::Logger* logger = nullptr;

    std::atomic<bool>          initialized{false};
    std::atomic<std::uint64_t> id_seq{1};

    mutable std::mutex state_mtx;
    State              state{};

    std::mutex            cb_mtx;
    StateCallback         state_cb;
    CommandStatusCallback status_cb;

    bool                                                         ros_ok = false;
    std::shared_ptr<rclcpp::Node>                                ros_node;
    std::shared_ptr<rclcpp::Publisher<std_msgs::msg::String>>    command_pub;

    void setupLogger()
    {
        quill::Backend::start();
        std::vector<std::shared_ptr<quill::Sink>> sinks;
        try {
            std::error_code ec;
            std::filesystem::create_directories(cfg.logging.log_dir, ec);
            const std::string full_path =
                cfg.logging.log_dir + "/" + cfg.logging.log_file;
            quill::RotatingFileSinkConfig rotation;
            rotation.set_rotation_max_file_size(cfg.logging.rotation_max_bytes);
            rotation.set_max_backup_files(static_cast<std::uint32_t>(
                std::max(1, cfg.logging.rotation_max_files)));
            rotation.set_open_mode('a');
            auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
                full_path, rotation);
            sinks.push_back(file_sink);
        } catch (...) {
        }
        if (cfg.logging.console) {
            auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "gripper_controller_console_sink");
            sinks.push_back(console_sink);
        }
        if (sinks.empty()) {
            auto fallback = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "gripper_controller_fallback_sink");
            sinks.push_back(fallback);
        }
        logger = quill::Frontend::create_or_get_logger(
            "GripperController", std::move(sinks));
        logger->set_log_level(quill::LogLevel::Info);
    }

    bool initRos()
    {
        if (!cfg.enabled || !cfg.ros2.enabled) {
            LOG_WARNING(logger, "Gripper ROS2 publishing disabled by config");
            return false;
        }
        rclcppAcquire();
        try {
            const std::string node_name = cfg.ros2.node_name.empty()
                ? std::string("gripper_controller_client")
                : cfg.ros2.node_name;
            ros_node = std::make_shared<rclcpp::Node>(node_name, cfg.ros2.ns);

            rclcpp::QoS qos(cfg.ros2.qos_depth > 0 ? cfg.ros2.qos_depth : 10);
            if (cfg.ros2.qos_reliability == "best_effort") {
                qos.best_effort();
            } else {
                qos.reliable();
            }
            if (cfg.ros2.qos_history == "keep_all") {
                qos.keep_all();
            } else {
                qos.keep_last(cfg.ros2.qos_depth > 0 ? cfg.ros2.qos_depth : 10);
            }

            const std::string topic = cfg.ros2.command_topic.empty()
                ? std::string("/gripper_controller/command")
                : cfg.ros2.command_topic;
            command_pub = ros_node->create_publisher<std_msgs::msg::String>(topic, qos);

            LOG_INFO(logger,
                     "ROS2 node '{}' publishing gripper commands on '{}' (qos={}, depth={})",
                     node_name, topic, cfg.ros2.qos_reliability, cfg.ros2.qos_depth);
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR(logger, "Gripper ROS2 node creation failed: {}", e.what());
            command_pub.reset();
            ros_node.reset();
            rclcppRelease();
            return false;
        }
    }

    void teardownRos()
    {
        if (!ros_ok) return;
        command_pub.reset();
        ros_node.reset();
        rclcppRelease();
        ros_ok = false;
    }

    bool initImpl(GripperConfig in_cfg)
    {
        if (initialized.load(std::memory_order_acquire)) {
            return true;
        }
        cfg = std::move(in_cfg);
        setupLogger();

        ros_ok = initRos();
        if (!ros_ok) {
            LOG_WARNING(logger, "Gripper ROS2 unavailable - commands will be rejected");
        }

        initialized.store(true, std::memory_order_release);
        LOG_INFO(logger, "GripperController initialized");
        return true;
    }

    void shutdownImpl()
    {
        if (!initialized.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        teardownRos();
        if (logger) {
            LOG_INFO(logger, "GripperController shutdown complete");
        }
    }

    std::uint64_t nextId()
    {
        return id_seq.fetch_add(1, std::memory_order_acq_rel);
    }

    std::string payloadFor(Action action) const
    {
        switch (action) {
            case Action::kGrab:
                return cfg.ros2.grab_payload.empty() ? "grab" : cfg.ros2.grab_payload;
            case Action::kRelease:
                return cfg.ros2.release_payload.empty() ? "release" : cfg.ros2.release_payload;
            case Action::kStop:
                return cfg.ros2.stop_payload.empty() ? "stop" : cfg.ros2.stop_payload;
            case Action::kReset:
                return cfg.ros2.reset_payload.empty() ? "reset" : cfg.ros2.reset_payload;
        }
        return "";
    }

    void fireStatus(const Command& c, CommandStatus s)
    {
        CommandStatusCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx);
            cb = status_cb;
        }
        if (logger) {
            LOG_INFO(logger,
                     "cmd #{} ({}) target='{}' payload='{}' -> {}",
                     c.id, actionLabel(c.action), c.target, c.payload,
                     statusLabel(s));
        }
        if (cb) {
            try { cb(c, s); } catch (...) {}
        }
    }

    void fireState()
    {
        StateCallback cb;
        State snapshot;
        {
            std::lock_guard<std::mutex> lk(cb_mtx);
            cb = state_cb;
        }
        if (!cb) return;
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            snapshot = state;
        }
        try { cb(snapshot); } catch (...) {}
    }

    std::uint64_t send(Command cmd)
    {
        if (!initialized.load(std::memory_order_acquire)) {
            return 0;
        }
        if (cmd.id == 0) {
            cmd.id = nextId();
        }
        if (cmd.payload.empty()) {
            cmd.payload = payloadFor(cmd.action);
        }
        if (!command_pub || cmd.payload.empty()) {
            fireStatus(cmd, CommandStatus::kRejected);
            return 0;
        }

        fireStatus(cmd, CommandStatus::kAccepted);

        const int repeat_count = std::max(1, cfg.ros2.repeat_count);
        const int interval_ms = std::max(0, cfg.ros2.repeat_interval_ms);
        for (int i = 0; i < repeat_count; ++i) {
            std_msgs::msg::String msg;
            msg.data = cmd.payload;
            command_pub->publish(msg);
            if (i + 1 < repeat_count && interval_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
        }

        {
            std::lock_guard<std::mutex> lk(state_mtx);
            state.last_action = cmd.action;
            state.last_target = cmd.target;
            state.last_payload = cmd.payload;
            state.last_command_id = cmd.id;
            state.last_publish_ok = true;
            state.timestamp_us = nowMicros();
        }
        fireState();
        fireStatus(cmd, CommandStatus::kCompleted);
        return cmd.id;
    }
};

GripperController::GripperController() : impl_(std::make_unique<Impl>()) {}
GripperController::~GripperController() { if (impl_) impl_->shutdownImpl(); }

bool GripperController::init(const std::string& config_path)
{
    return impl_->initImpl(loadConfig(config_path));
}

bool GripperController::init(const GripperConfig& cfg)
{
    return impl_->initImpl(cfg);
}

void GripperController::shutdown() { impl_->shutdownImpl(); }

bool GripperController::isInitialized() const
{
    return impl_->initialized.load(std::memory_order_acquire);
}

std::uint64_t GripperController::sendCommand(Command cmd)
{
    return impl_->send(std::move(cmd));
}

std::uint64_t GripperController::grab(const std::string& target)
{
    Command c;
    c.action = Action::kGrab;
    c.target = target;
    return impl_->send(std::move(c));
}

std::uint64_t GripperController::release(const std::string& target)
{
    Command c;
    c.action = Action::kRelease;
    c.target = target;
    return impl_->send(std::move(c));
}

std::uint64_t GripperController::stop()
{
    Command c;
    c.action = Action::kStop;
    return impl_->send(std::move(c));
}

std::uint64_t GripperController::reset()
{
    Command c;
    c.action = Action::kReset;
    return impl_->send(std::move(c));
}

State GripperController::state() const
{
    std::lock_guard<std::mutex> lk(impl_->state_mtx);
    return impl_->state;
}

void GripperController::setStateCallback(StateCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->state_cb = std::move(cb);
}

void GripperController::setCommandStatusCallback(CommandStatusCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->status_cb = std::move(cb);
}

const GripperConfig& GripperController::config() const { return impl_->cfg; }

GripperConfig GripperController::loadConfig(const std::string& config_path)
{
    GripperConfig cfg;
    if (!fileExists(config_path)) {
        return cfg;
    }
    toml::table table = toml::parse_file(config_path);
    const toml::node_view in = table["gripper"];

    cfg.enabled = in["enabled"].value_or(cfg.enabled);

    const toml::node_view R = in["ros2"];
    cfg.ros2.enabled            = R["enabled"].value_or(cfg.ros2.enabled);
    cfg.ros2.node_name          = R["node_name"].value_or(cfg.ros2.node_name);
    cfg.ros2.ns                 = R["namespace"].value_or(cfg.ros2.ns);
    cfg.ros2.command_topic      = R["command_topic"].value_or(cfg.ros2.command_topic);
    cfg.ros2.grab_payload       = R["grab_payload"].value_or(cfg.ros2.grab_payload);
    cfg.ros2.release_payload    = R["release_payload"].value_or(cfg.ros2.release_payload);
    cfg.ros2.stop_payload       = R["stop_payload"].value_or(cfg.ros2.stop_payload);
    cfg.ros2.reset_payload      = R["reset_payload"].value_or(cfg.ros2.reset_payload);
    cfg.ros2.repeat_count       = R["repeat_count"].value_or(cfg.ros2.repeat_count);
    cfg.ros2.repeat_interval_ms = R["repeat_interval_ms"].value_or(cfg.ros2.repeat_interval_ms);
    cfg.ros2.qos_reliability    = R["qos_reliability"].value_or(cfg.ros2.qos_reliability);
    cfg.ros2.qos_history        = R["qos_history"].value_or(cfg.ros2.qos_history);
    cfg.ros2.qos_depth          = R["qos_depth"].value_or(cfg.ros2.qos_depth);

    const toml::node_view G = in["logging"];
    cfg.logging.log_dir            = G["log_dir"].value_or(cfg.logging.log_dir);
    cfg.logging.log_file           = G["log_file"].value_or(cfg.logging.log_file);
    cfg.logging.console            = G["console"].value_or(cfg.logging.console);
    cfg.logging.rotation_max_bytes = static_cast<std::size_t>(
        G["rotation_max_bytes"].value_or(static_cast<std::int64_t>(cfg.logging.rotation_max_bytes)));
    cfg.logging.rotation_max_files = G["rotation_max_files"].value_or(cfg.logging.rotation_max_files);

    return cfg;
}

}  // namespace gripper
