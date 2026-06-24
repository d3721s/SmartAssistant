#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "NetworkManager.h"

namespace audio {
class AudioManager;
}

namespace asr {

// ============================================================================
// Public enums
// ============================================================================

enum class ASRMode {
    kAuto      = 0,
    kStreaming = 1,
    kBatch     = 2,
};

enum class EngineType {
    kOffline = 0,
    kOnline  = 1,
};

enum class AsrErrorCode {
    kOk                = 0,
    kNotInitialized    = 1,
    kAlreadyRunning    = 2,
    kEngineUnavailable = 3,
    kNetworkFailure    = 4,
    kProtocolError     = 5,
    kAuthFailure       = 6,
    kTimeout           = 7,
    kInternalError     = 99,
};

// ============================================================================
// Public result / event types
// ============================================================================

struct ASRPartialResult {
    std::string text;
    bool        is_final     = false;
    float       confidence   = 0.0f;
    EngineType  engine       = EngineType::kOffline;
    std::int64_t timestamp_us = 0;
};

struct ASRFinalResult {
    std::string text;
    float       confidence   = 1.0f;
    EngineType  engine       = EngineType::kOffline;
    std::int64_t timestamp_us = 0;
};

struct ASRError {
    AsrErrorCode code = AsrErrorCode::kOk;
    std::string  message;
    EngineType   engine = EngineType::kOffline;
};

using PartialCallback = std::function<void(const ASRPartialResult&)>;
using FinalCallback   = std::function<void(const ASRFinalResult&)>;
using ErrorCallback   = std::function<void(const ASRError&)>;

// ============================================================================
// Configuration (parsed from TOML [engines.asr.*])
// ============================================================================

// Offline engine: sherpa-onnx streaming Zipformer / Paraformer / Zipformer2 CTC
struct OfflineAsrConfig {
    bool        enabled              = true;
    std::string model_type           = "transducer";  // transducer | paraformer | zipformer2_ctc
    std::string model_architecture;                   // optional sherpa model_config.model_type hint
    std::string encoder;
    std::string decoder;
    std::string joiner;
    std::string paraformer_encoder;
    std::string paraformer_decoder;
    std::string zipformer2_ctc_model;
    std::string tokens;
    std::string provider             = "cpu";
    std::string decoding_method      = "greedy_search";
    int         num_threads          = 1;
    int         max_active_paths     = 4;
    int         sample_rate          = 16000;
    int         feature_dim          = 80;
    bool        enable_endpoint      = true;
    float       rule1_min_trailing_silence = 2.4f;
    float       rule2_min_trailing_silence = 1.2f;
    float       rule3_min_utterance_length = 20.0f;
    std::string hotwords_file;
    float       hotwords_score       = 1.5f;
    bool        debug                = false;
};

// Online engine: Doubao (Bytedance) bigmodel WebSocket streaming ASR.
// See https://www.volcengine.com/docs/6561/1354869
struct OnlineAsrConfig {
    bool        enabled              = true;
    std::string endpoint             = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";
    std::string api_key;                          // new console: X-Api-Key
    std::string app_key;                          // resolved at init via env if empty
    std::string access_key;                       // resolved at init via env if empty
    std::string api_key_env          = "ASR_API_KEY";
    std::string app_key_env          = "ASR_APP_KEY";
    std::string access_key_env       = "ASR_ACCESS_KEY";
    std::string resource_id          = "volc.seedasr.sauc.duration";
    std::string uid                  = "smart_assistant";
    std::string format               = "pcm";
    std::string codec                = "raw";
    std::string language             = "zh-CN";
    int         sample_rate          = 16000;
    int         channels             = 1;
    int         bits                 = 16;
    int         segment_duration_ms  = 200;        // Doubao async streaming performs best around 200 ms chunks
    int         connect_timeout_ms   = 5000;
    int         receive_timeout_ms   = 30000;
    int         silence_finalize_ms  = 800;        // VAD-end → server-finalize hint
    int         max_retries          = 3;          // total online session-start attempts before falling back to offline
    int         retry_backoff_ms     = 200;        // delay between retries (ms)
    bool        enable_punctuation   = false;
    bool        enable_itn           = false;
    bool        enable_ddc           = false;
    bool        show_utterances      = true;
    std::string result_type          = "single";
    bool        enable_nonstream     = false;
    bool        enable_accelerate_text = true;
    int         accelerate_score     = 20;         // 0..20, larger returns first text faster
    int         end_window_size_ms   = 200;        // server min is 200 ms; lower final latency
    int         force_to_speech_time_ms = 1;       // 0 disables; min accepted by server is 1 ms
    int         vad_segment_duration_ms = 0;       // 0 disables; ignored when end_window_size is set
    bool        verify_ssl           = true;
    bool        debug                = false;
};

struct AsrLoggingConfig {
    std::string log_dir            = "logs";
    std::string log_file           = "asr.log";
    bool        console            = true;
    std::size_t rotation_max_bytes = 10 * 1024 * 1024;
    int         rotation_max_files = 7;
};

struct AsrConfig {
    OfflineAsrConfig  offline;
    OnlineAsrConfig   online;
    AsrLoggingConfig  logging;
    ASRMode           default_mode        = ASRMode::kStreaming;
    EngineType        preferred_engine    = EngineType::kOnline;
    bool              fallback_to_offline = true;
    std::size_t       frame_queue_depth   = 200;   // per architecture: 200 for ASR
    int               max_session_ms      = 30000; // hard limit for a single recognition
    int               no_result_timeout_ms = 10000;
};

// ============================================================================
// IEngineSwitch (shared by ASR / Intent / TTS managers)
// ============================================================================

class IEngineSwitch {
public:
    virtual ~IEngineSwitch() = default;

