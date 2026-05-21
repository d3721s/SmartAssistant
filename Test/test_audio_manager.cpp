#include "AudioManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace smartassistant;
using namespace std::chrono_literals;

namespace
{

const auto kTestStart = std::chrono::steady_clock::now();
std::mutex g_log_mutex;

std::string elapsedMs()
{
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - kTestStart).count();
    std::ostringstream out;
    out << std::setw(6) << ms << "ms";
    return out.str();
}

void logLine(const std::string& level, const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << "[AudioManagerTest][" << elapsedMs() << "][" << level << "] " << message << std::endl;
}

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        logLine("FAIL", message);
        throw std::runtime_error(message);
    }
    logLine("PASS", message);
}

bool near(float lhs, float rhs, float epsilon = 0.0001F)
{
    return std::abs(lhs - rhs) < epsilon;
}

std::filesystem::path projectRootFromExecutable(const char* executable)
{
    std::filesystem::path path = std::filesystem::absolute(executable).parent_path();
    for (int i = 0; i < 8; ++i)
    {
        if (std::filesystem::exists(path / "Config" / "config.toml"))
        {
            return path;
        }
        path = path.parent_path();
    }
    return std::filesystem::current_path();
}

const char* eventTypeName(AudioEventType type)
{
    switch (type)
    {
    case AudioEventType::CaptureStarted:
        return "CaptureStarted";
    case AudioEventType::CaptureStopped:
        return "CaptureStopped";
    case AudioEventType::CaptureError:
        return "CaptureError";
    case AudioEventType::VadSpeechStart:
        return "VadSpeechStart";
    case AudioEventType::VadSpeechEnd:
        return "VadSpeechEnd";
    case AudioEventType::PlaybackStarted:
        return "PlaybackStarted";
    case AudioEventType::PlaybackCompleted:
        return "PlaybackCompleted";
    case AudioEventType::PlaybackInterrupted:
        return "PlaybackInterrupted";
    case AudioEventType::PlaybackError:
        return "PlaybackError";
    case AudioEventType::DeviceConnected:
        return "DeviceConnected";
    case AudioEventType::DeviceDisconnected:
        return "DeviceDisconnected";
    case AudioEventType::DeviceRecovered:
        return "DeviceRecovered";
    case AudioEventType::DeviceFailed:
        return "DeviceFailed";
    case AudioEventType::VolumeChanged:
        return "VolumeChanged";
    case AudioEventType::HealthChanged:
        return "HealthChanged";
    }
    return "Unknown";
}

const char* playbackTypeName(PlaybackType type)
{
    switch (type)
    {
    case PlaybackType::Alarm:
        return "Alarm";
    case PlaybackType::System:
        return "System";
    case PlaybackType::Tts:
        return "Tts";
    case PlaybackType::Prompt:
        return "Prompt";
    case PlaybackType::Media:
        return "Media";
    case PlaybackType::Bluetooth:
        return "Bluetooth";
    }
    return "Unknown";
}

std::string boolText(bool value)
{
    return value ? "true" : "false";
}

std::string healthSummary(const HealthStatus& health)
{
    std::ostringstream out;
    out << "initialized=" << boolText(health.initialized)
        << ", capture_running=" << boolText(health.capture_running)
        << ", playback_running=" << boolText(health.playback_running)
        << ", capture_frames_total=" << health.capture_frames_total
        << ", capture_overrun_total=" << health.capture_overrun_total
        << ", playback_underrun_total=" << health.playback_underrun_total
        << ", dispatched_frames_total=" << health.dispatched_frames_total
        << ", consumer_dropped_frames_total=" << health.consumer_dropped_frames_total
        << ", capture_latency_ms=" << health.capture_latency_ms
        << ", playback_latency_ms=" << health.playback_latency_ms
        << ", processing_cost_ms=" << health.processing_cost_ms
        << ", aec_reference_delay_ms=" << health.aec_reference_delay_ms;
    return out.str();
}

void logHealth(const std::string& label, const AudioManager& manager)
{
    logLine("DEBUG", label + ": " + healthSummary(manager.health()));
}

void logDevices(const std::string& label, const std::vector<AudioDeviceInfo>& devices)
{
    std::ostringstream out;
    out << label << " count=" << devices.size();
    const std::size_t limit = std::min<std::size_t>(devices.size(), 5);
    for (std::size_t i = 0; i < limit; ++i)
    {
        out << " [" << i << "] id='" << devices[i].id << "' name='" << devices[i].name << "'";
    }
    logLine("DEBUG", out.str());
}

