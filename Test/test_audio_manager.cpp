#include "AudioManager.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace smartassistant;
using namespace std::chrono_literals;

namespace
{

const auto kTestStart = std::chrono::steady_clock::now();
constexpr int kRealAudioCaptureSeconds = 10;
std::mutex g_log_mutex;

quill::Logger* testLogger()
{
    static std::once_flag flag;
    static quill::Logger* logger{nullptr};
    std::call_once(flag, []() {
        quill::BackendOptions options;
        options.check_printable_char = {};
        quill::Backend::start(options);
        auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("audio_manager_test_console");
        logger = quill::Frontend::create_or_get_logger("AudioManagerTest", std::move(sink));
        logger->set_log_level(quill::LogLevel::Info);
        logger->set_immediate_flush();
    });
    return logger;
}

class TestScope
{
public:
    TestScope(std::string name, std::string content) : name_(std::move(name))
    {
        LOG_INFO(testLogger(), "开始测试：{}；测试内容：{}", name_, content);
    }

    ~TestScope()
    {
        LOG_INFO(testLogger(), "结束测试：{}", name_);
    }

    TestScope(const TestScope&) = delete;
    TestScope& operator=(const TestScope&) = delete;

private:
    std::string name_;
};

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

bool truthy(const char* value)
{
    if (value == nullptr)
    {
        return false;
    }
    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON" ||
           text == "yes" || text == "YES";
}

bool hasArgument(int argc, char** argv, const std::string& expected)
{
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] != nullptr && expected == argv[i])
        {
            return true;
        }
    }
    return false;
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

PlaybackRequest makePlaybackRequestFromPcm(const AudioConfig& config,
                                           PlaybackType type,
                                           std::vector<std::int16_t> pcm)
{
    PlaybackRequest request;
    request.sample_rate = config.playback.sample_rate;
    request.channels = config.playback.channels;
    request.type = type;
    request.pcm = std::move(pcm);
    return request;
}

struct PcmStats
{
    int peak{0};
    double rms{0.0};
    std::size_t nonzero_samples{0};
};

PcmStats analyzePcm(const std::vector<std::int16_t>& samples)
{
    PcmStats stats;
    if (samples.empty())
    {
        return stats;
    }

    double sum_squares = 0.0;
    for (std::int16_t sample : samples)
    {
        const int value = static_cast<int>(sample);
        const int magnitude = std::abs(value);
        stats.peak = std::max(stats.peak, magnitude);
        if (value != 0)
        {
            ++stats.nonzero_samples;
        }
        sum_squares += static_cast<double>(value) * static_cast<double>(value);
    }
    stats.rms = std::sqrt(sum_squares / static_cast<double>(samples.size()));
    return stats;
}

std::string pcmStatsSummary(const PcmStats& stats)
{
    std::ostringstream out;
    out << "peak=" << stats.peak
        << ", rms=" << stats.rms
        << ", nonzero_samples=" << stats.nonzero_samples;
    return out.str();
}

void writeUint16LE(std::ostream& out, std::uint16_t value)
{
    out.put(static_cast<char>(value & 0xFFU));
    out.put(static_cast<char>((value >> 8U) & 0xFFU));
}

void writeUint32LE(std::ostream& out, std::uint32_t value)
{
    out.put(static_cast<char>(value & 0xFFU));
    out.put(static_cast<char>((value >> 8U) & 0xFFU));
    out.put(static_cast<char>((value >> 16U) & 0xFFU));
    out.put(static_cast<char>((value >> 24U) & 0xFFU));
}

void writeWavFile(const std::filesystem::path& path,
                  const std::vector<std::int16_t>& samples,
                  int sample_rate,
                  int channels)
{
    const int safe_sample_rate = std::max(1, sample_rate);
    const int safe_channels = std::max(1, channels);
    const std::uint32_t data_size = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t byte_rate =
        static_cast<std::uint32_t>(safe_sample_rate * safe_channels * static_cast<int>(sizeof(std::int16_t)));
    const std::uint16_t block_align = static_cast<std::uint16_t>(safe_channels * static_cast<int>(sizeof(std::int16_t)));

    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        throw std::runtime_error("failed to create wav file: " + path.string());
    }

    out.write("RIFF", 4);
    writeUint32LE(out, 36U + data_size);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeUint32LE(out, 16U);
    writeUint16LE(out, 1U);
    writeUint16LE(out, static_cast<std::uint16_t>(safe_channels));
    writeUint32LE(out, static_cast<std::uint32_t>(safe_sample_rate));
    writeUint32LE(out, byte_rate);
    writeUint16LE(out, block_align);
    writeUint16LE(out, 16U);
    out.write("data", 4);
    writeUint32LE(out, data_size);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(data_size));
}

