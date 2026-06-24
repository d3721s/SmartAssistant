#include "ArmController.h"

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

namespace arm {

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

inline double approach(double cur, double target, double max_step)
{
    if (max_step <= 0.0) return target;
    const double delta = target - cur;
    if (std::abs(delta) <= max_step) return target;
    return cur + (delta > 0.0 ? max_step : -max_step);
}

inline bool axisDone(bool enabled, double traveled, double target, double velocity)
{
    return !enabled ||
           (traveled >= std::abs(target) && std::abs(velocity) < kEps);
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

struct ArmController::Impl {
    ArmConfig     cfg;
    quill::Logger* logger = nullptr;

    std::atomic<bool>          initialized{false};
    std::atomic<std::uint64_t> id_seq{1};

    std::mutex                 queue_mtx;
    std::condition_variable    queue_cv;
    std::optional<Command>     pending;
    std::optional<double>      pending_speed_ratio;
    std::atomic<bool>          worker_stop{false};
    std::thread                worker;

    Command current_cmd{};
    std::atomic<bool> has_current{false};
    double  cur_lx           = 0.0;
    double  cur_ly           = 0.0;
    double  cur_lz           = 0.0;
    double  cur_ax           = 0.0;
    double  cur_ay           = 0.0;
    double  cur_az           = 0.0;
    double  traveled_x       = 0.0;
    double  traveled_y       = 0.0;
    double  traveled_z       = 0.0;
    double  rotated_x        = 0.0;
    double  rotated_y        = 0.0;
    double  rotated_z        = 0.0;
    bool    emergency_mode   = false;
    std::chrono::steady_clock::time_point cmd_started_at;
    std::chrono::steady_clock::time_point last_heartbeat_at;

    mutable std::mutex state_mtx;
    State              state{};

    std::mutex            cb_mtx;
    StateCallback         state_cb;
    CommandStatusCallback status_cb;

    bool                                                          ros_ok = false;
    std::shared_ptr<rclcpp::Node>                                 ros_node;
    std::shared_ptr<rclcpp::Publisher<geometry_msgs::msg::Twist>> cmd_pub;

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
                "arm_controller_console_sink");
            sinks.push_back(console_sink);
        }
        if (sinks.empty()) {
            auto fallback = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "arm_controller_fallback_sink");
            sinks.push_back(fallback);
        }
        logger = quill::Frontend::create_or_get_logger(
            "ArmController", std::move(sinks));
        logger->set_log_level(quill::LogLevel::Info);
    }

    bool initRos()
    {
        if (!cfg.enabled || !cfg.ros2.enabled) {
            LOG_WARNING(logger, "Arm ROS2 publishing disabled by config");
            return false;
        }
        rclcppAcquire();
        try {
            const std::string node_name = cfg.ros2.node_name.empty()
                ? std::string("arm_end_effector_controller")
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
                ? std::string("/arm_end_effector_controller/cmd_vel")
                : cfg.ros2.cmd_vel_topic;
            cmd_pub = ros_node->create_publisher<geometry_msgs::msg::Twist>(topic, qos);

            LOG_INFO(logger,
                     "ROS2 node '{}' publishing end-effector Twist on '{}' (qos={}, depth={}, hz={})",
                     node_name, topic, cfg.ros2.qos_reliability,
                     cfg.ros2.qos_depth, cfg.ros2.publish_rate_hz);
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR(logger, "Arm ROS2 node creation failed: {}", e.what());
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

    bool initImpl(ArmConfig in_cfg)
    {
        if (initialized.load(std::memory_order_acquire)) {
            return true;
        }
        cfg = std::move(in_cfg);

        setupLogger();
        LOG_INFO(logger,
                 "ArmController init: enabled={}, max_lin={} m/s, max_ang={} rad/s, hz={}",
                 cfg.enabled, cfg.limits.max_linear_velocity,
                 cfg.limits.max_angular_velocity, cfg.ros2.publish_rate_hz);

        ros_ok = initRos();
        if (!ros_ok) {
            LOG_WARNING(logger, "Arm ROS2 unavailable - commands tracked but not published");
        }

        worker_stop.store(false, std::memory_order_release);
        worker = std::thread(&Impl::workerLoop, this);

        if (cfg.safety.zero_velocity_on_init) {
            publishTwist(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        }

        initialized.store(true, std::memory_order_release);
        LOG_INFO(logger, "ArmController initialized");
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
            publishTwist(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        }
        teardownRos();
        if (logger) {
            LOG_INFO(logger, "ArmController shutdown complete");
        }
    }

    std::uint64_t nextId()
    {
        return id_seq.fetch_add(1, std::memory_order_acq_rel);
    }

    std::uint64_t enqueue(Command cmd)
    {
        if (!initialized.load(std::memory_order_acquire)) {
            return 0;
        }
        cmd.linear_x  = clampAbs(cmd.linear_x,  cfg.limits.max_linear_velocity);
        cmd.linear_y  = clampAbs(cmd.linear_y,  cfg.limits.max_linear_velocity);
        cmd.linear_z  = clampAbs(cmd.linear_z,  cfg.limits.max_linear_velocity);
        cmd.angular_x = clampAbs(cmd.angular_x, cfg.limits.max_angular_velocity);
        cmd.angular_y = clampAbs(cmd.angular_y, cfg.limits.max_angular_velocity);
        cmd.angular_z = clampAbs(cmd.angular_z, cfg.limits.max_angular_velocity);
        if (cmd.id == 0) {
            cmd.id = nextId();
        }
        const std::uint64_t id = cmd.id;
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            pending = std::move(cmd);
        }
        queue_cv.notify_all();
        return id;
    }

    bool adjustSpeedImpl(double ratio)
    {
        if (!initialized.load(std::memory_order_acquire)) return false;
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            pending_speed_ratio = ratio;
        }
        queue_cv.notify_all();
        return has_current.load(std::memory_order_acquire);
    }

    void publishTwist(double lx, double ly, double lz,
                      double ax, double ay, double az)
    {
        if (!cmd_pub) return;
        geometry_msgs::msg::Twist msg;
        msg.linear.x  = lx;
        msg.linear.y  = ly;
        msg.linear.z  = lz;
        msg.angular.x = ax;
        msg.angular.y = ay;
        msg.angular.z = az;
        cmd_pub->publish(msg);
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
                     "cmd #{} ({}) -> {} lin=({}, {}, {}) ang=({}, {}, {})",
                     c.id, c.label.empty() ? std::string("?") : c.label,
                     statusLabel(s), c.linear_x, c.linear_y, c.linear_z,
                     c.angular_x, c.angular_y, c.angular_z);
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

    void resetTravelState()
    {
        traveled_x = traveled_y = traveled_z = 0.0;
        rotated_x = rotated_y = rotated_z = 0.0;
        std::lock_guard<std::mutex> lk(state_mtx);
        state.traveled_x = state.traveled_y = state.traveled_z = 0.0;
        state.rotated_x = state.rotated_y = state.rotated_z = 0.0;
    }

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
            if (has_current.load(std::memory_order_acquire) &&
                !new_cmd->is_emergency_stop) {
                fireStatus(current_cmd, CommandStatus::kPreempted);
            } else if (has_current.load(std::memory_order_acquire) &&
                       new_cmd->is_emergency_stop) {
                fireStatus(current_cmd, CommandStatus::kAborted);
            }
            current_cmd       = std::move(*new_cmd);
            has_current.store(true, std::memory_order_release);
            emergency_mode    = current_cmd.is_emergency_stop;
            cmd_started_at    = now;
            last_heartbeat_at = now;
            resetTravelState();
            {
                std::lock_guard<std::mutex> lk(state_mtx);
                state.current_command_id  = current_cmd.id;
                state.current_label       = current_cmd.label;
                state.is_executing        = true;
                state.is_emergency        = emergency_mode;
                state.commanded_linear_x  = current_cmd.linear_x;
                state.commanded_linear_y  = current_cmd.linear_y;
                state.commanded_linear_z  = current_cmd.linear_z;
                state.commanded_angular_x = current_cmd.angular_x;
                state.commanded_angular_y = current_cmd.angular_y;
                state.commanded_angular_z = current_cmd.angular_z;
            }
            fireStatus(current_cmd, CommandStatus::kAccepted);
        }

        if (new_ratio && has_current.load(std::memory_order_acquire) &&
            !emergency_mode) {
            const double r = 1.0 + *new_ratio;
            current_cmd.linear_x  = clampAbs(current_cmd.linear_x  * r, cfg.limits.max_linear_velocity);
            current_cmd.linear_y  = clampAbs(current_cmd.linear_y  * r, cfg.limits.max_linear_velocity);
            current_cmd.linear_z  = clampAbs(current_cmd.linear_z  * r, cfg.limits.max_linear_velocity);
            current_cmd.angular_x = clampAbs(current_cmd.angular_x * r, cfg.limits.max_angular_velocity);
            current_cmd.angular_y = clampAbs(current_cmd.angular_y * r, cfg.limits.max_angular_velocity);
            current_cmd.angular_z = clampAbs(current_cmd.angular_z * r, cfg.limits.max_angular_velocity);
            last_heartbeat_at = now;
            std::lock_guard<std::mutex> lk(state_mtx);
            state.commanded_linear_x  = current_cmd.linear_x;
            state.commanded_linear_y  = current_cmd.linear_y;
            state.commanded_linear_z  = current_cmd.linear_z;
            state.commanded_angular_x = current_cmd.angular_x;
            state.commanded_angular_y = current_cmd.angular_y;
            state.commanded_angular_z = current_cmd.angular_z;
        }
    }

    void applyBrakeLookahead(double& target, double cur, bool position_mode,
                             double traveled, double requested, double decel)
    {
        if (!position_mode || decel <= 0.0) return;
        const double remaining = std::abs(requested) - traveled;
        const double brake = (cur * cur) / (2.0 * decel);
        if (remaining <= 0.0 || remaining <= brake) {
            target = 0.0;
        }
    }

    void tickOnce(double dt_s, std::chrono::steady_clock::time_point now)
    {
        promoteIfPending(now);

        if (!has_current.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(state_mtx);
            state.timestamp_us = nowMicros();
            return;
        }

        const Limits& L = cfg.limits;

        const bool px = current_cmd.translation_x != 0.0;
        const bool py = current_cmd.translation_y != 0.0;
        const bool pz = current_cmd.translation_z != 0.0;
        const bool rx = current_cmd.rotation_x != 0.0;
        const bool ry = current_cmd.rotation_y != 0.0;
        const bool rz = current_cmd.rotation_z != 0.0;
        const bool any_position = px || py || pz || rx || ry || rz;
        const bool velocity_mode = !any_position &&
                                   current_cmd.duration_ms == 0 &&
                                   !current_cmd.is_emergency_stop;

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - cmd_started_at).count();
        const bool duration_expired =
            current_cmd.duration_ms > 0 && elapsed_ms >= current_cmd.duration_ms;

        bool heartbeat_expired = false;
        if (velocity_mode && cfg.safety.heartbeat_timeout_ms > 0) {
            const auto since_hb = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_heartbeat_at).count();
            heartbeat_expired = since_hb >= cfg.safety.heartbeat_timeout_ms;
        }

        double target_lx = current_cmd.linear_x;
        double target_ly = current_cmd.linear_y;
        double target_lz = current_cmd.linear_z;
        double target_ax = current_cmd.angular_x;
        double target_ay = current_cmd.angular_y;
        double target_az = current_cmd.angular_z;

        const double decel_lin = emergency_mode
            ? L.emergency_linear_decel : L.max_linear_decel;
        const double decel_ang = emergency_mode
            ? L.emergency_angular_decel : L.max_angular_decel;

        applyBrakeLookahead(target_lx, cur_lx, px, traveled_x,
                            current_cmd.translation_x, decel_lin);
        applyBrakeLookahead(target_ly, cur_ly, py, traveled_y,
                            current_cmd.translation_y, decel_lin);
        applyBrakeLookahead(target_lz, cur_lz, pz, traveled_z,
                            current_cmd.translation_z, decel_lin);
        applyBrakeLookahead(target_ax, cur_ax, rx, rotated_x,
                            current_cmd.rotation_x, decel_ang);
        applyBrakeLookahead(target_ay, cur_ay, ry, rotated_y,
                            current_cmd.rotation_y, decel_ang);
        applyBrakeLookahead(target_az, cur_az, rz, rotated_z,
                            current_cmd.rotation_z, decel_ang);

        if (emergency_mode || heartbeat_expired || duration_expired) {
            target_lx = target_ly = target_lz = 0.0;
            target_ax = target_ay = target_az = 0.0;
        }

        target_lx = clampAbs(target_lx, L.max_linear_velocity);
        target_ly = clampAbs(target_ly, L.max_linear_velocity);
        target_lz = clampAbs(target_lz, L.max_linear_velocity);
        target_ax = clampAbs(target_ax, L.max_angular_velocity);
        target_ay = clampAbs(target_ay, L.max_angular_velocity);
        target_az = clampAbs(target_az, L.max_angular_velocity);

        auto step = [](double cur, double target, double accel, double decel,
                       double dt) {
            const bool slowing = std::abs(target) < std::abs(cur) ||
                                 (cur != 0.0 && target != 0.0 &&
                                  ((cur > 0.0) != (target > 0.0)));
            const double rate = (slowing ? decel : accel) * dt;
            return approach(cur, target, rate);
        };

        cur_lx = step(cur_lx, target_lx, L.max_linear_accel, decel_lin, dt_s);
        cur_ly = step(cur_ly, target_ly, L.max_linear_accel, decel_lin, dt_s);
        cur_lz = step(cur_lz, target_lz, L.max_linear_accel, decel_lin, dt_s);
        cur_ax = step(cur_ax, target_ax, L.max_angular_accel, decel_ang, dt_s);
        cur_ay = step(cur_ay, target_ay, L.max_angular_accel, decel_ang, dt_s);
        cur_az = step(cur_az, target_az, L.max_angular_accel, decel_ang, dt_s);

        publishTwist(cur_lx, cur_ly, cur_lz, cur_ax, cur_ay, cur_az);

        traveled_x += std::abs(cur_lx) * dt_s;
        traveled_y += std::abs(cur_ly) * dt_s;
        traveled_z += std::abs(cur_lz) * dt_s;
        rotated_x  += std::abs(cur_ax) * dt_s;
        rotated_y  += std::abs(cur_ay) * dt_s;
        rotated_z  += std::abs(cur_az) * dt_s;

        {
            std::lock_guard<std::mutex> lk(state_mtx);
            state.linear_x            = cur_lx;
            state.linear_y            = cur_ly;
            state.linear_z            = cur_lz;
            state.angular_x           = cur_ax;
            state.angular_y           = cur_ay;
            state.angular_z           = cur_az;
            state.commanded_linear_x  = current_cmd.linear_x;
            state.commanded_linear_y  = current_cmd.linear_y;
            state.commanded_linear_z  = current_cmd.linear_z;
            state.commanded_angular_x = current_cmd.angular_x;
            state.commanded_angular_y = current_cmd.angular_y;
            state.commanded_angular_z = current_cmd.angular_z;
            state.traveled_x          = traveled_x;
            state.traveled_y          = traveled_y;
            state.traveled_z          = traveled_z;
            state.rotated_x           = rotated_x;
            state.rotated_y           = rotated_y;
            state.rotated_z           = rotated_z;
            state.is_executing        = true;
            state.is_emergency        = emergency_mode;
            state.current_command_id  = current_cmd.id;
            state.current_label       = current_cmd.label;
            state.timestamp_us        = nowMicros();
        }
        fireState();

        const bool stopped = std::abs(cur_lx) < kEps &&
                             std::abs(cur_ly) < kEps &&
                             std::abs(cur_lz) < kEps &&
                             std::abs(cur_ax) < kEps &&
                             std::abs(cur_ay) < kEps &&
                             std::abs(cur_az) < kEps;

        bool complete = false;
        CommandStatus outcome = CommandStatus::kCompleted;

        if (emergency_mode) {
            if (stopped) {
                complete = true;
                outcome = CommandStatus::kAborted;
            }
        } else {
            const bool position_done =
                axisDone(px, traveled_x, current_cmd.translation_x, cur_lx) &&
                axisDone(py, traveled_y, current_cmd.translation_y, cur_ly) &&
                axisDone(pz, traveled_z, current_cmd.translation_z, cur_lz) &&
                axisDone(rx, rotated_x, current_cmd.rotation_x, cur_ax) &&
                axisDone(ry, rotated_y, current_cmd.rotation_y, cur_ay) &&
                axisDone(rz, rotated_z, current_cmd.rotation_z, cur_az);

            if (any_position && position_done) {
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
                complete = true;
            }
        }

        if (complete) {
            fireStatus(current_cmd, outcome);
            publishTwist(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
            has_current.store(false, std::memory_order_release);
            emergency_mode = false;
            cur_lx = cur_ly = cur_lz = 0.0;
            cur_ax = cur_ay = cur_az = 0.0;
            {
                std::lock_guard<std::mutex> lk(state_mtx);
                state.linear_x = state.linear_y = state.linear_z = 0.0;
                state.angular_x = state.angular_y = state.angular_z = 0.0;
                state.commanded_linear_x = 0.0;
                state.commanded_linear_y = 0.0;
                state.commanded_linear_z = 0.0;
                state.commanded_angular_x = 0.0;
                state.commanded_angular_y = 0.0;
                state.commanded_angular_z = 0.0;
                state.is_executing = false;
                state.is_emergency = false;
                state.current_command_id = 0;
                state.current_label.clear();
                state.timestamp_us = nowMicros();
            }
            fireState();
        }
    }

    void workerLoop()
    {
        const int hz = cfg.ros2.publish_rate_hz > 0 ? cfg.ros2.publish_rate_hz : 50;
        const auto period = std::chrono::duration<double>(1.0 / hz);
        const double dt_s = period.count();

        auto next_tick = std::chrono::steady_clock::now() + period;

        while (!worker_stop.load(std::memory_order_acquire)) {
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
            tickOnce(dt_s, now);
            do { next_tick += period; } while (next_tick <= now);
        }

        publishTwist(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    }
};