class EventRecorder
{
public:
    void record(const AudioEvent& event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(event);
        }

        std::ostringstream out;
        out << eventTypeName(event.type) << " handle=" << event.playback_handle;
        if (!event.message.empty())
        {
            out << " message='" << event.message << "'";
        }
        logLine("EVENT", out.str());
        cv_.notify_all();
    }

    bool waitFor(AudioEventType type, std::chrono::milliseconds timeout) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&]() { return containsLocked(type); });
    }

    bool waitFor(AudioEventType type, PlaybackHandle handle, std::chrono::milliseconds timeout) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&]() { return containsLocked(type, handle); });
    }

    bool waitUntil(const std::function<bool(const std::vector<AudioEvent>&)>& predicate,
                   std::chrono::milliseconds timeout) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&]() { return predicate(events_); });
    }

    bool contains(AudioEventType type) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return containsLocked(type);
    }

    bool contains(AudioEventType type, PlaybackHandle handle) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return containsLocked(type, handle);
    }

    std::size_t count(AudioEventType type) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(std::count_if(events_.begin(), events_.end(), [&](const AudioEvent& event) {
            return event.type == type;
        }));
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_.size();
    }

private:
    bool containsLocked(AudioEventType type) const
    {
        return std::any_of(events_.begin(), events_.end(), [&](const AudioEvent& event) {
            return event.type == type;
        });
    }

    bool containsLocked(AudioEventType type, PlaybackHandle handle) const
    {
        return std::any_of(events_.begin(), events_.end(), [&](const AudioEvent& event) {
            return event.type == type && event.playback_handle == handle;
        });
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<AudioEvent> events_;
};

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

std::vector<std::int16_t> makeTone(int sample_rate, int channels, int duration_ms, int amplitude)
{
    const int frame_count = std::max(1, sample_rate * duration_ms / 1000);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(frame_count * std::max(1, channels)));
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kFrequencyHz = 440.0;
    for (int frame = 0; frame < frame_count; ++frame)
    {
        const double phase = (2.0 * kPi * kFrequencyHz * static_cast<double>(frame)) / static_cast<double>(sample_rate);
        const auto sample = static_cast<std::int16_t>(std::lround(std::sin(phase) * amplitude));
        for (int channel = 0; channel < std::max(1, channels); ++channel)
        {
            pcm[static_cast<std::size_t>(frame * std::max(1, channels) + channel)] = sample;
        }
    }
    return pcm;
}

PlaybackRequest makePlaybackRequest(const AudioConfig& config, PlaybackType type, int duration_ms)
{
    PlaybackRequest request;
    request.sample_rate = config.playback.sample_rate;
    request.channels = config.playback.channels;
    request.type = type;
    request.pcm = makeTone(request.sample_rate, request.channels, duration_ms, 4000);
    return request;
}

bool playbackTerminalObserved(const EventRecorder& events, PlaybackHandle handle, std::chrono::milliseconds timeout)
{
    return events.waitUntil([&](const std::vector<AudioEvent>& recorded) {
        return std::any_of(recorded.begin(), recorded.end(), [&](const AudioEvent& event) {
            if (event.type == AudioEventType::PlaybackCompleted && event.playback_handle == handle)
            {
                return true;
            }
            if (event.type == AudioEventType::PlaybackError &&
                (event.playback_handle == handle || event.playback_handle == 0))
            {
                return true;
            }
            return false;
        });
    }, timeout);
}

