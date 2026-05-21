// test_audio_manager.cpp
// Smoke test for the AudioManager API.  This exercises the full lifecycle
// without asserting on real audio I/O so it runs cleanly on systems without
// audio hardware (such as plain WSL).

#include "AudioManager.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

bool fileExists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
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

int countTests = 0;
int failedTests = 0;

#define EXPECT(cond, msg)                                                  \
    do {                                                                   \
        ++countTests;                                                      \
        if (!(cond)) {                                                     \
            ++failedTests;                                                 \
            std::cerr << "[FAIL] " << msg                                  \
                      << " @ " << __FILE__ << ":" << __LINE__ << "\n";     \
        } else {                                                           \
            std::cout << "[PASS] " << msg << "\n";                         \
        }                                                                  \
    } while (0)

std::vector<int16_t> makeSineFrame(int rate, int channels, int duration_ms,
                                   float freq_hz, float amplitude = 0.3f) {
    const int frames = rate * duration_ms / 1000;
    std::vector<int16_t> out((size_t)frames * channels);
    for (int n = 0; n < frames; ++n) {
        const float v = amplitude * std::sin(2.0f * 3.14159265f * freq_hz *
                                             (float)n / (float)rate);
        const int16_t s = (int16_t)(v * 32767.0f);
        for (int c = 0; c < channels; ++c) {
            out[(size_t)n * channels + c] = s;
        }
    }
    return out;
}

} // namespace

int main() {
    using namespace audio;

    const std::string cfg_path = findConfig();
    std::cout << "Loading config: " << cfg_path << "\n";
    Config cfg = loadConfig(cfg_path);

    EXPECT(cfg.backend == "alsa",                "backend defaults to alsa");
    EXPECT(cfg.capture.sample_rate == 48000,     "capture rate 48000");
    EXPECT(cfg.processing.output_sample_rate == 16000,
                                                 "processing output 16000");
    EXPECT(cfg.processing.aec_reference_delay_ms == 120,
                                                 "AEC ref delay 120ms");
    EXPECT(cfg.volume.master > 0.0f,             "master volume > 0");

    AudioManager mgr;
    std::atomic<int> events{0};
    mgr.subscribe([&events](const AudioEvent& ev) {
        ++events;
        std::cout << "event type=" << (int)ev.type << " handle=" << ev.handle
                  << "\n";
    });

    bool ok = mgr.init(cfg);
    if (!ok) {
        std::cerr << "[WARN] AudioManager::init failed (no audio device).  "
                     "Smoke test will only validate config loading.\n";
        // We still consider this a pass for environments without audio
        // because the architecture mandates graceful degradation.
        std::cout << "Tests run: " << countTests << "  failed: " << failedTests
                  << "\n";
        return failedTests == 0 ? 0 : 1;
    }
    EXPECT(ok, "AudioManager::init returns true on success");

    // Frame consumer registration.
    std::atomic<int> frames{0};
    auto h1 = mgr.addFrameConsumer(
        [&frames](const AudioFrame& f) {
            (void)f;
            ++frames;
        },
        16);
    EXPECT(h1 != kInvalidConsumerHandle, "consumer added");

    // Start capture (may not deliver frames in WSL but must not crash).
    bool cap_ok = mgr.startCapture();
    EXPECT(cap_ok, "startCapture succeeds");

    // Volume control.
    mgr.setMasterVolume(0.5f);
    EXPECT(std::fabs(mgr.masterVolume() - 0.5f) < 1e-3f, "master volume set");

    // Simple playback request – brief sine.
    PlaybackRequest req;
    req.pcm_data    = makeSineFrame(cfg.playback.sample_rate,
                                    cfg.playback.channels, 200, 440.0f);
    req.sample_rate = cfg.playback.sample_rate;
    req.channels    = cfg.playback.channels;
    req.priority    = PlaybackPriority::PROMPT;
    req.stream_gain = 0.5f;
    PlaybackHandle ph = mgr.play(req);
    EXPECT(ph != kInvalidPlaybackHandle, "playback handle issued");

    // Allow some time for capture / playback threads to run.
    std::this_thread::sleep_for(500ms);

    // Health snapshot.
    HealthStatus hs = mgr.health();
    EXPECT(hs.aec_reference_delay_ms == cfg.processing.aec_reference_delay_ms,
           "health includes AEC delay");

    // Device listings.
    auto in_devs  = mgr.listInputDevices();
    auto out_devs = mgr.listOutputDevices();
    EXPECT(!in_devs.empty(),  "inputs enumerated");
    EXPECT(!out_devs.empty(), "outputs enumerated");

    // Stop.
    mgr.stop(ph);
    mgr.stopAll();
    mgr.removeFrameConsumer(h1);
    mgr.stopCapture();
    mgr.shutdown();

    std::cout << "Tests run: " << countTests << "  failed: " << failedTests
              << "  events seen: " << events.load() << "\n";
    return failedTests == 0 ? 0 : 1;
}
