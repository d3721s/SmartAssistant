#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace e2echat {

enum class ChatState {
    kIdle,
    kConnecting,
    kConnected,
    kSessionStarted,
    kClosing,
    kClosed,
    kFailed,
};

enum class ChatEventType {
    kConnectionStarted,
    kConnectionFinished,
    kSessionStarted,
    kSessionFinished,
    kASRInfo,
    kASRResponse,
    kASREnded,
    kTTSSentenceStart,
    kTTSSentenceEnd,
    kTTSEnded,
    kExitIntent,
    kChatResponse,
    kChatEnded,
    kUsage,
    kConfigUpdated,
    kError,
    kRawJson,
};

struct E2EChatConfig {
    std::string endpoint = "wss://openspeech.bytedance.com/api/v3/realtime/dialogue";
    std::string app_id;
    std::string access_key;
    std::string resource_id = "volc.speech.dialog";
    std::string app_key = "PlgvMymc7f3tQnJ6";
    std::string app_id_env = "E2ECHAT_APP_ID";
    std::string access_key_env = "E2ECHAT_ACCESS_KEY";
    std::string connect_id;

    std::string model = "1.2.1.1"; // O2.0
    std::string speaker = "zh_female_vv_jupiter_bigtts";
    std::string bot_name = "温柔可靠的出行陪伴助手——弈宝";
    std::string system_role =
        "你是搭载在智能轮椅上的 AI 语音助手，专注于陪伴用户安全、舒适、便捷地出行。"
        "你能够理解用户的语音指令，协助完成移动控制、路线提醒、环境提示、健康关怀和日常陪伴。"
        "你不是冷冰冰的机器，而是一位耐心、细心、可靠的伙伴，始终把用户的安全和感受放在第一位。";
    std::string speaking_style =
        "语气温和、清晰、稳定，避免过度活泼或夸张。回答简短直接，适合语音播报。"
        "遇到行动相关指令时，优先确认安全。对老人、行动不便者保持耐心和尊重。"
        "主动提醒风险，但不制造紧张感。不使用复杂术语，多用日常表达。"
        "必要时进行二次确认，例如转弯、上坡、靠近障碍物等场景。";
    std::string opening_line =
        "您好，我是您的智能轮椅语音助手。我会陪您安全出行、提醒周围情况，也可以随时听您的指令。需要帮助时，请直接叫我。";
    std::string dialog_id;
    std::string input_mod = "keep_alive";
    int end_smooth_window_ms = 500;
    bool enable_custom_vad = true;
    bool enable_asr_twopass = false;

    int input_sample_rate = 16000;
    int input_channels = 1;
    std::string input_format = "pcm";
    int input_chunk_ms = 20;

    int output_sample_rate = 24000;
    int output_channels = 1;
    std::string output_format = "pcm_s16le";
    int speech_rate = 0;
    int loudness_rate = 0;

    bool strict_audit = true;
    bool enable_loudness_norm = false;
    bool enable_conversation_truncate = false;
    bool enable_user_query_exit = false;
    std::string exit_status_code = "20000002";
    bool send_opening_line = false;
    bool verify_ssl = true;
    bool debug = false;

    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds close_timeout{1500};

    struct Logging {
        std::string log_dir = "logs";
        std::string log_file = "e2echat.log";
        bool console = true;
        std::size_t rotation_max_bytes = 10 * 1024 * 1024;
        int rotation_max_files = 7;
    } logging;
};

struct ChatEvent {
    ChatEventType type = ChatEventType::kRawJson;
    int event_id = 0;
    std::string session_id;
    std::string dialog_id;
    std::string question_id;
    std::string reply_id;
    std::string text;
    std::string status_code;
    std::string message;
    std::string raw_json;
    bool is_interim = false;
    bool exit_intent = false;
    std::int64_t timestamp_us = 0;

    // tts_type from TTSSentenceStart payload — used by the orchestrator's
    // audio gate to distinguish model-narrated speech ("default") from
    // ChatTTSText replies / network search audio / etc.  Empty for events
    // that don't carry this field.
    std::string tts_type;
};

struct AudioChunk {
    std::vector<std::uint8_t> bytes;
    int sample_rate = 24000;
    int channels = 1;
    std::string format = "pcm_s16le";
    std::string question_id;
    std::string reply_id;
    std::int64_t timestamp_us = 0;
};

using EventCallback = std::function<void(const ChatEvent&)>;
using AudioCallback = std::function<void(const AudioChunk&)>;

class E2EChat {
public:
    E2EChat();
    ~E2EChat();

    E2EChat(const E2EChat&) = delete;
    E2EChat& operator=(const E2EChat&) = delete;

    bool init(const std::string& config_path = "Config/config.toml");
    bool init(const E2EChatConfig& config);
    void shutdown();

    bool start();
    void stop();
    bool finishSession();
    bool finishConnection();

    bool sendAudio(const std::int16_t* samples,
                   std::size_t sample_count,
                   int channels,
                   int sample_rate);
    bool sendAudio(const std::vector<std::int16_t>& samples,
                   int channels,
                   int sample_rate);
    bool endInput();
    bool interrupt();
    bool sayHello(const std::string& text);

    // Submit a free-form text query (event 501 ChatTextQuery) on the
    // active session.  The server responds with the same ChatResponse /
    // TTSSentenceStart / TTSResponse / TTSEnded sequence as a voice query,
    // synthesising audio in the session's configured voice.  Used by the
    // orchestrator's chat-intent path to let e2e generate a free-form
    // reply after DeepSeek classifies the user input as chat/unknown.
    // Returns false if no session is open or the WS write failed.
    bool sendChatTextQuery(const std::string& text);

    void setEventCallback(EventCallback callback);
    void setAudioCallback(AudioCallback callback);

    ChatState state() const;
    bool isRunning() const;
    std::string sessionId() const;
    std::string dialogId() const;
    const E2EChatConfig& config() const;

    static E2EChatConfig loadConfig(const std::string& config_path);
    static const char* toString(ChatState state);
    static const char* toString(ChatEventType type);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace e2echat