void assertParsedConfig(const AudioConfig& config)
{
    require(config.backend == "alsa", "config backend parsed: alsa");
    require(config.capture.device == "pulse", "config capture.device parsed: pulse");
    require(config.capture.sample_rate == 48000, "config capture.sample_rate parsed: 48000");
    require(config.capture.channels == 2, "config capture.channels parsed: 2");
    require(config.capture.format == "s16", "config capture.format parsed: s16");
    require(config.capture.frame_ms == 10, "config capture.frame_ms parsed: 10");
    require(config.capture.channel_map.size() == 2, "config capture.channel_map parsed with 2 entries");
    require(config.capture.channel_map[0] == 0 && config.capture.channel_map[1] == 1,
            "config capture.channel_map values parsed: [0, 1]");

    require(config.playback.device == "pulse", "config playback.device parsed: pulse");
    require(config.playback.sample_rate == 48000, "config playback.sample_rate parsed: 48000");
    require(config.playback.channels == 2, "config playback.channels parsed: 2");
    require(config.playback.format == "s16", "config playback.format parsed: s16");
    require(config.playback.frame_ms == 10, "config playback.frame_ms parsed: 10");

    require(config.processing.pipeline == "far_field", "config processing.pipeline parsed: far_field");
    require(config.processing.aec, "config processing.aec parsed: true");
    require(config.processing.ns, "config processing.ns parsed: true");
    require(config.processing.agc, "config processing.agc parsed: true");
    require(config.processing.vad, "config processing.vad parsed: true");
    require(config.processing.beamforming, "config processing.beamforming parsed: true");
    require(config.processing.output_sample_rate == 16000, "config processing.output_sample_rate parsed: 16000");
    require(config.processing.output_channels == 1, "config processing.output_channels parsed: 1");
    require(config.processing.aec_reference_delay_ms == 120, "config processing.aec_reference_delay_ms parsed: 120");

    require(near(config.volume.master, 0.75F), "config volume.master parsed: 0.75");
    require(near(config.volume.tts, 0.90F), "config volume.tts parsed: 0.90");
    require(near(config.volume.prompt, 0.80F), "config volume.prompt parsed: 0.80");
    require(near(config.volume.media, 0.70F), "config volume.media parsed: 0.70");
    require(near(config.volume.alarm, 1.00F), "config volume.alarm parsed: 1.00");
    require(near(config.volume.ducking_gain, 0.35F), "config volume.ducking_gain parsed: 0.35");

    require(config.max_recover_retries == 5, "config device.max_recover_retries parsed: 5");
    require(config.recover_backoff_base_ms == 500, "config device.recover_backoff_base_ms parsed: 500");
    require(config.enable_metrics, "config diagnostics.enable_metrics parsed: true");
    require(!config.enable_audio_dump, "config diagnostics.enable_audio_dump parsed: false");
    require(config.dump_dir == "/var/log/audio/dump", "config diagnostics.dump_dir parsed");
}

AudioConfig runtimeConfig(AudioConfig config)
{
    config.capture.device = "null";
    config.playback.device = "null";
    config.capture.frame_ms = 10;
    config.playback.frame_ms = 10;
    config.volume.master = 0.75F;
    return config;
}

void testPreInitAndInvalidInit(const AudioConfig& config, const std::filesystem::path& config_path)
{
    logLine("INFO", "Testing pre-init guards and init(config_path)");

    AudioManager pre_init;
    logHealth("pre-init health", pre_init);
    require(!pre_init.health().initialized, "pre-init health reports not initialized");
    require(!pre_init.startCapture(), "startCapture before init is rejected");
    pre_init.stopCapture();
    pre_init.stop(12345);
    pre_init.stopAll();
    pre_init.shutdown();
    require(pre_init.play(makePlaybackRequest(config, PlaybackType::Media, 10)) == 0, "play before init is rejected");

    AudioManager invalid_backend;
    AudioConfig bad_backend = config;
    bad_backend.backend = "unsupported-backend";
    require(!invalid_backend.init(bad_backend), "init rejects unsupported backend");
    require(!invalid_backend.health().initialized, "unsupported backend manager remains uninitialized");

    AudioManager from_path;
    require(from_path.init(config_path.string()), "init(config_path) succeeds");
    require(from_path.config().backend == "alsa", "config() is available after init(config_path)");
    logHealth("from-path initialized health", from_path);
    from_path.shutdown();
    require(!from_path.health().initialized, "shutdown after init(config_path) clears initialized");

    AudioManager missing_config;
    require(!missing_config.init((config_path.parent_path() / "__missing_audio_config.toml").string()),
            "init(missing_config_path) fails cleanly");
}

