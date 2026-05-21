// AudioManager.cpp
// Full implementation following AUDIO_MANAGER_ARCHITECTURE.md.
//
// Threading model:
//   - CaptureThread       (SCHED_FIFO) : ALSA read -> CaptureRingBuf
//   - PlaybackThread      (SCHED_FIFO) : Mixer -> ALSA write + AEC reference tap
//   - ProcessingThread    (normal)     : DSP chain, dispatch frames
//   - ControlThread       (normal)     : API, state machine, event delivery
//   - DeviceThread        (normal)     : udev hot-plug netlink listener
//   - DiagnosticsThread   (normal)     : metrics & PCM dump
//
// Real-time threads must not call malloc, mutex, log formatting, or blocking IO.
// They use lock-free SPSC ring buffers and an MPSC event queue to communicate.

#include "AudioManager.h"

#include <alsa/asoundlib.h>

#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include <toml++/toml.hpp>

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/FileSink.h"

// External DSP / system libraries.
#include <soxr.h>
#include <libudev.h>

#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>

namespace audio {
namespace {

// ============================================================================
// Logger
// ============================================================================

std::once_flag                  g_logger_once;
quill::Logger*                  g_logger = nullptr;

quill::Logger* getLogger() {
    std::call_once(g_logger_once, [] {
        quill::BackendOptions opts;
        opts.thread_name = "quill_backend";
        quill::Backend::start(opts);

        // Console sink for live tracing.
        auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "audio_console_sink");

        // Optional file sink for persistent logs.
        std::vector<std::shared_ptr<quill::Sink>> sinks;
        sinks.push_back(std::move(console_sink));
        try {
            quill::FileSinkConfig fcfg;
            fcfg.set_open_mode('a');
            auto file_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
                "/tmp/audio_manager.log", fcfg);
            sinks.push_back(std::move(file_sink));
        } catch (...) {
            // File sink is best-effort; console is enough.
        }
        g_logger = quill::Frontend::create_or_get_logger(
            "audio", std::move(sinks),
            quill::PatternFormatterOptions{
                "%(time) [%(thread_id)] %(log_level:<8) %(logger:<10) %(message)"});
    });
    return g_logger;
}

#define ALOG_INFO(...)  LOG_INFO(getLogger(), __VA_ARGS__)
#define ALOG_WARN(...)  LOG_WARNING(getLogger(), __VA_ARGS__)
#define ALOG_ERR(...)   LOG_ERROR(getLogger(), __VA_ARGS__)
#define ALOG_DEBUG(...) LOG_DEBUG(getLogger(), __VA_ARGS__)

// ============================================================================
// Utilities
// ============================================================================

inline int64_t nowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float linearToLogGain(float linear) {
    // Map [0,1] UI value to a gentle logarithmic gain curve.
    // The previous -80 dB curve made normal values such as 0.75 become 0.1
    // before stream gains were applied, which made playback much quieter than
    // users expect.  Keep 0.0 as silence, and map the rest over a 20 dB range.
    if (linear <= 0.0f) return 0.0f;
    return std::pow(10.0f, (clampf(linear, 0.0f, 1.0f) - 1.0f));
}

void setRealtimePriority(const char* name, int priority) {
    pthread_setname_np(pthread_self(), name);
    sched_param sp{};
    sp.sched_priority = priority;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        // Fall back to normal scheduling – running unprivileged is acceptable
        // for development environments such as WSL.
        ALOG_WARN("Failed to set SCHED_FIFO for {} ({}), falling back to default",
                  name, std::strerror(rc));
    }
}

// ============================================================================
// Lock-free SPSC ring buffer (audio frames)
//
// Producer writes one frame at a time, consumer reads one frame at a time.
// Capacity must be a power of two.  Frames are pre-allocated with reusable
// std::vector storage – callers only push by std::move'ing into the slot.
// ============================================================================

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity_pow2) : mask_(capacity_pow2 - 1), slots_(capacity_pow2) {
        // capacity_pow2 must be a power of two.
    }

    bool push(T&& v) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        if (((w - r) & mask_) == mask_) {
            return false; // full
        }
        slots_[w & mask_] = std::move(v);
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& v) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false; // empty
        v = std::move(slots_[r & mask_]);
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t r = read_.load(std::memory_order_acquire);
        return (w - r) & mask_;
    }

    size_t capacity() const { return mask_; }

private:
    const size_t                   mask_;
    std::vector<T>                 slots_;
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
};

// ============================================================================
// Real-time event queue – posts events from RT threads to ControlThread.
// MPSC, bounded; uses a fixed array of preformatted events.
// ============================================================================

struct RtEvent {
    AudioEventType type;
    uint64_t       handle = 0;
    int            code   = 0;
};

class RtEventQueue {
public:
    explicit RtEventQueue(size_t cap_pow2) : mask_(cap_pow2 - 1), slots_(cap_pow2) {}

    bool push(const RtEvent& ev) {
        size_t w = write_.load(std::memory_order_relaxed);
        while (true) {
            const size_t r = read_.load(std::memory_order_acquire);
            if (((w - r) & mask_) == mask_) return false;
            if (write_.compare_exchange_weak(w, w + 1, std::memory_order_acq_rel)) {
                slots_[w & mask_] = ev;
                committed_.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
    }

    bool pop(RtEvent& ev) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t c = committed_.load(std::memory_order_acquire);
        if (r == c) return false;
        ev = slots_[r & mask_];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    const size_t                   mask_;
    std::vector<RtEvent>           slots_;
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> committed_{0};
    alignas(64) std::atomic<size_t> read_{0};
};

// ============================================================================
// ALSA Backend
// ============================================================================

class AlsaBackend final : public IAudioBackend {
public:
    AlsaBackend() = default;
    ~AlsaBackend() override {
        closeInput();
        closeOutput();
    }

    BackendError openInput(const DeviceConfig& cfg, NegotiatedFormat& neg) override {
        BackendError err = openPcm(cfg, neg, /*capture=*/true, in_pcm_, in_period_frames_, in_channels_);
        if (err == BackendError::kOk) in_rate_ = neg.sample_rate;
        return err;
    }

    BackendError openOutput(const DeviceConfig& cfg, NegotiatedFormat& neg) override {
        BackendError err = openPcm(cfg, neg, /*capture=*/false, out_pcm_, out_period_frames_, out_channels_);
        if (err == BackendError::kOk) out_rate_ = neg.sample_rate;
        return err;
    }

    BackendError readInput(AudioFrame& frame) override {
        if (!in_pcm_) return BackendError::kInvalidState;
        const int channels = in_channels_;
        const int period   = in_period_frames_;
        if ((int)frame.samples.size() != period * channels) {
            frame.samples.resize(period * channels);
        }
        snd_pcm_sframes_t got = snd_pcm_readi(in_pcm_, frame.samples.data(), period);
        if (got == -EPIPE) {
            snd_pcm_prepare(in_pcm_);
            return BackendError::kXrun;
        }
        if (got == -ESTRPIPE) {
            int rc = 0;
            while ((rc = snd_pcm_resume(in_pcm_)) == -EAGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (rc < 0) snd_pcm_prepare(in_pcm_);
            return BackendError::kXrun;
        }
        if (got == -ENODEV || got == -ENOENT) return BackendError::kDeviceGone;
        if (got < 0) return BackendError::kIOError;
        if (got != period) {
            frame.samples.resize((size_t)got * channels);
        }
        frame.channels    = channels;
        frame.sample_rate = in_rate_;
        frame.timestamp_us = nowMicros();
        return BackendError::kOk;
    }

    BackendError writeOutput(const AudioFrame& frame) override {
        if (!out_pcm_) return BackendError::kInvalidState;
        const int channels = out_channels_;
        const int frames   = (int)frame.samples.size() / channels;
        snd_pcm_sframes_t wrote = snd_pcm_writei(out_pcm_, frame.samples.data(), frames);
        if (wrote == -EPIPE) {
            snd_pcm_prepare(out_pcm_);
            return BackendError::kXrun;
        }
        if (wrote == -ESTRPIPE) {
            int rc = 0;
            while ((rc = snd_pcm_resume(out_pcm_)) == -EAGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (rc < 0) snd_pcm_prepare(out_pcm_);
            return BackendError::kXrun;
        }
        if (wrote == -ENODEV || wrote == -ENOENT) return BackendError::kDeviceGone;
        if (wrote < 0) return BackendError::kIOError;
        return BackendError::kOk;
    }

    AudioDeviceList listInputDevices() override  { return enumerate(/*capture=*/true); }
    AudioDeviceList listOutputDevices() override { return enumerate(/*capture=*/false); }

    void closeInput() override {
        if (in_pcm_) {
            snd_pcm_drop(in_pcm_);
            snd_pcm_close(in_pcm_);
            in_pcm_ = nullptr;
        }
    }

    void closeOutput() override {
        if (out_pcm_) {
            snd_pcm_drain(out_pcm_);
            snd_pcm_close(out_pcm_);
            out_pcm_ = nullptr;
        }
    }

private:
    static BackendError openPcm(const DeviceConfig& cfg, NegotiatedFormat& neg,
                                bool capture, snd_pcm_t*& pcm,
                                int& period_frames, int& channels_out) {
        if (pcm) {
            snd_pcm_close(pcm);
            pcm = nullptr;
        }
        const snd_pcm_stream_t stream =
            capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;

        int err = snd_pcm_open(&pcm, cfg.device_name.c_str(), stream, 0);
        if (err < 0) {
            ALOG_ERR("snd_pcm_open({}, {}) failed: {}", cfg.device_name,
                     capture ? "capture" : "playback", snd_strerror(err));
            return BackendError::kDeviceGone;
        }

        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm, hw);
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);

        unsigned int rate = (unsigned int)cfg.sample_rate;
        snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
        unsigned int channels = (unsigned int)cfg.channels;
        snd_pcm_hw_params_set_channels_near(pcm, hw, &channels);

        snd_pcm_uframes_t period = (snd_pcm_uframes_t)(rate * cfg.frame_ms / 1000);
        if (period == 0) period = (snd_pcm_uframes_t)(rate / 100);
        snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);

        snd_pcm_uframes_t buffer = period * 4;
        snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

        err = snd_pcm_hw_params(pcm, hw);
        if (err < 0) {
            ALOG_ERR("snd_pcm_hw_params failed: {}", snd_strerror(err));
            snd_pcm_close(pcm);
            pcm = nullptr;
            return BackendError::kInvalidState;
        }

        snd_pcm_uframes_t actual_period = 0, actual_buffer = 0;
        snd_pcm_hw_params_get_period_size(hw, &actual_period, nullptr);
        snd_pcm_hw_params_get_buffer_size(hw, &actual_buffer);

        neg.sample_rate   = (int)rate;
        neg.channels      = (int)channels;
        neg.period_frames = (int)actual_period;
        neg.buffer_frames = (int)actual_buffer;
        neg.is_float      = false;

        period_frames = (int)actual_period;
        channels_out  = (int)channels;

        snd_pcm_prepare(pcm);
        ALOG_INFO("ALSA {} opened device='{}' rate={} ch={} period={} buf={}",
                  capture ? "input" : "output", cfg.device_name,
                  neg.sample_rate, neg.channels,
                  neg.period_frames, neg.buffer_frames);
        return BackendError::kOk;
    }

    AudioDeviceList enumerate(bool capture) {
        AudioDeviceList list;
        // Always offer the canonical aliases; this works in WSL/PulseAudio too.
        list.push_back({"default", "ALSA default", 2, true});
        list.push_back({"pulse", "PulseAudio plugin", 2, false});

        void** hints = nullptr;
        int err = snd_device_name_hint(-1, "pcm", &hints);
        if (err == 0 && hints) {
            for (void** h = hints; *h != nullptr; ++h) {
                char* name  = snd_device_name_get_hint(*h, "NAME");
                char* desc  = snd_device_name_get_hint(*h, "DESC");
                char* ioid  = snd_device_name_get_hint(*h, "IOID");
                bool match  = (ioid == nullptr)
                                  ? true
                                  : (capture ? std::strcmp(ioid, "Input") == 0
                                             : std::strcmp(ioid, "Output") == 0);
                if (match && name) {
                    AudioDeviceInfo info;
                    info.id           = name;
                    info.name         = desc ? desc : name;
                    info.max_channels = 2;
                    list.push_back(info);
                }
                if (name) free(name);
                if (desc) free(desc);
                if (ioid) free(ioid);
            }
            snd_device_name_free_hint(hints);
        }
        return list;
    }

    snd_pcm_t* in_pcm_  = nullptr;
    snd_pcm_t* out_pcm_ = nullptr;
    int        in_period_frames_  = 0;
    int        out_period_frames_ = 0;
    int        in_channels_       = 0;
    int        out_channels_      = 0;
    int        in_rate_           = 48000;
    int        out_rate_          = 48000;
};

} // namespace
} // namespace audio

