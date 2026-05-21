#include "AudioManager.h"
#include "WakeupManager.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

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

bool envEnabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return false;
    }
    return std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0;
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

quill::Logger* testLogger()
{
    static quill::Logger* logger = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        quill::Backend::start();
        logger = quill::Frontend::create_or_get_logger(
            "WakeupManagerFunctionalTest",
            quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "wakeup_manager_functional_test_console_sink"));
    });
    return logger;
}

struct SkipTest : std::exception {
    explicit SkipTest(std::string why) : reason(std::move(why)) {}
    const char* what() const noexcept override { return reason.c_str(); }
    std::string reason;
};

class TestRunner {
public:
    explicit TestRunner(quill::Logger* logger) : logger_(logger) {}

    void run(const std::string& name, const std::string& content,
             const std::function<void()>& body)
    {
        LOG_INFO(logger_, "BEGIN TEST [{}] {}", name, content);
        logger_->flush_log();
        std::cout << "\n[TEST] " << name << " - " << content << "\n";

        const int failed_before = failed_;
        const int skipped_before = skipped_;
        try {
            body();
        } catch (const SkipTest& e) {
            ++skipped_;
            LOG_WARNING(logger_, "SKIP TEST [{}] {}", name, e.what());
            std::cout << "[SKIP] " << name << ": " << e.what() << "\n";
        } catch (const std::exception& e) {
            ++failed_;
            LOG_ERROR(logger_, "EXCEPTION TEST [{}] {}", name, e.what());
            std::cerr << "[FAIL] " << name << " threw: " << e.what() << "\n";
        } catch (...) {
            ++failed_;
            LOG_ERROR(logger_, "UNKNOWN EXCEPTION TEST [{}]", name);
            std::cerr << "[FAIL] " << name << " threw unknown exception\n";
        }

        const bool skipped = skipped_ > skipped_before;
        const bool failed = failed_ > failed_before;
        const char* result = skipped ? "SKIPPED" : (failed ? "FAILED" : "PASSED");
        LOG_INFO(logger_, "END TEST [{}] result={}", name, result);
        logger_->flush_log();
        std::cout << "[END] " << name << " -> " << result << "\n";
    }

    void expect(bool cond, const std::string& msg)
    {
        ++assertions_;
        if (!cond) {
            ++failed_;
            LOG_ERROR(logger_, "EXPECT FAILED: {}", msg);
            std::cerr << "[FAIL] " << msg << "\n";
        } else {
            LOG_INFO(logger_, "EXPECT PASSED: {}", msg);
            std::cout << "[PASS] " << msg << "\n";
        }
    }

    void skip(const std::string& reason)
    {
        throw SkipTest(reason);
    }

    int assertions() const { return assertions_; }
    int failed() const { return failed_; }
    int skipped() const { return skipped_; }

private:
    quill::Logger* logger_{nullptr};
    int assertions_{0};
    int failed_{0};
    int skipped_{0};
};

audio::Config withEnvOverrides(audio::Config cfg)
{
    if (const char* dev = std::getenv("AUDIO_TEST_DEVICE")) {
        if (*dev) {
            cfg.capture.device = dev;
            cfg.playback.device = dev;
        }
    }
    if (const char* dev = std::getenv("AUDIO_TEST_CAPTURE_DEVICE")) {
        if (*dev) {
            cfg.capture.device = dev;
        }
    }
    if (const char* dev = std::getenv("AUDIO_TEST_PLAYBACK_DEVICE")) {
        if (*dev) {
            cfg.playback.device = dev;
        }
    }
    if (envEnabled("AUDIO_TEST_USE_NULL") || envEnabled("WAKEUP_TEST_USE_NULL")) {
        cfg.capture.device = "null";
        cfg.playback.device = "null";
    }
    return cfg;
}

