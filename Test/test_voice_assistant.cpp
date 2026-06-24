#include "AudioManager.h"
#include "ArmController.h"
#include "GripperController.h"
#include "IntentManager.h"
#include "MusicPlayer.h"
#include "NavigationController.h"
#include "Orchestrator.h"
#include "SystemVolumeController.h"
#include "TTSManager.h"
#include "WakeupManager.h"
#include "WheelchairController.h"
#include "e2echat.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true, std::memory_order_release); }

#if defined(__unix__) || defined(__APPLE__)
void writeAll(int fd, const char* data, std::size_t len)
{
    while (len > 0) {
        const ssize_t written = ::write(fd, data, len);
        if (written <= 0) return;
        data += written;
        len -= static_cast<std::size_t>(written);
    }
}

void writeCString(int fd, const char* text)
{
    if (text) writeAll(fd, text, std::strlen(text));
}

void writeInt(int fd, int value)
{
    char buf[32] = {};
    int pos = static_cast<int>(sizeof(buf));
    bool negative = value < 0;
    unsigned int n = negative
        ? static_cast<unsigned int>(-static_cast<long long>(value))
        : static_cast<unsigned int>(value);
    do {
        buf[--pos] = static_cast<char>('0' + (n % 10));
        n /= 10;
    } while (n > 0 && pos > 0);
    if (negative && pos > 0) buf[--pos] = '-';
    writeAll(fd, buf + pos, sizeof(buf) - static_cast<std::size_t>(pos));
}

void writeFatalHeader(int fd, int sig)
{
    writeCString(fd, "\n[FATAL] test_voice_assistant caught signal ");
    writeInt(fd, sig);
    writeCString(fd, "\n");
}

void onFatalSignal(int sig)
{
    void* frames[64] = {};
    const int frame_count = ::backtrace(frames, 64);
    writeFatalHeader(STDERR_FILENO, sig);
    ::backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);

    const int fd = ::open("/tmp/test_voice_assistant_crash.log",
                          O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
        writeFatalHeader(fd, sig);
        ::backtrace_symbols_fd(frames, frame_count, fd);
        ::close(fd);
    }
    _exit(128 + sig);
}

void installFatalSignalHandlers()
{
    std::signal(SIGSEGV, onFatalSignal);
    std::signal(SIGABRT, onFatalSignal);
    std::signal(SIGBUS, onFatalSignal);
    std::signal(SIGILL, onFatalSignal);
    std::signal(SIGFPE, onFatalSignal);
}
#else
void installFatalSignalHandlers() {}
#endif