ArmController::ArmController() : impl_(std::make_unique<Impl>()) {}
ArmController::~ArmController() { if (impl_) impl_->shutdownImpl(); }

bool ArmController::init(const std::string& config_path)
{
    return impl_->initImpl(loadConfig(config_path));
}

bool ArmController::init(const ArmConfig& cfg)
{
    return impl_->initImpl(cfg);
}

void ArmController::shutdown() { impl_->shutdownImpl(); }

bool ArmController::isInitialized() const
{
    return impl_->initialized.load(std::memory_order_acquire);
}

std::uint64_t ArmController::sendCommand(Command cmd)
{
    return impl_->enqueue(std::move(cmd));
}

std::uint64_t ArmController::raise(double distance_m, double velocity_mps)
{
    Command c;
    const double v = velocity_mps > 0.0 ? velocity_mps : impl_->cfg.defaults.linear_velocity;
    const double d = distance_m > 0.0 ? distance_m : impl_->cfg.defaults.step_distance;
    c.linear_z = std::abs(v);
    c.translation_z = std::abs(d);
    c.label = "raise";
    return impl_->enqueue(std::move(c));
}

std::uint64_t ArmController::lower(double distance_m, double velocity_mps)
{
    Command c;
    const double v = velocity_mps > 0.0 ? velocity_mps : impl_->cfg.defaults.linear_velocity;
    const double d = distance_m > 0.0 ? distance_m : impl_->cfg.defaults.step_distance;
    c.linear_z = -std::abs(v);
    c.translation_z = -std::abs(d);
    c.label = "lower";
    return impl_->enqueue(std::move(c));
}

