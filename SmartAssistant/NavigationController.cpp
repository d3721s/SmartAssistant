#include "NavigationController.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

#if defined(NAVIGATION_HAS_ROS2)
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/empty.hpp>
#include <std_msgs/msg/string.hpp>
#endif

namespace navigation {

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

#if defined(NAVIGATION_HAS_ROS2)
// rclcpp::init() may only be called once per process; share across instances
// and across the other ROS2 controllers (each translation unit keeps its own
// refcount but all gate on the shared rclcpp::ok() state).  We intentionally
// never call rclcpp::shutdown() — other components in the process may still
// hold nodes when this controller tears down.
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
#endif  // NAVIGATION_HAS_ROS2

}  // namespace

struct NavigationController::Impl {
    NavigationConfig cfg;
    quill::Logger*   logger = nullptr;

    std::atomic<bool>          initialized{false};
    std::atomic<std::uint64_t> id_seq{1};

    mutable std::mutex state_mtx;
    State              state{};

    std::mutex            cb_mtx;
    StateCallback         state_cb;
    CommandStatusCallback status_cb;

#if defined(NAVIGATION_HAS_ROS2)
    bool                                                         ros_ok = false;
    std::shared_ptr<rclcpp::Node>                                ros_node;
    std::shared_ptr<rclcpp::Publisher<std_msgs::msg::String>>    goal_pub;
    std::shared_ptr<rclcpp::Publisher<std_msgs::msg::Empty>>     clear_pub;
#endif

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
            // Fall back to console-only.
        }
        if (cfg.logging.console || sinks.empty()) {
            sinks.push_back(quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "navigation_console"));
        }
        logger = quill::Frontend::create_or_get_logger("navigation", std::move(sinks));
    }

#if defined(NAVIGATION_HAS_ROS2)
    bool initRos()
    {
        if (!cfg.ros2.enabled) {
            if (logger) LOG_WARNING(logger, "ROS2 publishing disabled by config");
            return false;
        }
        rclcppAcquire();
        try {
            const std::string node_name = cfg.ros2.node_name.empty()
                ? std::string("navigation_voice_bridge")
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

            const std::string goal_topic = cfg.ros2.voice_command_topic.empty()
                ? std::string("/nav_bridge/voice_command")
                : cfg.ros2.voice_command_topic;
            const std::string clear_topic = cfg.ros2.nav_clear_topic.empty()
                ? std::string("/nav_clear")
                : cfg.ros2.nav_clear_topic;

            goal_pub  = ros_node->create_publisher<std_msgs::msg::String>(goal_topic, qos);
            clear_pub = ros_node->create_publisher<std_msgs::msg::Empty>(clear_topic, qos);

            if (logger) {
                LOG_INFO(logger,
                         "ROS2 node '{}' ready — voice_command='{}' nav_clear='{}' (qos={}, depth={})",
                         node_name, goal_topic, clear_topic,
                         cfg.ros2.qos_reliability, cfg.ros2.qos_depth);
            }
            return true;
        } catch (const std::exception& e) {
            if (logger) LOG_ERROR(logger, "ROS2 node creation failed: {}", e.what());
            goal_pub.reset();
            clear_pub.reset();
            ros_node.reset();
            rclcppRelease();
            return false;
        }
    }

    void teardownRos()
    {
        if (!ros_ok) return;
        goal_pub.reset();
        clear_pub.reset();
        ros_node.reset();
        rclcppRelease();
        ros_ok = false;
    }

    bool publishGoal(const std::string& destination)
    {
        if (!goal_pub) return false;
        std_msgs::msg::String msg;
        msg.data = destination;
        goal_pub->publish(msg);
        return true;
    }

    bool publishClear()
    {
        if (!clear_pub) return false;
        clear_pub->publish(std_msgs::msg::Empty());
        return true;
    }
