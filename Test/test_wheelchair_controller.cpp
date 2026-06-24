#include "WheelchairController.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int)
{
    g_stop.store(true, std::memory_order_release);
}

bool fileExists(const std::string& path)
{
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string findConfig()
{
    const char* env = std::getenv("WHEELCHAIR_CONFIG");
    if (env && *env && fileExists(env)) {
        return env;
    }
    const char* candidates[] = {
        "Config/config.toml",
        "../Config/config.toml",
        "../../Config/config.toml",
        "../../../Config/config.toml",
    };
    for (const char* path : candidates) {
        if (fileExists(path)) {
            return path;
        }
    }
    return "Config/config.toml";
}

const char* statusName(wheelchair::CommandStatus s)
{
    switch (s) {
        case wheelchair::CommandStatus::kAccepted:  return "accepted";
        case wheelchair::CommandStatus::kRejected:  return "rejected";
        case wheelchair::CommandStatus::kCompleted: return "completed";
        case wheelchair::CommandStatus::kPreempted: return "preempted";
        case wheelchair::CommandStatus::kAborted:   return "aborted";
    }
    return "?";
}

// Wait until either: predicate becomes true, deadline elapses, or g_stop fires.
template <typename Pred>
bool waitFor(Pred&& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!g_stop.load(std::memory_order_acquire)) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    quill::Backend::start();
    (void)quill::Frontend::create_or_get_logger(
        "WheelchairTest",
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "wheelchair_test_console_sink"));

    const std::string cfg_path = findConfig();
    auto cfg = wheelchair::WheelchairController::loadConfig(cfg_path);

    std::cout << "Wheelchair test driver\n"
              << "config: " << cfg_path << "\n"
              << "ros2:   enabled=" << (cfg.ros2.enabled ? "true" : "false")
              << " topic=" << cfg.ros2.cmd_vel_topic
              << " hz="    << cfg.ros2.publish_rate_hz << "\n"
              << "limits: max_lin=" << cfg.limits.max_linear_velocity
              << " m/s, max_ang="   << cfg.limits.max_angular_velocity
              << " rad/s, accel_lin=" << cfg.limits.max_linear_accel
              << " m/s^2\n\n";

    wheelchair::WheelchairController ctrl;
    if (!ctrl.init(cfg)) {
        std::cerr << "WheelchairController::init failed\n";
        return EXIT_FAILURE;
    }

    std::atomic<std::uint64_t> last_completed_id{0};
    std::mutex                 cout_mtx;

    ctrl.setCommandStatusCallback(
        [&](const wheelchair::Command& c, wheelchair::CommandStatus s) {
            std::lock_guard<std::mutex> lk(cout_mtx);
            std::cout << "  [STATUS] cmd #" << c.id
                      << " (" << (c.label.empty() ? std::string("?") : c.label)
                      << ")  → " << statusName(s) << "\n";
            if (s == wheelchair::CommandStatus::kCompleted ||
                s == wheelchair::CommandStatus::kAborted) {
                last_completed_id.store(c.id, std::memory_order_release);
            }
        });

    // Throttle state logging.  We rate-limit prints to ~10 Hz so the console
    // doesn't get drowned at the 50 Hz publish rate.
    std::atomic<std::int64_t> last_state_print_us{0};
    ctrl.setStateCallback([&](const wheelchair::State& st) {
        const auto now_us = st.timestamp_us;
        const auto last   = last_state_print_us.load(std::memory_order_relaxed);
        if (now_us - last < 100000) return;
        last_state_print_us.store(now_us, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(cout_mtx);
        std::cout << "    state: v_lin=" << st.linear_velocity
                  << " v_ang=" << st.angular_velocity
                  << " trav_d=" << st.traveled_distance
                  << " trav_r=" << st.traveled_rotation
                  << (st.is_emergency ? " [E-STOP]" : "")
                  << "\n";
    });

    auto runAndWait = [&](std::uint64_t id, const std::string& label,
                          std::chrono::milliseconds budget) {
        if (id == 0) {
            std::cout << "  [enqueue failed]\n";
            return;
        }
        std::cout << "→ " << label << " (cmd #" << id << ")\n";
        const bool finished = waitFor(
            [&] { return last_completed_id.load(std::memory_order_acquire) >= id; },
            budget);
        if (!finished) {
            std::cout << "  [TIMEOUT after " << budget.count() << " ms]\n";
        }
    };

    // ====== Test sequence =====================================================
    std::cout << "==== sequence 1: forward 1 m ====\n";
    runAndWait(ctrl.moveForward(1.0), "forward 1.0 m", 12000ms);
    if (g_stop.load()) goto done;

    std::cout << "\n==== sequence 2: turn left 90 deg ====\n";
    runAndWait(ctrl.turnLeft(/*angle_rad=*/1.5707963),
               "turn left 90 deg", 6000ms);
    if (g_stop.load()) goto done;

    std::cout << "\n==== sequence 3: backward 0.5 m at 0.2 m/s ====\n";
    runAndWait(ctrl.moveBackward(0.5, 0.2),
               "backward 0.5 m @ 0.2 m/s", 8000ms);
    if (g_stop.load()) goto done;

    std::cout << "\n==== sequence 4: velocity mode for 1.5 s ====\n";
    runAndWait(ctrl.moveAtVelocity(0.2, 0.0, /*duration_ms=*/1500),
               "velocity (0.2 m/s, 0 rad/s, 1500 ms)", 4000ms);
    if (g_stop.load()) goto done;

    std::cout << "\n==== sequence 5: emergency stop during forward 5 m ====\n";
    {
        const auto big_id = ctrl.moveForward(5.0);
        std::cout << "→ moveForward 5 m (cmd #" << big_id << ")\n";
        std::this_thread::sleep_for(1500ms);
        std::cout << "→ emergencyStop()\n";
        ctrl.emergencyStop();
        waitFor(
            [&] { return last_completed_id.load(std::memory_order_acquire) >= big_id + 1; },
            4000ms);
    }
    if (g_stop.load()) goto done;

    std::cout << "\n==== sequence 6: adjustSpeed during cruise ====\n";
    {
        const auto vid = ctrl.moveAtVelocity(0.2, 0.0, /*duration_ms=*/3000);
        std::cout << "→ velocity cruise 0.2 m/s for 3 s (cmd #" << vid << ")\n";
        std::this_thread::sleep_for(700ms);
        std::cout << "→ adjustSpeed(+0.5)  (target +50%)\n";
        const bool ok1 = ctrl.adjustSpeed(0.5);
        std::cout << "   accepted: " << (ok1 ? "yes" : "no") << "\n";
        std::this_thread::sleep_for(900ms);
        std::cout << "→ adjustSpeed(-0.7)  (target -70%)\n";
        const bool ok2 = ctrl.adjustSpeed(-0.7);
        std::cout << "   accepted: " << (ok2 ? "yes" : "no") << "\n";
        waitFor(
            [&] { return last_completed_id.load(std::memory_order_acquire) >= vid; },
            5000ms);
    }

done:
    std::cout << "\nShutting down...\n";
    ctrl.shutdown();
    std::cout << "Done.\n";
    return EXIT_SUCCESS;
}
