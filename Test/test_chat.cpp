#include "AudioManager.h"
#include "e2echat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_exit_requested{false};

void onSignal(int)
{
    g_stop.store(true, std::memory_order_release);
}

bool fileExists(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::fclose(f);
    return true;
}

std::string findConfig()
{
    if (const char* env = std::getenv("CHAT_CONFIG")) {
        if (*env && fileExists(env)) {
            return env;
        }
    }
    const char* candidates[] = {
        "Config/config.toml",
        "../Config/config.toml",
        "../../Config/config.toml",
    };
    for (const char* path : candidates) {
        if (fileExists(path)) {
            return path;
        }
    }
    return "Config/config.toml";
}

std::vector<std::int16_t> bytesToPcm16(const e2echat::AudioChunk& chunk)
{
    std::vector<std::int16_t> out;
    if (chunk.bytes.empty()) {
        return out;
    }

    if (chunk.format == "pcm" && chunk.bytes.size() >= 4) {
        const std::size_t samples = chunk.bytes.size() / 4;
        out.resize(samples, 0);
        for (std::size_t i = 0; i < samples; ++i) {
            const std::uint32_t b0 = chunk.bytes[i * 4];
            const std::uint32_t b1 = chunk.bytes[i * 4 + 1];
            const std::uint32_t b2 = chunk.bytes[i * 4 + 2];
            const std::uint32_t b3 = chunk.bytes[i * 4 + 3];
            const auto value = static_cast<std::int32_t>(
                b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
            out[i] = static_cast<std::int16_t>(value >> 16);
        }
        return out;
    }

    const std::size_t samples = chunk.bytes.size() / 2;
    out.resize(samples, 0);
    for (std::size_t i = 0; i < samples; ++i) {
        const std::uint16_t lo = chunk.bytes[i * 2];
        const std::uint16_t hi = chunk.bytes[i * 2 + 1];
        out[i] = static_cast<std::int16_t>((hi << 8) | lo);
    }
    return out;
}

class SequentialPlayer {
public:
    explicit SequentialPlayer(audio::AudioManager& audio_manager)
        : audio_(audio_manager)
    {
        audio_.subscribe([this](const audio::AudioEvent& ev) {
            if (ev.type != audio::AudioEventType::PlaybackCompleted &&
                ev.type != audio::AudioEventType::PlaybackInterrupted &&
                ev.type != audio::AudioEventType::PlaybackError) {
                return;
            }
            std::lock_guard<std::mutex> lk(mtx_);
            if (current_handle_ != audio::kInvalidPlaybackHandle &&
                ev.handle == current_handle_) {
                current_finished_ = true;
                cv_.notify_all();
            }
        });
    }

    ~SequentialPlayer()
    {
        stop();
    }

    void start()
    {
        running_.store(true, std::memory_order_release);
        worker_ = std::thread(&SequentialPlayer::loop, this);
    }

    void stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        interrupt();
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void push(e2echat::AudioChunk chunk)
    {
        if (!running_.load(std::memory_order_acquire) || chunk.bytes.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back(std::move(chunk));
        }
        cv_.notify_all();
    }

    void interrupt()
    {
        audio::PlaybackHandle handle = audio::kInvalidPlaybackHandle;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.clear();
            handle = current_handle_;
            current_finished_ = true;
            active_ = false;
        }
        if (handle != audio::kInvalidPlaybackHandle) {
            audio_.stop(handle);
        }
        cv_.notify_all();
    }

    bool waitIdle(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mtx_);
        return cv_.wait_for(lk, timeout, [this] {
            return queue_.empty() &&
                   current_handle_ == audio::kInvalidPlaybackHandle &&
                   !active_;
        });
    }

private:
    void loop()
    {
        while (running_.load(std::memory_order_acquire)) {
            e2echat::AudioChunk chunk;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] {
                    return !queue_.empty() || !running_.load(std::memory_order_acquire);
                });
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                chunk = std::move(queue_.front());
                queue_.pop_front();
                active_ = true;
                current_finished_ = false;
            }

            audio::PlaybackRequest request;
            request.pcm_data = bytesToPcm16(chunk);
            request.sample_rate = chunk.sample_rate;
            request.channels = chunk.channels;
            request.priority = audio::PlaybackPriority::TTS;
            request.stream_gain = 1.0f;
            request.loop = false;
            if (request.pcm_data.empty()) {
                std::lock_guard<std::mutex> lk(mtx_);
                active_ = false;
                cv_.notify_all();
                continue;
            }

            const audio::PlaybackHandle handle = audio_.play(request);
            if (handle == audio::kInvalidPlaybackHandle) {
                std::lock_guard<std::mutex> lk(mtx_);
                active_ = false;
                cv_.notify_all();
                continue;
            }

            const auto playback_timeout = std::chrono::milliseconds(
                std::max<std::int64_t>(
                    1000,
                    (static_cast<std::int64_t>(request.pcm_data.size()) /
                     std::max(1, request.channels)) *
                        1000 / std::max(1, request.sample_rate) + 1000));
            {
                std::unique_lock<std::mutex> lk(mtx_);
                current_handle_ = handle;
                cv_.wait_for(lk, playback_timeout, [this] {
                    return current_finished_ ||
                           !running_.load(std::memory_order_acquire);
                });
                if (current_handle_ == handle) {
                    current_handle_ = audio::kInvalidPlaybackHandle;
                }
                current_finished_ = false;
                active_ = false;
                cv_.notify_all();
            }
        }
    }

    audio::AudioManager& audio_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<e2echat::AudioChunk> queue_;
    audio::PlaybackHandle current_handle_ = audio::kInvalidPlaybackHandle;
    bool current_finished_ = false;
    bool active_ = false;
};