std::vector<std::int16_t> convertPcmForPlayback(const std::vector<std::int16_t>& input,
                                                int input_sample_rate,
                                                int input_channels,
                                                int output_sample_rate,
                                                int output_channels)
{
    const int in_channels = std::max(1, input_channels);
    const int out_channels = std::max(1, output_channels);
    const int in_rate = std::max(1, input_sample_rate);
    const int out_rate = std::max(1, output_sample_rate);
    const std::size_t in_frames = input.size() / static_cast<std::size_t>(in_channels);
    if (in_frames == 0)
    {
        return {};
    }

    const std::size_t out_frames =
        std::max<std::size_t>(1, (in_frames * static_cast<std::size_t>(out_rate)) / static_cast<std::size_t>(in_rate));
    std::vector<std::int16_t> output(out_frames * static_cast<std::size_t>(out_channels));

    for (std::size_t out_frame = 0; out_frame < out_frames; ++out_frame)
    {
        const std::size_t in_frame = std::min<std::size_t>(
            in_frames - 1,
            (out_frame * static_cast<std::size_t>(in_rate)) / static_cast<std::size_t>(out_rate));
        for (int channel = 0; channel < out_channels; ++channel)
        {
            const int in_channel = std::min(channel, in_channels - 1);
            output[out_frame * static_cast<std::size_t>(out_channels) + static_cast<std::size_t>(channel)] =
                input[in_frame * static_cast<std::size_t>(in_channels) + static_cast<std::size_t>(in_channel)];
        }
    }
    return output;
}