std::uint64_t ArmController::extend(double distance_m, double velocity_mps)
{
    Command c;
    const double v = velocity_mps > 0.0 ? velocity_mps : impl_->cfg.defaults.linear_velocity;
    const double d = distance_m > 0.0 ? distance_m : impl_->cfg.defaults.step_distance;
    c.linear_x = std::abs(v);
    c.translation_x = std::abs(d);
    c.label = "extend";
    return impl_->enqueue(std::move(c));
}

std::uint64_t ArmController::retract(double distance_m, double velocity_mps)
{
    Command c;
    const double v = velocity_mps > 0.0 ? velocity_mps : impl_->cfg.defaults.linear_velocity;
    const double d = distance_m > 0.0 ? distance_m : impl_->cfg.defaults.step_distance;
    c.linear_x = -std::abs(v);
    c.translation_x = -std::abs(d);
    c.label = "retract";
    return impl_->enqueue(std::move(c));
}

std::uint64_t ArmController::moveAtVelocity(double linear_x, double linear_y,
                                            double linear_z, double angular_x,
                                            double angular_y, double angular_z,
                                            int duration_ms)
{
    Command c;
    c.linear_x = linear_x;
    c.linear_y = linear_y;
    c.linear_z = linear_z;
    c.angular_x = angular_x;
    c.angular_y = angular_y;
    c.angular_z = angular_z;
    c.duration_ms = duration_ms;
    c.label = "velocity";
    return impl_->enqueue(std::move(c));
}