bool fileExists(const std::string& path)
{
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

std::string findConfig()
{
    if (const char* env = std::getenv("VOICE_ASSISTANT_CONFIG")) {
        if (*env && fileExists(env)) return env;
    }
    const char* candidates[] = {
        "Config/config.toml",
        "../Config/config.toml",
        "../../Config/config.toml",
        "../../../Config/config.toml",
    };
    for (const char* path : candidates) {
        if (fileExists(path)) return path;
    }
    return "Config/config.toml";
}

void applyAudioEnvOverrides(audio::Config& cfg)
{
    if (const char* dev = std::getenv("AUDIO_TEST_DEVICE")) {
        if (*dev) {
            cfg.capture.device = dev;
            cfg.playback.device = dev;
        }
    }
    if (const char* dev = std::getenv("AUDIO_TEST_CAPTURE_DEVICE")) {
        if (*dev) cfg.capture.device = dev;
    }
    if (const char* dev = std::getenv("AUDIO_TEST_PLAYBACK_DEVICE")) {
        if (*dev) cfg.playback.device = dev;
    }
}

struct Args {
    std::string config_path = findConfig();
    int seconds = 0;
};

Args parseArgs(int argc, char** argv)
{
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if ((arg == "--seconds" || arg == "-s") && i + 1 < argc) {
            args.seconds = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: test_voice_assistant [--config Config/config.toml] "
                << "[--seconds N]\n"
                << "  E2E orchestrator drives the full Wake → Doubao TTS greeting "
                << "→ E2E session → skill dispatch → ChatTTSText reply loop.\n"
                << "  All ASR/NLU/Chat/TTS work runs in the Doubao realtime "
                << "session; no local fallback paths are used.\n";
            std::exit(0);
        }
    }
    return args;
}

}  // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);
    installFatalSignalHandlers();

    const Args args = parseArgs(argc, argv);

    // ------------------------------------------------------------------
    // Audio + Wakeup + TTS — required dependencies.
    // ------------------------------------------------------------------
    audio::Config audio_config = audio::loadConfig(args.config_path);
    applyAudioEnvOverrides(audio_config);

    audio::AudioManager audio_manager;
    if (!audio_manager.init(audio_config)) {
        std::cerr << "AudioManager init failed\n";
        return 1;
    }
    if (!audio_manager.startCapture()) {
        std::cerr << "Audio capture start failed\n";
        audio_manager.shutdown();
        return 1;
    }

    tts::TTSManager tts_manager;
    if (!tts_manager.init(&audio_manager, args.config_path)) {
        std::cerr << "TTSManager init failed (greeting + fallback unavailable)\n";
    }

    wakeup::WakeupConfig wake_config = wakeup::WakeupManager::loadConfig(args.config_path);
    // Greetings are now driven by the Orchestrator via TTSManager, so the
    // legacy in-WakeupManager TTS-ack hook stays disabled.
    wake_config.tts_ack.enabled = false;

    wakeup::WakeupManager wakeup_manager;
    if (!wakeup_manager.init(&audio_manager, wake_config)) {
        std::cerr << "WakeupManager init failed\n";
        tts_manager.shutdown();
        audio_manager.stopCapture();
        audio_manager.shutdown();
        return 1;
    }

    // ------------------------------------------------------------------
    // Skills — best-effort init.  Each null pointer is acceptable and
    // the matching intent will be a no-op.
    // ------------------------------------------------------------------
    wheelchair::WheelchairConfig wheelchair_config =
        wheelchair::WheelchairController::loadConfig(args.config_path);
    wheelchair::WheelchairController wheelchair_ctrl;
    if (!wheelchair_ctrl.init(wheelchair_config)) {
        std::cerr << "[WARN] WheelchairController init failed\n";
    }

    arm::ArmConfig arm_config = arm::ArmController::loadConfig(args.config_path);
    arm::ArmController arm_ctrl;
    if (!arm_ctrl.init(arm_config)) {
        std::cerr << "[WARN] ArmController init failed\n";
    }

    gripper::GripperConfig gripper_config =
        gripper::GripperController::loadConfig(args.config_path);
    gripper::GripperController gripper_ctrl;
    if (!gripper_ctrl.init(gripper_config)) {
        std::cerr << "[WARN] GripperController init failed\n";
    }

    system_volume::SystemVolumeController volume_controller;
    if (!volume_controller.init()) {
        std::cerr << "[WARN] SystemVolumeController init failed: "
                  << volume_controller.lastError() << "\n";
    }

    music::MusicConfig music_config =
        music::MusicPlayer::loadConfig(args.config_path);
    music::MusicPlayer music_player;
    const bool music_ok =
        music_config.enabled && music_player.init(&audio_manager, music_config);
    if (!music_ok) {
        std::cerr << "[WARN] MusicPlayer disabled or init failed\n";
    }

    navigation::NavigationConfig navigation_config =
        navigation::NavigationController::loadConfig(args.config_path);
    navigation::NavigationController navigation_ctrl;
    if (!navigation_ctrl.init(navigation_config)) {
        std::cerr << "[WARN] NavigationController init failed\n";
    }

    intent::IntentConfig intent_config =
        intent::IntentManager::loadConfig(args.config_path);
    intent::IntentManager intent_manager;
    if (!intent_manager.init(intent_config)) {
        std::cerr << "[WARN] IntentManager init failed; "
                  << "ASR text will go to fallback chat path\n";
    }

    // ------------------------------------------------------------------
    // E2EChat
    // ------------------------------------------------------------------
    e2echat::E2EChatConfig chat_config =
        e2echat::E2EChat::loadConfig(args.config_path);
    chat_config.send_opening_line = false;
    e2echat::E2EChat chat;
    if (!chat.init(chat_config)) {
        std::cerr << "E2EChat init failed\n";
        intent_manager.shutdown();
        navigation_ctrl.shutdown();
        gripper_ctrl.shutdown();
        arm_ctrl.shutdown();
        wheelchair_ctrl.shutdown();
        wakeup_manager.shutdown();
        tts_manager.shutdown();
        music_player.shutdown();
        audio_manager.stopCapture();
        audio_manager.shutdown();
        return 1;
    }

    // ------------------------------------------------------------------
    // Orchestrator
    // ------------------------------------------------------------------
    orchestrator::OrchestratorConfig orch_cfg =
        orchestrator::OrchestratorConfig::load(args.config_path);

    if (!orch_cfg.enabled) {
        std::cerr << "[Orchestrator] disabled in config; nothing to run.\n";
        chat.shutdown();
        intent_manager.shutdown();
        navigation_ctrl.shutdown();
        gripper_ctrl.shutdown();
        arm_ctrl.shutdown();
        wheelchair_ctrl.shutdown();
        wakeup_manager.shutdown();
        tts_manager.shutdown();
        music_player.shutdown();
        audio_manager.stopCapture();
        audio_manager.shutdown();
        return 0;
    }

    orchestrator::OrchestratorDeps deps;
    deps.audio      = &audio_manager;
    deps.wakeup     = &wakeup_manager;
    deps.tts        = &tts_manager;
    deps.chat       = &chat;
    deps.intent     = &intent_manager;
    deps.wheelchair = &wheelchair_ctrl;
    deps.arm        = &arm_ctrl;
    deps.gripper    = &gripper_ctrl;
    deps.navigation = &navigation_ctrl;
    deps.music      = music_ok ? &music_player : nullptr;
    deps.volume     = &volume_controller;

    orchestrator::Orchestrator orch;
    if (!orch.init(deps, orch_cfg)) {
        std::cerr << "Orchestrator init failed\n";
        chat.shutdown();
        intent_manager.shutdown();
        navigation_ctrl.shutdown();
        gripper_ctrl.shutdown();
        arm_ctrl.shutdown();
        wheelchair_ctrl.shutdown();
        wakeup_manager.shutdown();
        tts_manager.shutdown();
        music_player.shutdown();
        audio_manager.stopCapture();
        audio_manager.shutdown();
        return 1;
    }

    wakeup_manager.startListening();
    wakeup_manager.muteWakeup(false);

    std::cout << "E2E voice assistant started.\n"
              << "Flow: wake -> Doubao TTS greeting -> E2E ASR session "
                 "-> DeepSeek intent -> skill dispatch + TTS reply.\n"
              << "Silence " << orch_cfg.silence_timeout_ms
              << "ms ends the session; new wake preempts it.\n"
              << "Press Ctrl+C to stop.\n";

    // Optional run-duration cap (mostly for CI / smoke runs).
    std::thread duration_thread;
    if (args.seconds > 0) {
        duration_thread = std::thread([&] {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);
            while (!g_stop.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(100ms);
            }
            g_stop.store(true, std::memory_order_release);
        });
    }

    orch.run(&g_stop);

    // ------------------------------------------------------------------
    // Shutdown
    // ------------------------------------------------------------------
    if (duration_thread.joinable()) duration_thread.join();
    orch.shutdown();
    chat.shutdown();
    intent_manager.shutdown();
    wakeup_manager.stopListening();
    wakeup_manager.shutdown();
    tts_manager.shutdown();
    music_player.shutdown();
    navigation_ctrl.shutdown();
    gripper_ctrl.shutdown();
    arm_ctrl.shutdown();
    wheelchair_ctrl.shutdown();
    audio_manager.stopCapture();
    audio_manager.shutdown();
    return 0;
}