int peakAbs(const std::vector<std::int16_t>& samples)
{
    int peak = 0;
    for (std::int16_t sample : samples)
    {
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    return peak;
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
    TestScope scope("配置文件解析", "读取 Config/config.toml 并验证 audio.capture、audio.playback、audio.processing、audio.volume、audio.device、audio.diagnostics 每个字段");

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

AudioConfig runtimeConfig(AudioConfig config, bool real_audio)
{
    if (!real_audio)
    {
        config.capture.device = "null";
        config.playback.device = "null";
    }
    else
    {
        config.capture.channels = 1;
        config.capture.channel_map = {0};
    }
    config.capture.frame_ms = 10;
    config.playback.frame_ms = 10;
    config.volume.master = 0.75F;
    return config;
}

void testPreInitAndInvalidInit(const AudioConfig& config, const std::filesystem::path& config_path)
{
    TestScope scope("初始化与异常路径", "覆盖初始化前保护、非法 backend、从配置路径初始化、缺失配置文件失败返回");
    logLine("INFO", "Testing pre-init guards and init(config_path)");

    {
        TestScope item("初始化前保护", "未 init 时验证 health、startCapture、play、stop、stopAll、shutdown 的保护逻辑");
        AudioManager pre_init;
        logHealth("pre-init health", pre_init);
        require(!pre_init.health().initialized, "pre-init health reports not initialized");
        require(!pre_init.startCapture(), "startCapture before init is rejected");
        pre_init.stopCapture();
        pre_init.stop(12345);
        pre_init.stopAll();
        pre_init.shutdown();
        require(pre_init.play(makePlaybackRequest(config, PlaybackType::Media, 10)) == 0, "play before init is rejected");
    }

    {
        TestScope item("非法 backend 初始化", "传入 unsupported-backend，验证 init 返回 false 且状态保持未初始化");
        AudioManager invalid_backend;
        AudioConfig bad_backend = config;
        bad_backend.backend = "unsupported-backend";
        require(!invalid_backend.init(bad_backend), "init rejects unsupported backend");
        require(!invalid_backend.health().initialized, "unsupported backend manager remains uninitialized");
    }

    {
        TestScope item("配置路径初始化", "调用 init(config_path)，验证配置加载、config()、health() 和 shutdown()");
        AudioManager from_path;
        require(from_path.init(config_path.string()), "init(config_path) succeeds");
        require(from_path.config().backend == "alsa", "config() is available after init(config_path)");
        logHealth("from-path initialized health", from_path);
        from_path.shutdown();
        require(!from_path.health().initialized, "shutdown after init(config_path) clears initialized");
    }

    {
        TestScope item("缺失配置文件", "调用 init() 加载不存在的 toml，验证失败返回且不崩溃");
        AudioManager missing_config;
        require(!missing_config.init((config_path.parent_path() / "__missing_audio_config.toml").string()),
                "init(missing_config_path) fails cleanly");
    }
}

void testStopAndStopAllWithQueuedPlayback(const AudioConfig& config)
{
    TestScope scope("播放停止接口", "使用不存在的 ALSA PCM 让播放线程失败，同时在队列阶段验证 stop(handle) 和 stopAll() 会发出 PlaybackInterrupted");
    logLine("INFO", "Testing stop(handle) and stopAll() interruption events with an invalid playback PCM");

    AudioConfig invalid_playback_config = config;
    invalid_playback_config.playback.device = "__smartassistant_missing_pcm__";

    {
        TestScope item("stop(handle)", "排队一个播放请求后调用 stop(handle)，验证指定 handle 被中断");
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
        TestScope item("stopAll()", "排队一个播放请求后调用 stopAll()，验证队列中的播放全部被中断");
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

void testRuntimeManager(AudioConfig config, bool real_audio)
{
    TestScope scope(real_audio ? "真实音频运行测试" : "空设备运行测试",
                    real_audio ? "使用 Config/config.toml 中的 ALSA 设备真实采集麦克风并播放测试音"
                               : "使用 ALSA null 设备覆盖采集、播放、音量、事件、健康状态，不依赖真实声卡");
    logLine("INFO", real_audio ? "Testing runtime manager with configured real ALSA devices"
                               : "Testing runtime manager with ALSA null capture/playback devices");

    EventRecorder events;
    AudioManager manager;
    manager.subscribe(AudioEventCallback{});
    manager.subscribe([&](const AudioEvent& event) { events.record(event); });

    {
        TestScope item("运行时初始化和健康状态", "调用 init(config)，验证 config()、health()、AEC 延迟字段和初始化状态");
        require(manager.init(config), "init(runtime config object) succeeds");
        require(manager.health().initialized, "health reports initialized after init(config)");
        require(manager.config().capture.device == config.capture.device, "config() returns runtime capture device");
        require(manager.config().playback.device == config.playback.device, "config() returns runtime playback device");
        require(manager.config().processing.output_sample_rate == 16000, "config() returns processing output sample rate");
        require(manager.health().aec_reference_delay_ms == config.processing.aec_reference_delay_ms,
                "health reports configured AEC reference delay");
        logHealth("runtime initialized health", manager);
    }

    {
        TestScope item("设备枚举", "调用 listInputDevices() 和 listOutputDevices()，打印前 5 个 ALSA PCM 设备");
        auto inputs = manager.listInputDevices();
        auto outputs = manager.listOutputDevices();
        logDevices("input devices", inputs);
        logDevices("output devices", outputs);
        require(!inputs.empty(), "listInputDevices returns at least one ALSA device");
        require(!outputs.empty(), "listOutputDevices returns at least one ALSA device");
    }

    {
        TestScope item("消费者参数校验", "验证 addFrameConsumer 会拒绝空 callback 和 max_queue_depth=0");
        require(manager.addFrameConsumer(FrameCallback{}, 4) == 0, "addFrameConsumer rejects empty callback");
        require(manager.addFrameConsumer([](const AudioFrame&) {}, 0) == 0,
                "addFrameConsumer rejects zero queue depth");
    }

    std::atomic<int> frame_callbacks{0};
    std::atomic<int> invalid_frames{0};
    std::atomic<int> capture_peak{0};
    std::mutex captured_pcm_mutex;
    std::vector<std::int16_t> captured_pcm;
    const std::size_t captured_pcm_limit =
        static_cast<std::size_t>(std::max(1, config.processing.output_sample_rate) *
                                 std::max(1, config.processing.output_channels) *
                                 (real_audio ? kRealAudioCaptureSeconds : 1));
    ConsumerHandle consumer = manager.addFrameConsumer([&](const AudioFrame& frame) {
        const int index = ++frame_callbacks;
        if (frame.sample_rate != config.processing.output_sample_rate ||
            frame.channels != config.processing.output_channels ||
            frame.samples.empty())
        {
            ++invalid_frames;
        }
        const int frame_peak = peakAbs(frame.samples);
        int old_peak = capture_peak.load();
        while (frame_peak > old_peak && !capture_peak.compare_exchange_weak(old_peak, frame_peak))
        {
        }
        {
            std::lock_guard<std::mutex> lock(captured_pcm_mutex);
            if (captured_pcm.size() < captured_pcm_limit)
            {
                const std::size_t remaining = captured_pcm_limit - captured_pcm.size();
                const std::size_t samples_to_copy = std::min<std::size_t>(remaining, frame.samples.size());
                captured_pcm.insert(captured_pcm.end(), frame.samples.begin(), frame.samples.begin() + samples_to_copy);
            }
        }
        if (index <= 3)
        {
            std::ostringstream out;
            out << "frame callback #" << index
                << " sample_rate=" << frame.sample_rate
                << " channels=" << frame.channels
                << " samples=" << frame.samples.size()
                << " peak=" << frame_peak;
            logLine("DEBUG", out.str());
        }
    }, 4);
    {
        TestScope item("消费者注册和删除", "注册两个帧消费者，删除其中一个，并验证未知 handle 删除不会崩溃");
        require(consumer != 0, "addFrameConsumer returns valid handle");

        ConsumerHandle second_consumer = manager.addFrameConsumer([](const AudioFrame&) {}, 1);
        require(second_consumer != 0, "addFrameConsumer supports a second consumer");
        manager.removeFrameConsumer(second_consumer);
        manager.removeFrameConsumer(999999);
        logLine("DEBUG", "removeFrameConsumer handles valid and unknown handles");
    }

    {
        TestScope item("音量设置", "调用 setMasterVolume() 验证 0.0 到 1.0 夹取、masterVolume() 返回值和 VolumeChanged 事件；该接口调整 AudioManager 内部播放增益，不调整系统音量");
        manager.setMasterVolume(1.5F);
        require(near(manager.masterVolume(), 1.0F), "setMasterVolume clamps values above 1.0");
        manager.setMasterVolume(-1.0F);
        require(near(manager.masterVolume(), 0.0F), "setMasterVolume clamps values below 0.0");
        manager.setMasterVolume(real_audio ? 0.80F : 0.42F);
        require(near(manager.masterVolume(), real_audio ? 0.80F : 0.42F),
                "masterVolume returns the configured in-range value");
        require(events.count(AudioEventType::VolumeChanged) >= 3, "VolumeChanged emitted for volume updates");
    }

    {
        TestScope item("空播放请求", "传入空 PCM 的 PlaybackRequest，验证 play() 返回 0 并拒绝播放");
        PlaybackRequest empty_request;
        require(manager.play(empty_request) == 0, "play rejects empty PCM request after init");
    }

    {
        TestScope item(real_audio ? "真实录音采集" : "空设备录音采集",
                       real_audio ? "打开配置中的采集设备，采集约 10 秒麦克风 PCM，打印回调帧格式和采样峰值；运行时请对麦克风说话"
                                  : "打开 ALSA null 采集设备，验证采集线程、回调、事件和健康状态，null 设备只产生静音帧");
        logLine("INFO", "Starting capture on ALSA device: " + config.capture.device);
        require(manager.startCapture(), "startCapture starts after init");
        const bool capture_reported = waitUntil([&]() {
            return frame_callbacks.load() > 0 ||
                   events.contains(AudioEventType::CaptureStarted) ||
                   events.contains(AudioEventType::CaptureError) ||
                   events.contains(AudioEventType::DeviceFailed);
        }, real_audio ? 2500ms : 1500ms);
        require(capture_reported, "capture reports a frame, started event, or error event");
        std::this_thread::sleep_for(real_audio ? std::chrono::seconds(kRealAudioCaptureSeconds) : 80ms);
        manager.stopCapture();
        events.waitFor(AudioEventType::CaptureStopped, 500ms);
        logHealth("after capture stop", manager);
        require(!manager.health().capture_running, "health reports capture stopped");
        require(invalid_frames.load() == 0, "captured callback frames match configured output format");
        if (real_audio)
        {
            require(frame_callbacks.load() > 0, "real capture receives callback frames");
            require(!events.contains(AudioEventType::CaptureError), "real capture does not emit CaptureError");
            require(!events.contains(AudioEventType::DeviceFailed), "real capture device does not fail");
        }

        std::ostringstream peak_out;
        peak_out << "capture callbacks=" << frame_callbacks.load() << ", max_peak=" << capture_peak.load();
        logLine("DEBUG", peak_out.str());
    }
    manager.removeFrameConsumer(consumer);

    bool playback_device_available = true;
    auto playAndValidate = [&](const std::string& label,
                               PlaybackRequest request,
                               std::chrono::milliseconds timeout) {
        if (!playback_device_available)
        {
            logLine("WARN", label + " skipped because playback device already failed");
            return false;
        }

        PlaybackHandle handle = manager.play(request);
        std::ostringstream started;
        started << label << " returned handle=" << handle
                << " samples=" << request.pcm.size()
                << " sample_rate=" << request.sample_rate
                << " channels=" << request.channels;
        logLine("DEBUG", started.str());
        require(handle != 0, label + " returns a valid playback handle");

        const bool terminal_event = playbackTerminalObserved(events, handle, timeout);
        require(terminal_event, label + " emits completed or error event");

        const bool playback_error = events.contains(AudioEventType::PlaybackError, handle) ||
                                    events.contains(AudioEventType::PlaybackError, 0);
        if (playback_error)
        {
            playback_device_available = false;
            if (real_audio)
            {
                require(false, label + " must not emit PlaybackError in real-audio mode");
            }
            return false;
        }

        require(events.contains(AudioEventType::PlaybackCompleted, handle), label + " emits PlaybackCompleted");
        return true;
    };

    {
        TestScope item(real_audio ? "播放采集录音" : "播放空设备采集数据",
                       real_audio ? "把刚采集到的 10 秒麦克风 PCM 转成播放设备格式，再调用 play() 完整回放这 10 秒录音"
                                  : "把 ALSA null 采集到的静音 PCM 转成播放设备格式，再调用 play() 覆盖采集数据回放链路");

        std::vector<std::int16_t> recorded;
        {
            std::lock_guard<std::mutex> lock(captured_pcm_mutex);
            recorded = captured_pcm;
        }
        const PcmStats recorded_stats = analyzePcm(recorded);
        std::ostringstream recorded_info;
        recorded_info << "captured pcm samples=" << recorded.size()
                      << ", processing_sample_rate=" << config.processing.output_sample_rate
                      << ", processing_channels=" << config.processing.output_channels
                      << ", " << pcmStatsSummary(recorded_stats);
        logLine("DEBUG", recorded_info.str());
        require(!recorded.empty(), "captured PCM buffer is available for playback");
        const std::filesystem::path processed_wav_path =
            std::filesystem::absolute("audio_manager_recorded_processed_16k_mono.wav");
        writeWavFile(processed_wav_path,
                     recorded,
                     config.processing.output_sample_rate,
                     config.processing.output_channels);
        logLine("INFO", "processed captured wav saved: " + processed_wav_path.string());
        if (real_audio)
        {
            require(recorded_stats.nonzero_samples > 0,
                    "real captured PCM contains nonzero samples; " + pcmStatsSummary(recorded_stats));
            require(recorded_stats.peak >= 128,
                    "real captured PCM peak is high enough to hear; " + pcmStatsSummary(recorded_stats));
        }

        std::vector<std::int16_t> playback_pcm = convertPcmForPlayback(
            recorded,
            config.processing.output_sample_rate,
            config.processing.output_channels,
            config.playback.sample_rate,
            config.playback.channels);

        const PcmStats playback_stats = analyzePcm(playback_pcm);
        std::ostringstream converted_info;
        converted_info << "converted captured pcm samples=" << playback_pcm.size()
                       << ", playback_sample_rate=" << config.playback.sample_rate
                       << ", playback_channels=" << config.playback.channels
                       << ", " << pcmStatsSummary(playback_stats);
        logLine("DEBUG", converted_info.str());
        require(!playback_pcm.empty(), "converted captured PCM is not empty");
        const std::filesystem::path playback_wav_path =
            std::filesystem::absolute("audio_manager_recorded_playback_format.wav");
        writeWavFile(playback_wav_path, playback_pcm, config.playback.sample_rate, config.playback.channels);
        logLine("INFO", "playback-format captured wav saved: " + playback_wav_path.string());
        if (real_audio)
        {
            require(playback_stats.nonzero_samples > 0,
                    "converted captured PCM contains nonzero samples; " + pcmStatsSummary(playback_stats));
            require(playback_stats.peak >= 128,
                    "converted captured PCM peak is high enough to hear; " + pcmStatsSummary(playback_stats));
        }

        manager.setMasterVolume(real_audio ? 0.80F : 0.42F);
        require(near(manager.masterVolume(), real_audio ? 0.80F : 0.42F),
                "master volume is set before recorded PCM playback");
        playAndValidate("play captured recording", makePlaybackRequestFromPcm(config, PlaybackType::Prompt, std::move(playback_pcm)),
                        real_audio ? std::chrono::seconds(kRealAudioCaptureSeconds + 3) : 1500ms);
    }

    {
        TestScope item(real_audio ? "可听音量阶梯测试" : "空设备音量阶梯测试",
                       real_audio ? "依次设置 masterVolume=0.20、0.60、1.00 并播放 440Hz 测试音，人工确认音量逐级变大"
                                  : "依次设置 masterVolume=0.20、0.60、1.00 并播放到 ALSA null，验证音量接口和播放链路");
        const std::vector<std::pair<float, std::string>> volume_steps{
            {0.20F, "low volume"},
            {0.60F, "middle volume"},
            {1.00F, "high volume"},
        };

        for (const auto& step : volume_steps)
        {
            manager.setMasterVolume(step.first);
            require(near(manager.masterVolume(), step.first), step.second + " master volume is applied");
            playAndValidate(step.second + " tone playback",
                            makePlaybackRequest(config, PlaybackType::Alarm, real_audio ? 500 : 40),
                            real_audio ? 1800ms : 1500ms);
        }
    }

    const std::vector<PlaybackType> playback_types{
        PlaybackType::Alarm,
        PlaybackType::System,
        PlaybackType::Tts,
        PlaybackType::Prompt,
        PlaybackType::Media,
        PlaybackType::Bluetooth,
    };

    for (PlaybackType type : playback_types)
    {
        TestScope item(std::string("播放音频 - ") + playbackTypeName(type),
                       real_audio ? "向配置中的播放设备写入一段 440Hz 测试音，验证 PlaybackStarted 和 PlaybackCompleted/PlaybackError 事件"
                                  : "向 ALSA null 播放设备写入一段 PCM，覆盖播放线程和事件；null 设备会丢弃声音，所以听不到音频");
        PlaybackRequest request = makePlaybackRequest(config, type, real_audio ? 600 : 40);
        playAndValidate(std::string("play ") + playbackTypeName(type), request, real_audio ? 2000ms : 1500ms);
    }

    {
        TestScope item("停止与关闭", "调用 stop(未知 handle)、stopAll()、shutdown()、重复 shutdown()，验证健康状态清理");
        manager.stop(424242);
        manager.stopAll();
        logHealth("before runtime shutdown", manager);
        manager.shutdown();
        require(!manager.health().initialized, "shutdown clears initialized");
        require(!manager.health().capture_running, "shutdown leaves capture stopped");
        require(!manager.health().playback_running, "shutdown leaves playback stopped");
        manager.shutdown();
        require(!manager.health().initialized, "second shutdown is harmless");
    }

    std::ostringstream event_count;
    event_count << "recorded event count=" << events.size();
    logLine("DEBUG", event_count.str());
}

}

int main(int argc, char** argv)
{
    try
    {
        TestScope scope("AudioManager 全功能测试", "覆盖配置解析、初始化、消费者、录音采集、播放音频、调整音量、事件、健康状态、停止和关闭");
        logLine("INFO", "AudioManager full API test started");
        const bool real_audio = hasArgument(argc, argv, "--real-audio") ||
                                truthy(std::getenv("SMARTASSISTANT_TEST_REAL_AUDIO"));
        LOG_INFO(testLogger(),
                 "测试音频模式：{}；默认 null 模式不使用真实麦克风和扬声器，真实模式请传 --real-audio 或设置 SMARTASSISTANT_TEST_REAL_AUDIO=1",
                 real_audio ? "真实音频设备" : "ALSA null 空设备");

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

        AudioConfig runtime_config = runtimeConfig(parsed_config, real_audio);
        logLine("DEBUG", "runtime config capture.device=" + runtime_config.capture.device +
                             ", playback.device=" + runtime_config.playback.device);

        testPreInitAndInvalidInit(parsed_config, config_path);
        testRuntimeManager(runtime_config, real_audio);
        testStopAndStopAllWithQueuedPlayback(runtime_config);

        logLine("INFO", "AudioManager full API test finished");
    }
    catch (const std::exception& ex)
    {
        logLine("ERROR", ex.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
