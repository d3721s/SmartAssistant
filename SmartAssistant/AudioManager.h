#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace smartassistant
{

enum class BackendError
{
    kOk = 0,
    kDeviceGone = 1,
    kXrun = 2,
    kIOError = 3,
    kInvalidState = 4,
};

enum class PlaybackType
{
    Alarm,
    System,
    Tts,
    Prompt,
    Media,
    Bluetooth,
};

enum class AudioEventType
{
    CaptureStarted,
    CaptureStopped,
    CaptureError,
    VadSpeechStart,
    VadSpeechEnd,
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

using ConsumerHandle = std::uint64_t;
using PlaybackHandle = std::uint64_t;

struct AudioFrame
{
    std::vector<std::int16_t> samples;
    int sample_rate{16000};
    int channels{1};
    std::chrono::steady_clock::time_point timestamp{std::chrono::steady_clock::now()};
};

struct DeviceConfig
{
    std::string device{"default"};
    int sample_rate{48000};
    int channels{2};
    std::string format{"s16"};
    int frame_ms{10};
    std::vector<int> channel_map;
};

struct ProcessingConfig
{
    std::string pipeline{"far_field"};
    bool aec{true};
    bool ns{true};
    bool agc{true};
    bool vad{true};
    bool beamforming{true};
    int output_sample_rate{16000};
    int output_channels{1};
    int aec_reference_delay_ms{120};
};

struct VolumeConfig
{
    float master{0.75F};
    float tts{0.90F};
    float prompt{0.80F};
    float media{0.70F};
    float alarm{1.00F};
    float ducking_gain{0.35F};
};

struct AudioConfig
{
    std::string backend{"alsa"};
    DeviceConfig capture;
    DeviceConfig playback;
    ProcessingConfig processing;
    VolumeConfig volume;
    int max_recover_retries{5};
    int recover_backoff_base_ms{500};
    bool enable_metrics{true};
    bool enable_audio_dump{false};
    std::string dump_dir{"/var/log/audio/dump"};
};

struct PlaybackRequest
{
    std::vector<std::int16_t> pcm;
    int sample_rate{48000};
    int channels{2};
    PlaybackType type{PlaybackType::Media};
    bool interrupt{false};
    bool loop{false};
};

struct AudioDeviceInfo
{
    std::string id;
    std::string name;
};

struct AudioEvent
{
    AudioEventType type{AudioEventType::HealthChanged};
    PlaybackHandle playback_handle{0};
    std::string message;
};

struct HealthStatus
{
    bool initialized{false};
    bool capture_running{false};
    bool playback_running{false};
    std::uint64_t capture_frames_total{0};
    std::uint64_t capture_overrun_total{0};
    std::uint64_t playback_underrun_total{0};
    std::uint64_t device_recover_total{0};
    std::uint64_t device_recover_failed_total{0};
    std::uint64_t dispatched_frames_total{0};
    std::uint64_t consumer_dropped_frames_total{0};
    double capture_latency_ms{0.0};
    double playback_latency_ms{0.0};
    double processing_cost_ms{0.0};
    int aec_reference_delay_ms{120};
};

using FrameCallback = std::function<void(const AudioFrame&)>;
using AudioEventCallback = std::function<void(const AudioEvent&)>;

class AudioManager
{
public:
    AudioManager();
    ~AudioManager();

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    bool init(const AudioConfig& config);
    bool init(const std::string& config_path);
    void shutdown();

    bool startCapture();
    void stopCapture();

    ConsumerHandle addFrameConsumer(FrameCallback callback, std::size_t max_queue_depth);
    void removeFrameConsumer(ConsumerHandle handle);

    PlaybackHandle play(const PlaybackRequest& request);
    void stop(PlaybackHandle handle);
    void stopAll();

    void setMasterVolume(float volume);
    float masterVolume() const;

    std::vector<AudioDeviceInfo> listInputDevices() const;
    std::vector<AudioDeviceInfo> listOutputDevices() const;

    HealthStatus health() const;
    void subscribe(AudioEventCallback callback);

    const AudioConfig& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

AudioConfig loadAudioConfigFromToml(const std::string& config_path);

}
