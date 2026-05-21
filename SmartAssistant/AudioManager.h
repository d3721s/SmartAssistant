#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace audio {

// ============================================================================
// Backend interface types
// ============================================================================

enum class BackendError {
    kOk           = 0,
    kDeviceGone   = 1,
    kXrun         = 2,
    kIOError      = 3,
    kInvalidState = 4,
};

struct NegotiatedFormat {
    int  sample_rate   = 0;
    int  channels      = 0;
    int  period_frames = 0;
    int  buffer_frames = 0;
    bool is_float      = false;
};

struct AudioFrame {
    std::vector<int16_t> samples;
    int     channels     = 0;
    int     sample_rate  = 0;
    int64_t timestamp_us = 0;
};

struct DeviceConfig {
    std::string device_name;
    int  sample_rate = 48000;
    int  channels    = 2;
    int  frame_ms    = 10;
    bool is_float    = false;
};

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    int  max_channels = 0;
    bool is_default   = false;
};

using AudioDeviceList = std::vector<AudioDeviceInfo>;
using DeviceList      = std::vector<AudioDeviceInfo>;

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual BackendError    openInput(const DeviceConfig&, NegotiatedFormat&) = 0;
    virtual BackendError    openOutput(const DeviceConfig&, NegotiatedFormat&) = 0;
    virtual BackendError    readInput(AudioFrame&) = 0;
    virtual BackendError    writeOutput(const AudioFrame&) = 0;
    virtual AudioDeviceList listInputDevices() = 0;
    virtual AudioDeviceList listOutputDevices() = 0;
    virtual void            closeInput() = 0;
    virtual void            closeOutput() = 0;
};

// ============================================================================
// Public events / handles
// ============================================================================

enum class AudioEventType {
    CaptureStarted,
    CaptureStopped,
    CaptureError,
    VAD_SPEECH_START,
    VAD_SPEECH_END,
    PlaybackStarted,
    PlaybackCompleted,
    PlaybackInterrupted,
    PlaybackError,
    DeviceConnected,
    DeviceDisconnected,
    DeviceRecovered,
    DeviceFailed,
    VolumeChanged,
    HealthChanged,
};

struct AudioEvent {
    AudioEventType type;
    uint64_t       handle = 0;
    std::string    device_id;
    std::string    detail;
};

using AudioEventCallback = std::function<void(const AudioEvent&)>;
using FrameCallback      = std::function<void(const AudioFrame&)>;
using ConsumerHandle     = uint32_t;
constexpr ConsumerHandle kInvalidConsumerHandle = 0;

enum class PlaybackPriority {
    BLUETOOTH = 0,
    MEDIA     = 1,
    PROMPT    = 2,
    TTS       = 3,
    SYSTEM    = 4,
    ALARM     = 5,
};

struct PlaybackRequest {
    std::vector<int16_t> pcm_data;
    std::string          file_path;
    int              sample_rate = 48000;
    int              channels    = 2;
    PlaybackPriority priority    = PlaybackPriority::MEDIA;
    float            stream_gain = 1.0f;
    bool             loop        = false;
};

using PlaybackHandle = uint64_t;
constexpr PlaybackHandle kInvalidPlaybackHandle = 0;

struct HealthStatus {
    bool     capture_running             = false;
    bool     playback_running            = false;
    uint64_t capture_frames_total        = 0;
    uint64_t capture_overrun_total       = 0;
    uint64_t playback_underrun_total     = 0;
    uint64_t device_recover_total        = 0;
    uint64_t device_recover_failed_total = 0;
    double   capture_latency_ms          = 0.0;
    double   playback_latency_ms         = 0.0;
    double   processing_cost_ms          = 0.0;
    int      aec_reference_delay_ms      = 0;
    int64_t  aec_clock_drift_us          = 0;
    size_t   capture_queue_depth         = 0;
    size_t   reference_queue_depth       = 0;
    uint64_t wakeword_dropped_frames     = 0;
    uint64_t asr_dropped_frames          = 0;
    uint64_t diag_dropped_frames         = 0;
};

// ============================================================================
// Configuration (parsed from TOML)
// ============================================================================

struct CaptureConfig {
    std::string      device      = "hw:0,0";
    int              sample_rate = 48000;
    int              channels    = 4;
    std::vector<int> channel_map = {0, 1, 2, 3};
    std::string      format      = "s16";
    int              frame_ms    = 10;
};

struct PlaybackConfig {
    std::string device      = "hw:0,0";
    int         sample_rate = 48000;
    int         channels    = 2;
    std::string format      = "s16";
    int         frame_ms    = 10;
};

struct ProcessingConfig {
    std::string pipeline               = "far_field";
    bool        aec                    = true;
    bool        ns                     = true;
    bool        agc                    = true;
    bool        vad                    = true;
    bool        beamforming            = true;
    int         output_sample_rate     = 16000;
    int         output_channels        = 1;
    int         aec_reference_delay_ms = 120;
};

struct VolumeConfig {
    float master       = 0.75f;
    float tts          = 0.90f;
    float prompt       = 0.80f;
    float media        = 0.70f;
    float alarm        = 1.00f;
    float ducking_gain = 0.35f;
};

struct DeviceRecoveryConfig {
    int max_recover_retries     = 5;
    int recover_backoff_base_ms = 500;
};

struct DiagnosticsConfig {
    bool        enable_metrics    = true;
    bool        enable_audio_dump = false;
    std::string dump_dir          = "/var/log/audio/dump";
};

struct Config {
    std::string          backend = "alsa";
    CaptureConfig        capture;
    PlaybackConfig       playback;
    ProcessingConfig     processing;
    VolumeConfig         volume;
    DeviceRecoveryConfig device;
    DiagnosticsConfig    diagnostics;
};

Config loadConfig(const std::string& config_path);

// ============================================================================
// AudioManager public API (pimpl)
// ============================================================================

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    AudioManager(const AudioManager&)            = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    bool init(const Config& cfg);
    void shutdown();

    bool startCapture();
    void stopCapture();

    ConsumerHandle addFrameConsumer(FrameCallback cb, size_t max_queue_depth);
    void           removeFrameConsumer(ConsumerHandle handle);

    PlaybackHandle play(const PlaybackRequest& req);
    void           stop(PlaybackHandle handle);
    void           stopAll();

    void  setMasterVolume(float v);
    float masterVolume() const;

    DeviceList listInputDevices() const;
    DeviceList listOutputDevices() const;

    HealthStatus health() const;

    void subscribe(AudioEventCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace audio