// ============================================================================
// DSP wrappers (WebRTC APM + libsoxr)
// ============================================================================

namespace audio {
namespace {

// ----------------------------------------------------------------------------
// WebRtcApmWrapper
// Wraps the entire WebRTC AudioProcessing module providing:
//   - HighPassFilter   (DC removal, ~80 Hz cut-off)
//   - EchoCancellation (per-channel AEC, delay-based)
//   - NoiseSuppression (kHighSuppression)
//   - GainControl      (kAdaptiveDigital, target -3 dBFS)
//   - VoiceDetection   (kLowLikelihood)
//
// WebRTC APM operates on 10 ms int16 frames at NativeRates (8/16/32/48 kHz).
// We feed it directly with the captured int16 PCM; reverse stream is the
// playback audio used as AEC reference.
// ----------------------------------------------------------------------------
class WebRtcApmWrapper {
public:
    WebRtcApmWrapper() = default;
    ~WebRtcApmWrapper() { delete apm_; apm_ = nullptr; }

    bool init(int input_rate, int input_channels,
              int output_rate, int output_channels,
              int reverse_rate,
              bool aec, bool ns, bool agc, bool vad,
              const ProcessingConfig& cfg) {
        (void)cfg;
        (void)output_rate;
        (void)output_channels;
        apm_ = webrtc::AudioProcessing::Create();
        if (!apm_) return false;

        // The int16 ProcessStream interface requires input == output (same
        // rate and same number of channels).  We therefore configure the APM
        // to keep the original capture rate / channels and rely on
        // SoxrResampler + a downstream channel reducer to produce the
        // 16 kHz mono stream demanded by the public API.
        webrtc::ProcessingConfig pc;
        pc.input_stream().set_sample_rate_hz(input_rate);
        pc.input_stream().set_num_channels(input_channels);
        pc.output_stream().set_sample_rate_hz(input_rate);
        pc.output_stream().set_num_channels(input_channels);
        pc.reverse_input_stream().set_sample_rate_hz(reverse_rate);
        pc.reverse_input_stream().set_num_channels(1);
        pc.reverse_output_stream().set_sample_rate_hz(reverse_rate);
        pc.reverse_output_stream().set_num_channels(1);
        if (apm_->Initialize(pc) != webrtc::AudioProcessing::kNoError) {
            ALOG_ERR("WebRTC APM Initialize failed");
            delete apm_; apm_ = nullptr;
            return false;
        }

        // High-pass filter (always on – architecture mandates DC removal).
        apm_->high_pass_filter()->Enable(true);

        if (aec) {
            apm_->echo_cancellation()->enable_drift_compensation(false);
            apm_->echo_cancellation()->enable_metrics(true);
            apm_->echo_cancellation()->enable_delay_logging(true);
            apm_->echo_cancellation()->set_suppression_level(
                webrtc::EchoCancellation::kModerateSuppression);
            apm_->echo_cancellation()->Enable(true);
        }
        if (ns) {
            apm_->noise_suppression()->set_level(
                webrtc::NoiseSuppression::kHigh);
            apm_->noise_suppression()->Enable(true);
        }
        if (agc) {
            apm_->gain_control()->set_mode(webrtc::GainControl::kAdaptiveDigital);
            apm_->gain_control()->set_target_level_dbfs(3); // -3 dBFS
            apm_->gain_control()->set_compression_gain_db(9);
            apm_->gain_control()->enable_limiter(true);
            apm_->gain_control()->Enable(true);
        }
        if (vad) {
            apm_->voice_detection()->set_likelihood(
                webrtc::VoiceDetection::kLowLikelihood);
            apm_->voice_detection()->set_frame_size_ms(10);
            apm_->voice_detection()->Enable(true);
        }
        input_rate_     = input_rate;
        input_channels_ = input_channels;
        output_rate_    = output_rate;
        output_channels_= output_channels;
        reverse_rate_   = reverse_rate;
        return true;
    }

    // Pass the AEC reference (int16 mono).  Caller must ensure 10 ms / frame.
    int analyzeReverse(const int16_t* data, size_t samples_per_channel) {
        if (!apm_) return -1;
        webrtc::AudioFrame f;
        f.UpdateFrame(0, 0, data, samples_per_channel, reverse_rate_,
                      webrtc::AudioFrame::kNormalSpeech,
                      webrtc::AudioFrame::kVadUnknown, 1);
        return apm_->ProcessReverseStream(&f);
    }

    // Process one capture frame in-place.  Returns voice activity if VAD on.
    int processCapture(int16_t* data, size_t samples_per_channel,
                       int delay_ms, bool& voice_active) {
        if (!apm_) return -1;
        webrtc::AudioFrame f;
        f.UpdateFrame(0, 0, data, samples_per_channel, input_rate_,
                      webrtc::AudioFrame::kNormalSpeech,
                      webrtc::AudioFrame::kVadUnknown, input_channels_);
        apm_->set_stream_delay_ms(delay_ms);
        int r = apm_->ProcessStream(&f);
        if (r == webrtc::AudioProcessing::kNoError) {
            std::memcpy(data, f.data_,
                        f.samples_per_channel_ * f.num_channels_ * sizeof(int16_t));
            voice_active = apm_->voice_detection()->stream_has_voice();
        }
        return r;
    }

    int delayMetric() const {
        if (!apm_) return 0;
        int median = 0, std_dev = 0;
        float fraction_poor = 0.0f;
        apm_->echo_cancellation()->GetDelayMetrics(&median, &std_dev,
                                                   &fraction_poor);
        return median;
    }

private:
    webrtc::AudioProcessing* apm_ = nullptr;
    int input_rate_      = 48000;
    int input_channels_  = 1;
    int output_rate_     = 16000;
    int output_channels_ = 1;
    int reverse_rate_    = 48000;
};

// ----------------------------------------------------------------------------
// SoxrResampler
// Stream resampler with variable I/O ratio (set_io_ratio) for clock-drift
// compensation on the reference signal.
// ----------------------------------------------------------------------------
class SoxrResampler {
public:
    SoxrResampler() = default;
    ~SoxrResampler() { close(); }

    bool open(double in_rate, double out_rate, int channels,
              soxr_datatype_t in_type = SOXR_INT16_I,
              soxr_datatype_t out_type = SOXR_INT16_I,
              bool variable_rate = false) {
        close();
        soxr_io_spec_t      io_spec      = soxr_io_spec(in_type, out_type);
        soxr_quality_spec_t quality_spec = soxr_quality_spec(SOXR_HQ, 0);
        soxr_runtime_spec_t runtime_spec = soxr_runtime_spec(1);
        if (variable_rate) runtime_spec.flags |= SOXR_VR;

        soxr_error_t err = nullptr;
        resampler_ = soxr_create(in_rate, out_rate, channels, &err,
                                  &io_spec, &quality_spec, &runtime_spec);
        if (err) {
            ALOG_ERR("soxr_create failed: {}", err);
            resampler_ = nullptr;
            return false;
        }
        in_rate_   = in_rate;
        out_rate_  = out_rate;
        channels_  = channels;
        is_variable_ = variable_rate;
        return true;
    }

    void close() {
        if (resampler_) {
            soxr_delete(resampler_);
            resampler_ = nullptr;
        }
    }

    // Process int16 interleaved input frames -> int16 interleaved output.
    // Returns total output frames per channel produced.
    size_t processI16(const int16_t* in, size_t in_frames,
                      std::vector<int16_t>& out) {
        if (!resampler_) return 0;
        const double ratio   = out_rate_ / in_rate_;
        const size_t out_cap = (size_t)((double)in_frames * ratio) + 32;
        out.assign(out_cap * (size_t)channels_, 0);
        size_t idone = 0, odone = 0;
        soxr_error_t err = soxr_process(resampler_,
                                         in,  in_frames,        &idone,
                                         out.data(), out_cap,   &odone);
        if (err) {
            ALOG_ERR("soxr_process: {}", err);
            out.clear();
            return 0;
        }
        out.resize(odone * (size_t)channels_);
        return odone;
    }