    virtual EngineType currentEngine() const                                 = 0;
    virtual void       onNetworkStateChanged(network_manager::NetworkState)  = 0;
    virtual bool       isOnlineAvailable() const                             = 0;
};

// ============================================================================
// ASRManager
// ============================================================================

class ASRManager : public IEngineSwitch {
public:
    ASRManager();
    ~ASRManager() override;

    ASRManager(const ASRManager&)            = delete;
    ASRManager& operator=(const ASRManager&) = delete;

    // Two-phase init.  Caller supplies an already-init'd AudioManager.
    bool init(audio::AudioManager* audio_manager,
              const std::string&   config_path = "Config/config.toml");
    bool init(audio::AudioManager* audio_manager, const AsrConfig& cfg);

    void shutdown();

    // Recognition lifecycle
    bool startRecognition(ASRMode mode = ASRMode::kAuto);
    void stopRecognition();
    void hintSpeechEnd();   // streaming: soft hint, engine decides EOS
    void finalize();        // batch: force immediate finalization

    // Abort an in-flight startRecognition handshake.  Safe to call from any
    // thread.  If start() is currently blocked on its connect-timeout
    // wait_for, this wakes it up and forces it to return false.  No-op if no
    // start is pending.  Use this when a higher-level event (wake-word
    // barge-in, Ctrl+C) needs to interrupt the chain.
    void cancelPendingStart();

    // Callbacks
    void setPartialCallback(PartialCallback cb);
    void setFinalCallback(FinalCallback cb);
    void setErrorCallback(ErrorCallback cb);

    // IEngineSwitch
    EngineType currentEngine() const override;
    void       onNetworkStateChanged(network_manager::NetworkState s) override;
    bool       isOnlineAvailable() const override;

    // Introspection
    bool             isRecognizing() const;
    ASRMode          activeMode()    const;
    const AsrConfig& config()        const;

    // Standalone TOML loader (also used internally by init(audio, path)).
    static AsrConfig loadConfig(const std::string& config_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace asr
