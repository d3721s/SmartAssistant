#include "AudioManager.h"
#include "WakeupManager.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <atomic>
#include <chrono>
#include <csignal>
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
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string findConfig()
{
    const char* env = std::getenv("WAKEUP_CONFIG");
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

int envInt(const char* name, int fallback, int min_value, int max_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < min_value || parsed > max_value) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

void printWakeEvent(const wakeup::WakeupEvent& event, int count)
{
    std::cout << "\n[WAKEUP] #" << count
              << " keyword=\"" << event.keyword << "\""
              << " kws_confidence=" << event.kws_confidence
              << " sv_enabled=" << (event.sv_enabled ? "true" : "false")
              << " sv_passed=" << (event.sv_passed ? "true" : "false")
              << " sv_score=" << event.sv_score;
    if (!event.user_id.empty()) {
        std::cout << " user_id=\"" << event.user_id << "\"";
    }
    std::cout << " embedding_dim=" << event.embedding.size()
              << " timestamp_us=" << event.timestamp_us << "\n";
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    quill::Backend::start();
    (void)quill::Frontend::create_or_get_logger(
        "WakeupPulseListener",
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "wakeup_pulse_listener_console_sink"));

    const std::string cfg_path = findConfig();
    const int listen_seconds = envInt("WAKEUP_LISTEN_SECONDS", 0, 0, 24 * 60 * 60);

    audio::Config audio_cfg = audio::loadConfig(cfg_path);
    audio_cfg.capture.device = "pulse";
    audio_cfg.playback.device = "pulse";

    std::cout << "Wakeup pulse listener\n"
              << "config: " << cfg_path << "\n"
              << "capture: pulse\n"
              << "playback: pulse\n"
              << "duration: "
              << (listen_seconds > 0 ? std::to_string(listen_seconds) + "s" : "forever")
              << "\n"
              << "Press Ctrl+C to stop. Speak the configured wake word now.\n";

    audio::AudioManager am;
    if (!am.init(audio_cfg)) {
        std::cerr << "AudioManager::init failed for pulse\n";
        return EXIT_FAILURE;
    }

    if (!am.startCapture()) {
        std::cerr << "AudioManager::startCapture failed for pulse\n";
        am.shutdown();
        return EXIT_FAILURE;
    }

    wakeup::WakeupManager wk;
    if (!wk.init(&am, cfg_path)) {
        std::cerr << "WakeupManager::init failed\n";
        am.stopCapture();
        am.shutdown();
        return EXIT_FAILURE;
    }

    std::atomic<int> wake_count{0};
    std::mutex cout_mutex;

    wk.setWakeCallback([&](const wakeup::WakeupEvent& event) {
        const int count = wake_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> lock(cout_mutex);
        printWakeEvent(event, count);
    });

    wk.startListening();
    wk.muteWakeup(false);

    const auto started = std::chrono::steady_clock::now();
    int last_reported_seconds = -1;

    while (!g_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(200ms);

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started);
        const int elapsed_seconds = static_cast<int>(elapsed.count());

        if (listen_seconds > 0 && elapsed_seconds >= listen_seconds) {
            break;
        }

        if (elapsed_seconds > 0 &&
            elapsed_seconds % 10 == 0 &&
            elapsed_seconds != last_reported_seconds) {
            last_reported_seconds = elapsed_seconds;
            const audio::HealthStatus health = am.health();
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "[LISTENING] elapsed=" << elapsed_seconds
                      << "s wake_events=" << wake_count.load(std::memory_order_relaxed)
                      << " capture_frames=" << health.capture_frames_total
                      << " muted=" << (wk.isMuted() ? "true" : "false")
                      << "\n";
        }
    }

    wk.stopListening();
    wk.shutdown();
    am.stopCapture();
    am.shutdown();

    std::cout << "Stopped. wake_events="
              << wake_count.load(std::memory_order_relaxed) << "\n";
    return EXIT_SUCCESS;
}