    // For clock-drift compensation: adjust the actual I/O ratio with smooth
    // slewing.  ratio_drift in ppm (parts per million); positive means input
    // clock is faster than output and we should compress accordingly.
    bool setDriftPpm(int64_t ppm) {
        if (!resampler_ || !is_variable_) return false;
        const double new_ratio = (in_rate_ / out_rate_) *
                                 (1.0 + (double)ppm * 1e-6);
        // Slew over 200 ms worth of samples for smoothness.
        const size_t slew = (size_t)(out_rate_ * 0.2);
        return soxr_set_io_ratio(resampler_, new_ratio, slew) == nullptr;
    }

private:
    soxr_t resampler_   = nullptr;
    double in_rate_     = 48000.0;
    double out_rate_    = 16000.0;
    int    channels_    = 1;
    bool   is_variable_ = false;
};

// ----------------------------------------------------------------------------
// DelayCompensator
// A simple ring buffer of int16 samples used to align the AEC reference with
// the captured microphone path by `aec_reference_delay_ms`.
// ----------------------------------------------------------------------------
class DelayCompensator {
public:
    void configure(int sample_rate, int channels, int delay_ms) {
        sample_rate_ = sample_rate;
        channels_    = channels;
        delay_ms_    = delay_ms;
        const size_t need = (size_t)(sample_rate * delay_ms / 1000) * channels;
        // Pre-fill with silence so the first read returns 0 instead of
        // partial data.
        buf_.assign(need + sample_rate * channels, 0);
        write_idx_ = need;
        read_idx_  = 0;
    }
    void push(const int16_t* data, size_t samples) {
        for (size_t i = 0; i < samples; ++i) {
            buf_[write_idx_ % buf_.size()] = data[i];
            ++write_idx_;
        }
    }
    void pop(int16_t* dst, size_t samples) {
        for (size_t i = 0; i < samples; ++i) {
            if (read_idx_ + (sample_rate_ * delay_ms_ / 1000) * channels_ < write_idx_) {
                dst[i] = buf_[read_idx_ % buf_.size()];
                ++read_idx_;
            } else {
                dst[i] = 0;
            }
        }
    }
    int delayMs() const { return delay_ms_; }

private:
    std::vector<int16_t> buf_;
    int                  sample_rate_ = 48000;
    int                  channels_    = 1;
    int                  delay_ms_    = 0;
    size_t               write_idx_   = 0;
    size_t               read_idx_    = 0;
};

// ----------------------------------------------------------------------------
// ChannelMapper : selects/permutes input channels per channel_map config.
// ----------------------------------------------------------------------------
class ChannelMapper {
public:
    void configure(const std::vector<int>& map, int src_channels) {
        map_         = map;
        src_channels_ = src_channels;
    }
    // Map interleaved src to interleaved dst with map_.size() channels.
    void process(const int16_t* src, int16_t* dst, int frames) {
        const int dst_ch = (int)map_.size();
        for (int n = 0; n < frames; ++n) {
            for (int c = 0; c < dst_ch; ++c) {
                const int s = map_[c] < src_channels_ ? map_[c] : 0;
                dst[(size_t)n * dst_ch + c] =
                    src[(size_t)n * src_channels_ + s];
            }
        }
    }
private:
    std::vector<int> map_;
    int              src_channels_ = 0;
};

// ----------------------------------------------------------------------------
// LimiterFloat : soft-knee limiter operating on float32 mix output.
// ----------------------------------------------------------------------------
class LimiterFloat {
public:
    void configure(int sample_rate) {
        const float attack_s  = 0.001f;
        const float release_s = 0.100f;
        attack_coef_  = std::exp(-1.0f / (attack_s  * (float)sample_rate));
        release_coef_ = std::exp(-1.0f / (release_s * (float)sample_rate));
        envelope_     = 0.0f;
    }
    void process(float* data, int frames) {
        const float threshold = 0.95f;
        for (int n = 0; n < frames; ++n) {
            float a = std::fabs(data[n]);
            const float coef = (a > envelope_) ? attack_coef_ : release_coef_;
            envelope_ = coef * envelope_ + (1.0f - coef) * a;
            float gain = 1.0f;
            if (envelope_ > threshold) gain = threshold / envelope_;
            data[n] *= gain;
            if (data[n] >  1.0f) data[n] =  1.0f;
            if (data[n] < -1.0f) data[n] = -1.0f;
        }
    }
private:
    float attack_coef_  = 0.99f;
    float release_coef_ = 0.999f;
    float envelope_     = 0.0f;
};

// ----------------------------------------------------------------------------
// WAV header writer for diagnostic dumps.
// ----------------------------------------------------------------------------
struct WavWriter {
    FILE* fp = nullptr;
    int   sample_rate = 0;
    int   channels    = 0;
    uint32_t data_bytes = 0;

    bool open(const std::string& path, int rate, int ch) {
        fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        sample_rate = rate;
        channels    = ch;
        data_bytes  = 0;
        // Reserve header (44 bytes) – will be patched on close().
        char hdr[44] = {0};
        std::fwrite(hdr, 1, 44, fp);
        return true;
    }
    void write(const int16_t* samples, size_t count) {
        if (!fp) return;
        std::fwrite(samples, sizeof(int16_t), count, fp);
        data_bytes += count * sizeof(int16_t);
    }
    void close() {
        if (!fp) return;
        // Patch RIFF/WAVE header.
        std::fseek(fp, 0, SEEK_SET);
        uint32_t fsize = 36 + data_bytes;
        uint16_t fmt = 1; // PCM
        uint16_t ch = (uint16_t)channels;
        uint32_t rate = (uint32_t)sample_rate;
        uint16_t bits = 16;
        uint32_t byte_rate = rate * ch * (bits / 8);
        uint16_t block = ch * (bits / 8);
        std::fwrite("RIFF", 1, 4, fp);
        std::fwrite(&fsize, 4, 1, fp);
        std::fwrite("WAVEfmt ", 1, 8, fp);
        uint32_t fmt_chunk = 16;
        std::fwrite(&fmt_chunk, 4, 1, fp);
        std::fwrite(&fmt, 2, 1, fp);
        std::fwrite(&ch, 2, 1, fp);
        std::fwrite(&rate, 4, 1, fp);
        std::fwrite(&byte_rate, 4, 1, fp);
        std::fwrite(&block, 2, 1, fp);
        std::fwrite(&bits, 2, 1, fp);
        std::fwrite("data", 1, 4, fp);
        std::fwrite(&data_bytes, 4, 1, fp);
        std::fclose(fp);
        fp = nullptr;
    }
};

// ============================================================================
// FrameDispatcher : per-consumer queue, drops frames when full
// ============================================================================

struct ConsumerSlot {
    ConsumerHandle             handle;
    FrameCallback              cb;
    size_t                     max_depth;
    std::deque<AudioFrame>     queue;
    std::mutex                 mtx;
    std::condition_variable    cv;
    std::atomic<bool>          running{true};
    std::thread                worker;
    std::atomic<uint64_t>      dropped{0};
    std::string                name;
};

class FrameDispatcher {
public:
    ConsumerHandle addConsumer(FrameCallback cb, size_t max_queue_depth,
                               const std::string& name = {}) {
        if (!cb) return kInvalidConsumerHandle;
        auto slot = std::make_shared<ConsumerSlot>();
        slot->handle    = next_handle_++;
        slot->cb        = std::move(cb);
        slot->max_depth = std::max<size_t>(max_queue_depth, 1);
        slot->name      = name.empty() ? std::to_string(slot->handle) : name;
        slot->worker    = std::thread([s = slot]() { workerLoop(s); });
        {
            std::lock_guard<std::mutex> lk(list_mtx_);
            consumers_.push_back(slot);
        }
        ALOG_INFO("FrameDispatcher: consumer added handle={} name={} depth={}",
                  slot->handle, slot->name, slot->max_depth);
        return slot->handle;
    }

    void removeConsumer(ConsumerHandle h) {
        std::shared_ptr<ConsumerSlot> victim;
        {
            std::lock_guard<std::mutex> lk(list_mtx_);
            for (auto it = consumers_.begin(); it != consumers_.end(); ++it) {
                if ((*it)->handle == h) {
                    victim = *it;
                    consumers_.erase(it);
                    break;
                }
            }
        }
        if (!victim) return;
        victim->running.store(false, std::memory_order_release);
        victim->cv.notify_all();
        if (victim->worker.joinable()) victim->worker.join();
        ALOG_INFO("FrameDispatcher: consumer removed handle={}", h);
    }

    void clear() {
        std::vector<std::shared_ptr<ConsumerSlot>> snapshot;
        {
            std::lock_guard<std::mutex> lk(list_mtx_);
            snapshot.swap(consumers_);
        }
        for (auto& s : snapshot) {
            s->running.store(false, std::memory_order_release);
            s->cv.notify_all();
            if (s->worker.joinable()) s->worker.join();
        }
    }

    // Called by ProcessingThread.  Non-blocking; drops on full queue.
    void dispatch(const AudioFrame& frame) {
        std::vector<std::shared_ptr<ConsumerSlot>> snap;
        {
            std::lock_guard<std::mutex> lk(list_mtx_);
            snap = consumers_;
        }
        for (auto& s : snap) {
            std::unique_lock<std::mutex> lk(s->mtx, std::try_to_lock);
            if (!lk.owns_lock()) {
                s->dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (s->queue.size() >= s->max_depth) {
                s->dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            s->queue.push_back(frame);
            s->cv.notify_one();
        }
    }

    uint64_t droppedFrames(const std::string& name) const {
        std::lock_guard<std::mutex> lk(list_mtx_);
        for (auto& s : consumers_) {
            if (s->name == name) return s->dropped.load(std::memory_order_relaxed);
        }
        return 0;
    }

private:
    static void workerLoop(std::shared_ptr<ConsumerSlot> s) {
        pthread_setname_np(pthread_self(), "audio_consumer");
        AudioFrame frame;
        while (s->running.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lk(s->mtx);
                s->cv.wait(lk, [&] {
                    return !s->queue.empty() ||
                           !s->running.load(std::memory_order_acquire);
                });
                if (!s->running.load(std::memory_order_acquire)) break;
                frame = std::move(s->queue.front());
                s->queue.pop_front();
            }
            try {
                s->cb(frame);
            } catch (const std::exception& e) {
                ALOG_ERR("Consumer {} callback threw: {}", s->name, e.what());
            } catch (...) {
                ALOG_ERR("Consumer {} callback threw unknown exception", s->name);
            }
        }
    }

    mutable std::mutex                                  list_mtx_;
    std::vector<std::shared_ptr<ConsumerSlot>>          consumers_;
    std::atomic<ConsumerHandle>                         next_handle_{1};
};

// ============================================================================
// VolumeManager : log-curve gains + 10ms ramping, no system volume changes
// ============================================================================

class VolumeManager {
public:
    void configure(const VolumeConfig& cfg, int sample_rate) {
        cfg_         = cfg;
        sample_rate_ = sample_rate;
        master_target_ = linearToLogGain(cfg.master);
        master_actual_ = master_target_;
        ducking_       = 1.0f;
        ducking_target_ = 1.0f;
        muted_         = false;
    }

    void setMaster(float linear) {
        std::lock_guard<std::mutex> lk(mtx_);
        cfg_.master    = clampf(linear, 0.0f, 1.0f);
        master_target_ = linearToLogGain(cfg_.master);
    }

    float master() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return cfg_.master;
    }

    void setMute(bool m) {
        std::lock_guard<std::mutex> lk(mtx_);
        muted_ = m;
    }

    void setDuckingActive(bool ducking) {
        std::lock_guard<std::mutex> lk(mtx_);
        ducking_target_ = ducking ? cfg_.ducking_gain : 1.0f;
    }

    float streamGainFor(PlaybackPriority p) const {
        std::lock_guard<std::mutex> lk(mtx_);
        switch (p) {
            case PlaybackPriority::TTS:    return cfg_.tts;
            case PlaybackPriority::PROMPT: return cfg_.prompt;
            case PlaybackPriority::MEDIA:  return cfg_.media;
            case PlaybackPriority::ALARM:  return cfg_.alarm;
            case PlaybackPriority::SYSTEM: return cfg_.prompt;
            default:                       return cfg_.media;
        }
    }

    // Compute master gain for one frame; advance ramp by `frames` samples.
    float advanceMaster(int frames) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (muted_) return 0.0f;
        const int   ramp_samples = std::max(1, sample_rate_ * 10 / 1000);
        const float master_step  = (master_target_ - master_actual_) /
                                   (float)ramp_samples;
        master_actual_ += master_step * (float)frames;
        if (std::fabs(master_target_ - master_actual_) < 1e-4f)
            master_actual_ = master_target_;
        return master_actual_;
    }