void testStopAndStopAllWithQueuedPlayback(const AudioConfig& config)
{
    logLine("INFO", "Testing stop(handle) and stopAll() interruption events with an invalid playback PCM");

    AudioConfig invalid_playback_config = config;
    invalid_playback_config.playback.device = "__smartassistant_missing_pcm__";

    {
        EventRecorder stop_events;
        AudioManager manager;
        manager.subscribe([&](const AudioEvent& event) { stop_events.record(event); });
        require(manager.init(invalid_playback_config), "stop test manager init succeeds");

        PlaybackRequest request = makePlaybackRequest(invalid_playback_config, PlaybackType::Media, 200);
        PlaybackHandle handle = manager.play(request);
        require(handle != 0, "play returns handle before stop(handle)");
        manager.stop(handle);
        require(stop_events.waitFor(AudioEventType::PlaybackInterrupted, handle, 1s),
                "stop(handle) emits PlaybackInterrupted for queued playback");
        manager.stop(handle);
        manager.shutdown();
        require(!manager.health().initialized, "stop test manager shutdown clears initialized");
    }

    {
        EventRecorder stop_all_events;
        AudioManager manager;
        manager.subscribe([&](const AudioEvent& event) { stop_all_events.record(event); });
        require(manager.init(invalid_playback_config), "stopAll test manager init succeeds");

        PlaybackRequest request = makePlaybackRequest(invalid_playback_config, PlaybackType::Prompt, 200);
        PlaybackHandle handle = manager.play(request);
        require(handle != 0, "play returns handle before stopAll()");
        manager.stopAll();
        require(stop_all_events.waitFor(AudioEventType::PlaybackInterrupted, handle, 1s),
                "stopAll() emits PlaybackInterrupted for queued playback");
        manager.stopAll();
        manager.shutdown();
        require(!manager.health().initialized, "stopAll test manager shutdown clears initialized");
    }
}

