#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AudioManager.h"
#include "NetworkManager.h"

namespace tts {

// ============================================================================
// Public enums
// ============================================================================

enum class EngineType {
    kOffline = 0,
    kOnline  = 1,
};

enum class TTSErrorCode {
    kOk                = 0,
    kNotInitialized    = 1,
    kAlreadyRunning    = 2,
    kEngineUnavailable = 3,
    kNetworkFailure    = 4,
    kProtocolError     = 5,
    kAuthFailure       = 6,
    kSynthesisFailed   = 7,
    kPlaybackFailed    = 8,
    kCanceled          = 9,
    kTimeout           = 10,
    kInternalError     = 99,
};

// ============================================================================
// Public result / option types
// ============================================================================

struct TTSOptions {
    std::string voice    = "zh_female_vv_uranus_bigtts";
    float       speed    = 1.0f;      // [0.5, 2.0]
    float       volume   = 1.0f;      // [0.0, 1.0]
    std::string language = "zh-CN";
};

struct TTSResult {
    bool                  success         = false;
    TTSErrorCode          code            = TTSErrorCode::kOk;
    std::string           message;
    EngineType            engine          = EngineType::kOffline;
    audio::PlaybackHandle playback_handle = audio::kInvalidPlaybackHandle;
    std::int64_t          timestamp_us    = 0;
};

using TTSCallback = std::function<void(const TTSResult&)>;

// ============================================================================
// Configuration (parsed from TOML [tts] or [engines.tts.*])
// ============================================================================

struct OfflineTtsConfig {
    bool                     enabled       = true;
    std::string              executable    = "sherpa-onnx-offline-tts";
    std::string              model_type    = "kokoro";
    std::string              kokoro_model  = "models/tts/kokoro-multi-lang-v1_1/model.onnx";
    std::string              kokoro_voices = "models/tts/kokoro-multi-lang-v1_1/voices.bin";
    std::string              kokoro_tokens = "models/tts/kokoro-multi-lang-v1_1/tokens.txt";
    std::string              kokoro_data_dir = "models/tts/kokoro-multi-lang-v1_1/espeak-ng-data";
    std::vector<std::string> kokoro_lexicon = {
        "models/tts/kokoro-multi-lang-v1_1/lexicon-us-en.txt",
        "models/tts/kokoro-multi-lang-v1_1/lexicon-zh.txt",
    };
    std::vector<std::string> rule_fsts = {
        "models/tts/kokoro-multi-lang-v1_1/date-zh.fst",
        "models/tts/kokoro-multi-lang-v1_1/phone-zh.fst",
        "models/tts/kokoro-multi-lang-v1_1/number-zh.fst",
    };
    int                      sid           = 45;
    float                    speed         = 1.0f;
    int                      num_threads   = 2;
    int                      sample_rate   = 24000;
    int                      channels      = 1;
    int                      timeout_ms    = 30000;
    bool                     debug         = false;
};

struct OnlineTtsConfig {
    bool        enabled             = true;
    std::string endpoint            = "wss://openspeech.bytedance.com/api/v3/tts/bidirection";
    std::string api_key;                         // new console: X-Api-Key
    std::string api_key_env         = "TTS_API_KEY";
    std::string resource_id         = "seed-tts-2.0";
    std::string uid                 = "smart_assistant";
    std::string model               = "seed-tts-2.0-standard";
    std::string default_voice       = "zh_female_vv_uranus_bigtts";
    std::string audio_format        = "pcm";
    int         sample_rate         = 24000;
    int         bit_rate            = 64000;
    int         connect_timeout_ms  = 5000;
    int         receive_timeout_ms  = 30000;
    int         max_retries         = 3;     // total online attempts before falling back to offline
    int         retry_backoff_ms    = 200;   // delay between retries (ms)
    bool        reuse_connection    = true;  // keep the V3 bidirectional connection warm across sessions
    bool        preconnect          = true;  // open the warm connection during init when credentials exist
    bool        use_cache           = true;  // Doubao whole-text cache for repeated text
    bool        use_segment_cache   = true;  // Doubao segmented cache, lower first-packet latency
    int         cache_text_type     = 1;
    bool        verify_ssl          = true;
    bool        require_usage_tokens = false;
    bool        debug               = false;
};

struct TtsLoggingConfig {
    std::string log_dir            = "logs";
    std::string log_file           = "tts.log";
    bool        console            = true;
    std::size_t rotation_max_bytes = 10 * 1024 * 1024;
    int         rotation_max_files = 7;
};

struct TtsConfig {
    OfflineTtsConfig offline;
    OnlineTtsConfig  online;
    TtsLoggingConfig logging;
    EngineType       preferred_engine    = EngineType::kOnline;
    bool             fallback_to_offline = true;
    int              leading_silence_ms  = 0;
    int              trailing_silence_ms = 0;
};

// ============================================================================
// IEngineSwitch (shared contract used by ASR / Intent / TTS managers)
// ============================================================================

class IEngineSwitch {
public:
    virtual ~IEngineSwitch() = default;

    virtual EngineType currentEngine() const                                = 0;
    virtual void       onNetworkStateChanged(network_manager::NetworkState) = 0;
    virtual bool       isOnlineAvailable() const                            = 0;
};

// ============================================================================
// TTSManager
// ============================================================================

class TTSManager : public IEngineSwitch {
public:
    TTSManager();
    ~TTSManager() override;

    TTSManager(const TTSManager&)            = delete;
    TTSManager& operator=(const TTSManager&) = delete;

    // Two-phase init. Caller supplies an already-init'd AudioManager.
    bool init(audio::AudioManager* audio_manager,
              const std::string&   config_path = "Config/config.toml");
    bool init(audio::AudioManager* audio_manager, const TtsConfig& cfg);

    void shutdown();

    bool synthesizeAndPlay(const std::string& text,
                           const TTSOptions&  opts = TTSOptions{},
                           TTSCallback        on_complete = {});
    void stop();

    // IEngineSwitch
    EngineType currentEngine() const override;
    void       onNetworkStateChanged(network_manager::NetworkState s) override;
    bool       isOnlineAvailable() const override;

    // Introspection
    bool             isSpeaking() const;
    const TtsConfig& config()    const;

    // Standalone TOML loader (also used internally by init(audio, path)).
    static TtsConfig loadConfig(const std::string& config_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tts