    // Compute ducking gain for duckable streams only.  This must not be
    // multiplied into the final mix, otherwise foreground TTS is ducked too.
    float advanceDucking(int frames) {
        std::lock_guard<std::mutex> lk(mtx_);
        const int   ramp_samples = std::max(1, sample_rate_ * 10 / 1000);
        const float duck_step    = (ducking_target_ - ducking_) /
                                   (float)ramp_samples;
        ducking_ += duck_step * (float)frames;
        if (std::fabs(ducking_target_ - ducking_) < 1e-4f)
            ducking_ = ducking_target_;
        return ducking_;
    }

    bool muted() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return muted_;
    }

private:
    mutable std::mutex mtx_;
    VolumeConfig       cfg_;
    int                sample_rate_   = 48000;
    float              master_target_ = 0.5f;
    float              master_actual_ = 0.5f;
    float              ducking_       = 1.0f;
    float              ducking_target_ = 1.0f;
    bool               muted_         = false;
};

// ============================================================================
// FocusManager : priority arbitration + ducking signal
// ============================================================================

class FocusManager {
public:
    using DuckingCallback = std::function<void(bool)>;
    using PreemptCallback = std::function<void(PlaybackHandle)>;

    void setDuckingCallback(DuckingCallback cb) { duck_cb_ = std::move(cb); }
    void setPreemptCallback(PreemptCallback cb) { preempt_cb_ = std::move(cb); }

    // Acquire focus for a stream.  Returns true if the stream may play.
    bool acquire(PlaybackHandle h, PlaybackPriority p) {
        std::vector<PlaybackHandle> preempted;
        bool need_duck = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (p == PlaybackPriority::ALARM) {
                for (auto& kv : active_) preempted.push_back(kv.first);
                active_.clear();
                active_[h] = p;
                ducking_active_ = false;
                need_duck = false;
            } else {
                active_[h] = p;
                bool tts_present = false;
                bool media_present = false;
                for (auto& kv : active_) {
                    if (kv.second == PlaybackPriority::TTS) tts_present = true;
                    if (kv.second == PlaybackPriority::MEDIA) media_present = true;
                }
                bool want_duck = tts_present && media_present;
                if (want_duck != ducking_active_) {
                    ducking_active_ = want_duck;
                    need_duck = true;
                }
            }
        }
        if (preempt_cb_) {
            for (auto h2 : preempted) preempt_cb_(h2);
        }
        if (need_duck && duck_cb_) duck_cb_(ducking_active_);
        return true;
    }

    void release(PlaybackHandle h) {
        bool need_duck_change = false;
        bool new_duck = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            active_.erase(h);
            bool tts_present = false;
            bool media_present = false;
            for (auto& kv : active_) {
                if (kv.second == PlaybackPriority::TTS) tts_present = true;
                if (kv.second == PlaybackPriority::MEDIA) media_present = true;
            }
            bool want_duck = tts_present && media_present;
            if (want_duck != ducking_active_) {
                ducking_active_ = want_duck;
                need_duck_change = true;
                new_duck = want_duck;
            }
        }
        if (need_duck_change && duck_cb_) duck_cb_(new_duck);
    }

    void releaseAll() {
        std::vector<PlaybackHandle> handles;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& kv : active_) handles.push_back(kv.first);
            active_.clear();
            ducking_active_ = false;
        }
        if (duck_cb_) duck_cb_(false);
    }

private:
    std::mutex                                  mtx_;
    std::map<PlaybackHandle, PlaybackPriority>  active_;
    bool                                        ducking_active_ = false;
    DuckingCallback                             duck_cb_;
    PreemptCallback                             preempt_cb_;
};

// ============================================================================
// AudioRouter : maps streams -> output device, capture -> input device
// ============================================================================

class AudioRouter {
public:
    void setInputDevice(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        input_device_ = id;
    }
    void setOutputDevice(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        output_device_ = id;
    }
    std::string inputDevice() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return input_device_;
    }
    std::string outputDevice() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return output_device_;
    }
    // Allow forced routing for ALARM (architecture: ALARM -> built-in speaker).
    std::string outputDeviceFor(PlaybackPriority p) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (p == PlaybackPriority::ALARM && !alarm_device_.empty()) return alarm_device_;
        return output_device_;
    }
    void setAlarmDevice(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        alarm_device_ = id;
    }
private:
    mutable std::mutex mtx_;
    std::string        input_device_;
    std::string        output_device_;
    std::string        alarm_device_;
};

// ============================================================================
// Diagnostics : counters + optional PCM dump
// ============================================================================

class Diagnostics {
public:
    void configure(const DiagnosticsConfig& cfg, int cap_rate, int cap_ch,
                   int proc_rate, int proc_ch, int ref_rate, int ref_ch) {
        cfg_ = cfg;
        if (cfg_.enable_audio_dump && !cfg_.dump_dir.empty()) {
            ::mkdir(cfg_.dump_dir.c_str(), 0755);
            const std::string base = cfg_.dump_dir + "/";
            raw_.open(base + "mic_raw.wav",       cap_rate,  cap_ch);
            proc_.open(base + "mic_processed.wav", proc_rate, proc_ch);
            ref_.open(base + "aec_reference.wav",  ref_rate,  ref_ch);
            ALOG_INFO("Diagnostics: WAV dumps enabled at {}", cfg_.dump_dir);
        }
    }

    ~Diagnostics() {
        raw_.close();
        proc_.close();
        ref_.close();
    }

    void dumpRaw(const int16_t* data, size_t count) {
        if (raw_.fp) raw_.write(data, count);
    }
    void dumpProcessed(const int16_t* data, size_t count) {
        if (proc_.fp) proc_.write(data, count);
    }
    void dumpReference(const int16_t* data, size_t count) {
        if (ref_.fp) ref_.write(data, count);
    }

    // Counters (atomic).
    std::atomic<uint64_t> capture_frames_total{0};
    std::atomic<uint64_t> capture_overrun_total{0};
    std::atomic<uint64_t> playback_underrun_total{0};
    std::atomic<uint64_t> device_recover_total{0};
    std::atomic<uint64_t> device_recover_failed_total{0};
    std::atomic<int64_t>  capture_latency_us{0};
    std::atomic<int64_t>  playback_latency_us{0};
    std::atomic<int64_t>  processing_cost_us{0};
    std::atomic<int64_t>  aec_clock_drift_us{0};
    std::atomic<int>      aec_delay_metric_ms{0};
    std::atomic<size_t>   capture_queue_depth{0};
    std::atomic<size_t>   reference_queue_depth{0};

private:
    DiagnosticsConfig cfg_;
    WavWriter         raw_;
    WavWriter         proc_;
    WavWriter         ref_;
};

// ============================================================================
// Per-frame buffer used by CaptureRingBuf / ReferenceRingBuf
// ============================================================================

struct InternalFrame {
    std::vector<int16_t> samples;
    int                  channels    = 0;
    int                  sample_rate = 0;
    int64_t              timestamp_us = 0;
};

// ============================================================================
// CaptureManager : ALSA read -> CaptureRingBuf (CaptureThread, SCHED_FIFO)
// ============================================================================

class CaptureManager {
public:
    CaptureManager(IAudioBackend& backend, SpscRing<InternalFrame>& ring,
                   RtEventQueue& events, Diagnostics& diag)
        : backend_(backend), ring_(ring), events_(events), diag_(diag) {}

    bool open(const CaptureConfig& cfg) {
        cfg_ = cfg;
        DeviceConfig dc;
        dc.device_name = cfg.device;
        dc.sample_rate = cfg.sample_rate;
        dc.channels    = cfg.channels;
        dc.frame_ms    = cfg.frame_ms;
        BackendError err = backend_.openInput(dc, neg_);
        if (err != BackendError::kOk) {
            ALOG_ERR("CaptureManager: openInput failed err={}", (int)err);
            return false;
        }
        return true;
    }

    bool start() {
        if (running_.exchange(true)) return true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        backend_.closeInput();
    }

    bool running() const { return running_.load(std::memory_order_acquire); }
    const NegotiatedFormat& format() const { return neg_; }

private:
    void loop() {
        setRealtimePriority("audio_capture", 80);
        ALOG_INFO("CaptureThread started ({} ch @ {} Hz, {} frames period)",
                  neg_.channels, neg_.sample_rate, neg_.period_frames);
        InternalFrame frame;
        AudioFrame    raw;
        raw.samples.resize((size_t)neg_.period_frames * neg_.channels);
        while (running_.load(std::memory_order_acquire)) {
            const int64_t t0 = nowMicros();
            BackendError err = backend_.readInput(raw);
            const int64_t t1 = nowMicros();
            if (err == BackendError::kOk) {
                diag_.capture_latency_us.store(t1 - t0, std::memory_order_relaxed);
                diag_.capture_frames_total.fetch_add(1, std::memory_order_relaxed);
                frame.samples     = raw.samples; // copy
                frame.channels    = raw.channels;
                frame.sample_rate = raw.sample_rate;
                frame.timestamp_us = raw.timestamp_us;
                if (!ring_.push(std::move(frame))) {
                    diag_.capture_overrun_total.fetch_add(1, std::memory_order_relaxed);
                    events_.push({AudioEventType::CaptureError, 0, 1});
                }
                diag_.capture_queue_depth.store(ring_.size(),
                                                std::memory_order_relaxed);
            } else if (err == BackendError::kXrun) {
                diag_.capture_overrun_total.fetch_add(1, std::memory_order_relaxed);
            } else if (err == BackendError::kDeviceGone || err == BackendError::kIOError) {
                events_.push({AudioEventType::DeviceDisconnected, 0, (int)err});
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else {
                events_.push({AudioEventType::CaptureError, 0, (int)err});
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        ALOG_INFO("CaptureThread stopped");
    }

    IAudioBackend&            backend_;
    SpscRing<InternalFrame>&  ring_;
    RtEventQueue&             events_;
    Diagnostics&              diag_;
    CaptureConfig             cfg_;
    NegotiatedFormat          neg_;
    std::atomic<bool>         running_{false};
    std::thread               thread_;
};

} // namespace
} // namespace audio

// ============================================================================
// AudioProcessingChain : ProcessingThread DSP pipeline
//
// Raw mic frames (N ch / 48k) -> ChannelMapper -> HighPassFilter
//   -> per-channel AEC <- AEC reference -> Beamformer -> NoiseSuppressor
//   -> AGC / VAD -> Resampler (48k -> 16k) -> FrameDispatcher
// ============================================================================

namespace audio {
namespace {

class AudioProcessingChain {
public:
    AudioProcessingChain(SpscRing<InternalFrame>& cap_ring,
                         SpscRing<InternalFrame>& ref_ring,
                         FrameDispatcher&         dispatcher,
                         RtEventQueue&            events,
                         Diagnostics&             diag)
        : cap_ring_(cap_ring), ref_ring_(ref_ring),
          dispatcher_(dispatcher), events_(events), diag_(diag) {}