void testRuntimeManager(AudioConfig config)
{
    logLine("INFO", "Testing runtime manager with ALSA null capture/playback devices");

    EventRecorder events;
    AudioManager manager;
    manager.subscribe(AudioEventCallback{});
    manager.subscribe([&](const AudioEvent& event) { events.record(event); });

    require(manager.init(config), "init(runtime config object) succeeds");
    require(manager.health().initialized, "health reports initialized after init(config)");
    require(manager.config().capture.device == "null", "config() returns runtime capture device");
    require(manager.config().playback.device == "null", "config() returns runtime playback device");
    require(manager.config().processing.output_sample_rate == 16000, "config() returns processing output sample rate");
    require(manager.health().aec_reference_delay_ms == config.processing.aec_reference_delay_ms,
            "health reports configured AEC reference delay");
    logHealth("runtime initialized health", manager);

    auto inputs = manager.listInputDevices();
    auto outputs = manager.listOutputDevices();
    logDevices("input devices", inputs);
    logDevices("output devices", outputs);
    require(!inputs.empty(), "listInputDevices returns at least one ALSA device");
    require(!outputs.empty(), "listOutputDevices returns at least one ALSA device");

    require(manager.addFrameConsumer(FrameCallback{}, 4) == 0, "addFrameConsumer rejects empty callback");
    require(manager.addFrameConsumer([](const AudioFrame&) {}, 0) == 0,
            "addFrameConsumer rejects zero queue depth");

    std::atomic<int> frame_callbacks{0};
    std::atomic<int> invalid_frames{0};
    ConsumerHandle consumer = manager.addFrameConsumer([&](const AudioFrame& frame) {
        const int index = ++frame_callbacks;
        if (frame.sample_rate != config.processing.output_sample_rate ||
            frame.channels != config.processing.output_channels ||
            frame.samples.empty())
        {
            ++invalid_frames;
        }
        if (index <= 3)
        {
            std::ostringstream out;
            out << "frame callback #" << index
                << " sample_rate=" << frame.sample_rate
                << " channels=" << frame.channels
                << " samples=" << frame.samples.size();
            logLine("DEBUG", out.str());
        }
    }, 4);
    require(consumer != 0, "addFrameConsumer returns valid handle");

    ConsumerHandle second_consumer = manager.addFrameConsumer([](const AudioFrame&) {}, 1);
    require(second_consumer != 0, "addFrameConsumer supports a second consumer");
    manager.removeFrameConsumer(second_consumer);
    manager.removeFrameConsumer(999999);
    logLine("DEBUG", "removeFrameConsumer handles valid and unknown handles");

    manager.setMasterVolume(1.5F);
    require(near(manager.masterVolume(), 1.0F), "setMasterVolume clamps values above 1.0");
    manager.setMasterVolume(-1.0F);
    require(near(manager.masterVolume(), 0.0F), "setMasterVolume clamps values below 0.0");
    manager.setMasterVolume(0.42F);
    require(near(manager.masterVolume(), 0.42F), "masterVolume returns the configured in-range value");
    require(events.count(AudioEventType::VolumeChanged) >= 3, "VolumeChanged emitted for volume updates");

    PlaybackRequest empty_request;
    require(manager.play(empty_request) == 0, "play rejects empty PCM request after init");

    logLine("INFO", "Starting capture on ALSA null device");
    require(manager.startCapture(), "startCapture starts after init");
    const bool capture_reported = waitUntil([&]() {
        return frame_callbacks.load() > 0 ||
               events.contains(AudioEventType::CaptureStarted) ||
               events.contains(AudioEventType::CaptureError) ||
               events.contains(AudioEventType::DeviceFailed);
    }, 1500ms);
    require(capture_reported, "capture reports a frame, started event, or error event");
    std::this_thread::sleep_for(80ms);
    manager.stopCapture();
    events.waitFor(AudioEventType::CaptureStopped, 500ms);
    logHealth("after capture stop", manager);
    require(!manager.health().capture_running, "health reports capture stopped");
    require(invalid_frames.load() == 0, "captured callback frames match configured output format");
    manager.removeFrameConsumer(consumer);

    const std::vector<PlaybackType> playback_types{
        PlaybackType::Alarm,
        PlaybackType::System,
        PlaybackType::Tts,
        PlaybackType::Prompt,
        PlaybackType::Media,
        PlaybackType::Bluetooth,
    };

    bool playback_device_available = true;
    for (PlaybackType type : playback_types)
    {
        if (!playback_device_available)
        {
            std::ostringstream skipped;
            skipped << "Skipping " << playbackTypeName(type) << " playback because ALSA null playback already failed";
            logLine("WARN", skipped.str());
            continue;
        }

        PlaybackRequest request = makePlaybackRequest(config, type, 40);
        PlaybackHandle handle = manager.play(request);
        std::ostringstream started;
        started << "play(" << playbackTypeName(type) << ") returned handle=" << handle;
        logLine("DEBUG", started.str());
        require(handle != 0, std::string("play returns handle for ") + playbackTypeName(type));

        const bool terminal_event = playbackTerminalObserved(events, handle, 1500ms);
        require(terminal_event, std::string("playback emits completed or error event for ") + playbackTypeName(type));
        if (events.contains(AudioEventType::PlaybackError, handle) || events.contains(AudioEventType::PlaybackError, 0))
        {
            playback_device_available = false;
        }
    }

    manager.stop(424242);
    manager.stopAll();
    logHealth("before runtime shutdown", manager);
    manager.shutdown();
    require(!manager.health().initialized, "shutdown clears initialized");
    require(!manager.health().capture_running, "shutdown leaves capture stopped");
    require(!manager.health().playback_running, "shutdown leaves playback stopped");
    manager.shutdown();
    require(!manager.health().initialized, "second shutdown is harmless");

    std::ostringstream event_count;
    event_count << "recorded event count=" << events.size();
    logLine("DEBUG", event_count.str());
}

}

int main(int argc, char** argv)
{
    try
    {
        logLine("INFO", "AudioManager full API test started");
        const std::filesystem::path root = projectRootFromExecutable(argc > 0 ? argv[0] : ".");
        std::filesystem::path config_path = root / "Config" / "config.toml";
        if (!std::filesystem::exists(config_path))
        {
            config_path = std::filesystem::current_path() / "Config" / "config.toml";
        }
        logLine("INFO", "project root: " + root.string());
        logLine("INFO", "config path: " + config_path.string());

        AudioConfig parsed_config = loadAudioConfigFromToml(config_path.string());
        assertParsedConfig(parsed_config);

        AudioConfig null_device_config = runtimeConfig(parsed_config);
        logLine("DEBUG", "runtime config capture.device=" + null_device_config.capture.device +
                             ", playback.device=" + null_device_config.playback.device);

        testPreInitAndInvalidInit(parsed_config, config_path);
        testRuntimeManager(null_device_config);
        testStopAndStopAllWithQueuedPlayback(null_device_config);

        logLine("INFO", "AudioManager full API test finished");
    }
    catch (const std::exception& ex)
    {
        logLine("ERROR", ex.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
