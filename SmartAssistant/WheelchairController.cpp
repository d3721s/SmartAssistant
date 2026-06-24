#include "WheelchairController.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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
#include <geometry_msgs/msg/twist.hpp>

namespace wheelchair {

// ============================================================================
// Helpers
// ============================================================================
namespace {

constexpr double kEps = 1e-4;

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
        case CommandStatus::kPreempted: return "preempted";
        case CommandStatus::kAborted:   return "aborted";
    }
    return "?";
}

inline double clampAbs(double v, double max_abs)
{
    if (max_abs <= 0.0) return v;
    if (v >  max_abs) return  max_abs;
    if (v < -max_abs) return -max_abs;
    return v;
}

// Move `cur` toward `target` by at most |max_step|; returns the new value.
inline double approach(double cur, double target, double max_step)
{
    if (max_step <= 0.0) return target;
    const double delta = target - cur;
    if (std::abs(delta) <= max_step) return target;
    return cur + (delta > 0 ? max_step : -max_step);
}

// rclcpp::init() may only be called once per process; share across instances.
std::mutex      g_rclcpp_mtx;
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
    // We intentionally leave rclcpp running until the process exits — calling
    // rclcpp::shutdown() while other components in the process still hold
    // nodes would break them.  The user count is kept for symmetry.
    g_rclcpp_users.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace

// ============================================================================
// PIMPL
// ============================================================================
struct WheelchairController::Impl {
    WheelchairConfig cfg;
    quill::Logger*   logger = nullptr;

    std::atomic<bool>          initialized{false};
    std::atomic<std::uint64_t> id_seq{1};

    // ---- pending command slot (replace-on-arrive) -------------------------
    std::mutex                 queue_mtx;
    std::condition_variable    queue_cv;
    std::optional<Command>     pending;
    std::optional<double>      pending_speed_ratio;
    std::atomic<bool>          worker_stop{false};
    std::thread                worker;

    // ---- worker-owned execution state -------------------------------------
    Command current_cmd{};
    bool    has_current      = false;
    double  cur_linear_v     = 0.0;
    double  cur_angular_v    = 0.0;
    double  traveled_dist    = 0.0;
    double  traveled_rot     = 0.0;
    bool    emergency_mode   = false;
    std::chrono::steady_clock::time_point cmd_started_at;
    std::chrono::steady_clock::time_point last_heartbeat_at;

    // ---- published state --------------------------------------------------
    mutable std::mutex state_mtx;
    State              state{};

    // ---- callbacks --------------------------------------------------------
    std::mutex            cb_mtx;
    StateCallback         state_cb;
    CommandStatusCallback status_cb;

    // ---- ROS2 -------------------------------------------------------------
    bool                                                          ros_ok = false;
    std::shared_ptr<rclcpp::Node>                                 ros_node;
    std::shared_ptr<rclcpp::Publisher<geometry_msgs::msg::Twist>> cmd_pub;

