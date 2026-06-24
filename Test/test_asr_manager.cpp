#include "ASRManager.h"
#include "AudioManager.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cmath>
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
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string findConfig()
{
    const char* env = std::getenv("ASR_CONFIG");
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

const char* engineName(asr::EngineType e)
{
    return e == asr::EngineType::kOnline ? "online" : "offline";
}

struct LevelStats {
    std::uint64_t frames = 0;
    std::uint64_t samples = 0;
    double sum_squares = 0.0;
    int peak = 0;
    int sample_rate = 0;
    int channels = 0;
};

LevelStats consumeStats(LevelStats& stats, std::mutex& mutex)
{
    std::lock_guard<std::mutex> lock(mutex);
    LevelStats snapshot = stats;
    stats = {};
    return snapshot;
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    quill::Backend::start();
    (void)quill::Frontend::create_or_get_logger(
        "ASRPulseListener",
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "asr_pulse_listener_console_sink"));

    const std::string cfg_path = findConfig();
    const int listen_seconds = envInt("ASR_LISTEN_SECONDS", 0, 0, 24 * 60 * 60);
    const int report_seconds = envInt("ASR_REPORT_SECONDS", 5, 1, 3600);

    audio::Config audio_cfg = audio::loadConfig(cfg_path);
    audio_cfg.capture.device  = "pulse";
    audio_cfg.playback.device = "pulse";

    asr::AsrConfig asr_cfg = asr::ASRManager::loadConfig(cfg_path);
    // Honor the preferred_engine value from config.toml. Override only when an
    // explicit env var is set: ASR_FORCE_OFFLINE=1 / ASR_FORCE_ONLINE=1.
    if (std::getenv("ASR_FORCE_OFFLINE")) {
        asr_cfg.preferred_engine = asr::EngineType::kOffline;
    } else if (std::getenv("ASR_FORCE_ONLINE")) {
        asr_cfg.preferred_engine = asr::EngineType::kOnline;
    }

    std::cout << "ASR pulse listener\n"
              << "config: " << cfg_path << "\n"
              << "capture: pulse\n"
              << "preferred engine: " << engineName(asr_cfg.preferred_engine) << "\n"
              << "duration: "
              << (listen_seconds > 0 ? std::to_string(listen_seconds) + "s" : "forever")
              << "\n"
              << "report interval: " << report_seconds << "s"
              << "\n"
              << "Press Ctrl+C to stop. Initializing audio and ASR; wait for\n"
              << "\"Recognition started\" before speaking.\n\n";

    audio::AudioManager am;
    if (!am.init(audio_cfg)) {
        std::cerr << "AudioManager::init failed for pulse\n";
        return EXIT_FAILURE;
    }

    LevelStats level_stats;
    std::mutex level_mutex;
    audio::ConsumerHandle level_consumer = am.addFrameConsumer(
        [&](const audio::AudioFrame& frame) {
            if (frame.samples.empty()) {
                return;
            }

            double sum_squares = 0.0;
            int peak = 0;
            for (std::int16_t s : frame.samples) {
                const int v = std::abs(static_cast<int>(s));
                peak = std::max(peak, v);
                sum_squares += static_cast<double>(s) * static_cast<double>(s);
            }

            std::lock_guard<std::mutex> lock(level_mutex);
            level_stats.frames += 1;
            level_stats.samples += frame.samples.size();
            level_stats.sum_squares += sum_squares;
            level_stats.peak = std::max(level_stats.peak, peak);
            level_stats.sample_rate = frame.sample_rate;
            level_stats.channels = frame.channels;
        },
        50);

    asr::ASRManager asr_mgr;
    const auto asr_init_started = std::chrono::steady_clock::now();
    if (!asr_mgr.init(&am, asr_cfg)) {
        std::cerr << "ASRManager::init failed\n";
        if (level_consumer != audio::kInvalidConsumerHandle) {
            am.removeFrameConsumer(level_consumer);
        }
        am.shutdown();
        return EXIT_FAILURE;
    }
    const auto asr_init_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - asr_init_started);
    std::cout << "ASR initialized in " << asr_init_elapsed.count() << " ms.\n";

    std::atomic<int> partial_count{0};
    std::atomic<int> final_count{0};
    std::mutex cout_mutex;

    asr_mgr.setPartialCallback([&](const asr::ASRPartialResult& r) {
        partial_count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "  [partial " << engineName(r.engine) << "] " << r.text
                  << std::string(8, ' ') << "\r" << std::flush;
    });
    asr_mgr.setFinalCallback([&](const asr::ASRFinalResult& r) {
        const int n = final_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\n[FINAL #" << n << " " << engineName(r.engine)
                  << "] " << r.text << "\n";
    });
    asr_mgr.setErrorCallback([&](const asr::ASRError& e) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "\n[ERROR " << engineName(e.engine) << "] code="
                  << static_cast<int>(e.code) << " " << e.message << "\n";
    });

    if (!asr_mgr.startRecognition(asr::ASRMode::kStreaming)) {
        std::cerr << "ASRManager::startRecognition failed — "
                     "no engine available (check models / credentials)\n";
        asr_mgr.shutdown();
        if (level_consumer != audio::kInvalidConsumerHandle) {
            am.removeFrameConsumer(level_consumer);
        }
        am.shutdown();
        return EXIT_FAILURE;
    }

    if (!am.startCapture()) {
        std::cerr << "AudioManager::startCapture failed for pulse\n";
        asr_mgr.stopRecognition();
        asr_mgr.shutdown();
        if (level_consumer != audio::kInvalidConsumerHandle) {
            am.removeFrameConsumer(level_consumer);
        }
        am.shutdown();
        return EXIT_FAILURE;
    }

    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "Recognition started on the "
                  << engineName(asr_mgr.currentEngine()) << " engine.\n"
                  << "Speak now; partial results stream live.\n";
    }

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
            elapsed_seconds % report_seconds == 0 &&
            elapsed_seconds != last_reported_seconds) {
            last_reported_seconds = elapsed_seconds;
            const audio::HealthStatus health = am.health();
            const LevelStats levels = consumeStats(level_stats, level_mutex);
            const double rms = levels.samples > 0
                                   ? std::sqrt(levels.sum_squares /
                                               static_cast<double>(levels.samples)) / 32768.0
                                   : 0.0;
            const double peak = static_cast<double>(levels.peak) / 32768.0;
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "\n[LISTENING] elapsed=" << elapsed_seconds
                      << "s partials=" << partial_count.load(std::memory_order_relaxed)
                      << " finals=" << final_count.load(std::memory_order_relaxed)
                      << " capture_frames=" << health.capture_frames_total
                      << " capture_overruns=" << health.capture_overrun_total
                      << " capture_q=" << health.capture_queue_depth
                      << " asr_drops=" << health.asr_dropped_frames
                      << " audio_frames=" << levels.frames
                      << " audio_rate=" << levels.sample_rate
                      << " audio_ch=" << levels.channels
                      << " rms=" << rms
                      << " peak=" << peak << "\n";
        }
    }

    asr_mgr.stopRecognition();
    asr_mgr.shutdown();
    if (level_consumer != audio::kInvalidConsumerHandle) {
        am.removeFrameConsumer(level_consumer);
    }
    am.stopCapture();
    am.shutdown();

    std::cout << "\nStopped. partials="
              << partial_count.load(std::memory_order_relaxed)
              << " finals=" << final_count.load(std::memory_order_relaxed) << "\n";
    return EXIT_SUCCESS;
}