#endif  // NAVIGATION_HAS_ROS2

    bool rosActive() const
    {
#if defined(NAVIGATION_HAS_ROS2)
        return ros_ok;
#else
        return false;
#endif
    }

    void notifyState(const State& snap)
    {
        StateCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx);
            cb = state_cb;
        }
        if (cb) cb(snap);
    }

    void notifyStatus(const Command& cmd, CommandStatus status)
    {
        CommandStatusCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx);
            cb = status_cb;
        }
        if (cb) cb(cmd, status);
    }
};

NavigationController::NavigationController()
    : impl_(std::make_unique<Impl>())
{}

NavigationController::~NavigationController()
{
    shutdown();
}

bool NavigationController::init(const std::string& config_path)
{
    return init(loadConfig(config_path));
}

bool NavigationController::init(const NavigationConfig& cfg)
{
    if (impl_->initialized.load(std::memory_order_acquire)) {
        return true;
    }
    impl_->cfg = cfg;
    impl_->setupLogger();

    if (!impl_->cfg.enabled) {
        if (impl_->logger) {
            LOG_WARNING(impl_->logger, "NavigationController disabled via config");
        }
        impl_->initialized.store(true, std::memory_order_release);
        return true;
    }

#if defined(NAVIGATION_HAS_ROS2)
    impl_->ros_ok = impl_->initRos();
    if (!impl_->ros_ok && impl_->logger) {
        LOG_WARNING(impl_->logger,
                    "ROS2 unavailable — navigateTo()/stop() will be tracked but not published");
    }
#else
    if (impl_->logger) {
        LOG_WARNING(impl_->logger,
                    "Built without ROS2 — NavigationController runs as a log-only stub");
    }
#endif

    if (impl_->logger) {
        LOG_INFO(impl_->logger,
                 "NavigationController initialized (mode={}) — log_dir='{}' log_file='{}'",
                 impl_->rosActive() ? "ros2" : "stub",
                 impl_->cfg.logging.log_dir,
                 impl_->cfg.logging.log_file);
    }
    impl_->initialized.store(true, std::memory_order_release);
    return true;
}

void NavigationController::shutdown()
{
    if (!impl_) return;
    if (!impl_->initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
#if defined(NAVIGATION_HAS_ROS2)
    impl_->teardownRos();
#endif
    if (impl_->logger) {
        LOG_INFO(impl_->logger, "NavigationController shutdown");
    }
}

bool NavigationController::isInitialized() const
{
    return impl_ && impl_->initialized.load(std::memory_order_acquire);
}

std::uint64_t NavigationController::navigateTo(const std::string& destination)
{
    if (!isInitialized()) {
        return 0;
    }
    if (!impl_->cfg.enabled) {
        if (impl_->logger) {
            LOG_WARNING(impl_->logger,
                        "navigateTo('{}') skipped — navigation disabled",
                        destination);
        }
        return 0;
    }

    Command cmd;
    cmd.destination = destination;
    cmd.id          = impl_->id_seq.fetch_add(1, std::memory_order_acq_rel);

    impl_->notifyStatus(cmd, CommandStatus::kAccepted);

    bool published = false;
#if defined(NAVIGATION_HAS_ROS2)
    if (impl_->ros_ok) {
        published = impl_->publishGoal(cmd.destination);
        if (impl_->logger) {
            if (published) {
                LOG_INFO(impl_->logger,
                         "navigateTo destination='{}' cmd_id={} — published to nav_bridge",
                         cmd.destination, cmd.id);
            } else {
                LOG_ERROR(impl_->logger,
                          "navigateTo destination='{}' cmd_id={} — publish failed",
                          cmd.destination, cmd.id);
            }
        }
    }
#endif

    if (!published) {
        // Stub path — pretend success so upper layers behave identically.
        if (impl_->logger) {
            LOG_INFO(impl_->logger,
                     "[STUB] navigateTo destination='{}' cmd_id={} — no ROS2 backend, faking success",
                     cmd.destination, cmd.id);
        }
        if (impl_->cfg.simulated_delay_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(impl_->cfg.simulated_delay_ms));
        }
    }

    State snap;
    {
        std::lock_guard<std::mutex> lk(impl_->state_mtx);
        impl_->state.last_destination = cmd.destination;
        impl_->state.last_command_id  = cmd.id;
        impl_->state.last_publish_ok  = published || !impl_->rosActive();
        impl_->state.timestamp_us     = nowMicros();
        snap = impl_->state;
    }
    impl_->notifyState(snap);

    impl_->notifyStatus(cmd, CommandStatus::kCompleted);
    return cmd.id;
}