bool requiredWakeupFilesExist(const wakeup::WakeupConfig& cfg)
{
    if (!fileExists(cfg.kws.encoder) || !fileExists(cfg.kws.decoder) ||
        !fileExists(cfg.kws.joiner) || !fileExists(cfg.kws.tokens) ||
        !fileExists(cfg.kws.keywords_file)) {
        return false;
    }
    return !cfg.sv.enabled || fileExists(cfg.sv.model);
}

} // namespace

int main()
{
    quill::Logger* logger = testLogger();
    TestRunner runner(logger);

    const std::string cfg_path = findConfig();
    wakeup::WakeupConfig wake_cfg;

    runner.run("config_loading",
               "load TOML wakeup config and verify model paths",
               [&] {
        wake_cfg = wakeup::WakeupManager::loadConfig(cfg_path);

        runner.expect(!wake_cfg.keywords.empty(), "wakeup keywords are configured");
        runner.expect(wake_cfg.sample_rate > 0, "wakeup sample rate is valid");
        runner.expect(wake_cfg.frame_queue_depth > 0, "frame queue depth is valid");
        runner.expect(wake_cfg.cooldown_ms >= 0, "cooldown is non-negative");

        runner.expect(fileExists(wake_cfg.kws.encoder), "KWS encoder model exists");
        runner.expect(fileExists(wake_cfg.kws.decoder), "KWS decoder model exists");
        runner.expect(fileExists(wake_cfg.kws.joiner), "KWS joiner model exists");
        runner.expect(fileExists(wake_cfg.kws.tokens), "KWS tokens file exists");
        runner.expect(fileExists(wake_cfg.kws.keywords_file), "KWS keywords file exists");
        if (wake_cfg.sv.enabled) {
            runner.expect(fileExists(wake_cfg.sv.model), "SV model exists when enabled");
        }
    });

    runner.run("pre_init_guards",
               "verify WakeupManager APIs are safe before init",
               [&] {
        wakeup::WakeupManager wk;

        runner.expect(!wk.isListening(), "new WakeupManager is not listening");
        wk.startListening();
        runner.expect(!wk.isListening(), "startListening before init is ignored");

        wk.muteWakeup(true);
        runner.expect(wk.isMuted(), "muteWakeup(true) updates muted state before init");
        wk.muteWakeup(false);
        runner.expect(!wk.isMuted(), "muteWakeup(false) updates muted state before init");

        runner.expect(!wk.addVoiceprint("empty", {}), "empty voiceprint is rejected");
        wk.clearVoiceprints();
        wk.stopListening();
        wk.shutdown();
        runner.expect(!wk.isListening(), "shutdown before init leaves manager stopped");
    });

    runner.run("null_audio_rejected",
               "init rejects a null AudioManager pointer without touching KWS",
               [&] {
        wakeup::WakeupConfig cfg;
        cfg.logging.console = false;
        cfg.sv.enabled = false;

        wakeup::WakeupManager wk;
        runner.expect(!wk.init(nullptr, cfg), "init(nullptr, config) returns false");
        wk.startListening();
        runner.expect(!wk.isListening(), "null-audio init failure cannot start listening");
    });

    runner.run("lifecycle_without_capture",
               "initialize wakeup engine and exercise listening/mute lifecycle",
               [&] {
        if (!requiredWakeupFilesExist(wake_cfg)) {
            runner.skip("wakeup model files are not present");
        }

        audio::AudioManager am;
        wakeup::WakeupConfig cfg = wake_cfg;
        cfg.sv.enabled = false;
        cfg.mute_on_init = true;
        cfg.logging.log_file = "wakeup_manager_test.log";

        wakeup::WakeupManager wk;
        std::atomic<int> callback_count{0};

        const bool init_ok = wk.init(&am, cfg);
        runner.expect(init_ok, "WakeupManager::init succeeds with a valid AudioManager pointer");
        if (!init_ok) {
            return;
        }

        wk.setWakeCallback([&](const wakeup::WakeupEvent&) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });

        runner.expect(wk.config().sample_rate == cfg.sample_rate,
                      "config object is retained after init");
        runner.expect(wk.isMuted(), "mute_on_init is honored");
        runner.expect(wk.embeddingDim() == 0, "embedding dim is zero when SV is disabled");

        wk.startListening();
        runner.expect(wk.isListening(), "startListening enters listening state");
        wk.startListening();
        runner.expect(wk.isListening(), "startListening is idempotent");

        wk.muteWakeup(false);
        runner.expect(!wk.isMuted(), "muteWakeup(false) unmutes listening");
        wk.muteWakeup(true);
        runner.expect(wk.isMuted(), "muteWakeup(true) mutes listening");

        runner.expect(callback_count.load(std::memory_order_relaxed) == 0,
                      "no wake callback fires without audio frames");

        wk.stopListening();
        runner.expect(!wk.isListening(), "stopListening exits listening state");
        wk.shutdown();
        runner.expect(!wk.isListening(), "shutdown leaves manager stopped");
    });

    runner.run("audio_capture_integration",
               "run AudioManager capture followed by WakeupManager listening",
               [&] {
        audio::Config audio_cfg = withEnvOverrides(audio::loadConfig(cfg_path));

        audio::AudioManager am;
        if (!am.init(audio_cfg)) {
            runner.skip("AudioManager::init failed for configured audio devices");
        }
        if (!am.startCapture()) {
            runner.skip("AudioManager::startCapture failed for configured input device");
        }

        wakeup::WakeupManager wk;
        std::atomic<int> wake_events{0};

        const bool wake_init_ok = wk.init(&am, cfg_path);
        runner.expect(wake_init_ok, "WakeupManager::init succeeds after capture starts");
        if (!wake_init_ok) {
            am.stopCapture();
            am.shutdown();
            return;
        }

        wk.setWakeCallback([&](const wakeup::WakeupEvent& event) {
            if (!event.keyword.empty()) {
                wake_events.fetch_add(1, std::memory_order_relaxed);
            }
        });

        wk.startListening();
        runner.expect(wk.isListening(), "WakeupManager is listening on AudioManager frames");
        runner.expect(wk.isMuted() == wk.config().mute_on_init,
                      "initial muted state matches wakeup config");

        wk.muteWakeup(true);
        runner.expect(wk.isMuted(), "dialog mute turns wakeup off");
        std::this_thread::sleep_for(100ms);

        wk.muteWakeup(false);
        runner.expect(!wk.isMuted(), "dialog unmute turns wakeup back on");
        const int capture_ms = envInt("WAKEUP_TEST_CAPTURE_MS", 500, 0, 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(capture_ms));

        if (wk.config().sv.enabled) {
            const int dim = wk.embeddingDim();
            runner.expect(dim >= 0, "embedding dimension is readable");
            if (dim > 0) {
                runner.expect(!wk.addVoiceprint("bad_dim", {1.0f}),
                              "voiceprint with wrong dimension is rejected");

                std::vector<float> embedding(static_cast<std::size_t>(dim), 0.0f);
                embedding.front() = 1.0f;
                runner.expect(wk.addVoiceprint("test_user", embedding),
                              "voiceprint with matching dimension is accepted");
                wk.clearVoiceprints();
            }
        }

        LOG_INFO(logger, "Wakeup callbacks observed during integration test: {}",
                 wake_events.load(std::memory_order_relaxed));

        wk.stopListening();
        runner.expect(!wk.isListening(), "stopListening unregisters wakeup consumer");
        wk.shutdown();

        am.stopCapture();
        am.shutdown();
        runner.expect(!am.health().capture_running, "AudioManager capture stops cleanly");
    });

    LOG_INFO(logger, "WakeupManager tests finished assertions={} failed={} skipped={}",
             runner.assertions(), runner.failed(), runner.skipped());

    std::cout << "\nTests assertions: " << runner.assertions()
              << "  failed: " << runner.failed()
              << "  skipped: " << runner.skipped() << "\n";
    return runner.failed() == 0 ? 0 : 1;
}
