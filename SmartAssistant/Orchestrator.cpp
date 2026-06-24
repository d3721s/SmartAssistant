#include "Orchestrator.h"

#include "AudioManager.h"
#include "WakeupManager.h"
#include "TTSManager.h"
#include "e2echat.h"
#include "IntentManager.h"
#include "WheelchairController.h"
#include "ArmController.h"
#include "GripperController.h"
#include "NavigationController.h"
#include "MusicPlayer.h"
#include "SystemVolumeController.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace orchestrator {
namespace {

// ----------------------------------------------------------------------------
// Helpers — copied from test_voice_assistant.cpp so the orchestrator is
// self-contained and the test harness can shrink to a thin shell.
// ----------------------------------------------------------------------------

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

double parseLeadingDouble(const std::string& s, double fallback)
{
    if (s.empty()) return fallback;
    try {
        std::size_t pos = 0;
        const double v = std::stod(s, &pos);
        if (!std::isfinite(v)) return fallback;
        return v;
    } catch (...) {
        return fallback;
    }
}

std::string asciiLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

double parseRotationRadians(const std::string& s, double fallback)
{
    const double v = parseLeadingDouble(s, fallback);
    if (!std::isfinite(v)) return fallback;

    const std::string lower = asciiLower(s);
    const bool has_rad_unit =
        lower.find("rad") != std::string::npos ||
        s.find("\xE5\xBC\xA7\xE5\xBA\xA6") != std::string::npos; // "弧度"
    const bool has_deg_unit =
        lower.find("deg") != std::string::npos ||
        lower.find("degree") != std::string::npos ||
        s.find("\xE5\xBA\xA6") != std::string::npos ||           // "度"
        s.find("\xC2\xB0") != std::string::npos;                 // degree sign

    if (has_rad_unit) return v;

    constexpr double kPi = 3.14159265358979323846;
    if (has_deg_unit || std::abs(v) > kPi) {
        return v * kPi / 180.0;
    }
    return v;
}

// Slot fallback used by intents that ship `action_hint` (free Chinese verb)
// instead of a normalised `action`.  For e2e the model is strongly
// encouraged to emit `action`, but we keep these helpers as defence in
// depth in case the LLM falls back to verbs.
std::string hintToWheelchairAction(const std::string& hint)
{
    if (hint.empty()) return "";
    struct Entry { const char* needle; const char* action; };
    static const Entry table[] = {
        {"前进", "forward"},     {"向前", "forward"},   {"往前", "forward"},
        {"继续走", "forward"},   {"开始走", "forward"}, {"走",   "forward"},
        {"后退", "backward"},    {"向后", "backward"},  {"往后", "backward"},
        {"倒车", "backward"},    {"倒",   "backward"},
        {"左转", "left"},        {"向左", "left"},      {"往左", "left"},
        {"左拐", "left"},
        {"右转", "right"},       {"向右", "right"},     {"往右", "right"},
        {"右拐", "right"},
        {"停下", "stop"},        {"停车", "stop"},      {"停止", "stop"},
        {"刹车", "stop"},        {"别动", "stop"},
        {"加速", "faster"},      {"快一点", "faster"},  {"开快", "faster"},
        {"减速", "slower"},      {"慢一点", "slower"},  {"开慢", "slower"},
        {"调头", "turn_around"}, {"掉头", "turn_around"},
    };
    for (const auto& e : table) {
        if (hint.find(e.needle) != std::string::npos) {
            return e.action;
        }
    }
    return "";
}

std::string hintToArmAction(const std::string& hint)
{
    if (hint.empty()) return "";
    struct Entry { const char* needle; const char* action; };
    static const Entry table[] = {
        {"抬起", "raise"},   {"举起", "raise"},   {"升起", "raise"},
        {"向上", "raise"},   {"往上", "raise"},   {"上升", "raise"},
        {"放下", "lower"},   {"降下", "lower"},
        {"向下", "lower"},   {"往下", "lower"},   {"下降", "lower"},
        {"伸出", "extend"},  {"伸长", "extend"},
        {"向前", "extend"},  {"往前", "extend"},  {"前进", "extend"},
        {"前移", "extend"},
        {"缩回", "retract"}, {"收回", "retract"},
        {"向后", "retract"}, {"往后", "retract"}, {"后退", "retract"},
        {"后移", "retract"},
        {"复位", "reset"},   {"归位", "reset"},
        {"抓住", "grab"},    {"抓起", "grab"},    {"拿起", "grab"},
        {"松开", "release"}, {"放开", "release"},
    };
    for (const auto& e : table) {
        if (hint.find(e.needle) != std::string::npos) {
            return e.action;
        }
    }
    return "";
}

std::string hintToGripperAction(const std::string& hint)
{
    if (hint.empty()) return "";
    struct Entry { const char* needle; const char* action; };
    static const Entry table[] = {
        {"抓住", "grab"},    {"抓起", "grab"},    {"拿起", "grab"},
        {"抓取", "grab"},    {"夹住", "grab"},    {"闭合", "grab"},
        {"松开", "release"}, {"放开", "release"}, {"张开", "release"},
        {"打开", "release"},
        {"停止", "stop"},    {"停下", "stop"},
        {"复位", "reset"},   {"归位", "reset"},
    };
    for (const auto& e : table) {
        if (hint.find(e.needle) != std::string::npos) {
            return e.action;
        }
    }
    return "";
}

float parseVolumeLevel(const std::string& s, float fallback)
{
    if (s.empty()) return fallback;
    if (s.find("最大") != std::string::npos || s == "max") return 1.0f;
    if (s.find("最小") != std::string::npos || s == "min") return 0.0f;
    if (s.find("高") != std::string::npos) return 0.9f;
    if (s.find("中") != std::string::npos) return 0.6f;
    if (s.find("低") != std::string::npos) return 0.3f;
    try {
        std::size_t pos = 0;
        double v = std::stod(s, &pos);
        if (!std::isfinite(v)) return fallback;
        if (v > 1.0) v /= 100.0;
        v = std::clamp(v, 0.0, 1.0);
        return static_cast<float>(v);
    } catch (...) {
        return fallback;
    }
}

const std::string& pickRandomPhrase(const std::vector<std::string>& pool)
{
    static const std::string kEmpty;
    if (pool.empty()) return kEmpty;
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
    return pool[dist(rng)];
}

std::string slotOr(const std::map<std::string, std::string>& slots,
                   const std::string& key,
                   const std::string& fallback = "")
{
    auto it = slots.find(key);
    return it == slots.end() ? fallback : it->second;
}

bool isSupportedControlIntent(const std::string& intent_name)
{
    return intent_name == "wheelchair.move" ||
           intent_name == "arm.control" ||
           intent_name == "gripper.control" ||
           intent_name == "navigation.navigate" ||
           intent_name == "music.control" ||
           intent_name == "volume.control";
}

std::vector<std::string> loadPhrases(const std::string& config_path,
                                     const std::string& section,
                                     const std::vector<std::string>& defaults)
{
    std::vector<std::string> out;
    try {
        toml::table table = toml::parse_file(config_path);
        if (auto arr = table["voice_assistant"][section]["phrases"].as_array()) {
            for (const auto& node : *arr) {
                if (auto s = node.value<std::string>()) {
                    if (!s->empty()) out.push_back(*s);
                }
            }
        }
    } catch (const std::exception&) {
    }
    return out.empty() ? defaults : out;
}

// ----------------------------------------------------------------------------
// SequentialPlayer — drains e2e AudioChunk into the AudioManager queue, one
// chunk at a time.  Identical semantics to the inline class previously
// living in test_voice_assistant.cpp; lifted here so Orchestrator owns it.
// ----------------------------------------------------------------------------
class SequentialPlayer {
public:
    explicit SequentialPlayer(audio::AudioManager& audio_manager,
                              music::MusicPlayer* music_player = nullptr)
        : audio_(audio_manager), music_(music_player)
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