    bool configure(const ProcessingConfig& cfg, const CaptureConfig& cap,
                   const PlaybackConfig& pb) {
        proc_cfg_ = cfg;
        cap_cfg_  = cap;
        pb_cfg_   = pb;
        const int requested_ch = (int)std::max<size_t>(cap.channel_map.size(),
                                                       (size_t)cap.channels);
        const int in_rate  = cap.sample_rate;
        const int out_rate = cfg.output_sample_rate;
        const int out_ch   = cfg.output_channels;

        mapper_.configure(cap.channel_map, cap.channels);

        // WebRTC APM operates on N->1 channel reduction internally when
        // configured with input_channels=N and output_channels=1, and also
        // applies HPF/AEC/NS/AGC/VAD as a unified module.
        if (!apm_.init(in_rate, requested_ch,
                       in_rate, out_ch,
                       in_rate,
                       cfg.aec, cfg.ns, cfg.agc, cfg.vad, cfg)) {
            return false;
        }

        // Down-sampler: capture rate -> output rate (mono).
        if (!downsampler_.open((double)in_rate, (double)out_rate, out_ch)) {
            return false;
        }
        // Variable-rate resampler used to track AEC reference clock drift.
        if (!ref_resampler_.open((double)pb.sample_rate, (double)in_rate, 1,
                                 SOXR_INT16_I, SOXR_INT16_I,
                                 /*variable_rate=*/true)) {
            return false;
        }
        // Reference delay aligner.
        ref_delay_.configure(in_rate, 1, cfg.aec_reference_delay_ms);
        return true;
    }

    bool start() {
        if (running_.exchange(true)) return true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

private:
    void loop() {
        pthread_setname_np(pthread_self(), "audio_proc");
        ALOG_INFO("ProcessingThread started pipeline={} aec={} ns={} agc={} vad={} bf={} ref_delay={}ms",
                  proc_cfg_.pipeline, proc_cfg_.aec, proc_cfg_.ns,
                  proc_cfg_.agc, proc_cfg_.vad, proc_cfg_.beamforming,
                  proc_cfg_.aec_reference_delay_ms);
        const int requested_ch = (int)std::max<size_t>(cap_cfg_.channel_map.size(),
                                                       (size_t)cap_cfg_.channels);
        const int in_rate      = cap_cfg_.sample_rate;
        const int out_rate     = proc_cfg_.output_sample_rate;
        const int out_ch       = proc_cfg_.output_channels;
        const int in_frames_10ms  = in_rate / 100;
        const int out_frames_10ms = out_rate / 100;
        (void)out_frames_10ms;

        std::vector<int16_t> mapped((size_t)in_frames_10ms * requested_ch);
        std::vector<int16_t> ref_aligned((size_t)in_frames_10ms);
        std::vector<int16_t> ref_resampled;
        std::vector<int16_t> processed_i16;

        bool prev_speech = false;
        auto last_drift  = std::chrono::steady_clock::now();
        int64_t cap_frames_acc = 0;
        int64_t ref_frames_acc = 0;

        InternalFrame mic, ref;
        while (running_.load(std::memory_order_acquire)) {
            if (!cap_ring_.pop(mic)) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            const int64_t t0     = nowMicros();
            const int     frames = (int)mic.samples.size() /
                                   std::max(1, mic.channels);

            // 1) ChannelMapper.
            if ((int)mapped.size() != frames * requested_ch) {
                mapped.assign((size_t)frames * requested_ch, 0);
            }
            mapper_.process(mic.samples.data(), mapped.data(), frames);

            // 2) Reference handling (AEC) – pull from ref ring, resample to
            // capture rate, push through delay-aligner, and feed APM as the
            // reverse stream.
            if (proc_cfg_.aec) {
                if (ref_ring_.pop(ref)) {
                    // ref is interleaved playback rate/channels; downmix to mono.
                    const int rframes = (int)ref.samples.size() /
                                         std::max(1, ref.channels);
                    std::vector<int16_t> ref_mono((size_t)rframes);
                    for (int n = 0; n < rframes; ++n) {
                        int32_t sum = 0;
                        for (int c = 0; c < ref.channels; ++c) {
                            sum += ref.samples[(size_t)n * ref.channels + c];
                        }
                        ref_mono[n] = (int16_t)(sum / std::max(1, ref.channels));
                    }
                    // Resample reference -> capture rate (variable-rate to
                    // accommodate clock drift).
                    ref_resampler_.processI16(ref_mono.data(), rframes,
                                              ref_resampled);
                    if (!ref_resampled.empty()) {
                        ref_delay_.push(ref_resampled.data(), ref_resampled.size());
                        ref_frames_acc += (int64_t)ref_resampled.size();
                    }
                }

                // Pull aligned reference samples (10 ms).
                if ((int)ref_aligned.size() != frames) ref_aligned.resize(frames);
                ref_delay_.pop(ref_aligned.data(), frames);
                apm_.analyzeReverse(ref_aligned.data(), frames);
            }

            // 3) WebRTC APM ProcessStream: HPF + AEC + NS + AGC + VAD.
            // Compute stream delay = playback hardware delay + processing.
            const int stream_delay_ms =
                proc_cfg_.aec_reference_delay_ms;
            bool voice_active = false;
            if (apm_.processCapture(mapped.data(), frames, stream_delay_ms,
                                    voice_active) != 0) {
                // Fall through; continue but mark error.
                events_.push({AudioEventType::CaptureError, 0, 100});
            }

            // 4) Beamforming – APM with multi-channel input collapses to
            // out_ch internally.  We additionally take the first out_ch
            // channels as a robust fallback.
            std::vector<int16_t> mono((size_t)frames * out_ch);
            for (int n = 0; n < frames; ++n) {
                for (int c = 0; c < out_ch; ++c) {
                    mono[(size_t)n * out_ch + c] =
                        mapped[(size_t)n * requested_ch + c];
                }
            }

            // 5) VAD events.
            if (proc_cfg_.vad) {
                if (voice_active && !prev_speech) {
                    events_.push({AudioEventType::VAD_SPEECH_START, 0, 0});
                } else if (!voice_active && prev_speech) {
                    events_.push({AudioEventType::VAD_SPEECH_END, 0, 0});
                }
                prev_speech = voice_active;
            }

            // 6) Resample 48k -> 16k.
            downsampler_.processI16(mono.data(), frames, processed_i16);

            // 7) Dispatch to consumers.
            AudioFrame outframe;
            outframe.samples     = processed_i16;
            outframe.channels    = out_ch;
            outframe.sample_rate = out_rate;
            outframe.timestamp_us = mic.timestamp_us;
            dispatcher_.dispatch(outframe);

            // 8) Diagnostics.
            diag_.dumpRaw(mic.samples.data(), mic.samples.size());
            diag_.dumpProcessed(processed_i16.data(), processed_i16.size());
            diag_.aec_delay_metric_ms.store(apm_.delayMetric(),
                                            std::memory_order_relaxed);

            const int64_t t1 = nowMicros();
            diag_.processing_cost_us.store(t1 - t0, std::memory_order_relaxed);

            cap_frames_acc += frames;

            // 9) Clock drift compensation (every 30 s).
            auto now = std::chrono::steady_clock::now();
            if (proc_cfg_.aec && std::chrono::duration_cast<std::chrono::seconds>(
                                     now - last_drift).count() >= 30) {
                const double cap_seconds =
                    (double)cap_frames_acc / cap_cfg_.sample_rate;
                const double ref_seconds =
                    (double)ref_frames_acc / cap_cfg_.sample_rate;
                const int64_t drift_us =
                    (int64_t)std::llround((cap_seconds - ref_seconds) * 1e6);
                diag_.aec_clock_drift_us.store(drift_us,
                                               std::memory_order_relaxed);
                // If absolute drift > ±0.5 ms, nudge soxr io ratio.
                if (std::abs(drift_us) > 500) {
                    const int64_t ppm =
                        (int64_t)std::llround((double)drift_us / 30.0);
                    ref_resampler_.setDriftPpm(ppm);
                    ALOG_INFO("AEC clock drift={} us over 30s, applying ppm={}",
                              drift_us, ppm);
                }
                last_drift     = now;
                cap_frames_acc = 0;
                ref_frames_acc = 0;
            }
        }
        ALOG_INFO("ProcessingThread stopped");
    }

    SpscRing<InternalFrame>& cap_ring_;
    SpscRing<InternalFrame>& ref_ring_;
    FrameDispatcher&         dispatcher_;
    RtEventQueue&            events_;
    Diagnostics&             diag_;
    ProcessingConfig         proc_cfg_;
    CaptureConfig            cap_cfg_;
    PlaybackConfig           pb_cfg_;
    ChannelMapper            mapper_;
    WebRtcApmWrapper         apm_;
    SoxrResampler            downsampler_;
    SoxrResampler            ref_resampler_;
    DelayCompensator         ref_delay_;
    std::atomic<bool>        running_{false};
    std::thread              thread_;
};

// ============================================================================
// PlaybackManager : Mixer (≤4 streams, float32) -> Limiter -> AEC tap -> ALSA
// ============================================================================

struct MixerStream {
    PlaybackHandle       handle;
    std::vector<int16_t> pcm;          // already converted to playback rate/ch
    size_t               pos       = 0;
    PlaybackPriority     priority  = PlaybackPriority::MEDIA;
    float                gain      = 1.0f;
    float                gain_actual = 1.0f;
    bool                 loop      = false;
    bool                 active    = true;
};

class PlaybackManager {
public:
    PlaybackManager(IAudioBackend& backend, SpscRing<InternalFrame>& ref_ring,
                    VolumeManager& vol, FocusManager& focus,
                    AudioRouter& router, RtEventQueue& events,
                    Diagnostics& diag)
        : backend_(backend), ref_ring_(ref_ring), vol_(vol),
          focus_(focus), router_(router), events_(events), diag_(diag) {
        focus_.setPreemptCallback([this](PlaybackHandle h) { stop(h, /*interrupted=*/true); });
        focus_.setDuckingCallback([this](bool active) { vol_.setDuckingActive(active); });
    }

    bool open(const PlaybackConfig& cfg) {
        cfg_ = cfg;
        DeviceConfig dc;
        dc.device_name = router_.outputDevice().empty() ? cfg.device : router_.outputDevice();
        dc.sample_rate = cfg.sample_rate;
        dc.channels    = cfg.channels;
        dc.frame_ms    = cfg.frame_ms;
        BackendError err = backend_.openOutput(dc, neg_);
        if (err != BackendError::kOk) {
            ALOG_ERR("PlaybackManager: openOutput failed err={}", (int)err);
            return false;
        }
        limiter_.configure(neg_.sample_rate);
        return true;
    }

    bool start() {
        if (running_.exchange(true)) return true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stopThread() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        backend_.closeOutput();
    }