void NavigationController::stop()
{
    if (!isInitialized()) return;
    Command last_cmd{};
    {
        std::lock_guard<std::mutex> lk(impl_->state_mtx);
        last_cmd.destination = impl_->state.last_destination;
        last_cmd.id          = impl_->state.last_command_id;
    }

    bool published = false;
#if defined(NAVIGATION_HAS_ROS2)
    if (impl_->ros_ok) {
        published = impl_->publishClear();
    }
#endif

    if (impl_->logger) {
        if (published) {
            LOG_INFO(impl_->logger,
                     "stop() — published nav_clear (aborting cmd_id={} destination='{}')",
                     last_cmd.id, last_cmd.destination);
        } else {
            LOG_INFO(impl_->logger,
                     "[STUB] stop() — aborting cmd_id={} destination='{}'",
                     last_cmd.id, last_cmd.destination);
        }
    }
    impl_->notifyStatus(last_cmd, CommandStatus::kAborted);
}

State NavigationController::state() const
{
    if (!impl_) return State{};
    std::lock_guard<std::mutex> lk(impl_->state_mtx);
    return impl_->state;
}

void NavigationController::setStateCallback(StateCallback cb)
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->state_cb = std::move(cb);
}

void NavigationController::setCommandStatusCallback(CommandStatusCallback cb)
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->status_cb = std::move(cb);
}

const NavigationConfig& NavigationController::config() const
{
    return impl_->cfg;
}

NavigationConfig NavigationController::loadConfig(const std::string& config_path)
{
    NavigationConfig cfg;
    if (!fileExists(config_path)) {
        return cfg;
    }

    toml::table tbl;
    try {
        tbl = toml::parse_file(config_path);
    } catch (const std::exception&) {
        return cfg;
    }

    const toml::node_view nav = tbl["navigation"];
    if (!nav) {
        return cfg;
    }

    cfg.enabled            = nav["enabled"].value_or(cfg.enabled);
    cfg.simulated_delay_ms = nav["simulated_delay_ms"].value_or(cfg.simulated_delay_ms);

    const toml::node_view ros = nav["ros2"];
    cfg.ros2.enabled             = ros["enabled"].value_or(cfg.ros2.enabled);
    cfg.ros2.node_name           = ros["node_name"].value_or(cfg.ros2.node_name);
    cfg.ros2.ns                  = ros["namespace"].value_or(cfg.ros2.ns);
    cfg.ros2.voice_command_topic = ros["voice_command_topic"].value_or(cfg.ros2.voice_command_topic);
    cfg.ros2.nav_clear_topic     = ros["nav_clear_topic"].value_or(cfg.ros2.nav_clear_topic);
    cfg.ros2.qos_reliability     = ros["qos_reliability"].value_or(cfg.ros2.qos_reliability);
    cfg.ros2.qos_history         = ros["qos_history"].value_or(cfg.ros2.qos_history);
    cfg.ros2.qos_depth           = ros["qos_depth"].value_or(cfg.ros2.qos_depth);

    const toml::node_view log = nav["logging"];
    cfg.logging.log_dir            = log["log_dir"].value_or(cfg.logging.log_dir);
    cfg.logging.log_file           = log["log_file"].value_or(cfg.logging.log_file);
    cfg.logging.console            = log["console"].value_or(cfg.logging.console);
    cfg.logging.rotation_max_bytes = static_cast<std::size_t>(
        log["rotation_max_bytes"].value_or(
            static_cast<int64_t>(cfg.logging.rotation_max_bytes)));
    cfg.logging.rotation_max_files = log["rotation_max_files"].value_or(
        cfg.logging.rotation_max_files);

    return cfg;
}

}  // namespace navigation