    // ----------------------------------------------------------------------
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
            // fall back to console only
        }
        if (cfg.logging.console) {
            auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "wheelchair_console_sink");
            sinks.push_back(console_sink);
        }
        if (sinks.empty()) {
            auto fallback = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "wheelchair_fallback_sink");
            sinks.push_back(fallback);
        }
        logger = quill::Frontend::create_or_get_logger(
            "WheelchairController", std::move(sinks));
        logger->set_log_level(quill::LogLevel::Info);
    }

    bool initRos()
    {
        if (!cfg.ros2.enabled) {
            LOG_WARNING(logger, "ROS2 publishing disabled by config");
            return false;
        }
        rclcppAcquire();
        try {
            const std::string node_name = cfg.ros2.node_name.empty()
                ? std::string("wheelchair_controller")
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

            const std::string topic = cfg.ros2.cmd_vel_topic.empty()
                ? std::string("/wheelchair_controller/cmd_vel")
                : cfg.ros2.cmd_vel_topic;
            cmd_pub = ros_node->create_publisher<geometry_msgs::msg::Twist>(topic, qos);

            LOG_INFO(logger,
                     "ROS2 node '{}' publishing on '{}' (qos={}, depth={}, hz={})",
                     node_name, topic, cfg.ros2.qos_reliability,
                     cfg.ros2.qos_depth, cfg.ros2.publish_rate_hz);
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR(logger, "ROS2 node creation failed: {}", e.what());
            cmd_pub.reset();
            ros_node.reset();
            rclcppRelease();
            return false;
        }
    }

    void teardownRos()
    {
        if (!ros_ok) return;
        cmd_pub.reset();
        ros_node.reset();
        rclcppRelease();
        ros_ok = false;
    }

    bool initImpl(WheelchairConfig in_cfg)
    {
        if (initialized.load(std::memory_order_acquire)) {
            return true;
        }
        cfg = std::move(in_cfg);

        setupLogger();
        LOG_INFO(logger,
                 "WheelchairController init: enabled={}, max_lin={} m/s, max_ang={} rad/s, hz={}",
                 cfg.enabled, cfg.limits.max_linear_velocity,
                 cfg.limits.max_angular_velocity, cfg.ros2.publish_rate_hz);

        ros_ok = initRos();
        if (!ros_ok) {
            LOG_WARNING(logger, "ROS2 unavailable — commands will be tracked but not published");
        }

        worker_stop.store(false, std::memory_order_release);
        worker = std::thread(&Impl::workerLoop, this);

        if (cfg.safety.zero_velocity_on_init) {
            publishTwist(0.0, 0.0);
        }

        initialized.store(true, std::memory_order_release);
        LOG_INFO(logger, "WheelchairController initialized");
        return true;
    }

    void shutdownImpl()
    {
        if (!initialized.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        worker_stop.store(true, std::memory_order_release);
        queue_cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        if (cfg.safety.zero_velocity_on_shutdown) {
            publishTwist(0.0, 0.0);
        }
        teardownRos();
        if (logger) {
            LOG_INFO(logger, "WheelchairController shutdown complete");
        }
    }

    // ------------------------------------------------------------------ id
    std::uint64_t nextId()
    {
        return id_seq.fetch_add(1, std::memory_order_acq_rel);
    }

    // ------------------------------------------------------------- enqueue
    std::uint64_t enqueue(Command cmd)
    {
        if (!initialized.load(std::memory_order_acquire)) {
            return 0;
        }
        cmd.linear_velocity  = clampAbs(cmd.linear_velocity,  cfg.limits.max_linear_velocity);
        cmd.angular_velocity = clampAbs(cmd.angular_velocity, cfg.limits.max_angular_velocity);
        if (cmd.id == 0) {
            cmd.id = nextId();
        }
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            pending = std::move(cmd);
        }
        queue_cv.notify_all();
        return pending ? pending->id : 0;
    }

    // -------------------------------------------------------------- speed
    bool adjustSpeedImpl(double ratio)
    {
        if (!initialized.load(std::memory_order_acquire)) return false;
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            pending_speed_ratio = ratio;
        }
        queue_cv.notify_all();
        return has_current;
    }

    // ------------------------------------------------------------ publish
    void publishTwist(double lin, double ang)
    {
        if (!cmd_pub) return;
        geometry_msgs::msg::Twist msg;
        msg.linear.x  = lin;
        msg.linear.y  = 0.0;
        msg.linear.z  = 0.0;
        msg.angular.x = 0.0;
        msg.angular.y = 0.0;
        msg.angular.z = ang;
        cmd_pub->publish(msg);
    }

    // ---------------------------------------------------------- callbacks
    void fireStatus(const Command& c, CommandStatus s)
    {
        CommandStatusCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx);
            cb = status_cb;
        }
        if (logger) {
            LOG_INFO(logger,
                     "cmd #{} ({}) → {}  lin={} m/s ang={} rad/s dist={} rot={}",
                     c.id, c.label.empty() ? std::string("?") : c.label,
                     statusLabel(s), c.linear_velocity, c.angular_velocity,
                     c.distance, c.rotation);
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

    // ----------------------------------------------------- worker control
    void promoteIfPending(std::chrono::steady_clock::time_point now)
    {
        std::optional<Command> new_cmd;
        std::optional<double>  new_ratio;
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            if (pending) {
                new_cmd = std::move(pending);
                pending.reset();
            }
            if (pending_speed_ratio) {
                new_ratio = pending_speed_ratio;
                pending_speed_ratio.reset();
            }
        }

        if (new_cmd) {
            if (has_current && !new_cmd->is_emergency_stop) {
                fireStatus(current_cmd, CommandStatus::kPreempted);
            } else if (has_current && new_cmd->is_emergency_stop) {
                fireStatus(current_cmd, CommandStatus::kAborted);
            }
            current_cmd       = std::move(*new_cmd);
            has_current       = true;
            emergency_mode    = current_cmd.is_emergency_stop;
            cmd_started_at    = now;
            last_heartbeat_at = now;
            traveled_dist     = 0.0;
            traveled_rot      = 0.0;
            {
                std::lock_guard<std::mutex> lk(state_mtx);
                state.traveled_distance       = 0.0;
                state.traveled_rotation       = 0.0;
                state.current_command_id      = current_cmd.id;
                state.current_label           = current_cmd.label;
                state.is_executing            = true;
                state.is_emergency            = emergency_mode;
                state.commanded_linear_velocity  = current_cmd.linear_velocity;
                state.commanded_angular_velocity = current_cmd.angular_velocity;
            }
            fireStatus(current_cmd, CommandStatus::kAccepted);
        }

        if (new_ratio && has_current && !emergency_mode) {
            const double r = 1.0 + *new_ratio;
            current_cmd.linear_velocity  = clampAbs(
                current_cmd.linear_velocity * r, cfg.limits.max_linear_velocity);
            current_cmd.angular_velocity = clampAbs(
                current_cmd.angular_velocity * r, cfg.limits.max_angular_velocity);
            last_heartbeat_at = now;
            std::lock_guard<std::mutex> lk(state_mtx);
            state.commanded_linear_velocity  = current_cmd.linear_velocity;
            state.commanded_angular_velocity = current_cmd.angular_velocity;
        }
    }

    // ------------------------------------------------------- trapezoidal tick
    void tickOnce(double dt_s, std::chrono::steady_clock::time_point now)
    {
        promoteIfPending(now);

        if (!has_current) {
            // Idle: hold zero, no publishing (we already published 0 on
            // completion).  Just refresh timestamp on state.
            std::lock_guard<std::mutex> lk(state_mtx);
            state.timestamp_us = nowMicros();
            return;
        }

        const Limits& L = cfg.limits;

        const bool position_lin = (current_cmd.distance != 0.0);
        const bool position_ang = (current_cmd.rotation != 0.0);
        const bool velocity_mode = (!position_lin && !position_ang &&
                                    current_cmd.duration_ms == 0 &&
                                    !current_cmd.is_emergency_stop);

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - cmd_started_at).count();
        const bool duration_expired =
            current_cmd.duration_ms > 0 && elapsed_ms >= current_cmd.duration_ms;

        // Heartbeat timeout for velocity mode — ramp to zero gracefully.
        bool heartbeat_expired = false;
        if (velocity_mode && cfg.safety.heartbeat_timeout_ms > 0) {
            const auto since_hb = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_heartbeat_at).count();
            if (since_hb >= cfg.safety.heartbeat_timeout_ms) {
                heartbeat_expired = true;
            }
        }

        // ----- target velocities ------------------------------------------
        double target_lin = current_cmd.linear_velocity;
        double target_ang = current_cmd.angular_velocity;

        const double decel_lin = emergency_mode
            ? L.emergency_linear_decel : L.max_linear_decel;
        const double decel_ang = emergency_mode
            ? L.emergency_angular_decel : L.max_angular_decel;
        const double accel_lin = L.max_linear_accel;
        const double accel_ang = L.max_angular_accel;

        // Brake-distance lookahead for position-controlled axes.
        if (position_lin && decel_lin > 0.0) {
            const double remaining = std::abs(current_cmd.distance) - traveled_dist;
            const double brake = (cur_linear_v * cur_linear_v) / (2.0 * decel_lin);
            if (remaining <= 0.0 || remaining <= brake) {
                target_lin = 0.0;
            }
        }
        if (position_ang && decel_ang > 0.0) {
            const double remaining = std::abs(current_cmd.rotation) - traveled_rot;
            const double brake = (cur_angular_v * cur_angular_v) / (2.0 * decel_ang);
            if (remaining <= 0.0 || remaining <= brake) {
                target_ang = 0.0;
            }
        }

        // Emergency / heartbeat / duration overrides — force zero target.
        if (emergency_mode || heartbeat_expired || duration_expired) {
            target_lin = 0.0;
            target_ang = 0.0;
        }

        // Clamp targets.
        target_lin = clampAbs(target_lin, L.max_linear_velocity);
        target_ang = clampAbs(target_ang, L.max_angular_velocity);

        // ----- ramp -------------------------------------------------------
        // Pick accel vs decel by direction of change.
        auto step = [](double cur, double target, double accel, double decel,
                       double dt) {
            const bool slowing = std::abs(target) < std::abs(cur) ||
                                 (cur != 0.0 && target != 0.0 &&
                                  ((cur > 0) != (target > 0)));
            const double rate = (slowing ? decel : accel) * dt;
            return approach(cur, target, rate);
        };

        cur_linear_v  = step(cur_linear_v,  target_lin, accel_lin, decel_lin, dt_s);
        cur_angular_v = step(cur_angular_v, target_ang, accel_ang, decel_ang, dt_s);

        // ----- publish ----------------------------------------------------
        publishTwist(cur_linear_v, cur_angular_v);

        // ----- integrate --------------------------------------------------
        traveled_dist += std::abs(cur_linear_v) * dt_s;
        traveled_rot  += std::abs(cur_angular_v) * dt_s;
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            state.linear_velocity      = cur_linear_v;
            state.angular_velocity     = cur_angular_v;
            state.traveled_distance    = traveled_dist;
            state.traveled_rotation    = traveled_rot;
            state.is_executing         = true;
            state.is_emergency         = emergency_mode;
            state.commanded_linear_velocity  = current_cmd.linear_velocity;
            state.commanded_angular_velocity = current_cmd.angular_velocity;
            state.current_command_id   = current_cmd.id;
            state.current_label        = current_cmd.label;
            state.timestamp_us         = nowMicros();
        }
        fireState();

        // ----- completion -------------------------------------------------
        const bool stopped = std::abs(cur_linear_v)  < kEps &&
                             std::abs(cur_angular_v) < kEps;

        bool complete = false;
        CommandStatus outcome = CommandStatus::kCompleted;

        if (emergency_mode) {
            if (stopped) {
                complete = true;
                outcome  = CommandStatus::kAborted;
            }
        } else {
            const bool dist_done = !position_lin ||
                (traveled_dist >= std::abs(current_cmd.distance) && std::abs(cur_linear_v) < kEps);
            const bool rot_done  = !position_ang ||
                (traveled_rot  >= std::abs(current_cmd.rotation) && std::abs(cur_angular_v) < kEps);

            if ((position_lin || position_ang) && dist_done && rot_done) {
                complete = true;
            }
            if (duration_expired && stopped) {
                complete = true;
            }
            if (heartbeat_expired && stopped) {
                complete = true;
            }
            if (cfg.safety.command_timeout_ms > 0 &&
                elapsed_ms >= cfg.safety.command_timeout_ms) {
                // Hard timeout — force completion regardless of velocity (next
                // tick will publish zero anyway).
                complete = true;
            }
        }

        if (complete) {
            fireStatus(current_cmd, outcome);
            publishTwist(0.0, 0.0);
            has_current    = false;
            emergency_mode = false;
            cur_linear_v   = 0.0;
            cur_angular_v  = 0.0;
            {
                std::lock_guard<std::mutex> lk(state_mtx);
                state.linear_velocity            = 0.0;
                state.angular_velocity           = 0.0;
                state.commanded_linear_velocity  = 0.0;
                state.commanded_angular_velocity = 0.0;
                state.is_executing               = false;
                state.is_emergency               = false;
                state.current_command_id         = 0;
                state.current_label.clear();
                state.timestamp_us               = nowMicros();
            }
            fireState();
        }
    }

    // -------------------------------------------------------------- loop
    void workerLoop()
    {
        const int hz = cfg.ros2.publish_rate_hz > 0 ? cfg.ros2.publish_rate_hz : 50;
        const auto period = std::chrono::duration<double>(1.0 / hz);
        const double dt_s = period.count();

        auto next_tick = std::chrono::steady_clock::now() + period;

        while (!worker_stop.load(std::memory_order_acquire)) {
            // Block until next tick or until something is pending / stopping.
            {
                std::unique_lock<std::mutex> lk(queue_mtx);
                queue_cv.wait_until(lk, next_tick, [this] {
                    return worker_stop.load(std::memory_order_acquire) ||
                           pending.has_value() ||
                           pending_speed_ratio.has_value();
                });
                if (worker_stop.load(std::memory_order_acquire)) {
                    break;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            // Adjust dt to elapsed if we woke early due to a pending command.
            // (Use nominal dt to keep trapezoidal math stable.)
            tickOnce(dt_s, now);

            // Schedule next tick — skip ticks we missed instead of catching up.
            do { next_tick += period; } while (next_tick <= now);
        }

        // Final zero command.
        publishTwist(0.0, 0.0);
    }
};

// ============================================================================
// Public facade
// ============================================================================
WheelchairController::WheelchairController() : impl_(std::make_unique<Impl>()) {}
WheelchairController::~WheelchairController() { if (impl_) impl_->shutdownImpl(); }

bool WheelchairController::init(const std::string& config_path)
{
    return impl_->initImpl(loadConfig(config_path));
}

bool WheelchairController::init(const WheelchairConfig& cfg)
{
    return impl_->initImpl(cfg);
}

void WheelchairController::shutdown() { impl_->shutdownImpl(); }

bool WheelchairController::isInitialized() const
{
    return impl_->initialized.load(std::memory_order_acquire);
}

std::uint64_t WheelchairController::sendCommand(Command cmd)
{
    return impl_->enqueue(std::move(cmd));
}

std::uint64_t WheelchairController::moveForward(double distance_m, double velocity_mps)
{
    Command c;
    const double v = velocity_mps > 0.0 ? velocity_mps : impl_->cfg.defaults.linear_velocity;
    const double d = distance_m   > 0.0 ? distance_m   : impl_->cfg.defaults.forward_distance;
    c.linear_velocity = std::abs(v);
    c.distance        = std::abs(d);
    c.label           = "forward";
    return impl_->enqueue(std::move(c));
}

std::uint64_t WheelchairController::moveBackward(double distance_m, double velocity_mps)
{
    Command c;
    const double v = velocity_mps > 0.0 ? velocity_mps : impl_->cfg.defaults.linear_velocity;
    const double d = distance_m   > 0.0 ? distance_m   : impl_->cfg.defaults.forward_distance;
    c.linear_velocity = -std::abs(v);
    c.distance        = -std::abs(d);
    c.label           = "backward";
    return impl_->enqueue(std::move(c));
}

std::uint64_t WheelchairController::turnLeft(double angle_rad, double angular_rps)
{
    Command c;
    const double w = angular_rps > 0.0 ? angular_rps : impl_->cfg.defaults.angular_velocity;
    const double r = angle_rad   > 0.0 ? angle_rad   : impl_->cfg.defaults.turn_angle;
    const double entry_v = impl_->cfg.defaults.turn_entry_linear_velocity;
    const double entry_d = impl_->cfg.defaults.turn_entry_distance;
    if (entry_v > 0.0 && entry_d > 0.0) {
        const double max_arc_d = std::abs(w) > kEps
            ? (std::abs(entry_v) / std::abs(w)) * std::abs(r)
            : entry_d;
        c.linear_velocity = std::abs(entry_v);
        c.distance        = std::min(std::abs(entry_d), max_arc_d);
    }
    c.angular_velocity = std::abs(w);
    c.rotation         = std::abs(r);
    c.label            = "turn_left";
    return impl_->enqueue(std::move(c));
}

std::uint64_t WheelchairController::turnRight(double angle_rad, double angular_rps)
{
    Command c;
    const double w = angular_rps > 0.0 ? angular_rps : impl_->cfg.defaults.angular_velocity;
    const double r = angle_rad   > 0.0 ? angle_rad   : impl_->cfg.defaults.turn_angle;
    const double entry_v = impl_->cfg.defaults.turn_entry_linear_velocity;
    const double entry_d = impl_->cfg.defaults.turn_entry_distance;
    if (entry_v > 0.0 && entry_d > 0.0) {
        const double max_arc_d = std::abs(w) > kEps
            ? (std::abs(entry_v) / std::abs(w)) * std::abs(r)
            : entry_d;
        c.linear_velocity = std::abs(entry_v);
        c.distance        = std::min(std::abs(entry_d), max_arc_d);
    }
    c.angular_velocity = -std::abs(w);
    c.rotation         = -std::abs(r);
    c.label            = "turn_right";
    return impl_->enqueue(std::move(c));
}

std::uint64_t WheelchairController::moveAtVelocity(double linear_mps, double angular_rps,
                                                   int duration_ms)
{
    Command c;
    c.linear_velocity  = linear_mps;
    c.angular_velocity = angular_rps;
    c.duration_ms      = duration_ms;
    c.label            = "velocity";
    return impl_->enqueue(std::move(c));
}

bool WheelchairController::adjustSpeed(double ratio)
{
    return impl_->adjustSpeedImpl(ratio);
}

std::uint64_t WheelchairController::stop()
{
    Command c;
    c.linear_velocity  = 0.0;
    c.angular_velocity = 0.0;
    c.label            = "stop";
    return impl_->enqueue(std::move(c));
}

std::uint64_t WheelchairController::emergencyStop()
{
    Command c;
    c.linear_velocity  = 0.0;
    c.angular_velocity = 0.0;
    c.is_emergency_stop = true;
    c.label            = "emergency_stop";
    return impl_->enqueue(std::move(c));
}

State WheelchairController::state() const
{
    std::lock_guard<std::mutex> lk(impl_->state_mtx);
    return impl_->state;
}

void WheelchairController::setStateCallback(StateCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->state_cb = std::move(cb);
}

void WheelchairController::setCommandStatusCallback(CommandStatusCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->status_cb = std::move(cb);
}

const WheelchairConfig& WheelchairController::config() const { return impl_->cfg; }

// ============================================================================
// TOML loader
// ============================================================================
WheelchairConfig WheelchairController::loadConfig(const std::string& config_path)
{
    WheelchairConfig cfg;
    if (!fileExists(config_path)) {
        return cfg;
    }
    toml::table table = toml::parse_file(config_path);
    const toml::node_view in = table["wheelchair"];

    cfg.enabled = in["enabled"].value_or(cfg.enabled);

    const toml::node_view L = in["limits"];
    cfg.limits.max_linear_velocity     = L["max_linear_velocity"].value_or(cfg.limits.max_linear_velocity);
    cfg.limits.max_angular_velocity    = L["max_angular_velocity"].value_or(cfg.limits.max_angular_velocity);
    cfg.limits.max_linear_accel        = L["max_linear_accel"].value_or(cfg.limits.max_linear_accel);
    cfg.limits.max_angular_accel       = L["max_angular_accel"].value_or(cfg.limits.max_angular_accel);
    cfg.limits.max_linear_decel        = L["max_linear_decel"].value_or(cfg.limits.max_linear_decel);
    cfg.limits.max_angular_decel       = L["max_angular_decel"].value_or(cfg.limits.max_angular_decel);
    cfg.limits.emergency_linear_decel  = L["emergency_linear_decel"].value_or(cfg.limits.emergency_linear_decel);
    cfg.limits.emergency_angular_decel = L["emergency_angular_decel"].value_or(cfg.limits.emergency_angular_decel);

    const toml::node_view D = in["defaults"];
    cfg.defaults.linear_velocity  = D["linear_velocity"].value_or(cfg.defaults.linear_velocity);
    cfg.defaults.angular_velocity = D["angular_velocity"].value_or(cfg.defaults.angular_velocity);
    cfg.defaults.forward_distance = D["forward_distance"].value_or(cfg.defaults.forward_distance);
    cfg.defaults.turn_angle       = D["turn_angle"].value_or(cfg.defaults.turn_angle);
    cfg.defaults.turn_entry_linear_velocity =
        D["turn_entry_linear_velocity"].value_or(cfg.defaults.turn_entry_linear_velocity);
    cfg.defaults.turn_entry_distance =
        D["turn_entry_distance"].value_or(cfg.defaults.turn_entry_distance);
    cfg.defaults.speed_step_ratio = D["speed_step_ratio"].value_or(cfg.defaults.speed_step_ratio);

    const toml::node_view S = in["safety"];
    cfg.safety.command_timeout_ms        = S["command_timeout_ms"].value_or(cfg.safety.command_timeout_ms);
    cfg.safety.heartbeat_timeout_ms      = S["heartbeat_timeout_ms"].value_or(cfg.safety.heartbeat_timeout_ms);
    cfg.safety.zero_velocity_on_init     = S["zero_velocity_on_init"].value_or(cfg.safety.zero_velocity_on_init);
    cfg.safety.zero_velocity_on_shutdown = S["zero_velocity_on_shutdown"].value_or(cfg.safety.zero_velocity_on_shutdown);

    const toml::node_view R = in["ros2"];
    cfg.ros2.enabled         = R["enabled"].value_or(cfg.ros2.enabled);
    cfg.ros2.node_name       = R["node_name"].value_or(cfg.ros2.node_name);
    cfg.ros2.ns              = R["namespace"].value_or(cfg.ros2.ns);
    cfg.ros2.cmd_vel_topic   = R["cmd_vel_topic"].value_or(cfg.ros2.cmd_vel_topic);
    cfg.ros2.publish_rate_hz = R["publish_rate_hz"].value_or(cfg.ros2.publish_rate_hz);
    cfg.ros2.qos_reliability = R["qos_reliability"].value_or(cfg.ros2.qos_reliability);
    cfg.ros2.qos_history     = R["qos_history"].value_or(cfg.ros2.qos_history);
    cfg.ros2.qos_depth       = R["qos_depth"].value_or(cfg.ros2.qos_depth);

    const toml::node_view G = in["logging"];
    cfg.logging.log_dir            = G["log_dir"].value_or(cfg.logging.log_dir);
    cfg.logging.log_file           = G["log_file"].value_or(cfg.logging.log_file);
    cfg.logging.console            = G["console"].value_or(cfg.logging.console);
    cfg.logging.rotation_max_bytes = static_cast<std::size_t>(
        G["rotation_max_bytes"].value_or(static_cast<std::int64_t>(cfg.logging.rotation_max_bytes)));
    cfg.logging.rotation_max_files = G["rotation_max_files"].value_or(cfg.logging.rotation_max_files);

    return cfg;
}

}  // namespace wheelchair
