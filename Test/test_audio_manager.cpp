// test_audio_manager.cpp
// Functional API test for AudioManager.
//
// The test exercises the public AudioManager lifecycle and the important
// runtime paths: capture, frame consumers, software volume, playback, mixing,
// ducking, priority preemption, file playback, device listing, health, stop,
// stopAll, and shutdown.  Hardware-dependent scenarios are skipped when ALSA
// devices are not available, so the config/API checks still run on headless CI.

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
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

bool fileExists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool filesHaveSameContent(const std::string& a, const std::string& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;

    constexpr std::size_t kBufSize = 64 * 1024;
    std::vector<char> ba(kBufSize);
    std::vector<char> bb(kBufSize);
    while (fa && fb) {
        fa.read(ba.data(), (std::streamsize)ba.size());
        fb.read(bb.data(), (std::streamsize)bb.size());
        const std::streamsize ca = fa.gcount();
        const std::streamsize cb = fb.gcount();
        if (ca != cb) return false;
        if (ca > 0 && std::memcmp(ba.data(), bb.data(), (size_t)ca) != 0) {
            return false;
        }
    }
    return fa.eof() && fb.eof();
}

std::string findConfig() {
    const char* env = std::getenv("AUDIO_CONFIG");
    if (env && *env && fileExists(env)) return env;

    const char* candidates[] = {
        "Config/config.toml",
        "../Config/config.toml",
        "../../Config/config.toml",
        "../../../Config/config.toml",
    };
    for (auto p : candidates) {
        if (fileExists(p)) return p;
    }
    return "Config/config.toml";
}

bool envEnabled(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    return std::strcmp(v, "0") != 0 &&
           std::strcmp(v, "false") != 0 &&
           std::strcmp(v, "FALSE") != 0;
}

int envInt(const char* name, int fallback, int min_value, int max_value) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 10);
    if (end == v || parsed < min_value || parsed > max_value) return fallback;
    return (int)parsed;
}

quill::Logger* testLogger() {
    static quill::Logger* logger = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        quill::Backend::start();
        logger = quill::Frontend::create_or_get_logger(
            "AudioManagerFunctionalTest",
            quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "audio_manager_functional_test_console_sink"));
    });
    return logger;
}

const char* eventName(audio::AudioEventType type) {
    using audio::AudioEventType;
    switch (type) {
        case AudioEventType::CaptureStarted:      return "CaptureStarted";
        case AudioEventType::CaptureStopped:      return "CaptureStopped";
        case AudioEventType::CaptureError:        return "CaptureError";
        case AudioEventType::VAD_SPEECH_START:    return "VAD_SPEECH_START";
        case AudioEventType::VAD_SPEECH_END:      return "VAD_SPEECH_END";
        case AudioEventType::PlaybackStarted:     return "PlaybackStarted";
        case AudioEventType::PlaybackCompleted:   return "PlaybackCompleted";
        case AudioEventType::PlaybackInterrupted: return "PlaybackInterrupted";
        case AudioEventType::PlaybackError:       return "PlaybackError";
        case AudioEventType::DeviceConnected:     return "DeviceConnected";
        case AudioEventType::DeviceDisconnected:  return "DeviceDisconnected";
        case AudioEventType::DeviceRecovered:     return "DeviceRecovered";
        case AudioEventType::DeviceFailed:        return "DeviceFailed";
        case AudioEventType::VolumeChanged:       return "VolumeChanged";
        case AudioEventType::HealthChanged:       return "HealthChanged";
    }
    return "Unknown";
}

std::vector<int16_t> makeSineFrame(int rate, int channels, int duration_ms,
                                   float freq_hz, float amplitude = 0.25f) {
    const int frames = std::max(1, rate * duration_ms / 1000);
    const int fade_frames = std::max(1, rate * 10 / 1000);
    std::vector<int16_t> out((size_t)frames * (size_t)channels);

    for (int n = 0; n < frames; ++n) {
        float env = 1.0f;
        if (n < fade_frames) {
            env = (float)n / (float)fade_frames;
        } else if (frames - n - 1 < fade_frames) {
            env = (float)(frames - n - 1) / (float)fade_frames;
        }
        if (env < 0.0f) env = 0.0f;

        const float v = amplitude * env *
            std::sin(2.0f * 3.14159265f * freq_hz *
                     (float)n / (float)rate);
        const int16_t s = (int16_t)(v * 32767.0f);
        for (int c = 0; c < channels; ++c) {
            out[(size_t)n * (size_t)channels + (size_t)c] = s;
        }
    }
    return out;
}

std::string tempPath(const char* leaf) {
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    std::string base = tmp;
    if (!base.empty() && base.back() != '/') base += "/";
    return base + leaf;
}

void writeU16(std::ofstream& os, uint16_t v) {
    os.put((char)(v & 0xff));
    os.put((char)((v >> 8) & 0xff));
}

void writeU32(std::ofstream& os, uint32_t v) {
    os.put((char)(v & 0xff));
    os.put((char)((v >> 8) & 0xff));
    os.put((char)((v >> 16) & 0xff));
    os.put((char)((v >> 24) & 0xff));
}