bool ArmController::adjustSpeed(double ratio)
{
    return impl_->adjustSpeedImpl(ratio);
}

std::uint64_t ArmController::stop()
{
    Command c;
    c.duration_ms = 200;
    c.label = "stop";
    return impl_->enqueue(std::move(c));
}

std::uint64_t ArmController::emergencyStop()
{
    Command c;
    c.is_emergency_stop = true;
    c.label = "emergency_stop";
    return impl_->enqueue(std::move(c));
}

State ArmController::state() const
{
    std::lock_guard<std::mutex> lk(impl_->state_mtx);
    return impl_->state;
}

void ArmController::setStateCallback(StateCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->state_cb = std::move(cb);
}

void ArmController::setCommandStatusCallback(CommandStatusCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->status_cb = std::move(cb);
}

const ArmConfig& ArmController::config() const { return impl_->cfg; }

ArmConfig ArmController::loadConfig(const std::string& config_path)
{
    ArmConfig cfg;
    if (!fileExists(config_path)) {
        return cfg;
    }
    toml::table table = toml::parse_file(config_path);
    const toml::node_view in = table["arm"];

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
    cfg.defaults.step_distance    = D["step_distance"].value_or(cfg.defaults.step_distance);
    cfg.defaults.rotation_angle   = D["rotation_angle"].value_or(cfg.defaults.rotation_angle);
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

}  // namespace arm