    ~SequentialPlayer() { stop(); }

    void start()
    {
        running_.store(true, std::memory_order_release);
        worker_ = std::thread(&SequentialPlayer::loop, this);
    }

    void stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        interrupt();
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
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
        endStreamDucking();
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

    // Non-blocking snapshot: true iff there's audio queued, an active
    // chunk being played, or a playback handle still in flight.  Used
    // by the orchestrator's silence-timeout check to keep the timer
    // paused while the player is busy.
    bool isBusy() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return !queue_.empty() ||
               current_handle_ != audio::kInvalidPlaybackHandle ||
               active_;
    }

private:
    void loop()
    {
        while (running_.load(std::memory_order_acquire)) {
            e2echat::AudioChunk chunk;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] {
                    return !queue_.empty() ||
                           !running_.load(std::memory_order_acquire);
                });
                if (!running_.load(std::memory_order_acquire)) break;
                chunk = std::move(queue_.front());
                queue_.pop_front();
                active_ = true;
                current_finished_ = false;
            }

            audio::PlaybackRequest request;
            request.pcm_data    = bytesToPcm16(chunk);
            request.sample_rate = chunk.sample_rate;
            request.channels    = chunk.channels;
            request.priority    = audio::PlaybackPriority::TTS;
            request.stream_gain = 1.0f;
            request.loop        = false;
            if (request.pcm_data.empty()) {
                finishCurrent();
                continue;
            }

            beginStreamDucking();
            const audio::PlaybackHandle handle = audio_.play(request);
            if (handle == audio::kInvalidPlaybackHandle) {
                finishCurrent();
                endStreamDuckingIfIdle();
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
            endStreamDuckingIfIdle();
        }
        endStreamDucking();
    }

    void finishCurrent()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        active_ = false;
        current_finished_ = false;
        current_handle_ = audio::kInvalidPlaybackHandle;
        cv_.notify_all();
    }

    void beginStreamDucking()
    {
        if (!music_ || !music_->isInitialized()) return;
        bool expected = false;
        if (ducking_active_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            music_->beginDucking();
        }
    }

    void endStreamDucking()
    {
        bool expected = true;
        if (ducking_active_.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel)) {
            if (music_) music_->endDucking();
        }
    }

    void endStreamDuckingIfIdle()
    {
        bool idle = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            idle = queue_.empty() &&
                   current_handle_ == audio::kInvalidPlaybackHandle &&
                   !active_;
        }
        if (idle) endStreamDucking();
    }

    audio::AudioManager& audio_;
    music::MusicPlayer*  music_ = nullptr;
    std::atomic<bool> ducking_active_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<e2echat::AudioChunk> queue_;
    audio::PlaybackHandle current_handle_ = audio::kInvalidPlaybackHandle;
    bool current_finished_ = false;
    bool active_ = false;
};

}  // namespace

// ============================================================================
// OrchestratorConfig::load
// ============================================================================
OrchestratorConfig OrchestratorConfig::load(const std::string& config_path)
{
    OrchestratorConfig cfg;
    try {
        toml::table table = toml::parse_file(config_path);
        const auto root = table["e2e_orchestrator"];
        cfg.enabled = root["enabled"].value_or(cfg.enabled);
        cfg.silence_timeout_ms =
            root["silence_timeout_ms"].value_or(cfg.silence_timeout_ms);
        cfg.chat_tts_max_failures =
            root["chat_tts_max_failures"].value_or(cfg.chat_tts_max_failures);
        cfg.greeting_before_session =
            root["greeting_before_session"].value_or(cfg.greeting_before_session);
        cfg.startup_enabled =
            table["voice_assistant"]["startup"]["enabled"].value_or(true);
    } catch (const std::exception&) {
    }

    static const std::vector<std::string> kStartupDefaults = {
        "您好，我是弈宝，我在这里陪着您。",
        "弈宝准备好了，您需要时叫我就好。",
        "我在呢，我是弈宝。",
        "弈宝已经醒啦，随时听您吩咐。",
    };
    static const std::vector<std::string> kNoSpeechDefaults = {
        "没听到您说话，我先退下啦。",
        "您好像没出声，我先去忙别的啦。",
        "没收到指令，我先离开了。",
    };
    static const std::vector<std::string> kExitReplyDefaults = {
        "好的，我先退下啦。",
        "好，那我先不打扰您。",
        "明白，您需要时再叫我。",
    };
    static const std::vector<std::string> kFailureDefaults = {
        "对话好像断了，您稍后再叫我。",
        "刚才连接出了点问题，我先退下啦。",
        "网络有些不稳，您稍候再试。",
    };
    cfg.startup_phrases = loadPhrases(config_path, "startup", kStartupDefaults);
    cfg.no_speech_phrases =
        loadPhrases(config_path, "no_speech_reply", kNoSpeechDefaults);
    cfg.exit_reply_phrases =
        loadPhrases(config_path, "exit_reply", kExitReplyDefaults);
    cfg.e2e_failure_phrases =
        loadPhrases(config_path, "e2e_failure_reply", kFailureDefaults);

    // Wake-ack phrases live under [wakeup.tts_ack].phrases — different
    // path from the voice_assistant.* pools so handle it explicitly.
    static const std::vector<std::string> kWakeAckDefaults = {
        "您好，有什么可以帮您？",
        "我能帮您做什么？",
        "请吩咐。",
        "我在听。",
    };
    try {
        toml::table table = toml::parse_file(config_path);
        if (auto arr = table["wakeup"]["tts_ack"]["phrases"].as_array()) {
            for (const auto& node : *arr) {
                if (auto s = node.value<std::string>()) {
                    if (!s->empty()) cfg.wake_ack_phrases.push_back(*s);
                }
            }
        }
    } catch (const std::exception&) {
    }
    if (cfg.wake_ack_phrases.empty()) {
        cfg.wake_ack_phrases = kWakeAckDefaults;
    }

    // Unknown-intent fallback phrases live under [intent.unknown_reply]
    // (where the legacy IntentManager already loads them too).  Reuse
    // the existing pool so config stays single-sourced.
    static const std::vector<std::string> kUnknownDefaults = {
        "抱歉，我没听懂您的意思。",
        "这个我暂时还不会，可以换个说法吗？",
        "我没明白，请再说一次。",
    };
    try {
        toml::table table = toml::parse_file(config_path);
        if (auto arr = table["intent"]["unknown_reply"]["phrases"].as_array()) {
            for (const auto& node : *arr) {
                if (auto s = node.value<std::string>()) {
                    if (!s->empty()) cfg.unknown_reply_phrases.push_back(*s);
                }
            }
        }
    } catch (const std::exception&) {
    }
    if (cfg.unknown_reply_phrases.empty()) {
        cfg.unknown_reply_phrases = kUnknownDefaults;
    }
    return cfg;
}