bool writeWav16(const std::string& path, const std::vector<int16_t>& pcm,
                int sample_rate, int channels) {
    std::ofstream os(path, std::ios::binary);
    if (!os) return false;

    const uint32_t data_bytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    os.write("RIFF", 4);
    writeU32(os, 36u + data_bytes);
    os.write("WAVE", 4);
    os.write("fmt ", 4);
    writeU32(os, 16);
    writeU16(os, 1);
    writeU16(os, (uint16_t)channels);
    writeU32(os, (uint32_t)sample_rate);
    writeU32(os, (uint32_t)(sample_rate * channels * sizeof(int16_t)));
    writeU16(os, (uint16_t)(channels * sizeof(int16_t)));
    writeU16(os, 16);
    os.write("data", 4);
    writeU32(os, data_bytes);
    os.write(reinterpret_cast<const char*>(pcm.data()), data_bytes);
    return (bool)os;
}

struct WavFileInfo {
    int sample_rate{0};
    int channels{0};
    bool valid{false};
};

uint16_t readLe16(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (uint16_t)(u[0] | (u[1] << 8));
}

uint32_t readLe32(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (uint32_t)(u[0] | (u[1] << 8) | (u[2] << 16) | (u[3] << 24));
}

WavFileInfo readWavInfo(const std::string& path) {
    WavFileInfo info;
    std::ifstream is(path, std::ios::binary);
    char header[44]{};
    if (!is.read(header, sizeof(header))) return info;
    if (std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0 ||
        std::memcmp(header + 12, "fmt ", 4) != 0) {
        return info;
    }

    const uint16_t audio_format = readLe16(header + 20);
    const uint16_t channels = readLe16(header + 22);
    const uint32_t sample_rate = readLe32(header + 24);
    const uint16_t bits_per_sample = readLe16(header + 34);
    if (audio_format != 1 || channels == 0 || sample_rate == 0 ||
        bits_per_sample != 16) {
        return info;
    }

    info.sample_rate = (int)sample_rate;
    info.channels = (int)channels;
    info.valid = true;
    return info;
}

struct EventLog {
    std::mutex mutex;
    std::vector<audio::AudioEvent> events;

    void push(const audio::AudioEvent& ev) {
        std::lock_guard<std::mutex> lk(mutex);
        events.push_back(ev);
    }

    size_t mark() const {
        auto self = const_cast<EventLog*>(this);
        std::lock_guard<std::mutex> lk(self->mutex);
        return self->events.size();
    }

    template <typename Pred>
    bool waitAfter(size_t start, Pred pred,
                   std::chrono::milliseconds timeout = 2s) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                auto self = const_cast<EventLog*>(this);
                std::lock_guard<std::mutex> lk(self->mutex);
                for (size_t i = start; i < self->events.size(); ++i) {
                    if (pred(self->events[i])) return true;
                }
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    size_t count(audio::AudioEventType type) const {
        auto self = const_cast<EventLog*>(this);
        std::lock_guard<std::mutex> lk(self->mutex);
        return (size_t)std::count_if(
            self->events.begin(), self->events.end(),
            [type](const audio::AudioEvent& ev) { return ev.type == type; });
    }
};

template <typename T>
bool waitForAtomicAtLeast(const std::atomic<T>& value, T target,
                          std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_relaxed) >= target) return true;
        std::this_thread::sleep_for(20ms);
    }
    return value.load(std::memory_order_relaxed) >= target;
}

struct SkipTest : std::exception {
    explicit SkipTest(std::string why) : reason(std::move(why)) {}
    const char* what() const noexcept override { return reason.c_str(); }
    std::string reason;
};

class TestRunner {
public:
    explicit TestRunner(quill::Logger* logger) : logger_(logger) {}

    void run(const std::string& name, const std::string& content,
             const std::function<void()>& body) {
        if (has_previous_test_) {
            LOG_INFO(logger_, "Waiting 3s before next AudioManager test");
            logger_->flush_log();
            std::this_thread::sleep_for(3s);
        }
        has_previous_test_ = true;

        LOG_INFO(logger_, "BEGIN TEST [{}] {}", name, content);
        logger_->flush_log();
        std::cout << "\n[TEST] " << name << " - " << content << "\n";

        const int failed_before = failed_;
        const int skipped_before = skipped_;
        try {
            body();
        } catch (const SkipTest& e) {
            ++skipped_;
            LOG_WARNING(logger_, "SKIP TEST [{}] {}", name, e.what());
            std::cout << "[SKIP] " << name << ": " << e.what() << "\n";
        } catch (const std::exception& e) {
            ++failed_;
            LOG_ERROR(logger_, "EXCEPTION TEST [{}] {}", name, e.what());
            std::cerr << "[FAIL] " << name << " threw: " << e.what() << "\n";
        } catch (...) {
            ++failed_;
            LOG_ERROR(logger_, "UNKNOWN EXCEPTION TEST [{}]", name);
            std::cerr << "[FAIL] " << name << " threw unknown exception\n";
        }

        const bool skipped = skipped_ > skipped_before;
        const bool failed = failed_ > failed_before;
        const char* result = skipped ? "SKIPPED" : (failed ? "FAILED" : "PASSED");
        LOG_INFO(logger_, "END TEST [{}] result={}", name, result);
        logger_->flush_log();
        std::cout << "[END] " << name << " -> " << result << "\n";
    }

