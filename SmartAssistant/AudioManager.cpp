#include "AudioManager.h"

#include <alsa/asoundlib.h>
#include <toml++/toml.hpp>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace smartassistant
{
namespace
{

quill::Logger* audioLogger()
{
    static std::once_flag flag;
    static quill::Logger* logger{nullptr};
    std::call_once(flag, []() {
        quill::Backend::start();
        auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("audio_manager_console");
        logger = quill::Frontend::create_or_get_logger("AudioManager", std::move(sink));
        logger->set_log_level(quill::LogLevel::Info);
    });
    return logger;
}

float clamp01(float value)
{
    return std::max(0.0F, std::min(1.0F, value));
}

float streamGain(const VolumeConfig& volume, PlaybackType type)
{
    switch (type)
    {
    case PlaybackType::Alarm:
        return volume.alarm;
    case PlaybackType::Tts:
        return volume.tts;
    case PlaybackType::Prompt:
    case PlaybackType::System:
        return volume.prompt;
    case PlaybackType::Media:
    case PlaybackType::Bluetooth:
        return volume.media;
    }
    return volume.media;
}

std::int16_t limitToInt16(float value)
{
    value = std::max(-32768.0F, std::min(32767.0F, value));
    return static_cast<std::int16_t>(std::lrint(value));
}

template <typename Node>
std::vector<int> loadIntArray(const toml::node_view<Node>& node)
{
    std::vector<int> values;
    if (auto array = node.as_array())
    {
        for (const auto& item : *array)
        {
            if (auto value = item.template value<int64_t>())
            {
                values.push_back(static_cast<int>(*value));
            }
        }
    }
    return values;
}

void applyHardwareParams(snd_pcm_t* pcm, const DeviceConfig& config, snd_pcm_stream_t stream)
{
    snd_pcm_hw_params_t* params{nullptr};
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    unsigned int rate = static_cast<unsigned int>(config.sample_rate);
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr);
    snd_pcm_hw_params_set_channels(pcm, params, static_cast<unsigned int>(config.channels));
    snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(std::max(1, config.sample_rate * config.frame_ms / 1000));
    snd_pcm_uframes_t buffer = period * (stream == SND_PCM_STREAM_CAPTURE ? 20 : 5);
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period, nullptr);
    snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer);
    int rc = snd_pcm_hw_params(pcm, params);
    if (rc < 0)
    {
        throw std::runtime_error(snd_strerror(rc));
    }
}

std::vector<AudioDeviceInfo> listAlsaDevices(snd_pcm_stream_t stream)
{
    std::vector<AudioDeviceInfo> devices;
    void** hints{nullptr};
    if (snd_device_name_hint(-1, "pcm", &hints) != 0 || hints == nullptr)
    {
        return devices;
    }
    for (void** hint = hints; *hint != nullptr; ++hint)
    {
        char* name = snd_device_name_get_hint(*hint, "NAME");
        char* desc = snd_device_name_get_hint(*hint, "DESC");
        char* io = snd_device_name_get_hint(*hint, "IOID");
        bool include = io == nullptr || std::strcmp(io, stream == SND_PCM_STREAM_CAPTURE ? "Input" : "Output") == 0;
        if (name != nullptr && include)
        {
            devices.push_back({name, desc != nullptr ? desc : name});
        }
        if (name != nullptr)
        {
            std::free(name);
        }
        if (desc != nullptr)
        {
            std::free(desc);
        }
        if (io != nullptr)
        {
            std::free(io);
        }
    }
    snd_device_name_free_hint(hints);
    if (devices.empty())
    {
        devices.push_back({"default", "ALSA default PCM"});
    }
    return devices;
}

}

struct AudioManager::Impl
{
    struct Consumer
    {
        FrameCallback callback;
        std::size_t max_queue_depth{0};
        std::size_t queued{0};
        std::uint64_t dropped_frames{0};
    };

    struct PlaybackItem
    {
        PlaybackHandle handle{0};
        PlaybackRequest request;
        std::size_t offset{0};
    };