    PlaybackHandle play(const PlaybackRequest& req) {
        // Convert PCM to playback rate/channels.  Inputs already int16.
        std::vector<int16_t> pcm;
        if (!req.pcm_data.empty()) {
            pcm = req.pcm_data;
        } else if (!req.file_path.empty()) {
            pcm = loadWavOrRaw(req.file_path);
        }
        if (pcm.empty()) return kInvalidPlaybackHandle;
        // Up/down-convert to neg_ format (sample_rate + channels).  Simple
        // nearest-neighbour resampling and channel duplication.
        pcm = resampleToBackend(pcm, req.sample_rate, req.channels);

        const PlaybackHandle h = next_handle_.fetch_add(1) + 1;
        if (!focus_.acquire(h, req.priority)) {
            return kInvalidPlaybackHandle;
        }
        auto stream = std::make_shared<MixerStream>();
        stream->handle    = h;
        stream->pcm       = std::move(pcm);
        stream->priority  = req.priority;
        stream->gain      = clampf(req.stream_gain, 0.0f, 4.0f) *
                            vol_.streamGainFor(req.priority);
        stream->loop      = req.loop;
        stream->active    = true;
        stream->gain_actual = 0.0f; // will fade-in
        {
            std::lock_guard<std::mutex> lk(streams_mtx_);
            // Keep at most 4 active streams (architecture cap).
            cleanupInactiveLocked();
            if (streams_.size() >= 4) {
                // Drop the lowest-priority stream.
                auto worst = streams_.begin();
                for (auto it = streams_.begin(); it != streams_.end(); ++it) {
                    if ((*it)->priority < (*worst)->priority) worst = it;
                }
                (*worst)->active = false;
                events_.push({AudioEventType::PlaybackInterrupted,
                              (*worst)->handle, 0});
                streams_.erase(worst);
            }
            streams_.push_back(stream);
        }
        events_.push({AudioEventType::PlaybackStarted, h, 0});
        return h;
    }