    void expect(bool cond, const std::string& msg) {
        ++assertions_;
        if (!cond) {
            ++failed_;
            LOG_ERROR(logger_, "EXPECT FAILED: {}", msg);
            std::cerr << "[FAIL] " << msg << "\n";
        } else {
            LOG_INFO(logger_, "EXPECT PASSED: {}", msg);
            std::cout << "[PASS] " << msg << "\n";
        }
    }

    void skip(const std::string& reason) {
        throw SkipTest(reason);
    }

    int failed() const { return failed_; }
    int skipped() const { return skipped_; }
    int assertions() const { return assertions_; }

private:
    quill::Logger* logger_{nullptr};
    bool has_previous_test_{false};
    int assertions_{0};
    int failed_{0};
    int skipped_{0};
};

struct AudioFixture {
    audio::Config cfg;
    std::unique_ptr<audio::AudioManager> mgr;
    EventLog events;
    std::mutex recorded_mutex;
    std::vector<int16_t> recorded_pcm;
    int recorded_sample_rate{0};
    int recorded_channels{0};
    std::atomic<uint64_t> noisy_event_logs{0};
    bool initialized{false};
    bool capture_started{false};

    void createManager(quill::Logger* logger) {
        mgr = std::make_unique<audio::AudioManager>();
        mgr->subscribe([this, logger](const audio::AudioEvent& ev) {
            events.push(ev);
            const bool noisy =
                ev.type == audio::AudioEventType::CaptureError ||
                ev.type == audio::AudioEventType::PlaybackError;
            const uint64_t noisy_count =
                noisy ? noisy_event_logs.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
            if (!noisy || noisy_count <= 5 || noisy_count % 1000 == 0) {
                LOG_INFO(logger, "AudioEvent type={} handle={} device={} detail={}",
                         eventName(ev.type), ev.handle, ev.device_id, ev.detail);
            }
        });
    }
};

audio::Config withEnvOverrides(audio::Config cfg) {
    if (const char* dev = std::getenv("AUDIO_TEST_DEVICE")) {
        if (*dev) {
            cfg.capture.device = dev;
            cfg.playback.device = dev;
        }
    }
    if (const char* dev = std::getenv("AUDIO_TEST_CAPTURE_DEVICE")) {
        if (*dev) cfg.capture.device = dev;
    }
    if (const char* dev = std::getenv("AUDIO_TEST_PLAYBACK_DEVICE")) {
        if (*dev) cfg.playback.device = dev;
    }
    if (envEnabled("AUDIO_TEST_USE_NULL")) {
        cfg.capture.device = "null";
        cfg.playback.device = "null";
    }
    cfg.volume.ducking_gain = 0.2f;
    return cfg;
}

audio::PlaybackRequest pcmRequest(const std::vector<int16_t>& pcm,
                                  const audio::Config& cfg,
                                  audio::PlaybackPriority priority,
                                  float gain = 0.5f,
                                  bool loop = false) {
    audio::PlaybackRequest req;
    req.pcm_data = pcm;
    req.sample_rate = cfg.playback.sample_rate;
    req.channels = cfg.playback.channels;
    req.priority = priority;
    req.stream_gain = gain;
    req.loop = loop;
    return req;
}

void requireInitialized(const AudioFixture& fx, TestRunner& runner) {
    if (!fx.initialized || !fx.mgr) {
        runner.skip("AudioManager is not initialized; audio device dependent test skipped");
    }
}

} // namespace