    AudioConfig config;
    mutable std::mutex mutex;
    std::condition_variable playback_cv;
    std::unordered_map<ConsumerHandle, Consumer> consumers;
    std::vector<AudioEventCallback> subscribers;
    std::deque<PlaybackItem> playback_queue;
    std::thread capture_thread;
    std::thread playback_thread;
    std::atomic<bool> initialized{false};
    std::atomic<bool> capture_running{false};
    std::atomic<bool> playback_running{false};
    std::atomic<bool> shutdown_requested{false};
    std::atomic<ConsumerHandle> next_consumer{1};
    std::atomic<PlaybackHandle> next_playback{1};
    HealthStatus health;

    void emit(AudioEvent event)
    {
        std::vector<AudioEventCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callbacks = subscribers;
        }
        for (const auto& callback : callbacks)
        {
            if (callback)
            {
                callback(event);
            }
        }
    }

    AudioFrame processCaptureFrame(const std::vector<std::int16_t>& raw)
    {
        auto start = std::chrono::steady_clock::now();
        const int in_channels = std::max(1, config.capture.channels);
        const int in_frames = static_cast<int>(raw.size()) / in_channels;
        const int out_rate = std::max(1, config.processing.output_sample_rate);
        const int decimation = std::max(1, config.capture.sample_rate / out_rate);
        AudioFrame frame;
        frame.sample_rate = out_rate;
        frame.channels = std::max(1, config.processing.output_channels);
        frame.samples.reserve(static_cast<std::size_t>(std::max(1, in_frames / decimation)));
        for (int i = 0; i < in_frames; i += decimation)
        {
            int sum = 0;
            int count = 0;
            if (!config.capture.channel_map.empty())
            {
                for (int channel : config.capture.channel_map)
                {
                    if (channel >= 0 && channel < in_channels)
                    {
                        sum += raw[static_cast<std::size_t>(i * in_channels + channel)];
                        ++count;
                    }
                }
            }
            if (count == 0)
            {
                for (int channel = 0; channel < in_channels; ++channel)
                {
                    sum += raw[static_cast<std::size_t>(i * in_channels + channel)];
                }
                count = in_channels;
            }
            frame.samples.push_back(static_cast<std::int16_t>(sum / count));
        }
        auto end = std::chrono::steady_clock::now();
        health.processing_cost_ms = std::chrono::duration<double, std::milli>(end - start).count();
        return frame;
    }

    void dispatch(const AudioFrame& frame)
    {
        std::vector<FrameCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& item : consumers)
            {
                auto& consumer = item.second;
                if (consumer.queued >= consumer.max_queue_depth)
                {
                    ++consumer.dropped_frames;
                    ++health.consumer_dropped_frames_total;
                    continue;
                }
                ++consumer.queued;
                callbacks.push_back(consumer.callback);
                --consumer.queued;
            }
            ++health.dispatched_frames_total;
        }
        for (const auto& callback : callbacks)
        {
            if (callback)
            {
                callback(frame);
            }
        }
    }

    void captureLoop()
    {
        snd_pcm_t* pcm{nullptr};
        int rc = snd_pcm_open(&pcm, config.capture.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
        if (rc < 0)
        {
            LOG_ERROR(audioLogger(), "open capture device {} failed: {}", config.capture.device, snd_strerror(rc));
            capture_running = false;
            health.capture_running = false;
            emit({AudioEventType::CaptureError, 0, snd_strerror(rc)});
            emit({AudioEventType::DeviceFailed, 0, config.capture.device});
            return;
        }
        try
        {
            applyHardwareParams(pcm, config.capture, SND_PCM_STREAM_CAPTURE);
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR(audioLogger(), "configure capture device failed: {}", ex.what());
            snd_pcm_close(pcm);
            capture_running = false;
            health.capture_running = false;
            emit({AudioEventType::CaptureError, 0, ex.what()});
            return;
        }
        const int frames = std::max(1, config.capture.sample_rate * config.capture.frame_ms / 1000);
        std::vector<std::int16_t> buffer(static_cast<std::size_t>(frames * std::max(1, config.capture.channels)));
        health.capture_running = true;
        emit({AudioEventType::CaptureStarted, 0, {}});
        while (capture_running && !shutdown_requested)
        {
            auto start = std::chrono::steady_clock::now();
            snd_pcm_sframes_t read_frames = snd_pcm_readi(pcm, buffer.data(), static_cast<snd_pcm_uframes_t>(frames));
            if (read_frames == -EPIPE)
            {
                ++health.capture_overrun_total;
                snd_pcm_prepare(pcm);
                continue;
            }
            if (read_frames < 0)
            {
                LOG_ERROR(audioLogger(), "capture read failed: {}", snd_strerror(static_cast<int>(read_frames)));
                emit({AudioEventType::CaptureError, 0, snd_strerror(static_cast<int>(read_frames))});
                break;
            }
            std::vector<std::int16_t> raw(buffer.begin(), buffer.begin() + read_frames * std::max(1, config.capture.channels));
            AudioFrame frame = processCaptureFrame(raw);
            dispatch(frame);
            ++health.capture_frames_total;
            auto end = std::chrono::steady_clock::now();
            health.capture_latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
        }
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
        health.capture_running = false;
        emit({AudioEventType::CaptureStopped, 0, {}});
    }

    void playbackLoop()
    {
        snd_pcm_t* pcm{nullptr};
        int rc = snd_pcm_open(&pcm, config.playback.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (rc < 0)
        {
            LOG_ERROR(audioLogger(), "open playback device {} failed: {}", config.playback.device, snd_strerror(rc));
            playback_running = false;
            health.playback_running = false;
            emit({AudioEventType::PlaybackError, 0, snd_strerror(rc)});
            return;
        }
        try
        {
            applyHardwareParams(pcm, config.playback, SND_PCM_STREAM_PLAYBACK);
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR(audioLogger(), "configure playback device failed: {}", ex.what());
            snd_pcm_close(pcm);
            playback_running = false;
            health.playback_running = false;
            emit({AudioEventType::PlaybackError, 0, ex.what()});
            return;
        }
        health.playback_running = true;
        const int channels = std::max(1, config.playback.channels);
        const int frames = std::max(1, config.playback.sample_rate * config.playback.frame_ms / 1000);
        while (playback_running && !shutdown_requested)
        {
            PlaybackItem item;
            {
                std::unique_lock<std::mutex> lock(mutex);
                playback_cv.wait(lock, [&]() { return !playback_queue.empty() || !playback_running || shutdown_requested; });
                if (!playback_running || shutdown_requested)
                {
                    break;
                }
                item = playback_queue.front();
                playback_queue.pop_front();
            }
            emit({AudioEventType::PlaybackStarted, item.handle, {}});
            const float gain = clamp01(config.volume.master) * clamp01(streamGain(config.volume, item.request.type));
            while (playback_running && !shutdown_requested && item.offset < item.request.pcm.size())
            {
                auto start = std::chrono::steady_clock::now();
                const std::size_t samples = std::min<std::size_t>(static_cast<std::size_t>(frames * channels), item.request.pcm.size() - item.offset);
                std::vector<std::int16_t> out(samples);
                for (std::size_t i = 0; i < samples; ++i)
                {
                    out[i] = limitToInt16(static_cast<float>(item.request.pcm[item.offset + i]) * gain);
                }
                snd_pcm_sframes_t written = snd_pcm_writei(pcm, out.data(), static_cast<snd_pcm_uframes_t>(samples / channels));
                if (written == -EPIPE)
                {
                    ++health.playback_underrun_total;
                    snd_pcm_prepare(pcm);
                    continue;
                }
                if (written < 0)
                {
                    LOG_ERROR(audioLogger(), "playback write failed: {}", snd_strerror(static_cast<int>(written)));
                    emit({AudioEventType::PlaybackError, item.handle, snd_strerror(static_cast<int>(written))});
                    break;
                }
                item.offset += static_cast<std::size_t>(written) * channels;
                auto end = std::chrono::steady_clock::now();
                health.playback_latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
            }
            if (item.request.loop && playback_running && !shutdown_requested)
            {
                item.offset = 0;
                std::lock_guard<std::mutex> lock(mutex);
                playback_queue.push_back(std::move(item));
                playback_cv.notify_one();
            }
            else
            {
                emit({AudioEventType::PlaybackCompleted, item.handle, {}});
            }
        }
        snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
        health.playback_running = false;
    }
};

AudioConfig loadAudioConfigFromToml(const std::string& config_path)
{
    AudioConfig config;
    toml::table table = toml::parse_file(config_path);
    auto audio = table["audio"];
    config.backend = audio["backend"].value_or(config.backend);
    config.capture.device = audio["capture"]["device"].value_or(config.capture.device);
    config.capture.sample_rate = static_cast<int>(audio["capture"]["sample_rate"].value_or(config.capture.sample_rate));
    config.capture.channels = static_cast<int>(audio["capture"]["channels"].value_or(config.capture.channels));
    config.capture.format = audio["capture"]["format"].value_or(config.capture.format);
    config.capture.frame_ms = static_cast<int>(audio["capture"]["frame_ms"].value_or(config.capture.frame_ms));
    config.capture.channel_map = loadIntArray(audio["capture"]["channel_map"]);
    config.playback.device = audio["playback"]["device"].value_or(config.playback.device);
    config.playback.sample_rate = static_cast<int>(audio["playback"]["sample_rate"].value_or(config.playback.sample_rate));
    config.playback.channels = static_cast<int>(audio["playback"]["channels"].value_or(config.playback.channels));
    config.playback.format = audio["playback"]["format"].value_or(config.playback.format);
    config.playback.frame_ms = static_cast<int>(audio["playback"]["frame_ms"].value_or(config.playback.frame_ms));
    config.processing.pipeline = audio["processing"]["pipeline"].value_or(config.processing.pipeline);
    config.processing.aec = audio["processing"]["aec"].value_or(config.processing.aec);
    config.processing.ns = audio["processing"]["ns"].value_or(config.processing.ns);
    config.processing.agc = audio["processing"]["agc"].value_or(config.processing.agc);
    config.processing.vad = audio["processing"]["vad"].value_or(config.processing.vad);
    config.processing.beamforming = audio["processing"]["beamforming"].value_or(config.processing.beamforming);
    config.processing.output_sample_rate = static_cast<int>(audio["processing"]["output_sample_rate"].value_or(config.processing.output_sample_rate));
    config.processing.output_channels = static_cast<int>(audio["processing"]["output_channels"].value_or(config.processing.output_channels));
    config.processing.aec_reference_delay_ms = static_cast<int>(audio["processing"]["aec_reference_delay_ms"].value_or(config.processing.aec_reference_delay_ms));
    config.volume.master = static_cast<float>(audio["volume"]["master"].value_or(config.volume.master));
    config.volume.tts = static_cast<float>(audio["volume"]["tts"].value_or(config.volume.tts));
    config.volume.prompt = static_cast<float>(audio["volume"]["prompt"].value_or(config.volume.prompt));
    config.volume.media = static_cast<float>(audio["volume"]["media"].value_or(config.volume.media));
    config.volume.alarm = static_cast<float>(audio["volume"]["alarm"].value_or(config.volume.alarm));
    config.volume.ducking_gain = static_cast<float>(audio["volume"]["ducking_gain"].value_or(config.volume.ducking_gain));
    config.max_recover_retries = static_cast<int>(audio["device"]["max_recover_retries"].value_or(config.max_recover_retries));
    config.recover_backoff_base_ms = static_cast<int>(audio["device"]["recover_backoff_base_ms"].value_or(config.recover_backoff_base_ms));
    config.enable_metrics = audio["diagnostics"]["enable_metrics"].value_or(config.enable_metrics);
    config.enable_audio_dump = audio["diagnostics"]["enable_audio_dump"].value_or(config.enable_audio_dump);
    config.dump_dir = audio["diagnostics"]["dump_dir"].value_or(config.dump_dir);
    return config;
}

AudioManager::AudioManager() : impl_(std::make_unique<Impl>())
{
}

AudioManager::~AudioManager()
{
    shutdown();
}

bool AudioManager::init(const AudioConfig& config)
{
    if (config.backend != "alsa")
    {
        LOG_ERROR(audioLogger(), "unsupported audio backend {}", config.backend);
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->config = config;
    impl_->config.volume.master = clamp01(impl_->config.volume.master);
    impl_->health.initialized = true;
    impl_->health.aec_reference_delay_ms = config.processing.aec_reference_delay_ms;
    impl_->initialized = true;
    impl_->shutdown_requested = false;
    LOG_INFO(audioLogger(), "AudioManager initialized with capture device {} and playback device {}", config.capture.device, config.playback.device);
    return true;
}

bool AudioManager::init(const std::string& config_path)
{
    try
    {
        return init(loadAudioConfigFromToml(config_path));
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR(audioLogger(), "load audio config {} failed: {}", config_path, ex.what());
        return false;
    }
}

void AudioManager::shutdown()
{
    if (!impl_ || !impl_->initialized)
    {
        return;
    }
    impl_->shutdown_requested = true;
    stopCapture();
    stopAll();
    impl_->playback_running = false;
    impl_->playback_cv.notify_all();
    if (impl_->playback_thread.joinable())
    {
        impl_->playback_thread.join();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->health.initialized = false;
    impl_->initialized = false;
}

bool AudioManager::startCapture()
{
    if (!impl_->initialized || impl_->capture_running)
    {
        return false;
    }
    impl_->capture_running = true;
    impl_->capture_thread = std::thread([this]() { impl_->captureLoop(); });
    return true;
}

void AudioManager::stopCapture()
{
    impl_->capture_running = false;
    if (impl_->capture_thread.joinable())
    {
        impl_->capture_thread.join();
    }
}

ConsumerHandle AudioManager::addFrameConsumer(FrameCallback callback, std::size_t max_queue_depth)
{
    if (!callback || max_queue_depth == 0)
    {
        return 0;
    }
    ConsumerHandle handle = impl_->next_consumer++;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->consumers.emplace(handle, Impl::Consumer{std::move(callback), max_queue_depth, 0, 0});
    return handle;
}

void AudioManager::removeFrameConsumer(ConsumerHandle handle)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->consumers.erase(handle);
}

PlaybackHandle AudioManager::play(const PlaybackRequest& request)
{
    if (!impl_->initialized || request.pcm.empty())
    {
        return 0;
    }
    if (!impl_->playback_running)
    {
        impl_->playback_running = true;
        impl_->playback_thread = std::thread([this]() { impl_->playbackLoop(); });
    }
    PlaybackHandle handle = impl_->next_playback++;
    std::vector<PlaybackHandle> interrupted;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (request.interrupt)
        {
            for (const auto& item : impl_->playback_queue)
            {
                interrupted.push_back(item.handle);
            }
            impl_->playback_queue.clear();
        }
        impl_->playback_queue.push_back({handle, request, 0});
    }
    for (PlaybackHandle interrupted_handle : interrupted)
    {
        impl_->emit({AudioEventType::PlaybackInterrupted, interrupted_handle, {}});
    }
    impl_->playback_cv.notify_one();
    return handle;
}

void AudioManager::stop(PlaybackHandle handle)
{
    bool interrupted = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = std::remove_if(impl_->playback_queue.begin(), impl_->playback_queue.end(), [&](const Impl::PlaybackItem& item) { return item.handle == handle; });
        if (it != impl_->playback_queue.end())
        {
            impl_->playback_queue.erase(it, impl_->playback_queue.end());
            interrupted = true;
        }
    }
    if (interrupted)
    {
        impl_->emit({AudioEventType::PlaybackInterrupted, handle, {}});
    }
}