    void stop(PlaybackHandle h, bool interrupted = false) {
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(streams_mtx_);
            for (auto& s : streams_) {
                if (s->handle == h) {
                    s->active = false;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            focus_.release(h);
            events_.push({interrupted ? AudioEventType::PlaybackInterrupted
                                      : AudioEventType::PlaybackCompleted,
                          h, 0});
        }
    }

    void stopAll() {
        std::vector<PlaybackHandle> handles;
        {
            std::lock_guard<std::mutex> lk(streams_mtx_);
            for (auto& s : streams_) {
                s->active = false;
                handles.push_back(s->handle);
            }
        }
        focus_.releaseAll();
        for (auto h : handles) {
            events_.push({AudioEventType::PlaybackInterrupted, h, 0});
        }
    }

    bool running() const { return running_.load(std::memory_order_acquire); }
    const NegotiatedFormat& format() const { return neg_; }

private:
    void cleanupInactiveLocked() {
        for (auto it = streams_.begin(); it != streams_.end();) {
            if (!(*it)->active && (*it)->pos >= (*it)->pcm.size() && !(*it)->loop) {
                it = streams_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<int16_t> resampleToBackend(const std::vector<int16_t>& src,
                                           int src_rate, int src_channels) {
        if (src_rate <= 0 || src_channels <= 0) return {};
        const int dst_rate     = neg_.sample_rate;
        const int dst_channels = neg_.channels;
        const int src_frames   = (int)src.size() / src_channels;
        const double ratio     = (double)dst_rate / (double)src_rate;
        const int dst_frames   = (int)std::llround(src_frames * ratio);
        std::vector<int16_t> out((size_t)dst_frames * dst_channels, 0);
        for (int n = 0; n < dst_frames; ++n) {
            const int sn = (int)std::min<double>(
                src_frames - 1, std::floor((double)n / ratio));
            for (int c = 0; c < dst_channels; ++c) {
                const int sc = c < src_channels ? c : 0;
                out[(size_t)n * dst_channels + c] =
                    src[(size_t)sn * src_channels + sc];
            }
        }
        return out;
    }

    static std::vector<int16_t> loadWavOrRaw(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        std::vector<char> all((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (all.size() > 44 && std::memcmp(all.data(), "RIFF", 4) == 0 &&
            std::memcmp(all.data() + 8, "WAVE", 4) == 0) {
            // Naive WAV: assume 16-bit PCM, data chunk after 44-byte header.
            const size_t bytes = all.size() - 44;
            std::vector<int16_t> pcm(bytes / 2);
            std::memcpy(pcm.data(), all.data() + 44, bytes);
            return pcm;
        }
        std::vector<int16_t> pcm(all.size() / 2);
        if (!pcm.empty()) std::memcpy(pcm.data(), all.data(), pcm.size() * 2);
        return pcm;
    }

    static bool isDuckedPriority(PlaybackPriority p) {
        return p == PlaybackPriority::MEDIA ||
               p == PlaybackPriority::BLUETOOTH;
    }

    void loop() {
        setRealtimePriority("audio_playback", 79);
        ALOG_INFO("PlaybackThread started ({} ch @ {} Hz, period={})",
                  neg_.channels, neg_.sample_rate, neg_.period_frames);
        const int channels = neg_.channels;
        const int period   = std::max(1, neg_.period_frames);
        std::vector<float>   mix_f((size_t)period * channels, 0.0f);
        AudioFrame           out;
        out.samples.resize((size_t)period * channels);
        out.channels    = channels;
        out.sample_rate = neg_.sample_rate;
        InternalFrame ref_frame;
        while (running_.load(std::memory_order_acquire)) {
            const int64_t t0 = nowMicros();
            std::fill(mix_f.begin(), mix_f.end(), 0.0f);

            // Snapshot streams.
            std::vector<std::shared_ptr<MixerStream>> snap;
            {
                std::lock_guard<std::mutex> lk(streams_mtx_);
                snap = streams_;
            }
            std::vector<PlaybackHandle> finished;
            const float ducking = vol_.advanceDucking(period);
            for (auto& s : snap) {
                if (!s->active) continue;
                const float stream_ducking =
                    isDuckedPriority(s->priority) ? ducking : 1.0f;
                // Per-stream gain ramping (10 ms).
                const int ramp = std::max(1, neg_.sample_rate * 10 / 1000);
                for (int n = 0; n < period; ++n) {
                    const float step = (s->gain - s->gain_actual) /
                                       (float)ramp;
                    s->gain_actual += step;
                    if (s->pos >= s->pcm.size()) {
                        if (s->loop && !s->pcm.empty()) {
                            s->pos = 0;
                        } else {
                            if (s->active) {
                                finished.push_back(s->handle);
                                s->active = false;
                            }
                            break;
                        }
                    }
                    for (int c = 0; c < channels; ++c) {
                        const int16_t sample =
                            s->pcm[s->pos + (size_t)c % s->pcm.size()];
                        mix_f[(size_t)n * channels + c] +=
                            ((float)sample / 32768.0f) *
                            s->gain_actual * stream_ducking;
                    }
                    s->pos += channels;
                }
            }

            // Master volume is applied to the final mix.  Ducking is applied
            // per duckable stream above so foreground streams keep full gain.
            const float master = vol_.advanceMaster(period);
            for (size_t i = 0; i < mix_f.size(); ++i) {
                mix_f[i] *= master;
            }

            // LimiterFloat (envelope-based soft-knee, configured at open()).
            limiter_.process(mix_f.data(), period * channels);

            // int16 conversion.
            for (size_t i = 0; i < mix_f.size(); ++i) {
                int s = (int)std::lrint(mix_f[i] * 32767.0f);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                out.samples[i] = (int16_t)s;
            }

            // AEC reference tap: push int16 frame into ref ring.
            ref_frame.samples     = out.samples;
            ref_frame.channels    = channels;
            ref_frame.sample_rate = neg_.sample_rate;
            ref_frame.timestamp_us = nowMicros();
            if (!ref_ring_.push(std::move(ref_frame))) {
                // Reference ring full – drop oldest by popping one frame.
                InternalFrame drop;
                ref_ring_.pop(drop);
            }
            diag_.reference_queue_depth.store(ref_ring_.size(),
                                              std::memory_order_relaxed);
            diag_.dumpReference(out.samples.data(), out.samples.size());

            // Write to ALSA.
            BackendError werr = backend_.writeOutput(out);
            const int64_t t1  = nowMicros();
            diag_.playback_latency_us.store(t1 - t0, std::memory_order_relaxed);
            if (werr == BackendError::kXrun) {
                diag_.playback_underrun_total.fetch_add(1, std::memory_order_relaxed);
                events_.push({AudioEventType::PlaybackError, 0, (int)werr});
            } else if (werr == BackendError::kDeviceGone ||
                       werr == BackendError::kIOError) {
                events_.push({AudioEventType::DeviceDisconnected, 0, (int)werr});
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else if (werr != BackendError::kOk) {
                events_.push({AudioEventType::PlaybackError, 0, (int)werr});
            }

            for (auto h : finished) {
                focus_.release(h);
                events_.push({AudioEventType::PlaybackCompleted, h, 0});
            }
            // Reap finished streams.
            {
                std::lock_guard<std::mutex> lk(streams_mtx_);
                cleanupInactiveLocked();
            }
        }
        ALOG_INFO("PlaybackThread stopped");
    }

    IAudioBackend&                                backend_;
    SpscRing<InternalFrame>&                      ref_ring_;
    VolumeManager&                                vol_;
    FocusManager&                                 focus_;
    AudioRouter&                                  router_;
    RtEventQueue&                                 events_;
    Diagnostics&                                  diag_;
    PlaybackConfig                                cfg_;
    NegotiatedFormat                              neg_;
    std::atomic<bool>                             running_{false};
    std::thread                                   thread_;
    std::mutex                                    streams_mtx_;
    std::vector<std::shared_ptr<MixerStream>>     streams_;
    std::atomic<PlaybackHandle>                   next_handle_{0};
    LimiterFloat                                  limiter_;
};

// ============================================================================
// DeviceManager : enumerate, hot-plug poll, recovery state machine
// ============================================================================

enum class DeviceState {
    AVAILABLE,
    ACTIVE,
    DISCONNECTED,
    RECOVERING,
    FAILED,
};

class DeviceManager {
public:
    using RecoverFn = std::function<bool()>;

    DeviceManager(IAudioBackend& backend, AudioRouter& router,
                  Diagnostics& diag, const DeviceRecoveryConfig& cfg)
        : backend_(backend), router_(router), diag_(diag), cfg_(cfg) {}

    ~DeviceManager() { stop(); }

    void start(RecoverFn recover, std::function<void(AudioEventType, std::string)> emit) {
        recover_ = std::move(recover);
        emit_    = std::move(emit);
        running_.store(true, std::memory_order_release);
        // Recovery state machine thread.
        sm_thread_  = std::thread([this] { stateMachineLoop(); });
        // udev hot-plug monitoring thread.
        udev_thread_ = std::thread([this] { udevLoop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (sm_thread_.joinable())  sm_thread_.join();
        if (udev_thread_.joinable()) udev_thread_.join();
    }

    void notifyDisconnected(const std::string& reason = {}) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = DeviceState::DISCONNECTED;
            disc_reason_ = reason;
        }
        cv_.notify_all();
    }

    void markActive() {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = DeviceState::ACTIVE;
    }

    DeviceState state() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return state_;
    }

    AudioDeviceList listInputs()  { return backend_.listInputDevices(); }
    AudioDeviceList listOutputs() { return backend_.listOutputDevices(); }

private:
    // Drives RECOVERING -> AVAILABLE / FAILED state transitions.
    void stateMachineLoop() {
        pthread_setname_np(pthread_self(), "audio_dev_sm");
        ALOG_INFO("DeviceManager state-machine thread started");
        std::unique_lock<std::mutex> lk(mtx_);
        while (running_.load(std::memory_order_acquire)) {
            cv_.wait_for(lk, std::chrono::seconds(2), [&] {
                return !running_.load(std::memory_order_acquire) ||
                       state_ == DeviceState::DISCONNECTED;
            });
            if (!running_.load(std::memory_order_acquire)) break;

            if (state_ == DeviceState::DISCONNECTED) {
                std::string reason = disc_reason_;
                state_ = DeviceState::RECOVERING;
                lk.unlock();
                if (emit_) emit_(AudioEventType::DeviceDisconnected, reason);
                bool ok = false;
                for (int attempt = 1;
                     attempt <= cfg_.max_recover_retries && !ok &&
                     running_.load(std::memory_order_acquire);
                     ++attempt) {
                    const int backoff = cfg_.recover_backoff_base_ms *
                                        (1 << std::min(attempt - 1, 4));
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(backoff));
                    diag_.device_recover_total.fetch_add(1,
                        std::memory_order_relaxed);
                    if (recover_) ok = recover_();
                    ALOG_INFO("DeviceManager recovery attempt {}/{} -> {}",
                              attempt, cfg_.max_recover_retries,
                              ok ? "OK" : "FAIL");
                }
                lk.lock();
                if (ok) {
                    state_ = DeviceState::ACTIVE;
                    lk.unlock();
                    if (emit_) emit_(AudioEventType::DeviceRecovered, "");
                    lk.lock();
                } else {
                    state_ = DeviceState::FAILED;
                    diag_.device_recover_failed_total.fetch_add(1,
                        std::memory_order_relaxed);
                    lk.unlock();
                    if (emit_) emit_(AudioEventType::DeviceFailed, "max retries");
                    lk.lock();
                }
            }
        }
        ALOG_INFO("DeviceManager state-machine thread stopped");
    }

    // udev netlink listener for hot-plug events on the "sound" subsystem.
    void udevLoop() {
        pthread_setname_np(pthread_self(), "audio_udev");
        struct udev* udev = udev_new();
        if (!udev) {
            ALOG_WARN("udev_new() failed; hot-plug detection disabled");
            return;
        }
        struct udev_monitor* mon = udev_monitor_new_from_netlink(udev, "udev");
        if (!mon) {
            ALOG_WARN("udev_monitor_new_from_netlink failed");
            udev_unref(udev);
            return;
        }
        udev_monitor_filter_add_match_subsystem_devtype(mon, "sound", nullptr);
        udev_monitor_enable_receiving(mon);
        const int fd = udev_monitor_get_fd(mon);
        ALOG_INFO("DeviceManager udev hot-plug monitor active (fd={})", fd);

        while (running_.load(std::memory_order_acquire)) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(fd, &rs);
            timeval tv{1, 0};
            int rc = ::select(fd + 1, &rs, nullptr, nullptr, &tv);
            if (rc <= 0) continue;
            if (!FD_ISSET(fd, &rs)) continue;

            struct udev_device* dev = udev_monitor_receive_device(mon);
            if (!dev) continue;
            const char* action = udev_device_get_action(dev);
            const char* node   = udev_device_get_devnode(dev);
            if (action) {
                ALOG_INFO("udev event action={} node={}",
                          action, node ? node : "(null)");
                if (std::strcmp(action, "add") == 0 && emit_) {
                    emit_(AudioEventType::DeviceConnected, node ? node : "");
                } else if (std::strcmp(action, "remove") == 0) {
                    notifyDisconnected(node ? node : "udev remove");
                }
            }
            udev_device_unref(dev);
        }

        udev_monitor_unref(mon);
        udev_unref(udev);
        ALOG_INFO("DeviceManager udev thread stopped");
    }

    IAudioBackend&                       backend_;
    AudioRouter&                         router_;
    Diagnostics&                         diag_;
    DeviceRecoveryConfig                 cfg_;
    mutable std::mutex                   mtx_;
    std::condition_variable              cv_;
    DeviceState                          state_       = DeviceState::AVAILABLE;
    std::string                          disc_reason_;
    std::atomic<bool>                    running_{false};
    std::thread                          sm_thread_;
    std::thread                          udev_thread_;
    RecoverFn                            recover_;
    std::function<void(AudioEventType, std::string)> emit_;
};

} // namespace
} // namespace audio

// ============================================================================
// AudioManager::Impl
// ============================================================================

namespace audio {

struct AudioManager::Impl {
    Config                                  cfg;
    std::unique_ptr<AlsaBackend>            backend;
    // Capture ring (200 ms = 20 frames at 10 ms).  Power-of-two cap = 32.
    std::unique_ptr<SpscRing<InternalFrame>> capture_ring;
    // Reference ring (500 ms = 50 frames at 10 ms).  Power-of-two cap = 64.
    std::unique_ptr<SpscRing<InternalFrame>> reference_ring;
    std::unique_ptr<RtEventQueue>           rt_events;
    std::unique_ptr<Diagnostics>            diag;
    std::unique_ptr<VolumeManager>          volume;
    std::unique_ptr<FocusManager>           focus;
    std::unique_ptr<AudioRouter>            router;
    std::unique_ptr<FrameDispatcher>        dispatcher;
    std::unique_ptr<CaptureManager>         capture;
    std::unique_ptr<AudioProcessingChain>   processing;
    std::unique_ptr<PlaybackManager>        playback;
    std::unique_ptr<DeviceManager>          devices;

    // Control thread + diagnostics thread.
    std::atomic<bool>                       running{false};
    std::thread                             control_thread;
    std::thread                             diag_thread;

    // Subscribers.
    std::mutex                              sub_mtx;
    std::vector<AudioEventCallback>         subscribers;

    // Saved init state for recovery.
    bool                                    initialized = false;
    bool                                    capture_started = false;

    void emit(const AudioEvent& ev) {
        std::vector<AudioEventCallback> snap;
        {
            std::lock_guard<std::mutex> lk(sub_mtx);
            snap = subscribers;
        }
        for (auto& cb : snap) {
            try {
                cb(ev);
            } catch (...) {
                ALOG_ERR("AudioEvent subscriber threw");
            }
        }
    }

    void controlLoop() {
        pthread_setname_np(pthread_self(), "audio_ctrl");
        ALOG_INFO("ControlThread started");
        RtEvent rt;
        while (running.load(std::memory_order_acquire)) {
            bool any = false;
            while (rt_events && rt_events->pop(rt)) {
                any = true;
                AudioEvent ev;
                ev.type   = rt.type;
                ev.handle = rt.handle;
                if (rt.code != 0) ev.detail = std::to_string(rt.code);
                if (rt.type == AudioEventType::DeviceDisconnected) {
                    if (devices) devices->notifyDisconnected(
                        "RT thread err=" + std::to_string(rt.code));
                }
                emit(ev);
            }
            if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ALOG_INFO("ControlThread stopped");
    }

    void diagLoop() {
        pthread_setname_np(pthread_self(), "audio_diag");
        ALOG_INFO("DiagnosticsThread started");
        if (!cfg.diagnostics.enable_metrics) return;
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!running.load(std::memory_order_acquire)) break;
            ALOG_INFO("metrics: cap_total={} overrun={} pb_under={} drift_us={} aec_delay_ms={} qd_cap={} qd_ref={} proc_us={}",
                      diag->capture_frames_total.load(),
                      diag->capture_overrun_total.load(),
                      diag->playback_underrun_total.load(),
                      diag->aec_clock_drift_us.load(),
                      diag->aec_delay_metric_ms.load(),
                      diag->capture_queue_depth.load(),
                      diag->reference_queue_depth.load(),
                      diag->processing_cost_us.load());
        }
        ALOG_INFO("DiagnosticsThread stopped");
    }

    bool buildBackends() {
        // Capture
        if (!capture->open(cfg.capture)) {
            ALOG_ERR("Capture device unavailable: {}", cfg.capture.device);
            return false;
        }
        // Playback
        if (!playback->open(cfg.playback)) {
            ALOG_WARN("Playback device unavailable: {} (continuing without playback)",
                      cfg.playback.device);
        }
        return true;
    }

    bool recoverDevices() {
        // Full architecture-mandated recovery flow:
        //   stop capture/playback -> close backend -> re-enumerate ->
        //   AudioRouter selects target -> rebuild backend (re-negotiate format)
        //   -> restart capture/playback -> emit DeviceRecovered.
        ALOG_INFO("DeviceRecovery: starting recovery sequence");
        if (capture && capture->running())   capture->stop();
        if (playback && playback->running()) playback->stopThread();
        if (processing) processing->stop();

        backend->closeInput();
        backend->closeOutput();

        // Re-enumerate; on missing primary, fall back to the first available.
        AudioDeviceList ins  = backend->listInputDevices();
        AudioDeviceList outs = backend->listOutputDevices();
        ALOG_INFO("DeviceRecovery: {} inputs, {} outputs available",
                  ins.size(), outs.size());

        // Auto-fallback: if the configured device is missing, pick "default".
        auto chooseDevice = [&](const AudioDeviceList& list,
                                const std::string& want) {
            for (auto& d : list) if (d.id == want) return want;
            for (auto& d : list) if (d.id == "default") return d.id;
            if (!list.empty()) return list.front().id;
            return std::string("default");
        };
        const std::string in_dev  = chooseDevice(ins,  cfg.capture.device);
        const std::string out_dev = chooseDevice(outs, cfg.playback.device);
        if (in_dev  != cfg.capture.device)
            ALOG_INFO("Routing capture {} -> {}", cfg.capture.device, in_dev);
        if (out_dev != cfg.playback.device)
            ALOG_INFO("Routing playback {} -> {}", cfg.playback.device, out_dev);
        router->setInputDevice(in_dev);
        router->setOutputDevice(out_dev);

        // Re-open backends with re-negotiated format.
        CaptureConfig  cap = cfg.capture;
        PlaybackConfig pb  = cfg.playback;
        cap.device = in_dev;
        pb.device  = out_dev;

        if (!capture->open(cap))   return false;
        if (!playback->open(pb))   ALOG_WARN("Playback open failed during recovery");

        // Restart pipeline.
        if (!processing->start()) return false;
        if (!playback->start())   return false;
        if (!capture->start())    return false;
        return true;
    }
};

// ============================================================================
// AudioManager public methods
// ============================================================================

AudioManager::AudioManager() : impl_(std::make_unique<Impl>()) {
    getLogger();
}

AudioManager::~AudioManager() {
    shutdown();
}

bool AudioManager::init(const Config& cfg) {
    if (impl_->initialized) {
        ALOG_WARN("AudioManager::init called twice; ignoring");
        return true;
    }
    impl_->cfg = cfg;
    impl_->backend        = std::make_unique<AlsaBackend>();
    impl_->capture_ring   = std::make_unique<SpscRing<InternalFrame>>(32);
    impl_->reference_ring = std::make_unique<SpscRing<InternalFrame>>(64);
    impl_->rt_events      = std::make_unique<RtEventQueue>(256);
    impl_->diag           = std::make_unique<Diagnostics>();
    impl_->diag->configure(cfg.diagnostics,
                           cfg.capture.sample_rate,
                           (int)std::max<size_t>(cfg.capture.channel_map.size(),
                                                  (size_t)cfg.capture.channels),
                           cfg.processing.output_sample_rate,
                           cfg.processing.output_channels,
                           cfg.capture.sample_rate, 1);
    impl_->volume         = std::make_unique<VolumeManager>();
    impl_->volume->configure(cfg.volume, cfg.playback.sample_rate);
    impl_->focus          = std::make_unique<FocusManager>();
    impl_->router         = std::make_unique<AudioRouter>();
    impl_->router->setInputDevice(cfg.capture.device);
    impl_->router->setOutputDevice(cfg.playback.device);
    impl_->router->setAlarmDevice(cfg.playback.device); // default same
    impl_->dispatcher     = std::make_unique<FrameDispatcher>();
    impl_->capture        = std::make_unique<CaptureManager>(
        *impl_->backend, *impl_->capture_ring, *impl_->rt_events, *impl_->diag);
    impl_->processing     = std::make_unique<AudioProcessingChain>(
        *impl_->capture_ring, *impl_->reference_ring, *impl_->dispatcher,
        *impl_->rt_events, *impl_->diag);
    if (!impl_->processing->configure(cfg.processing, cfg.capture, cfg.playback)) {
        ALOG_ERR("AudioManager: ProcessingChain configure failed (WebRTC APM/soxr)");
        return false;
    }
    impl_->playback       = std::make_unique<PlaybackManager>(
        *impl_->backend, *impl_->reference_ring, *impl_->volume,
        *impl_->focus, *impl_->router, *impl_->rt_events, *impl_->diag);
    impl_->devices        = std::make_unique<DeviceManager>(
        *impl_->backend, *impl_->router, *impl_->diag, cfg.device);

    if (!impl_->buildBackends()) {
        ALOG_ERR("AudioManager: failed to open required devices");
        return false;
    }

    // Open and start playback only (capture starts via startCapture()).
    impl_->playback->start();
    impl_->processing->start();

    impl_->running.store(true, std::memory_order_release);
    impl_->control_thread = std::thread([this] { impl_->controlLoop(); });
    impl_->diag_thread    = std::thread([this] { impl_->diagLoop(); });

    impl_->devices->markActive();
    impl_->devices->start(
        [this] { return impl_->recoverDevices(); },
        [this](AudioEventType t, std::string id) {
            AudioEvent ev;
            ev.type      = t;
            ev.device_id = std::move(id);
            impl_->emit(ev);
        });

    impl_->initialized = true;
    ALOG_INFO("AudioManager initialized backend={} pipeline={}",
              cfg.backend, cfg.processing.pipeline);
    return true;
}

void AudioManager::shutdown() {
    if (!impl_ || !impl_->initialized) return;
    impl_->initialized = false;

    if (impl_->capture && impl_->capture->running()) impl_->capture->stop();
    if (impl_->processing) impl_->processing->stop();
    if (impl_->playback)   impl_->playback->stopThread();
    if (impl_->dispatcher) impl_->dispatcher->clear();
    if (impl_->devices)    impl_->devices->stop();

    impl_->running.store(false, std::memory_order_release);
    if (impl_->control_thread.joinable()) impl_->control_thread.join();
    if (impl_->diag_thread.joinable())    impl_->diag_thread.join();

    ALOG_INFO("AudioManager shutdown complete");
}

bool AudioManager::startCapture() {
    if (!impl_->initialized) return false;
    if (impl_->capture_started) return true;
    if (!impl_->capture->start()) return false;
    impl_->capture_started = true;
    AudioEvent ev;
    ev.type = AudioEventType::CaptureStarted;
    impl_->emit(ev);
    return true;
}

void AudioManager::stopCapture() {
    if (!impl_->initialized || !impl_->capture_started) return;
    impl_->capture->stop();
    impl_->capture_started = false;
    AudioEvent ev;
    ev.type = AudioEventType::CaptureStopped;
    impl_->emit(ev);
}

ConsumerHandle AudioManager::addFrameConsumer(FrameCallback cb,
                                              size_t max_queue_depth) {
    if (!impl_ || !impl_->dispatcher) return kInvalidConsumerHandle;
    return impl_->dispatcher->addConsumer(std::move(cb), max_queue_depth);
}

void AudioManager::removeFrameConsumer(ConsumerHandle h) {
    if (!impl_ || !impl_->dispatcher) return;
    impl_->dispatcher->removeConsumer(h);
}

PlaybackHandle AudioManager::play(const PlaybackRequest& req) {
    if (!impl_ || !impl_->playback) return kInvalidPlaybackHandle;
    return impl_->playback->play(req);
}

void AudioManager::stop(PlaybackHandle h) {
    if (!impl_ || !impl_->playback) return;
    impl_->playback->stop(h);
}

void AudioManager::stopAll() {
    if (!impl_ || !impl_->playback) return;
    impl_->playback->stopAll();
}

void AudioManager::setMasterVolume(float v) {
    if (!impl_ || !impl_->volume) return;
    impl_->volume->setMaster(v);
    AudioEvent ev;
    ev.type = AudioEventType::VolumeChanged;
    impl_->emit(ev);
}

float AudioManager::masterVolume() const {
    if (!impl_ || !impl_->volume) return 0.0f;
    return impl_->volume->master();
}

DeviceList AudioManager::listInputDevices() const {
    if (!impl_ || !impl_->devices) return {};
    return impl_->devices->listInputs();
}

DeviceList AudioManager::listOutputDevices() const {
    if (!impl_ || !impl_->devices) return {};
    return impl_->devices->listOutputs();
}

HealthStatus AudioManager::health() const {
    HealthStatus h;
    if (!impl_) return h;
    h.capture_running             = impl_->capture && impl_->capture->running();
    h.playback_running            = impl_->playback && impl_->playback->running();
    h.capture_frames_total        = impl_->diag->capture_frames_total.load();
    h.capture_overrun_total       = impl_->diag->capture_overrun_total.load();
    h.playback_underrun_total     = impl_->diag->playback_underrun_total.load();
    h.device_recover_total        = impl_->diag->device_recover_total.load();
    h.device_recover_failed_total = impl_->diag->device_recover_failed_total.load();
    h.capture_latency_ms          = impl_->diag->capture_latency_us.load() / 1000.0;
    h.playback_latency_ms         = impl_->diag->playback_latency_us.load() / 1000.0;
    h.processing_cost_ms          = impl_->diag->processing_cost_us.load() / 1000.0;
    h.aec_reference_delay_ms      = impl_->cfg.processing.aec_reference_delay_ms;
    h.aec_clock_drift_us          = impl_->diag->aec_clock_drift_us.load();
    h.capture_queue_depth         = impl_->diag->capture_queue_depth.load();
    h.reference_queue_depth       = impl_->diag->reference_queue_depth.load();
    h.wakeword_dropped_frames     = impl_->dispatcher ?
                                    impl_->dispatcher->droppedFrames("wakeword") : 0;
    h.asr_dropped_frames          = impl_->dispatcher ?
                                    impl_->dispatcher->droppedFrames("asr") : 0;
    h.diag_dropped_frames         = impl_->dispatcher ?
                                    impl_->dispatcher->droppedFrames("diag") : 0;
    return h;
}

void AudioManager::subscribe(AudioEventCallback cb) {
    if (!impl_ || !cb) return;
    std::lock_guard<std::mutex> lk(impl_->sub_mtx);
    impl_->subscribers.push_back(std::move(cb));
}

// ============================================================================
// loadConfig : TOML -> Config
// ============================================================================

Config loadConfig(const std::string& config_path) {
    Config out;
    try {
        auto tbl = toml::parse_file(config_path);
        auto audio = tbl["audio"];
        out.backend = audio["backend"].value_or("alsa");

        auto cap = audio["capture"];
        out.capture.device      = cap["device"].value_or(out.capture.device);
        out.capture.sample_rate = cap["sample_rate"].value_or(out.capture.sample_rate);
        out.capture.channels    = cap["channels"].value_or(out.capture.channels);
        out.capture.format      = cap["format"].value_or(out.capture.format);
        out.capture.frame_ms    = cap["frame_ms"].value_or(out.capture.frame_ms);
        if (auto arr = cap["channel_map"].as_array()) {
            out.capture.channel_map.clear();
            for (auto& v : *arr) out.capture.channel_map.push_back((int)v.value_or(0));
        }

        auto pb = audio["playback"];
        out.playback.device      = pb["device"].value_or(out.playback.device);
        out.playback.sample_rate = pb["sample_rate"].value_or(out.playback.sample_rate);
        out.playback.channels    = pb["channels"].value_or(out.playback.channels);
        out.playback.format      = pb["format"].value_or(out.playback.format);
        out.playback.frame_ms    = pb["frame_ms"].value_or(out.playback.frame_ms);

        auto pr = audio["processing"];
        out.processing.pipeline               = pr["pipeline"].value_or(out.processing.pipeline);
        out.processing.aec                    = pr["aec"].value_or(out.processing.aec);
        out.processing.ns                     = pr["ns"].value_or(out.processing.ns);
        out.processing.agc                    = pr["agc"].value_or(out.processing.agc);
        out.processing.vad                    = pr["vad"].value_or(out.processing.vad);
        out.processing.beamforming            = pr["beamforming"].value_or(out.processing.beamforming);
        out.processing.output_sample_rate     = pr["output_sample_rate"].value_or(out.processing.output_sample_rate);
        out.processing.output_channels        = pr["output_channels"].value_or(out.processing.output_channels);
        out.processing.aec_reference_delay_ms = pr["aec_reference_delay_ms"].value_or(out.processing.aec_reference_delay_ms);

        auto vol = audio["volume"];
        out.volume.master       = (float)vol["master"].value_or((double)out.volume.master);
        out.volume.tts          = (float)vol["tts"].value_or((double)out.volume.tts);
        out.volume.prompt       = (float)vol["prompt"].value_or((double)out.volume.prompt);
        out.volume.media        = (float)vol["media"].value_or((double)out.volume.media);
        out.volume.alarm        = (float)vol["alarm"].value_or((double)out.volume.alarm);
        out.volume.ducking_gain = (float)vol["ducking_gain"].value_or((double)out.volume.ducking_gain);

        auto dev = audio["device"];
        out.device.max_recover_retries     = dev["max_recover_retries"].value_or(out.device.max_recover_retries);
        out.device.recover_backoff_base_ms = dev["recover_backoff_base_ms"].value_or(out.device.recover_backoff_base_ms);

        auto diag = audio["diagnostics"];
        out.diagnostics.enable_metrics    = diag["enable_metrics"].value_or(out.diagnostics.enable_metrics);
        out.diagnostics.enable_audio_dump = diag["enable_audio_dump"].value_or(out.diagnostics.enable_audio_dump);
        out.diagnostics.dump_dir          = diag["dump_dir"].value_or(out.diagnostics.dump_dir);
    } catch (const std::exception& e) {
        ALOG_ERR("loadConfig({}) failed: {}", config_path, e.what());
    }
    return out;
}

} // namespace audio