// ============================================================================
// Orchestrator::Impl
// ============================================================================

struct Orchestrator::Impl {
    enum class State {
        Idle,             // waiting for wake
        PlayingGreeting,  // local TTS playing the wake-ack phrase
        InSession,        // e2e session active, mic frames forwarded
        ClosingSession,   // finishSession in flight
        Failing,          // playing e2e_failure_reply, will return to Idle
    };

    OrchestratorDeps deps;
    OrchestratorConfig cfg;
    std::optional<wakeup::WakeupTtsAckConfig> wake_ack_cfg;

    std::mutex state_mtx;
    std::condition_variable state_cv;
    State state = State::Idle;
    bool stop_requested = false;
    bool wake_requested = false;
    bool failure_requested = false;
    std::string failure_reason;

    std::int64_t last_event_us = 0;
    std::atomic<int> chat_tts_failures{0};

    // Why the current session is being closed (drives the farewell phrase).
    enum class CloseReason {
        SilenceNoSpeech,    // closed before any user speech was detected
        SilenceAfterSpeech, // user spoke earlier but went silent
        UserExitIntent,     // model emitted intent="session.exit"
        UnknownIntent,      // DeepSeek returned `unknown` — not a chat,
                            // not a supported skill; close after speaking
                            // a [intent.unknown_reply] phrase
        WakeBargeIn,        // a new wake event preempted us
    };
    CloseReason close_reason = CloseReason::SilenceNoSpeech;
    bool user_speech_seen = false;

    // Audio gate.  Three states:
    //   Idle    — default; e2e audio is dropped (we have no decision yet)
    //   Pending — ASR final received, IntentManager.recognizeAsync in
    //             flight; e2e audio is buffered while we wait
    //   Open    — DeepSeek classified the turn as chat/unknown; we flush
    //             the buffer and let later chunks through until TTSEnded
    //
    // Why buffer instead of letting audio play immediately?  e2e starts
    // synthesising its own reply the moment ASR ends — *before* DeepSeek
    // returns.  If the intent turns out to be a control command we want
    // to discard that audio and play our TTS-API "收到，正在 X" instead.
    // Buffering for the ~300-500ms DeepSeek round-trip keeps both paths
    // smooth.  If DeepSeek says chat, we flush the buffered audio and
    // the user just hears e2e's natural reply with imperceptible latency.
    enum class GateState { Idle, Pending, Open };
    std::mutex gate_mtx;
    GateState gate_state = GateState::Idle;
    std::deque<e2echat::AudioChunk> pending_audio;  // buffered while Pending
    std::set<std::string> open_reply_ids;           // unmuted while Open
    // Once DeepSeek classifies an utterance as `chat` we stay in chat
    // mode for the rest of the session: every subsequent ASR final is
    // treated as chat (e2e's auto-reply is allowed to play) regardless
    // of what DeepSeek now thinks, with one exception — `session.exit`
    // still closes the session.  Reset on session close / wake / failure.
    bool chat_mode = false;

    audio::ConsumerHandle mic_consumer = audio::kInvalidConsumerHandle;
    std::unique_ptr<SequentialPlayer> player;

    float last_audible_volume = 0.7f;

    bool initImpl(const OrchestratorDeps& in_deps,
                  const OrchestratorConfig& in_cfg,
                  const wakeup::WakeupTtsAckConfig* wake_ack)
    {
        deps = in_deps;
        cfg = in_cfg;
        if (wake_ack) wake_ack_cfg = *wake_ack;

        if (!deps.audio || !deps.wakeup || !deps.tts || !deps.chat) {
            std::cerr << "[Orchestrator] init failed: required deps missing"
                      << std::endl;
            return false;
        }

        if (deps.volume) {
            float current = 0.0f;
            if (deps.volume->volume(current) && current > 0.0f) {
                last_audible_volume = current;
            }
        }
        if (last_audible_volume <= 0.0f) last_audible_volume = 0.7f;

        player = std::make_unique<SequentialPlayer>(*deps.audio, deps.music);
        player->start();

        deps.chat->setEventCallback(
            [this](const e2echat::ChatEvent& ev) { onChatEvent(ev); });
        deps.chat->setAudioCallback(
            [this](const e2echat::AudioChunk& chunk) { onChatAudio(chunk); });

        deps.wakeup->setWakeCallback(
            [this](const wakeup::WakeupEvent& ev) { onWake(ev); });

        if (deps.intent) {
            deps.intent->setErrorCallback(
                [this](const intent::IntentError& e) {
                    std::cerr << "[INTENT] error: " << e.message << std::endl;
                    speakReplyAsync("我没听懂，请再说一遍。");
                });
        }

        return true;
    }

    void shutdownImpl()
    {
        if (deps.chat) {
            deps.chat->setEventCallback({});
            deps.chat->setAudioCallback({});
        }
        if (deps.wakeup) {
            deps.wakeup->setWakeCallback({});
        }
        detachMicConsumer();
        if (player) {
            player->stop();
            player.reset();
        }
    }