struct Args {
    std::string config_path = findConfig();
    int seconds = 0;
    bool opening = true;
};

Args parseArgs(int argc, char** argv)
{
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if ((arg == "--seconds" || arg == "-s") && i + 1 < argc) {
            args.seconds = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--no-opening") {
            args.opening = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: test_chat [--config Config/config.toml] [--seconds N] [--no-opening]\n"
                << "Credentials: [e2echat].app_id/access_key or env E2ECHAT_APP_ID/E2ECHAT_ACCESS_KEY.\n";
            std::exit(0);
        }
    }
    return args;
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const Args args = parseArgs(argc, argv);

    audio::Config audio_config = audio::loadConfig(args.config_path);
    audio::AudioManager audio_manager;
    if (!audio_manager.init(audio_config)) {
        std::cerr << "AudioManager init failed\n";
        return 1;
    }

    SequentialPlayer player(audio_manager);
    player.start();

    e2echat::E2EChatConfig chat_config;
    try {
        chat_config = e2echat::E2EChat::loadConfig(args.config_path);
    } catch (const std::exception& ex) {
        std::cerr << "E2EChat config load failed: " << ex.what() << "\n";
        return 1;
    }
    chat_config.send_opening_line = args.opening;

    e2echat::E2EChat chat;
    chat.setEventCallback([&](const e2echat::ChatEvent& ev) {
        using e2echat::ChatEventType;
        if (ev.type == ChatEventType::kExitIntent) {
            std::cout << "[ExitIntent] status=" << ev.status_code
                      << ", stopping capture after final playback\n";
            g_exit_requested.store(true, std::memory_order_release);
            g_stop.store(true, std::memory_order_release);
        } else if (ev.type == ChatEventType::kASRInfo) {
            if (!g_exit_requested.load(std::memory_order_acquire)) {
                player.interrupt();
            }
            std::cout << "[ASRInfo] speech detected\n";
        } else if (ev.type == ChatEventType::kASRResponse && !ev.text.empty()) {
            std::cout << "[ASR] " << (ev.is_interim ? "(interim) " : "") << ev.text << "\n";
        } else if (ev.type == ChatEventType::kChatResponse && !ev.text.empty()) {
            std::cout << "[CHAT] " << ev.text << "\n";
        } else if (ev.type == ChatEventType::kTTSSentenceStart && !ev.text.empty()) {
            std::cout << "[TTS] " << ev.text << "\n";
        } else if (ev.type == ChatEventType::kTTSEnded) {
            std::cout << "[TTSEnded]";
            if (!ev.status_code.empty()) {
                std::cout << " status=" << ev.status_code;
            }
            if (ev.exit_intent) {
                std::cout << " exit_intent=true";
            }
            std::cout << "\n";
        } else if (ev.type == ChatEventType::kError) {
            std::cerr << "[ERROR] " << ev.status_code << " " << ev.message << "\n";
        } else if (ev.type == ChatEventType::kSessionStarted) {
            std::cout << "[SessionStarted] dialog_id=" << ev.dialog_id << "\n";
        } else if (ev.type == ChatEventType::kSessionFinished) {
            std::cout << "[SessionFinished]\n";
        }
    });
    chat.setAudioCallback([&](const e2echat::AudioChunk& chunk) {
        player.push(chunk);
    });

    if (!chat.init(chat_config)) {
        std::cerr << "E2EChat init failed. Configure [e2echat].app_id/access_key or env credentials.\n";
        player.stop();
        audio_manager.shutdown();
        return 1;
    }
    if (!chat.start()) {
        std::cerr << "E2EChat start failed\n";
        chat.shutdown();
        player.stop();
        audio_manager.shutdown();
        return 1;
    }

    const audio::ConsumerHandle consumer = audio_manager.addFrameConsumer(
        [&chat](const audio::AudioFrame& frame) {
            chat.sendAudio(frame.samples, frame.channels, frame.sample_rate);
        },
        200);

    if (!audio_manager.startCapture()) {
        std::cerr << "Audio capture start failed\n";
        audio_manager.removeFrameConsumer(consumer);
        chat.shutdown();
        player.stop();
        audio_manager.shutdown();
        return 1;
    }

    std::cout << "Realtime voice chat started. Press Ctrl+C to stop.\n";
    const auto started_at = std::chrono::steady_clock::now();
    while (!g_stop.load(std::memory_order_acquire)) {
        if (args.seconds > 0 &&
            std::chrono::steady_clock::now() - started_at >= std::chrono::seconds(args.seconds)) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }

    audio_manager.stopCapture();
    audio_manager.removeFrameConsumer(consumer);
    if (g_exit_requested.load(std::memory_order_acquire)) {
        std::cout << "Waiting for final TTS playback to finish...\n";
        if (!player.waitIdle(30s)) {
            std::cerr << "Timed out waiting for final TTS playback; forcing shutdown\n";
        }
    }
    chat.stop();
    player.stop();
    audio_manager.shutdown();
    return 0;
}