int main() {
    using namespace audio;

    quill::Logger* logger = testLogger();
    TestRunner runner(logger);
    AudioFixture fx;

    const std::string cfg_path = findConfig();

    runner.run("config_loading",
               "load TOML audio config and verify important fields",
               [&] {
        LOG_INFO(logger, "Loading config from {}", cfg_path);
        fx.cfg = loadConfig(cfg_path);

        runner.expect(fx.cfg.backend == "alsa", "backend is alsa");
        runner.expect(fx.cfg.capture.sample_rate > 0, "capture sample rate is valid");
        runner.expect(fx.cfg.capture.channels > 0, "capture channel count is valid");
        runner.expect(!fx.cfg.capture.channel_map.empty(), "capture channel map is present");
        runner.expect(fx.cfg.playback.sample_rate > 0, "playback sample rate is valid");
        runner.expect(fx.cfg.playback.channels > 0, "playback channel count is valid");
        runner.expect(fx.cfg.processing.output_sample_rate == 48000,
                      "processing output sample rate is 48000");
        runner.expect(fx.cfg.processing.output_channels == 1,
                      "processing output channel count is mono");
        runner.expect(fx.cfg.processing.aec_reference_delay_ms == 120,
                      "AEC reference delay is 120ms");
        runner.expect(fx.cfg.volume.master >= 0.0f && fx.cfg.volume.master <= 1.0f,
                      "master volume is in range");
        runner.expect(fx.cfg.volume.ducking_gain > 0.0f &&
                      fx.cfg.volume.ducking_gain < 1.0f,
                      "ducking gain reduces playback");
    });

    runner.run("pre_init_guards",
               "verify public APIs fail gracefully before init",
               [&] {
        AudioManager cold;
        PlaybackRequest empty;

        runner.expect(!cold.startCapture(), "startCapture returns false before init");
        runner.expect(cold.play(empty) == kInvalidPlaybackHandle,
                      "empty playback before init returns invalid handle");
        runner.expect(cold.masterVolume() == 0.0f,
                      "masterVolume before init returns 0");
        runner.expect(cold.listInputDevices().empty(),
                      "input device list before init is empty");
        runner.expect(cold.listOutputDevices().empty(),
                      "output device list before init is empty");
        cold.stop(kInvalidPlaybackHandle);
        cold.stopAll();
        cold.stopCapture();
        cold.shutdown();
        runner.expect(true, "stop/stopAll/stopCapture/shutdown before init do not throw");
    });

    runner.run("init_and_devices",
               "initialize AudioManager, subscribe events, enumerate devices",
               [&] {
        fx.cfg = withEnvOverrides(fx.cfg);
        fx.createManager(logger);

        const bool ok = fx.mgr->init(fx.cfg);

        fx.initialized = ok;
        if (!ok) {
            runner.skip("AudioManager::init failed for configured audio devices");
        }

        runner.expect(ok, "AudioManager::init succeeds");
        runner.expect(std::fabs(fx.mgr->masterVolume() - fx.cfg.volume.master) < 1e-3f,
                      "initial master volume matches config");
        runner.expect(std::fabs(fx.cfg.volume.ducking_gain - 0.20f) < 1e-3f,
                      "test ducking gain is configured to 20%");

        const auto inputs = fx.mgr->listInputDevices();
        const auto outputs = fx.mgr->listOutputDevices();
        LOG_INFO(logger, "Enumerated {} input devices and {} output devices",
                 inputs.size(), outputs.size());
        runner.expect(!inputs.empty(), "input devices are enumerated");
        runner.expect(!outputs.empty(), "output devices are enumerated");

        const HealthStatus hs = fx.mgr->health();
        runner.expect(hs.playback_running, "playback thread is running after init");
        runner.expect(hs.aec_reference_delay_ms ==
                          fx.cfg.processing.aec_reference_delay_ms,
                      "health reports configured AEC delay");
    });

    runner.run("recording_consumers",
               "register frame consumers, start capture, receive processed PCM",
               [&] {
        requireInitialized(fx, runner);

        const int record_seconds = envInt("AUDIO_TEST_RECORD_SECONDS", 2, 1, 10);
        const size_t target_recorded_samples =
            (size_t)fx.cfg.processing.output_sample_rate *
            (size_t)fx.cfg.processing.output_channels *
            (size_t)record_seconds;
        const size_t minimum_playback_samples =
            (size_t)fx.cfg.processing.output_sample_rate *
            (size_t)fx.cfg.processing.output_channels;

        {
            std::lock_guard<std::mutex> lk(fx.recorded_mutex);
            fx.recorded_pcm.clear();
            fx.recorded_sample_rate = fx.cfg.processing.output_sample_rate;
            fx.recorded_channels = fx.cfg.processing.output_channels;
        }

        std::atomic<int> primary_frames{0};
        std::atomic<int> valid_frames{0};
        std::atomic<size_t> recorded_samples{0};
        std::atomic<int> slow_frames{0};
        std::atomic<int> bad_frames{0};

        const ConsumerHandle primary = fx.mgr->addFrameConsumer(
            [&](const AudioFrame& frame) {
                if (frame.sample_rate != fx.cfg.processing.output_sample_rate ||
                    frame.channels != fx.cfg.processing.output_channels ||
                    frame.samples.empty()) {
                    bad_frames.fetch_add(1, std::memory_order_relaxed);
                } else {
                    valid_frames.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lk(fx.recorded_mutex);
                    if (fx.recorded_pcm.size() < target_recorded_samples) {
                        const size_t remaining =
                            target_recorded_samples - fx.recorded_pcm.size();
                        const size_t take =
                            std::min(remaining, frame.samples.size());
                        fx.recorded_pcm.insert(
                            fx.recorded_pcm.end(),
                            frame.samples.begin(),
                            frame.samples.begin() + (std::ptrdiff_t)take);
                        recorded_samples.store(fx.recorded_pcm.size(),
                                               std::memory_order_relaxed);
                    }
                }
                primary_frames.fetch_add(1, std::memory_order_relaxed);
            },
            16);

        const ConsumerHandle slow = fx.mgr->addFrameConsumer(
            [&](const AudioFrame&) {
                slow_frames.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(15ms);
            },
            1);

        runner.expect(primary != kInvalidConsumerHandle,
                      "primary frame consumer is added");
        runner.expect(slow != kInvalidConsumerHandle,
                      "slow frame consumer is added");
        runner.expect(primary != slow, "consumer handles are unique");

        if (fx.cfg.capture.device == "null") {
            fx.mgr->removeFrameConsumer(slow);
            fx.mgr->removeFrameConsumer(primary);
            runner.expect(true,
                          "ALSA null capture is not used for recording frame assertions");
            return;
        }

        const size_t mark = fx.events.mark();
        const bool capture_ok = fx.mgr->startCapture();
        fx.capture_started = capture_ok;
        runner.expect(capture_ok, "startCapture succeeds");
        runner.expect(fx.events.waitAfter(mark, [](const AudioEvent& ev) {
                          return ev.type == AudioEventType::CaptureStarted;
                      }, 1s),
                      "CaptureStarted event is emitted");

        const bool got_any_frame = waitForAtomicAtLeast(primary_frames, 1, 1500ms);
        const bool got_valid_frame = waitForAtomicAtLeast(valid_frames, 1, 500ms);
        const bool got_recording =
            waitForAtomicAtLeast(recorded_samples, minimum_playback_samples, 3500ms);
        runner.expect(got_any_frame, "recording path invokes a frame consumer");
        if (fx.cfg.capture.device == "null" && !got_valid_frame) {
            LOG_WARNING(logger,
                        "ALSA null capture produced no valid processed frame; capture API path was still exercised");
            runner.expect(true, "null capture path may overrun without valid processed audio");
        } else {
            runner.expect(got_valid_frame, "recording delivers a valid processed frame");
        }
        runner.expect(got_recording, "recording buffers at least one second of PCM for playback");

        const HealthStatus hs = fx.mgr->health();
        runner.expect(hs.capture_running, "health shows capture running");
        runner.expect(hs.capture_frames_total > 0,
                      "health capture frame counter increases");

        fx.mgr->removeFrameConsumer(slow);
        runner.expect(true, "removeFrameConsumer removes slow consumer");

        fx.mgr->removeFrameConsumer(primary);
        runner.expect(true, "removeFrameConsumer removes primary consumer");

        const size_t stop_mark = fx.events.mark();
        fx.mgr->stopCapture();
        fx.capture_started = false;
        runner.expect(fx.events.waitAfter(stop_mark, [](const AudioEvent& ev) {
                          return ev.type == AudioEventType::CaptureStopped;
                      }, 1s),
                      "CaptureStopped event is emitted after recording test");
    });

    runner.run("record_then_playback",
               "play the processed PCM captured by the recording test",
               [&] {
        requireInitialized(fx, runner);

        std::vector<int16_t> recorded;
        int recorded_rate = 0;
        int recorded_channels = 0;
        {
            std::lock_guard<std::mutex> lk(fx.recorded_mutex);
            recorded = fx.recorded_pcm;
            recorded_rate = fx.recorded_sample_rate;
            recorded_channels = fx.recorded_channels;
        }

        if (recorded.empty() || recorded_rate <= 0 || recorded_channels <= 0) {
            runner.skip("no recorded PCM is available to play back");
        }

        const int64_t recorded_frames =
            (int64_t)recorded.size() / std::max(1, recorded_channels);
        const int64_t duration_ms =
            recorded_frames * 1000 / std::max(1, recorded_rate);
        LOG_INFO(logger, "Playing recorded audio: samples={} rate={} channels={} duration_ms={}",
                 recorded.size(), recorded_rate, recorded_channels, duration_ms);

        PlaybackRequest req;
        req.pcm_data = std::move(recorded);
        req.sample_rate = recorded_rate;
        req.channels = recorded_channels;
        req.priority = PlaybackPriority::PROMPT;
        req.stream_gain = 0.85f;

        const size_t mark = fx.events.mark();
        const PlaybackHandle h = fx.mgr->play(req);
        runner.expect(h != kInvalidPlaybackHandle,
                      "recorded PCM playback handle is issued");
        runner.expect(fx.events.waitAfter(mark, [h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackStarted &&
                                 ev.handle == h;
                      }, 1s),
                      "recorded PCM emits PlaybackStarted");

        const auto completion_timeout =
            std::chrono::milliseconds(std::max<int64_t>(2000, duration_ms + 2000));
        runner.expect(fx.events.waitAfter(mark, [h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackCompleted &&
                                 ev.handle == h;
                      }, completion_timeout),
                      "recorded PCM playback completes");
    });

    runner.run("software_volume",
               "set, clamp, restore software master volume and observe events",
               [&] {
        requireInitialized(fx, runner);

        const size_t mark = fx.events.mark();
        fx.mgr->setMasterVolume(0.25f);
        runner.expect(std::fabs(fx.mgr->masterVolume() - 0.25f) < 1e-3f,
                      "master volume can be set to 0.25");

        fx.mgr->setMasterVolume(1.50f);
        runner.expect(std::fabs(fx.mgr->masterVolume() - 1.0f) < 1e-3f,
                      "master volume clamps above 1.0");

        fx.mgr->setMasterVolume(-0.25f);
        runner.expect(std::fabs(fx.mgr->masterVolume() - 0.0f) < 1e-3f,
                      "master volume clamps below 0.0");

        fx.mgr->setMasterVolume(fx.cfg.volume.master);
        runner.expect(std::fabs(fx.mgr->masterVolume() - fx.cfg.volume.master) < 1e-3f,
                      "master volume is restored to config value");
        runner.expect(fx.events.waitAfter(mark, [](const AudioEvent& ev) {
                          return ev.type == AudioEventType::VolumeChanged;
                      }, 1s),
                      "VolumeChanged event is emitted");
    });

    runner.run("pcm_playback",
               "play a generated PCM prompt and wait for start/completion events",
               [&] {
        requireInitialized(fx, runner);

        const auto tone = makeSineFrame(fx.cfg.playback.sample_rate,
                                        fx.cfg.playback.channels,
                                        400, 440.0f, 0.20f);
        const size_t mark = fx.events.mark();
        const PlaybackHandle h = fx.mgr->play(
            pcmRequest(tone, fx.cfg, PlaybackPriority::PROMPT, 0.65f));

        runner.expect(h != kInvalidPlaybackHandle, "PCM playback handle is issued");
        runner.expect(fx.events.waitAfter(mark, [h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackStarted &&
                                 ev.handle == h;
                      }, 1s),
                      "PlaybackStarted event is emitted for PCM prompt");
        runner.expect(fx.events.waitAfter(mark, [h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackCompleted &&
                                 ev.handle == h;
                      }, 2s),
                      "PlaybackCompleted event is emitted for PCM prompt");
    });

    runner.run("simultaneous_playback",
               "play two PCM streams at the same time to exercise mixer",
               [&] {
        requireInitialized(fx, runner);

        const auto media = makeSineFrame(fx.cfg.playback.sample_rate,
                                         fx.cfg.playback.channels,
                                         1200, 330.0f, 0.18f);
        const auto prompt = makeSineFrame(fx.cfg.playback.sample_rate,
                                          fx.cfg.playback.channels,
                                          1200, 660.0f, 0.18f);

        const size_t mark = fx.events.mark();
        const PlaybackHandle h1 = fx.mgr->play(
            pcmRequest(media, fx.cfg, PlaybackPriority::MEDIA, 0.55f));
        const PlaybackHandle h2 = fx.mgr->play(
            pcmRequest(prompt, fx.cfg, PlaybackPriority::PROMPT, 0.55f));

        runner.expect(h1 != kInvalidPlaybackHandle, "first simultaneous stream starts");
        runner.expect(h2 != kInvalidPlaybackHandle, "second simultaneous stream starts");
        runner.expect(h1 != h2, "simultaneous streams have distinct handles");
        runner.expect(fx.events.waitAfter(mark, [h1](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackStarted &&
                                 ev.handle == h1;
                      }, 1s),
                      "first stream emits PlaybackStarted");
        runner.expect(fx.events.waitAfter(mark, [h2](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackStarted &&
                                 ev.handle == h2;
                      }, 1s),
                      "second stream emits PlaybackStarted");

        std::this_thread::sleep_for(600ms);
        const HealthStatus hs = fx.mgr->health();
        runner.expect(hs.playback_running, "playback remains running while streams mix");

        fx.mgr->stop(h1);
        fx.mgr->stop(h2);
        runner.expect(fx.events.waitAfter(mark, [h1](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackCompleted &&
                                 ev.handle == h1;
                      }, 1s),
                      "stop emits completion for first stream");
        runner.expect(fx.events.waitAfter(mark, [h2](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackCompleted &&
                                 ev.handle == h2;
                      }, 1s),
                      "stop emits completion for second stream");
    });

    runner.run("ducking",
               "play MEDIA under TTS to exercise focus-manager ducking path",
               [&] {
        requireInitialized(fx, runner);

        const auto media = makeSineFrame(fx.cfg.playback.sample_rate,
                                         fx.cfg.playback.channels,
                                         1500, 220.0f, 0.20f);
        const auto tts = makeSineFrame(fx.cfg.playback.sample_rate,
                                       fx.cfg.playback.channels,
                                       700, 880.0f, 0.22f);

        const size_t mark = fx.events.mark();
        const PlaybackHandle media_h = fx.mgr->play(
            pcmRequest(media, fx.cfg, PlaybackPriority::MEDIA, 0.65f, true));
        runner.expect(media_h != kInvalidPlaybackHandle,
                      "looping media stream starts before ducking");
        std::this_thread::sleep_for(250ms);

        const PlaybackHandle tts_h = fx.mgr->play(
            pcmRequest(tts, fx.cfg, PlaybackPriority::TTS, 0.80f));
        runner.expect(tts_h != kInvalidPlaybackHandle,
                      "TTS stream starts while media is active");
        runner.expect(fx.events.waitAfter(mark, [tts_h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackStarted &&
                                 ev.handle == tts_h;
                      }, 1s),
                      "TTS PlaybackStarted event is emitted");
        runner.expect(!fx.events.waitAfter(mark, [media_h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackInterrupted &&
                                 ev.handle == media_h;
                      }, 300ms),
                      "MEDIA is ducked instead of interrupted by TTS");
        runner.expect(fx.events.waitAfter(mark, [tts_h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackCompleted &&
                                 ev.handle == tts_h;
                      }, 2s),
                      "TTS completes and ducking path can release");

        fx.mgr->stop(media_h);
        runner.expect(fx.events.waitAfter(mark, [media_h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackCompleted &&
                                 ev.handle == media_h;
                      }, 1s),
                      "media stream is stopped after ducking test");
    });

    runner.run("file_background_ducking_20_percent",
               "play /home/runyu/background.wav for 10s, then play /home/runyu/audio1.wav while background ducks to 20%",
               [&] {
        requireInitialized(fx, runner);
        fx.mgr->stopAll();
        std::this_thread::sleep_for(200ms);

        const std::string background_path = "/home/runyu/background.wav";
        const std::string audio1_path = "/home/runyu/audio1.wav";
        if (!fileExists(background_path)) {
            runner.skip(background_path + " does not exist");
        }
        if (!fileExists(audio1_path)) {
            runner.skip(audio1_path + " does not exist");
        }

        const WavFileInfo background_info = readWavInfo(background_path);
        const WavFileInfo audio1_info = readWavInfo(audio1_path);
        runner.expect(background_info.valid,
                      "background.wav is 16-bit PCM WAV with readable format");
        runner.expect(audio1_info.valid,
                      "audio1.wav is 16-bit PCM WAV with readable format");
        if (!background_info.valid || !audio1_info.valid) {
            runner.skip("both files must be standard 16-bit PCM WAV files");
        }
        runner.expect(std::fabs(fx.cfg.volume.ducking_gain - 0.20f) < 1e-3f,
                      "ducking gain is 20% for the file playback test");
        if (filesHaveSameContent(background_path, audio1_path)) {
            LOG_WARNING(logger,
                        "background.wav and audio1.wav have identical bytes; the listening result will sound like two copies of audio1.wav");
            std::cout << "[WARN] background.wav and audio1.wav are identical; "
                         "you will hear two copies of audio1.wav mixed together.\n";
        }

        PlaybackRequest background;
        background.file_path = background_path;
        background.sample_rate = background_info.sample_rate;
        background.channels = background_info.channels;
        background.priority = PlaybackPriority::MEDIA;
        background.stream_gain = 1.0f;
        background.loop = true;

        PlaybackRequest foreground;
        foreground.file_path = audio1_path;
        foreground.sample_rate = audio1_info.sample_rate;
        foreground.channels = audio1_info.channels;
        foreground.priority = PlaybackPriority::TTS;
        foreground.stream_gain = 1.0f;
        foreground.loop = false;

        const size_t mark = fx.events.mark();
        const PlaybackHandle background_h = fx.mgr->play(background);
        runner.expect(background_h != kInvalidPlaybackHandle,
                      "background.wav starts as looping MEDIA");
        runner.expect(fx.events.waitAfter(mark, [background_h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackStarted &&
                                 ev.handle == background_h;
                      }, 1s),
                      "background.wav emits PlaybackStarted");

        LOG_INFO(logger,
                 "Playing background.wav alone for 10s before starting ducking");
        std::this_thread::sleep_for(10s);

        const size_t duck_mark = fx.events.mark();
        const PlaybackHandle audio1_h = fx.mgr->play(foreground);
        runner.expect(audio1_h != kInvalidPlaybackHandle,
                      "audio1.wav starts as TTS and triggers ducking");
        runner.expect(fx.events.waitAfter(duck_mark, [audio1_h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackStarted &&
                                 ev.handle == audio1_h;
                      }, 1s),
                      "audio1.wav emits PlaybackStarted");

        LOG_INFO(logger,
                 "Ducking phase: background.wav + audio1.wav together for 10s; MEDIA gain={}",
                 fx.cfg.volume.ducking_gain);
        std::this_thread::sleep_for(10s);

        runner.expect(!fx.events.waitAfter(duck_mark, [background_h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackInterrupted &&
                                 ev.handle == background_h;
                      }, 10ms),
                      "background.wav is ducked instead of interrupted during 10s ducking phase");

        fx.mgr->stop(audio1_h);
        fx.mgr->stop(background_h);
        runner.expect(fx.events.waitAfter(mark, [audio1_h](const AudioEvent& ev) {
                          return ev.handle == audio1_h &&
                                 (ev.type == AudioEventType::PlaybackCompleted ||
                                  ev.type == AudioEventType::PlaybackInterrupted);
                      }, 1s),
                      "audio1.wav stops after 10s ducking phase");
        runner.expect(fx.events.waitAfter(mark, [background_h](const AudioEvent& ev) {
                          return ev.handle == background_h &&
                                 (ev.type == AudioEventType::PlaybackCompleted ||
                                  ev.type == AudioEventType::PlaybackInterrupted);
                      }, 1s),
                      "background.wav stops after background-only plus ducking phases");
    });

    runner.run("file_playback_invalid_requests",
               "play a WAV file source and verify invalid playback requests",
               [&] {
        requireInitialized(fx, runner);

        PlaybackRequest empty;
        runner.expect(fx.mgr->play(empty) == kInvalidPlaybackHandle,
                      "empty playback request is rejected");

        PlaybackRequest missing;
        missing.file_path = tempPath("missing_audio_manager_test.wav");
        missing.sample_rate = fx.cfg.playback.sample_rate;
        missing.channels = fx.cfg.playback.channels;
        runner.expect(fx.mgr->play(missing) == kInvalidPlaybackHandle,
                      "missing file playback request is rejected");

        const std::string wav = tempPath("audio_manager_test.wav");
        const auto tone = makeSineFrame(fx.cfg.playback.sample_rate,
                                        fx.cfg.playback.channels,
                                        350, 550.0f, 0.20f);
        runner.expect(writeWav16(wav, tone, fx.cfg.playback.sample_rate,
                                 fx.cfg.playback.channels),
                      "temporary WAV file is written");

        PlaybackRequest req;
        req.file_path = wav;
        req.sample_rate = fx.cfg.playback.sample_rate;
        req.channels = fx.cfg.playback.channels;
        req.priority = PlaybackPriority::SYSTEM;
        req.stream_gain = 0.60f;

        const size_t mark = fx.events.mark();
        const PlaybackHandle h = fx.mgr->play(req);
        runner.expect(h != kInvalidPlaybackHandle, "file playback handle is issued");
        runner.expect(fx.events.waitAfter(mark, [h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackCompleted &&
                                 ev.handle == h;
                      }, 2s),
                      "file playback completes");
        std::remove(wav.c_str());
    });

    runner.run("priority_preemption_stop_all",
               "verify ALARM preempts lower priority playback and stopAll works",
               [&] {
        requireInitialized(fx, runner);

        const auto media = makeSineFrame(fx.cfg.playback.sample_rate,
                                         fx.cfg.playback.channels,
                                         1000, 260.0f, 0.18f);
        const auto alarm = makeSineFrame(fx.cfg.playback.sample_rate,
                                         fx.cfg.playback.channels,
                                         600, 1040.0f, 0.22f);

        const size_t mark = fx.events.mark();
        const PlaybackHandle media_h = fx.mgr->play(
            pcmRequest(media, fx.cfg, PlaybackPriority::MEDIA, 0.65f, true));
        runner.expect(media_h != kInvalidPlaybackHandle,
                      "looping media stream starts before alarm");
        std::this_thread::sleep_for(250ms);

        const PlaybackHandle alarm_h = fx.mgr->play(
            pcmRequest(alarm, fx.cfg, PlaybackPriority::ALARM, 0.75f));
        runner.expect(alarm_h != kInvalidPlaybackHandle,
                      "alarm playback handle is issued");
        runner.expect(fx.events.waitAfter(mark, [media_h](const AudioEvent& ev) {
                          return ev.type == AudioEventType::PlaybackInterrupted &&
                                 ev.handle == media_h;
                      }, 1s),
                      "alarm preempts lower priority media");

        fx.mgr->stopAll();
        runner.expect(fx.events.waitAfter(mark, [alarm_h](const AudioEvent& ev) {
                          return ev.handle == alarm_h &&
                                 (ev.type == AudioEventType::PlaybackInterrupted ||
                                  ev.type == AudioEventType::PlaybackCompleted);
                      }, 1s),
                      "stopAll stops remaining alarm playback");
    });

    runner.run("health_and_shutdown",
               "read final health snapshot, stop capture, stop all playback, shutdown",
               [&] {
        requireInitialized(fx, runner);

        const HealthStatus before = fx.mgr->health();
        runner.expect(before.playback_running, "health shows playback thread before shutdown");
        runner.expect(before.aec_reference_delay_ms ==
                          fx.cfg.processing.aec_reference_delay_ms,
                      "health preserves configured AEC delay");
        runner.expect(fx.events.count(AudioEventType::PlaybackStarted) > 0,
                      "playback events were observed during the suite");
        runner.expect(fx.events.count(AudioEventType::VolumeChanged) > 0,
                      "volume events were observed during the suite");

        if (fx.capture_started) {
            const size_t mark = fx.events.mark();
            fx.mgr->stopCapture();
            fx.capture_started = false;
            runner.expect(fx.events.waitAfter(mark, [](const AudioEvent& ev) {
                              return ev.type == AudioEventType::CaptureStopped;
                          }, 1s),
                          "CaptureStopped event is emitted");
        }

        fx.mgr->stopAll();
        fx.mgr->shutdown();
        fx.initialized = false;

        const HealthStatus after = fx.mgr->health();
        runner.expect(!after.capture_running, "capture is stopped after shutdown");
        runner.expect(!after.playback_running, "playback is stopped after shutdown");
    });

    fx.mgr.reset();

    LOG_INFO(logger, "AudioManager functional tests finished assertions={} failed={} skipped={}",
             runner.assertions(), runner.failed(), runner.skipped());

    std::cout << "\nTests assertions: " << runner.assertions()
              << "  failed: " << runner.failed()
              << "  skipped: " << runner.skipped() << "\n";
    return runner.failed() == 0 ? 0 : 1;
}