    // -----------------------------------------------------------------------
    // Wake handler — runs on Wakeup's worker thread.
    // -----------------------------------------------------------------------
    void onWake(const wakeup::WakeupEvent& ev)
    {
        std::cout << "[Wake] keyword=" << ev.keyword
                  << " sv_passed=" << (ev.sv_passed ? "true" : "false")
                  << " sv_score=" << ev.sv_score << std::endl;
        bool need_close_old = false;
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            wake_requested = true;
            if (state == State::InSession ||
                state == State::PlayingGreeting ||
                state == State::Failing) {
                need_close_old = true;
            }
        }
        if (need_close_old) {
            // Barge-in: kill any in-flight playback and signal the run loop
            // to close the session before starting a new one.
            if (player) player->interrupt();
            if (deps.audio) deps.audio->stopAll();
        }
        state_cv.notify_all();
    }

    // -----------------------------------------------------------------------
    // Chat event handler — runs on the IXWebSocket message thread.  Keep work
    // here short: bookkeeping, mic gating, intent dispatch, then return.
    // -----------------------------------------------------------------------
    void onChatEvent(const e2echat::ChatEvent& ev)
    {
        using e2echat::ChatEventType;
        bumpActivity();

        switch (ev.type) {
        case ChatEventType::kASRInfo:
            std::cout << "[ASR] speech detected (q=" << ev.question_id << ")"
                      << std::endl;
            // User started speaking — drop any TTS already queued so the
            // new reply can play without overlap.
            if (player) player->interrupt();
            {
                std::lock_guard<std::mutex> lk(state_mtx);
                user_speech_seen = true;
            }
            break;
        case ChatEventType::kASRResponse:
            if (!ev.text.empty()) {
                std::cout << "[ASR] " << (ev.is_interim ? "(interim) " : "")
                          << ev.text << std::endl;
                {
                    std::lock_guard<std::mutex> lk(state_mtx);
                    user_speech_seen = true;
                }
                if (!ev.is_interim) {
                    onAsrFinal(ev.text);
                }
            }
            break;
        case ChatEventType::kChatResponse:
            if (!ev.text.empty()) {
                std::cout << "[CHAT] " << ev.text << std::endl;
            }
            break;
        case ChatEventType::kTTSSentenceStart:
            if (!ev.text.empty()) {
                std::cout << "[TTS] " << ev.text << std::endl;
            }
            // While the gate is Open, claim the first default-tts sentence
            // of the chat reply by adding its reply_id to open_reply_ids
            // so subsequent audio chunks pass the gate check.
            if ((ev.tts_type == "default" || ev.tts_type.empty()) &&
                !ev.reply_id.empty()) {
                std::lock_guard<std::mutex> lk(gate_mtx);
                if (gate_state == GateState::Open) {
                    open_reply_ids.insert(ev.reply_id);
                    std::cout << "[GATE] open reply_id=" << ev.reply_id
                              << "\n";
                }
            }
            break;
        case ChatEventType::kTTSEnded:
            // Close the gate for this reply_id and reset to Idle so the
            // next ASR turn starts from a clean state.
            if (!ev.reply_id.empty()) {
                std::lock_guard<std::mutex> lk(gate_mtx);
                if (open_reply_ids.erase(ev.reply_id) > 0) {
                    std::cout << "[GATE] close reply_id=" << ev.reply_id
                              << "\n";
                    if (open_reply_ids.empty()) {
                        gate_state = GateState::Idle;
                        pending_audio.clear();
                    }
                }
            }
            break;
        case ChatEventType::kExitIntent:
            std::cout << "[ExitIntent] status=" << ev.status_code << std::endl;
            requestSessionClose(CloseReason::UserExitIntent);
            break;
        case ChatEventType::kError:
            std::cerr << "[E2E_ERROR] " << ev.status_code << " "
                      << ev.message << std::endl;
            requestFailure(
                ev.message.empty() ? std::string("e2e error") : ev.message);
            break;
        case ChatEventType::kSessionFinished:
        case ChatEventType::kConnectionFinished:
            std::cout << "[E2E] " << e2echat::E2EChat::toString(ev.type)
                      << std::endl;
            break;
        default:
            break;
        }
    }

    void onChatAudio(const e2echat::AudioChunk& chunk)
    {
        // Three-state gate decision:
        //   Idle    → drop (no pending intent, this is stale or stray audio)
        //   Pending → buffer (DeepSeek not back yet; bytes preserved so we
        //             can flush them with imperceptible delay if the turn
        //             turns out to be chat)
        //   Open    → forward iff this reply_id was unmuted by dispatchIntent
        bool forward = false;
        {
            std::lock_guard<std::mutex> lk(gate_mtx);
            switch (gate_state) {
            case GateState::Idle:
                return;
            case GateState::Pending:
                pending_audio.push_back(chunk);
                return;
            case GateState::Open:
                forward = !chunk.reply_id.empty() &&
                          open_reply_ids.count(chunk.reply_id) > 0;
                break;
            }
        }
        if (forward && player) {
            player->push(chunk);
            bumpActivity();
        }
    }

    // -----------------------------------------------------------------------
    // ASR final → IntentManager.recognizeAsync → dispatchIntent.
    // Runs on the IXWebSocket thread (callback origin); recognizeAsync()
    // returns immediately and the result callback fires later on
    // IntentManager's worker thread.  dispatchIntent does the actual skill
    // work; everything below it is non-blocking.
    // -----------------------------------------------------------------------
    void onAsrFinal(const std::string& text)
    {
        if (!deps.intent || text.empty()) return;

        // Move the gate to Pending so any e2e audio that arrives while
        // DeepSeek is thinking gets buffered instead of leaking through
        // (or being silently dropped, which would lose the chat reply).
        {
            std::lock_guard<std::mutex> lk(gate_mtx);
            gate_state = GateState::Pending;
            pending_audio.clear();
            open_reply_ids.clear();
        }

        intent::IntentRequest req;
        req.text = text;
        const bool ok = deps.intent->recognizeAsync(
            req,
            [this, text](const intent::IntentResult& r) {
                dispatchIntent(r, text);
            });
        if (!ok) {
            std::cerr << "[INTENT] recognizeAsync rejected (manager not init?)\n";
            // Fall back: treat as chat — keep the buffer, open the gate
            // so e2e's natural reply still plays.
            std::lock_guard<std::mutex> lk(gate_mtx);
            gate_state = GateState::Open;
            flushPendingAudio();
        }
    }

    // Move buffered audio to the player.  Caller must hold gate_mtx.
    void flushPendingAudio()
    {
        if (!player) {
            pending_audio.clear();
            return;
        }
        for (const auto& chunk : pending_audio) {
            if (!chunk.reply_id.empty()) {
                open_reply_ids.insert(chunk.reply_id);
            }
            player->push(chunk);
        }
        pending_audio.clear();
    }

    void dispatchIntent(const intent::IntentResult& result,
                        const std::string& asr_text)
    {
        const std::string& intent = result.intent_name;
        std::cout << "[INTENT] " << intent;
        if (!result.slots.empty()) {
            std::cout << " slots={";
            bool first = true;
            for (const auto& kv : result.slots) {
                if (!first) std::cout << ", ";
                first = false;
                std::cout << kv.first << "=" << kv.second;
            }
            std::cout << "}";
        }
        std::cout << std::endl;

        // Sticky chat mode: once we entered chat mode in this session,
        // every subsequent ASR final is funneled to e2e — *except*
        // session.exit, which always closes the session.  This avoids
        // a per-turn DeepSeek round-trip changing its mind and flipping
        // us back to "我还不会" replies.
        bool in_chat_mode;
        {
            std::lock_guard<std::mutex> lk(gate_mtx);
            in_chat_mode = chat_mode;
        }
        if (in_chat_mode) {
            if (intent == "session.exit") {
                interruptE2EAndDropBuffer();
                requestSessionClose(CloseReason::UserExitIntent);
                return;
            }
            // Anything else while in chat mode → let e2e's auto-reply
            // play.  The buffered audio that arrived during the
            // DeepSeek round-trip is flushed below.
            std::lock_guard<std::mutex> lk(gate_mtx);
            gate_state = GateState::Open;
            std::cout << "[GATE] state=Open (chat-mode, "
                      << pending_audio.size() << " buffered chunks)\n";
            flushPendingAudio();
            return;
        }

        // session.exit — silence e2e's auto-reply and let the session
        // close path play the configured exit_reply via TTS API.
        if (intent == "session.exit") {
            interruptE2EAndDropBuffer();
            requestSessionClose(CloseReason::UserExitIntent);
            return;
        }

        // chat — only hit when DeepSeek classifies the user input as
        // an explicit "let's chat" request (per the NLU prompt's chat
        // rule).  e2e is already mid-reply on its own; just open the
        // gate, flush whatever it produced during the round-trip, and
        // let the rest stream through.  Latches chat_mode=true so the
        // next ASR final goes straight to the chat path.
        if (intent == "chat") {
            std::lock_guard<std::mutex> lk(gate_mtx);
            gate_state = GateState::Open;
            chat_mode = true;
            std::cout << "[GATE] state=Open (entered chat mode, "
                      << pending_audio.size() << " buffered chunks)\n";
            flushPendingAudio();
            return;
        }

        // unknown / empty — unsupported utterance.  Cut e2e's auto-reply
        // and close the session immediately; handleSessionClose() will
        // play a randomised "我还不会..." phrase via TTS API and then
        // tear down e2e.  Closing avoids the standalone-TTS voice
        // feeding back into e2e ASR (it has no built-in cancellation
        // for voices it didn't synthesise itself), and matches the
        // user's mental model: an unsupported utterance ends the round.
        if (intent == "unknown" || intent.empty()) {
            interruptE2EAndDropBuffer();
            requestSessionClose(CloseReason::UnknownIntent);
            return;
        }

        // Skill intent — interrupt e2e's auto-reply (we don't want it
        // talking over our TTS-API ack) and dispatch.
        interruptE2EAndDropBuffer();
        if (isSupportedControlIntent(intent)) {
            executeSkill(intent, result.slots);
        }
        const std::string reply = replyTemplate(intent, result.slots);
        if (!reply.empty()) {
            speakReplyAsync(reply);
        }
        (void)asr_text;
    }

    // Send ClientInterrupt(515) to e2e so it stops auto-generating the
    // current reply, then drop any audio buffered during Pending.
    void interruptE2EAndDropBuffer()
    {
        if (deps.chat) deps.chat->interrupt();
        std::lock_guard<std::mutex> lk(gate_mtx);
        gate_state = GateState::Idle;
        pending_audio.clear();
        open_reply_ids.clear();
    }

    // Canned acknowledgement table keyed by intent + dominant slot.  These
    // are *receipt* phrases ("收到，正在 X"), not success notifications —
    // skill execution is asynchronous and we don't know the outcome yet.
    static std::string replyTemplate(
        const std::string& intent,
        const std::map<std::string, std::string>& slots)
    {
        auto get = [&](const char* k) -> std::string {
            auto it = slots.find(k);
            return it == slots.end() ? std::string() : it->second;
        };
        const std::string action = get("action");

        if (intent == "wheelchair.move") {
            if (action == "forward")     return "收到，正在向前。";
            if (action == "backward")    return "收到，正在向后。";
            if (action == "left")        return "收到，向左转。";
            if (action == "right")       return "收到，向右转。";
            if (action == "stop")        return "好的，已停止。";
            if (action == "faster")      return "好的，加速。";
            if (action == "slower")      return "好的，减速。";
            if (action == "turn_around") return "好的，正在掉头。";
        } else if (intent == "arm.control") {
            if (action == "raise")   return "收到，正在抬起机械臂。";
            if (action == "lower")   return "收到，正在放下机械臂。";
            if (action == "extend")  return "收到，正在伸出机械臂。";
            if (action == "retract") return "收到，正在收回机械臂。";
            if (action == "reset")   return "好的，机械臂复位。";
            if (action == "stop")    return "好的，机械臂停下。";
            if (action == "move_to") return "收到，正在调整机械臂。";
        } else if (intent == "gripper.control") {
            if (action == "grab")    return "收到，正在抓取。";
            if (action == "release") return "好的，松开夹爪。";
            if (action == "reset")   return "好的，夹爪复位。";
            if (action == "stop")    return "好的，夹爪停下。";
        } else if (intent == "navigation.navigate") {
            const std::string target = get("target");
            if (!target.empty()) return "收到，正在导航到" + target + "。";
            return "收到，正在导航。";
        } else if (intent == "music.control") {
            if (action == "pause")    return "好的，已暂停。";
            if (action == "stop")     return "好的，已停止。";
            if (action == "next")     return "好的，下一首。";
            if (action == "previous") return "好的，上一首。";
            if (action == "resume")   return "好的，继续播放。";
            if (action == "play" || action == "search") {
                const std::string q = get("query");
                if (!q.empty()) return "好的，正在播放" + q + "。";
                return "好的，正在播放。";
            }
        } else if (intent == "volume.control") {
            if (action == "up")     return "好的，调大音量。";
            if (action == "down")   return "好的，调小音量。";
            if (action == "mute")   return "好的，已静音。";
            if (action == "unmute") return "好的，恢复声音。";
            if (action == "set")    return "好的，调节音量。";
        }
        return "收到，正在执行。";
    }

    // Launch a TTSManager.synthesizeAndPlay() that returns immediately and
    // tracks failures via the same counter the legacy chatTTSText path
    // used.  On chat_tts_max_failures consecutive failures the run loop
    // tears down the session and plays the e2e_failure_reply phrase.
    void speakReplyAsync(const std::string& reply)
    {
        if (!deps.tts || reply.empty()) return;
        std::cout << "[REPLY] " << reply << std::endl;
        const bool started = deps.tts->synthesizeAndPlay(
            reply, tts::TTSOptions{},
            [this](const tts::TTSResult& r) {
                if (r.success) {
                    chat_tts_failures.store(0, std::memory_order_release);
                    return;
                }
                const int n = chat_tts_failures.fetch_add(
                                  1, std::memory_order_acq_rel) + 1;
                std::cerr << "[REPLY] TTS failed (" << n << "/"
                          << cfg.chat_tts_max_failures << "): "
                          << r.message << std::endl;
                if (n >= cfg.chat_tts_max_failures) {
                    requestFailure("reply TTS repeated failure");
                }
            });
        if (!started) {
            const int n = chat_tts_failures.fetch_add(
                              1, std::memory_order_acq_rel) + 1;
            std::cerr << "[REPLY] TTS start rejected (" << n << "/"
                      << cfg.chat_tts_max_failures << ")" << std::endl;
            if (n >= cfg.chat_tts_max_failures) {
                requestFailure("reply TTS start rejected");
            }
        }
    }

    void executeSkill(const std::string& intent,
                      const std::map<std::string, std::string>& slots)
    {
        if (intent == "wheelchair.move" && deps.wheelchair &&
            deps.wheelchair->isInitialized()) {
            std::string action = slotOr(slots, "action");
            if (action.empty()) {
                action = hintToWheelchairAction(slotOr(slots, "action_hint"));
            }
            const double distance =
                parseLeadingDouble(slotOr(slots, "distance"), 0.0);
            const double rotation =
                parseRotationRadians(slotOr(slots, "rotation"), 0.0);
            const std::string hint = slotOr(slots, "action_hint");
            const double speed_step = std::clamp(
                deps.wheelchair->config().defaults.speed_step_ratio, 0.0, 0.5);

            if (action == "forward")        deps.wheelchair->moveForward(distance);
            else if (action == "backward")  deps.wheelchair->moveBackward(distance);
            else if (action == "left")      deps.wheelchair->turnLeft(rotation);
            else if (action == "right")     deps.wheelchair->turnRight(rotation);
            else if (action == "stop") {
                const bool emergency =
                    hint.find("急") != std::string::npos ||
                    hint.find("刹") != std::string::npos;
                if (emergency) deps.wheelchair->emergencyStop();
                else           deps.wheelchair->stop();
            }
            else if (action == "faster")     deps.wheelchair->adjustSpeed( speed_step);
            else if (action == "slower")     deps.wheelchair->adjustSpeed(-speed_step);
            else if (action == "turn_around")deps.wheelchair->turnLeft(3.1415926);
            else std::cout << "[ACTION] wheelchair: unmapped '" << action
                           << "'\n";
            return;
        }

        if (intent == "arm.control" && deps.arm) {
            std::string action = slotOr(slots, "action");
            if (action.empty()) {
                action = hintToArmAction(slotOr(slots, "action_hint"));
            }
            const std::string target = slotOr(slots, "target");
            const double distance =
                parseLeadingDouble(slotOr(slots, "distance"), 0.0);

            if ((action == "grab" || action == "release") && deps.gripper &&
                deps.gripper->isInitialized()) {
                if (action == "grab") deps.gripper->grab(target);
                else                  deps.gripper->release(target);
                return;
            }
            if (!deps.arm->isInitialized()) {
                std::cout << "[ACTION] arm controller not initialized; skip\n";
                return;
            }
            if (action == "raise")        deps.arm->raise(distance);
            else if (action == "lower")   deps.arm->lower(distance);
            else if (action == "extend")  deps.arm->extend(distance);
            else if (action == "retract") deps.arm->retract(distance);
            else if (action == "reset" || action == "stop") deps.arm->stop();
            else if (action == "move_to") {
                const std::string ta = hintToArmAction(target);
                if (ta == "raise")        deps.arm->raise(distance);
                else if (ta == "lower")   deps.arm->lower(distance);
                else if (ta == "extend")  deps.arm->extend(distance);
                else if (ta == "retract") deps.arm->retract(distance);
                else std::cout << "[ACTION] arm.move_to target='" << target
                               << "' unmapped\n";
            }
            else std::cout << "[ACTION] arm: unmapped '" << action << "'\n";
            return;
        }

        if (intent == "gripper.control" && deps.gripper &&
            deps.gripper->isInitialized()) {
            std::string action = slotOr(slots, "action");
            const std::string hint = slotOr(slots, "action_hint");
            const std::string target = slotOr(slots, "target");
            if (action.empty()) action = hintToGripperAction(hint);
            if (action.empty()) action = hintToGripperAction(target);

            if (action == "grab")         deps.gripper->grab(target);
            else if (action == "release") deps.gripper->release(target);
            else if (action == "stop")    deps.gripper->stop();
            else if (action == "reset")   deps.gripper->reset();
            else std::cout << "[ACTION] gripper: unmapped '" << action << "'\n";
            return;
        }

        if (intent == "navigation.navigate" && deps.navigation &&
            deps.navigation->isInitialized()) {
            const std::string target = slotOr(slots, "target");
            if (target.empty()) {
                std::cout << "[ACTION] navigation: missing target\n";
                return;
            }
            deps.navigation->navigateTo(target);
            return;
        }

        if (intent == "music.control" && deps.music &&
            deps.music->isInitialized()) {
            std::string action = slotOr(slots, "action");
            const std::string query = slotOr(slots, "query");
            if (action.empty()) action = "play";
            if (action == "play" || action == "search") {
                const auto pr = deps.music->play(query);
                std::cout << "[ACTION] music.play '" << query << "' -> "
                          << (pr.ok ? "ok" : pr.message) << "\n";
            } else if (action == "pause") {
                deps.music->pause();
            } else if (action == "resume") {
                if (!deps.music->resume() &&
                    !deps.music->currentTitle().empty()) {
                    deps.music->play();
                }
            } else if (action == "stop") {
                deps.music->stop();
            } else if (action == "next") {
                deps.music->next();
            } else if (action == "previous") {
                deps.music->previous();
            }
            return;
        }

        if (intent == "volume.control" && deps.volume) {
            std::string action = slotOr(slots, "action");
            float current = 0.0f;
            if (!deps.volume->volume(current)) {
                std::cout << "[ACTION] volume unavailable\n";
                return;
            }
            const float step = 0.15f;
            float target = current;
            if (action == "up") {
                target = std::min(1.0f, current + step);
                deps.volume->setMuted(false);
                deps.volume->setVolume(target);
            } else if (action == "down") {
                target = std::max(0.0f, current - step);
                deps.volume->setMuted(false);
                deps.volume->setVolume(target);
            } else if (action == "mute") {
                if (current > 0.0f) last_audible_volume = current;
                deps.volume->setMuted(true);
            } else if (action == "unmute") {
                target = last_audible_volume > 0.0f ? last_audible_volume : 0.7f;
                deps.volume->setVolume(target);
                deps.volume->setMuted(false);
            } else if (action == "set") {
                target = parseVolumeLevel(slotOr(slots, "level"), current);
                deps.volume->setMuted(false);
                deps.volume->setVolume(target);
            }
            if (target > 0.0f) last_audible_volume = target;
            return;
        }
    }

    // -----------------------------------------------------------------------
    // Reply playback failure tracking (chat_tts_failures) is shared with
    // the e2e_failure_reply fallback path: a synth/playback failure or
    // explicit kError event increments the counter, and chat_tts_max_failures
    // consecutive failures tear the session down via requestFailure().
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Activity + state-transition helpers.
    // -----------------------------------------------------------------------
    void bumpActivity()
    {
        const auto now = nowMicros();
        std::lock_guard<std::mutex> lk(state_mtx);
        last_event_us = now;
        state_cv.notify_all();
    }

    void requestSessionClose(CloseReason reason)
    {
        std::lock_guard<std::mutex> lk(state_mtx);
        if (state == State::InSession) {
            close_reason = reason;
            state = State::ClosingSession;
            state_cv.notify_all();
        }
    }

    void requestFailure(const std::string& reason)
    {
        std::lock_guard<std::mutex> lk(state_mtx);
        if (state == State::InSession || state == State::PlayingGreeting) {
            failure_requested = true;
            failure_reason = reason;
            state = State::Failing;
            state_cv.notify_all();
        }
    }

    static std::int64_t nowMicros()
    {
        using namespace std::chrono;
        return duration_cast<microseconds>(
                   steady_clock::now().time_since_epoch()).count();
    }

    void attachMicConsumer()
    {
        if (mic_consumer != audio::kInvalidConsumerHandle) return;
        if (!deps.audio || !deps.chat) return;
        mic_consumer = deps.audio->addFrameConsumer(
            [this](const audio::AudioFrame& frame) {
                std::lock_guard<std::mutex> lk(state_mtx);
                if (state != State::InSession) return;
                deps.chat->sendAudio(frame.samples,
                                     frame.channels,
                                     frame.sample_rate);
            },
            200);
    }

    void detachMicConsumer()
    {
        if (mic_consumer == audio::kInvalidConsumerHandle) return;
        deps.audio->removeFrameConsumer(mic_consumer);
        mic_consumer = audio::kInvalidConsumerHandle;
    }

    // -----------------------------------------------------------------------
    // Greeting + failure-reply playback (uses TTSManager directly).  Both
    // are blocking with respect to a CV that's signalled from the
    // TTSCallback.  Returns true on success, false on synthesis/playback
    // failure (we then skip starting / closing without further fallback).
    // -----------------------------------------------------------------------
    bool speakWithTTS(const std::string& text,
                      std::chrono::milliseconds timeout)
    {
        if (!deps.tts || text.empty()) return false;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        std::cout << "[SPEAK] " << text << std::endl;
        const bool started = deps.tts->synthesizeAndPlay(
            text, tts::TTSOptions{},
            [&](const tts::TTSResult& r) {
                {
                    std::lock_guard<std::mutex> lk(m);
                    done = true;
                    ok = r.success;
                }
                cv.notify_all();
            });
        if (!started) return false;
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, timeout, [&] { return done; })) {
            std::cerr << "[SPEAK] timeout" << std::endl;
            return false;
        }
        return ok;
    }

    // -----------------------------------------------------------------------
    // Run loop — single state-machine driver.
    // -----------------------------------------------------------------------
    void runLoop(std::atomic<bool>* external_stop)
    {
        // Announce that the wake listener is up so the user knows they
        // can start talking.  Reuses [voice_assistant.startup].phrases —
        // same vv voice as the wake-ack and the reply playback, so the
        // assistant sounds consistent.  Skipped when startup.enabled is
        // false in config.toml.  Blocking on the announcement is fine:
        // a wake fired during it is buffered as wake_requested and
        // processed as soon as the announcement returns.
        if (cfg.startup_enabled && !cfg.startup_phrases.empty()) {
            const std::string& phrase = pickRandomPhrase(cfg.startup_phrases);
            speakWithTTS(phrase, 8s);
        }

        while (true) {
            {
                std::unique_lock<std::mutex> lk(state_mtx);
                state_cv.wait_for(lk, 100ms, [&] {
                    return stop_requested ||
                           wake_requested ||
                           failure_requested ||
                           state == State::ClosingSession ||
                           (external_stop && external_stop->load());
                });
                if (stop_requested ||
                    (external_stop && external_stop->load())) {
                    break;
                }
            }

            // Handle state transitions outside the lock.
            handleTickedState();
        }

        // Tear down on exit.
        if (deps.chat && deps.chat->isRunning()) {
            deps.chat->stop();
        }
        detachMicConsumer();
    }

    void handleTickedState()
    {
        State current;
        bool wake;
        bool failure;
        std::string failure_msg;
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            current = state;
            wake = wake_requested;
            failure = failure_requested;
            failure_msg = failure_reason;
        }

        // Failure preempts everything — play the fallback phrase, close
        // the session, return to Idle.
        if (failure) {
            handleFailure(failure_msg);
            return;
        }

        if (current == State::ClosingSession) {
            handleSessionClose();
            return;
        }

        if (current == State::InSession) {
            // Silence-timeout decision needs to ignore three "we're not
            // really silent" states:
            //   1) the player is still rendering a reply — the user
            //      hasn't even had a chance to talk yet, do NOT close
            //   2) DeepSeek is still chewing on the last ASR final
            //      (gate is Pending) — same reason
            //   3) e2e is mid-reply with the gate Open — bytes are
            //      flowing to the player, give the user time to react
            // Each of these continuously bumps the activity timestamp
            // so the silence window only starts running once the audio
            // actually went quiet.
            const bool player_busy = player && player->isBusy();
            bool gate_active = false;
            {
                std::lock_guard<std::mutex> lk(gate_mtx);
                gate_active = (gate_state != GateState::Idle);
            }
            if (player_busy || gate_active) {
                bumpActivity();
                return;
            }

            // Check silence timeout
            const auto now = nowMicros();
            std::int64_t last;
            {
                std::lock_guard<std::mutex> lk(state_mtx);
                last = last_event_us;
            }
            if (cfg.silence_timeout_ms > 0 &&
                (now - last) >
                    static_cast<std::int64_t>(cfg.silence_timeout_ms) * 1000) {
                bool spoken;
                {
                    std::lock_guard<std::mutex> lk(state_mtx);
                    spoken = user_speech_seen;
                }
                std::cout << "[Orchestrator] silence timeout, closing session "
                          << (spoken ? "(after speech)" : "(no speech)")
                          << std::endl;
                requestSessionClose(spoken
                                        ? CloseReason::SilenceAfterSpeech
                                        : CloseReason::SilenceNoSpeech);
            }
            return;
        }

        if (wake) {
            handleWake();
            return;
        }
    }

    void handleWake()
    {
        bool was_in_session = false;
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            wake_requested = false;
            // If a previous session was up, close it before starting a new one.
            if (state == State::InSession) {
                state = State::ClosingSession;
                close_reason = CloseReason::WakeBargeIn;
                was_in_session = true;
            } else {
                state = State::PlayingGreeting;
            }
        }
        // If there was an old session, close it first.
        if (was_in_session && deps.chat && deps.chat->isRunning()) {
            std::cout << "[Orchestrator] barging in: closing old session"
                      << std::endl;
            detachMicConsumer();
            if (player) player->interrupt();
            deps.chat->stop();
        }

        {
            std::lock_guard<std::mutex> lk(state_mtx);
            state = State::PlayingGreeting;
            // Reset per-session bookkeeping for the new session.
            user_speech_seen = false;
            close_reason = CloseReason::SilenceNoSpeech;
        }
        {
            // Reset audio gate too, so a leftover open reply_id from a
            // previous session can't leak chat audio into the new one.
            std::lock_guard<std::mutex> lk(gate_mtx);
            gate_state = GateState::Idle;
            pending_audio.clear();
            open_reply_ids.clear();
            chat_mode = false;
        }

        // 1+2 in parallel: kick off e2e session in a background thread
        // while we play the wake-ack on this thread.  e2e.start()
        // takes ~1.5s (TLS handshake + StartSession round-trip) and the
        // wake-ack takes ~1.5s too — running them sequentially would
        // make the user wait ~3s before they can talk.  The mic
        // consumer is attached only after both finish so wake-ack
        // playback isn't fed back into ASR.
        chat_tts_failures.store(0, std::memory_order_release);
        std::atomic<bool> chat_started{false};
        std::atomic<bool> chat_failed{false};
        std::thread chat_starter([&] {
            std::cout << "[Orchestrator] starting e2e session" << std::endl;
            if (deps.chat->start()) {
                chat_started.store(true, std::memory_order_release);
            } else {
                chat_failed.store(true, std::memory_order_release);
            }
        });

        std::int64_t tts_end_us = 0;
        if (cfg.greeting_before_session && !cfg.wake_ack_phrases.empty()) {
            const std::string& phrase = pickRandomPhrase(cfg.wake_ack_phrases);
            speakWithTTS(phrase, 8s);
            // Mark the moment the user's talking window opens.  Silence
            // timeout will count from here, not from chat.start()
            // completion: e2e startup latency shouldn't eat into the
            // user's ~10s window.
            tts_end_us = nowMicros();
        }

        // Wait for the chat session to finish coming up.  In the common
        // case (~1.5s wake-ack vs ~1.5s start) both finish within tens of
        // milliseconds of each other.  Guard with a generous overall
        // deadline matching the original ~e2e connect_timeout (5s).
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (!chat_started.load(std::memory_order_acquire) &&
               !chat_failed.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        if (chat_starter.joinable()) chat_starter.join();
        if (!chat_started.load(std::memory_order_acquire)) {
            std::cerr << "[Orchestrator] e2e start failed" << std::endl;
            handleFailure("session start failed");
            return;
        }

        // Mic forwarding is attached only now — both wake-ack playback
        // and e2e StartSession are done, so the mic feed won't be
        // mistakenly captured by ASR while the wake-ack was still
        // playing through the speaker.
        attachMicConsumer();

        {
            std::lock_guard<std::mutex> lk(state_mtx);
            state = State::InSession;
            // Anchor silence timer at wake-ack-TTS-end.  Falls back to
            // "now" if no wake-ack was configured / played.
            last_event_us = tts_end_us != 0 ? tts_end_us : nowMicros();
        }
        std::cout << "[Orchestrator] in session" << std::endl;
    }

    void handleSessionClose()
    {
        CloseReason reason;
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            reason = close_reason;
            // Move out of ClosingSession early so onChatEvent doesn't loop
            // back into requestSessionClose().
            state = State::Idle;
        }
        {
            // Drop sticky chat mode so the next session starts clean.
            std::lock_guard<std::mutex> lk(gate_mtx);
            chat_mode = false;
            gate_state = GateState::Idle;
            pending_audio.clear();
            open_reply_ids.clear();
        }
        detachMicConsumer();
        if (player) player->waitIdle(8s);
        if (deps.chat) deps.chat->stop();

        // Pick a farewell phrase that fits why the session ended.
        // Wake-barge-in skips the phrase entirely — the new wake's
        // greeting will play immediately and a farewell would only
        // delay it.  Both silence flavours map to no_speech_reply
        // ("我没听到您…")—we only want the sociable exit_reply
        // ("好的我先退下") when the user actively asked to leave.
        const std::vector<std::string>* pool = nullptr;
        const char* tag = "";
        switch (reason) {
        case CloseReason::SilenceNoSpeech:
            pool = &cfg.no_speech_phrases;
            tag = "no_speech";
            break;
        case CloseReason::SilenceAfterSpeech:
            pool = &cfg.no_speech_phrases;
            tag = "silence_after_speech";
            break;
        case CloseReason::UserExitIntent:
            pool = &cfg.exit_reply_phrases;
            tag = "exit_intent";
            break;
        case CloseReason::UnknownIntent:
            pool = &cfg.unknown_reply_phrases;
            tag = "unknown_intent";
            break;
        case CloseReason::WakeBargeIn:
            break;
        }
        if (pool && !pool->empty()) {
            const std::string& phrase = pickRandomPhrase(*pool);
            std::cout << "[Orchestrator] farewell (" << tag << ")\n";
            speakWithTTS(phrase, 8s);
        }
        std::cout << "[Orchestrator] session closed" << std::endl;
    }

    void handleFailure(const std::string& reason)
    {
        std::cerr << "[Orchestrator] failure: " << reason << std::endl;
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            failure_requested = false;
            failure_reason.clear();
            state = State::Failing;
        }
        {
            std::lock_guard<std::mutex> lk(gate_mtx);
            chat_mode = false;
            gate_state = GateState::Idle;
            pending_audio.clear();
            open_reply_ids.clear();
        }
        detachMicConsumer();
        if (player) player->interrupt();
        if (deps.chat) deps.chat->stop();

        if (!cfg.e2e_failure_phrases.empty()) {
            const std::string& phrase =
                pickRandomPhrase(cfg.e2e_failure_phrases);
            speakWithTTS(phrase, 8s);
        }

        std::lock_guard<std::mutex> lk(state_mtx);
        state = State::Idle;
    }
};

// ============================================================================
// Orchestrator public API
// ============================================================================

Orchestrator::Orchestrator() : impl_(std::make_unique<Impl>()) {}

Orchestrator::~Orchestrator() { shutdown(); }

bool Orchestrator::init(const OrchestratorDeps& deps,
                        const OrchestratorConfig& cfg,
                        const wakeup::WakeupTtsAckConfig* wake_ack_cfg)
{
    return impl_ && impl_->initImpl(deps, cfg, wake_ack_cfg);
}

void Orchestrator::run(std::atomic<bool>* stop_flag)
{
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->state_mtx);
        impl_->stop_requested = false;
    }
    impl_->runLoop(stop_flag);
}

void Orchestrator::stop()
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->state_mtx);
    impl_->stop_requested = true;
    impl_->state_cv.notify_all();
}

void Orchestrator::shutdown()
{
    if (impl_) impl_->shutdownImpl();
}

}  // namespace orchestrator
