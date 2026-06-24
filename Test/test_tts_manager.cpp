#include "AudioManager.h"
#include "TTSManager.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

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

std::string envString(const char* name, std::string fallback = {})
{
    const char* value = std::getenv(name);
    if (value && *value) {
        return value;
    }
    return fallback;
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

float envFloat(const char* name, float fallback, float min_value, float max_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end == value || parsed < min_value || parsed > max_value) {
        return fallback;
    }
    return parsed;
}

std::string findConfig()
{
    const std::string env = envString("TTS_CONFIG");
    if (!env.empty() && fileExists(env)) {
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

const char* engineName(tts::EngineType e)
{
    return e == tts::EngineType::kOnline ? "online" : "offline";
}

std::string join(const std::vector<std::string>& parts)
{
    std::ostringstream oss;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << parts[i];
    }
    return oss.str();
}

void printUsage(const char* exe)
{
    std::cout
        << "Usage:\n"
        << "  " << exe << " --text \"你好，我是智能语音助手。\" [options]\n"
        << "  " << exe << " \"你好，我是智能语音助手。\"\n\n"
        << "Options:\n"
        << "  --config PATH          TOML config path (default: Config/config.toml or TTS_CONFIG)\n"
        << "  --text TEXT            Text to synthesize\n"
        << "  --voice VOICE          Doubao speaker id / offline Kokoro sid (number)\n"
        << "  --language LANG        Language, default zh-CN\n"
        << "  --speed VALUE          Speed [0.5, 2.0], default 1.0\n"
        << "  --volume VALUE         Relative volume [0.0, 1.0], default 1.0\n"
        << "  --online               Force online TTS only\n"
        << "  --offline              Force offline TTS only\n"
        << "  --api-key KEY          Set Doubao X-Api-Key for this run\n"
        << "  --resource-id ID       Doubao resource id, default seed-tts-2.0\n"
        << "  --endpoint URL         Doubao V3 bidirectional endpoint\n"
        << "  --device NAME          Set both capture/playback audio devices\n"
        << "  --playback-device NAME Set playback device only\n"
        << "  --capture-device NAME  Set capture device only\n"
        << "  --null-audio           Use null audio devices\n"
        << "  --wait-ms MS           Max wait for synthesis+playback, default 120000\n";
}

struct Args {
    std::string config_path = findConfig();
    std::string text = envString("TTS_TEXT", "你好，我是智能语音助手。");
    std::string voice = envString("TTS_VOICE");
    std::string language = envString("TTS_LANGUAGE", "zh-CN");
    std::string api_key = envString("TTS_API_KEY");
    std::string resource_id = envString("TTS_RESOURCE_ID");
    std::string endpoint = envString("TTS_ENDPOINT");
    std::string device = envString("TTS_AUDIO_DEVICE");
    std::string playback_device = envString("TTS_PLAYBACK_DEVICE");
    std::string capture_device = envString("TTS_CAPTURE_DEVICE");
    float speed = envFloat("TTS_SPEED", 1.0f, 0.5f, 2.0f);
    float volume = envFloat("TTS_VOLUME", 1.0f, 0.0f, 1.0f);
    int wait_ms = envInt("TTS_TEST_WAIT_MS", 120000, 1000, 30 * 60 * 1000);
    bool force_online = envEnabled("TTS_FORCE_ONLINE");
    bool force_offline = envEnabled("TTS_FORCE_OFFLINE");
    bool null_audio = envEnabled("TTS_TEST_USE_NULL");
};

bool parseArgs(int argc, char** argv, Args& args)
{
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        } else if (arg == "--config") {
            args.config_path = requireValue("--config");
        } else if (arg == "--text") {
            args.text = requireValue("--text");
        } else if (arg == "--voice") {
            args.voice = requireValue("--voice");
        } else if (arg == "--language") {
            args.language = requireValue("--language");
        } else if (arg == "--speed") {
            args.speed = std::strtof(requireValue("--speed").c_str(), nullptr);
        } else if (arg == "--volume") {
            args.volume = std::strtof(requireValue("--volume").c_str(), nullptr);
        } else if (arg == "--online") {
            args.force_online = true;
        } else if (arg == "--offline") {
            args.force_offline = true;
        } else if (arg == "--api-key") {
            args.api_key = requireValue("--api-key");
        } else if (arg == "--resource-id") {
            args.resource_id = requireValue("--resource-id");
        } else if (arg == "--endpoint") {
            args.endpoint = requireValue("--endpoint");
        } else if (arg == "--device") {
            args.device = requireValue("--device");
        } else if (arg == "--playback-device") {
            args.playback_device = requireValue("--playback-device");
        } else if (arg == "--capture-device") {
            args.capture_device = requireValue("--capture-device");
        } else if (arg == "--null-audio") {
            args.null_audio = true;
        } else if (arg == "--wait-ms") {
            args.wait_ms = std::strtol(requireValue("--wait-ms").c_str(), nullptr, 10);
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option: " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (!positional.empty()) {
        args.text = join(positional);
    }
    if (args.force_online && args.force_offline) {
        throw std::runtime_error("--online and --offline cannot be used together");
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Args args;
    try {
        if (!parseArgs(argc, argv, args)) {
            return EXIT_SUCCESS;
        }
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n\n";
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    quill::Backend::start();
    (void)quill::Frontend::create_or_get_logger(
        "TTSManagerFunctionalTest",
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "tts_manager_functional_test_console_sink"));

    audio::Config audio_cfg;
    tts::TtsConfig tts_cfg;
    try {
        audio_cfg = audio::loadConfig(args.config_path);
        tts_cfg = tts::TTSManager::loadConfig(args.config_path);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load config '" << args.config_path << "': "
                  << e.what() << "\n";
        return EXIT_FAILURE;
    }

    if (args.null_audio) {
        audio_cfg.capture.device = "null";
        audio_cfg.playback.device = "null";
    }
    if (!args.device.empty()) {
        audio_cfg.capture.device = args.device;
        audio_cfg.playback.device = args.device;
    }
    if (!args.capture_device.empty()) {
        audio_cfg.capture.device = args.capture_device;
    }
    if (!args.playback_device.empty()) {
        audio_cfg.playback.device = args.playback_device;
    }

    if (args.force_online) {
        tts_cfg.preferred_engine = tts::EngineType::kOnline;
        tts_cfg.offline.enabled = false;
    } else if (args.force_offline) {
        tts_cfg.preferred_engine = tts::EngineType::kOffline;
        tts_cfg.online.enabled = false;
    }
    if (!args.api_key.empty()) {
        tts_cfg.online.api_key = args.api_key;
    }
    if (!args.resource_id.empty()) {
        tts_cfg.online.resource_id = args.resource_id;
    }
    if (!args.endpoint.empty()) {
        tts_cfg.online.endpoint = args.endpoint;
    }

    tts::TTSOptions opts;
    opts.voice = args.voice.empty() ? tts_cfg.online.default_voice : args.voice;
    opts.language = args.language;
    opts.speed = args.speed;
    opts.volume = args.volume;

    std::cout << "TTS manager functional test\n"
              << "config: " << args.config_path << "\n"
              << "playback device: " << audio_cfg.playback.device << "\n"
              << "capture device: " << audio_cfg.capture.device << "\n"
              << "preferred engine: " << engineName(tts_cfg.preferred_engine) << "\n"
              << "voice: " << opts.voice << "\n"
              << "language: " << opts.language << "\n"
              << "speed: " << opts.speed << "\n"
              << "volume: " << opts.volume << "\n"
              << "text: " << args.text << "\n\n";

    audio::AudioManager audio_mgr;
    if (!audio_mgr.init(audio_cfg)) {
        std::cerr << "AudioManager::init failed\n";
        return EXIT_FAILURE;
    }

    tts::TTSManager tts_mgr;
    if (!tts_mgr.init(&audio_mgr, tts_cfg)) {
        std::cerr << "TTSManager::init failed\n";
        audio_mgr.shutdown();
        return EXIT_FAILURE;
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    tts::TTSResult final_result;

    if (!tts_mgr.synthesizeAndPlay(args.text, opts, [&](const tts::TTSResult& result) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                final_result = result;
                done = true;
            }
            cv.notify_all();
        })) {
        std::cerr << "TTSManager::synthesizeAndPlay failed to start\n";
        tts_mgr.shutdown();
        audio_mgr.shutdown();
        return EXIT_FAILURE;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(args.wait_ms);
    while (!g_stop.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(mutex);
        if (cv.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + 200ms),
                          [&] { return done; })) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    if (!done) {
        std::cerr << "TTS timed out or interrupted; stopping\n";
        tts_mgr.stop();
        tts_mgr.shutdown();
        audio_mgr.shutdown();
        return EXIT_FAILURE;
    }

    tts_mgr.shutdown();
    audio_mgr.shutdown();

    std::cout << "TTS result: success=" << (final_result.success ? "true" : "false")
              << " engine=" << engineName(final_result.engine)
              << " code=" << static_cast<int>(final_result.code)
              << " playback_handle=" << final_result.playback_handle
              << " message=" << final_result.message << "\n";

    return final_result.success ? EXIT_SUCCESS : EXIT_FAILURE;
}
