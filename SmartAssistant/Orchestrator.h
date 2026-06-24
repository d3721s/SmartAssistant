#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio        { class AudioManager; }
namespace wakeup       { class WakeupManager; struct WakeupTtsAckConfig; }
namespace tts          { class TTSManager; }
namespace e2echat      { class E2EChat; struct ChatEvent; struct AudioChunk; }
namespace intent       { class IntentManager; }
namespace wheelchair   { class WheelchairController; }
namespace arm          { class ArmController; }
namespace gripper      { class GripperController; }
namespace navigation   { class NavigationController; }
namespace music        { class MusicPlayer; }
namespace system_volume{ class SystemVolumeController; }

namespace orchestrator {

// ============================================================================
// Configuration parsed from [e2e_orchestrator] + reused phrase pools.
// ============================================================================

struct OrchestratorConfig {
    bool        enabled               = true;
    int         silence_timeout_ms    = 10000;
    int         chat_tts_max_failures = 2;
    bool        greeting_before_session = true;

    // Phrase pools (loaded from [voice_assistant.*].phrases and
    // [wakeup.tts_ack].phrases).
    std::vector<std::string> startup_phrases;
    std::vector<std::string> wake_ack_phrases;
    std::vector<std::string> no_speech_phrases;
    std::vector<std::string> exit_reply_phrases;
    std::vector<std::string> e2e_failure_phrases;
    // Played when DeepSeek classifies the user input as `unknown`.  Loaded
    // from [intent.unknown_reply].phrases.
    std::vector<std::string> unknown_reply_phrases;

    // Loaded from [voice_assistant.startup].enabled (UI flag).
    bool        startup_enabled       = true;

    static OrchestratorConfig load(const std::string& config_path);
};

// ============================================================================
// Dependencies — caller owns lifetimes; Orchestrator only borrows.
// Any controller pointer may be nullptr if the corresponding hardware was
// not initialised; the matching intent will be acknowledged but no-op.
// ============================================================================

struct OrchestratorDeps {
    audio::AudioManager*                   audio       = nullptr;
    wakeup::WakeupManager*                 wakeup      = nullptr;
    tts::TTSManager*                       tts         = nullptr;
    e2echat::E2EChat*                      chat        = nullptr;
    intent::IntentManager*                 intent      = nullptr;

    wheelchair::WheelchairController*      wheelchair  = nullptr;
    arm::ArmController*                    arm         = nullptr;
    gripper::GripperController*            gripper     = nullptr;
    navigation::NavigationController*      navigation  = nullptr;
    music::MusicPlayer*                    music       = nullptr;
    system_volume::SystemVolumeController* volume      = nullptr;
};

// ============================================================================
// Orchestrator: drives the wake → e2e session → skill dispatch → reply loop.
//
// Lifecycle:
//   init(deps, cfg)      // wires callbacks, registers wake handler
//   run()                // blocks until stop() or external SIGINT flag
//   stop()               // unblocks run(); idempotent
//   shutdown()           // unwires callbacks; safe to call after run() exits
// ============================================================================

class Orchestrator {
public:
    Orchestrator();
    ~Orchestrator();

    Orchestrator(const Orchestrator&)            = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    bool init(const OrchestratorDeps& deps,
              const OrchestratorConfig& cfg,
              const wakeup::WakeupTtsAckConfig* wake_ack_cfg = nullptr);

    // Block until stop() is called or the external `stop_flag` becomes true.
    // Pass nullptr if you only want stop() control.
    void run(std::atomic<bool>* stop_flag = nullptr);

    void stop();
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace orchestrator