void AudioManager::stopAll()
{
    std::vector<PlaybackHandle> interrupted;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& item : impl_->playback_queue)
        {
            interrupted.push_back(item.handle);
        }
        impl_->playback_queue.clear();
    }
    for (PlaybackHandle handle : interrupted)
    {
        impl_->emit({AudioEventType::PlaybackInterrupted, handle, {}});
    }
}

void AudioManager::setMasterVolume(float volume)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->config.volume.master = clamp01(volume);
    }
    impl_->emit({AudioEventType::VolumeChanged, 0, {}});
}

float AudioManager::masterVolume() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->config.volume.master;
}

std::vector<AudioDeviceInfo> AudioManager::listInputDevices() const
{
    return listAlsaDevices(SND_PCM_STREAM_CAPTURE);
}

std::vector<AudioDeviceInfo> AudioManager::listOutputDevices() const
{
    return listAlsaDevices(SND_PCM_STREAM_PLAYBACK);
}

HealthStatus AudioManager::health() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    HealthStatus status = impl_->health;
    status.capture_running = impl_->capture_running;
    status.playback_running = impl_->playback_running;
    status.initialized = impl_->initialized;
    return status;
}

void AudioManager::subscribe(AudioEventCallback callback)
{
    if (!callback)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->subscribers.push_back(std::move(callback));
}

const AudioConfig& AudioManager::config() const
{
    return impl_->config;
}

}
