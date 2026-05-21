#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace audio {
class AudioManager;
}

namespace wakeup {

// ============================================================================
// Configuration (parsed from TOML [wakeup] section)
// ============================================================================

struct WakeupKwsConfig {
    std::string encoder;
    std::string decoder;
    std::string joiner;
    std::string tokens;
    std::string keywords_file;
    std::string provider           = "cpu";
    int         num_threads        = 1;
    int         max_active_paths   = 4;
    float       keywords_score     = 1.5f;
    float       keywords_threshold = 0.25f;
    int         num_trailing_blanks = 1;
    int         feature_dim        = 80;
    bool        debug              = false;
};

struct WakeupSvConfig {
    bool        enabled                = true;
    std::string model;
    std::string provider               = "cpu";
    int         num_threads            = 1;
    float       threshold              = 0.55f;
    int         sample_rate            = 16000;
    int         analyze_window_ms      = 1500;
    bool        require_enrolled_user  = false;
    bool        debug                  = false;
};

struct WakeupLoggingConfig {
    std::string log_dir            = "logs";
    std::string log_file           = "wakeup.log";
    bool        console            = true;
    std::size_t rotation_max_bytes = 10 * 1024 * 1024;
    int         rotation_max_files = 7;
};

struct WakeupConfig {
    std::vector<std::string> keywords{"你好弈宝", "弈宝弈宝"};
    int                      sample_rate         = 16000;
    std::size_t              frame_queue_depth   = 50;
    bool                     mute_on_init        = false;
    int                      cooldown_ms         = 1500;
    WakeupKwsConfig          kws;
    WakeupSvConfig           sv;
    WakeupLoggingConfig      logging;
};

// ============================================================================
// Public events
// ============================================================================

struct WakeupEvent {
    std::string keyword;             // matched keyword text (e.g. "你好弈宝")
    float       kws_confidence  = 0.0f;
    bool        sv_enabled      = false;
    bool        sv_passed       = true;
    float       sv_score        = 0.0f;
    std::string user_id;             // empty when SV disabled / no enrolled match
    std::vector<float> embedding;    // raw speaker embedding (may be empty)
    int64_t     timestamp_us    = 0;
};

using WakeCallback = std::function<void(const WakeupEvent&)>;

// ============================================================================
// WakeupManager (KWS + SV)
// ============================================================================

class WakeupManager {
public:
    WakeupManager();
    ~WakeupManager();

    WakeupManager(const WakeupManager&)            = delete;
    WakeupManager& operator=(const WakeupManager&) = delete;

    // Two-phase init: caller supplies a started AudioManager and either a
    // path to a TOML file or a pre-built WakeupConfig.
    bool init(audio::AudioManager* audio_manager,
              const std::string&   config_path = "Config/config.toml");
    bool init(audio::AudioManager* audio_manager, const WakeupConfig& cfg);

    void shutdown();

    void startListening();
    void stopListening();

    void setWakeCallback(WakeCallback cb);

    // Silence wakeup processing during LISTENING / RESPONDING / MULTI_TURN_WAIT
    // (frames are still consumed but discarded so no false positive fires).
    void muteWakeup(bool mute);
    bool isMuted() const;

    // Voiceprint management used by UserManager. WakeupManager keeps the
    // references locally for fast SV initial-screening; persistent storage
    // belongs to UserManager.
    bool addVoiceprint(const std::string& user_id, std::vector<float> embedding);
    void clearVoiceprints();

    int  embeddingDim()  const;
    bool isListening()   const;

    const WakeupConfig& config() const;

    // Standalone TOML loader (also used internally by init(audio, path)).
    static WakeupConfig loadConfig(const std::string& config_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wakeup
