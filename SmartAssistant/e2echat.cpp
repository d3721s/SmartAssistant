#include "e2echat.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

#include <toml++/toml.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessageType.h>

#include <zlib.h>

namespace e2echat {
namespace {

inline std::int64_t nowMicros()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void rawCallbackLog(const char* component, const std::string& message)
{
    static std::mutex log_mtx;
    std::lock_guard<std::mutex> lk(log_mtx);
    constexpr std::size_t kMaxMessage = 8192;
    std::string clipped = message;
    if (clipped.size() > kMaxMessage) {
        clipped.resize(kMaxMessage);
        clipped += "...[truncated]";
    }
    const auto ts = static_cast<long long>(nowMicros());
    std::fprintf(stderr, "[WS_DIAG] %lld %s %s\n", ts, component, clipped.c_str());
    std::fflush(stderr);
    if (FILE* fp = std::fopen("/tmp/smartassistant_ws_callbacks.log", "a")) {
        std::fprintf(fp, "[WS_DIAG] %lld %s %s\n", ts, component, clipped.c_str());
        std::fclose(fp);
    }
}

std::string envOr(const std::string& name, const std::string& fallback)
{
    if (name.empty()) {
        return fallback;
    }
    const char* value = std::getenv(name.c_str());
    if (value && *value) {
        return value;
    }
    return fallback;
}

std::string jsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7] = {};
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string makeRandomHex(int bytes)
{
    static thread_local std::mt19937_64 rng{
        static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    std::string out;
    out.reserve(static_cast<std::size_t>(bytes) * 2);
    for (int i = 0; i < bytes; ++i) {
        const auto v = static_cast<std::uint8_t>(rng() & 0xff);
        char buf[3] = {};
        std::snprintf(buf, sizeof(buf), "%02x", v);
        out += buf;
    }
    return out;
}

void writeBE32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::uint32_t readBE32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
            static_cast<std::uint32_t>(p[3]);
}

bool gzipDecompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out)
{
    out.clear();
    if (size == 0) {
        return true;
    }

    z_stream stream{};
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        return false;
    }
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(size);

    std::vector<std::uint8_t> buffer(std::max<std::size_t>(256, size * 4));
    int code = Z_OK;
    do {
        if (stream.total_out >= buffer.size()) {
            buffer.resize(buffer.size() * 2);
        }
        stream.next_out = buffer.data() + stream.total_out;
        stream.avail_out = static_cast<uInt>(buffer.size() - stream.total_out);
        code = inflate(&stream, Z_NO_FLUSH);
        if (code == Z_NEED_DICT || code == Z_DATA_ERROR || code == Z_MEM_ERROR) {
            inflateEnd(&stream);
            return false;
        }
    } while (code != Z_STREAM_END);

    out.assign(buffer.begin(), buffer.begin() + stream.total_out);
    inflateEnd(&stream);
    return true;
}

bool extractJsonString(const std::string& json, const std::string& key, std::string& out)
{
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;

    out.clear();
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (escaped) {
            switch (ch) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(ch); break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return true;
        }
        out.push_back(ch);
    }
    return false;
}

bool extractJsonBool(const std::string& json, const std::string& key, bool& out)
{
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (json.compare(pos, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

std::vector<std::int16_t> toMono(const std::int16_t* samples,
                                 std::size_t sample_count,
                                 int channels)
{
    if (!samples || sample_count == 0) {
        return {};
    }
    if (channels <= 1) {
        return std::vector<std::int16_t>(samples, samples + sample_count);
    }

    const std::size_t frames = sample_count / static_cast<std::size_t>(channels);
    std::vector<std::int16_t> out(frames, 0);
    for (std::size_t i = 0; i < frames; ++i) {
        int sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
            sum += samples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)];
        }
        out[i] = static_cast<std::int16_t>(sum / channels);
    }
    return out;
}

std::vector<std::int16_t> resampleNearest(const std::vector<std::int16_t>& in,
                                          int from_rate,
                                          int to_rate)
{
    if (in.empty() || from_rate <= 0 || to_rate <= 0 || from_rate == to_rate) {
        return in;
    }
    const std::size_t out_size =
        std::max<std::size_t>(1, (in.size() * static_cast<std::size_t>(to_rate)) /
                                     static_cast<std::size_t>(from_rate));
    std::vector<std::int16_t> out(out_size, 0);
    for (std::size_t i = 0; i < out_size; ++i) {
        const std::size_t src =
            (i * static_cast<std::size_t>(from_rate)) / static_cast<std::size_t>(to_rate);
        out[i] = in[std::min(src, in.size() - 1)];
    }
    return out;
}

std::vector<std::uint8_t> pcm16ToBytesLE(const std::int16_t* samples, std::size_t sample_count)
{
    std::vector<std::uint8_t> out(sample_count * sizeof(std::int16_t));
    for (std::size_t i = 0; i < sample_count; ++i) {
        const auto value = static_cast<std::uint16_t>(samples[i]);
        out[i * 2] = static_cast<std::uint8_t>(value & 0xff);
        out[i * 2 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    }
    return out;
}

} // namespace

struct E2EChat::Impl {
    enum : std::uint8_t {
        kProtocolVersion = 0x1,
        kHeaderSize = 0x1,
        kMsgFullClient = 0x1,
        kMsgAudioClient = 0x2,
        kMsgFullServer = 0x9,
        kMsgAudioServer = 0xb,
        kMsgError = 0xf,
        kSerRaw = 0x0,
        kSerJson = 0x1,
        kCompNone = 0x0,
        kCompGzip = 0x1,
        kFlagEvent = 0x4,
    };

    enum EventId : std::int32_t {
        kStartConnection = 1,
        kFinishConnection = 2,
        kConnectionStarted = 50,
        kConnectionFailed = 51,
        kConnectionFinished = 52,
        kStartSession = 100,
        kFinishSession = 102,
        kSessionStarted = 150,
        kSessionFinished = 152,
        kSessionFailed = 153,
        kUsageResponse = 154,
        kTaskRequest = 200,
        kUpdateConfig = 201,
        kConfigUpdated = 251,
        kSayHello = 300,
        kTTSSentenceStart = 350,
        kTTSSentenceEnd = 351,
        kTTSResponse = 352,
        kTTSEnded = 359,
        kEndASR = 400,
        kASRInfo = 450,
        kASRResponse = 451,
        kASREnded = 459,
        kChatResponse = 550,
        kChatEnded = 559,
        kDialogCommonError = 599,
        kClientInterrupt = 515,
        kChatTextQuery = 501,
    };

    struct ParsedFrame {
        std::uint8_t message_type = 0;
        std::uint8_t flags = 0;
        std::uint8_t serialization = 0;
        std::uint8_t compression = 0;
        std::int32_t event = 0;
        std::int32_t error_code = 0;
        std::string id;
        std::vector<std::uint8_t> payload;
    };

    E2EChatConfig cfg;
    quill::Logger* logger = nullptr;
    // Recreated on every startImpl().  Reusing a single ix::WebSocket
    // across reconnects lets stale frame-parser state from a failed
    // previous connection bleed into the next one (we've seen Doubao
    // come back with "unexpected reserved bits 0x50/0x60" 1002 closures
    // when the service is restarted while ws was still half-alive).
    std::unique_ptr<ix::WebSocket> ws;
    bool net_inited = false;

    mutable std::mutex state_mtx;
    std::condition_variable state_cv;
    ChatState chat_state = ChatState::kIdle;
    bool initialized = false;
    bool connection_started = false;
    bool session_started = false;
    bool failed = false;
    std::string failure_message;
    std::string connect_id;
    std::string session_id;
    std::string dialog_id;
    std::string current_question_id;
    std::string current_reply_id;

    std::mutex callback_mtx;
    EventCallback event_callback;
    AudioCallback audio_callback;

    std::mutex send_mtx;

    std::mutex audio_mtx;
    std::condition_variable audio_cv;
    std::deque<std::int16_t> audio_queue;
    std::thread sender_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> sender_running{false};
    std::atomic<bool> stop_requested{false};

    void setupLogger()
    {
        quill::Backend::start();
        std::vector<std::shared_ptr<quill::Sink>> sinks;
        try {
            std::error_code ec;
            std::filesystem::create_directories(cfg.logging.log_dir, ec);
            quill::RotatingFileSinkConfig rotation;
            rotation.set_rotation_max_file_size(cfg.logging.rotation_max_bytes);
            rotation.set_max_backup_files(static_cast<std::uint32_t>(
                std::max(1, cfg.logging.rotation_max_files)));
            rotation.set_open_mode('a');
            const std::string full_path = cfg.logging.log_dir + "/" + cfg.logging.log_file;
            sinks.push_back(quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
                full_path, rotation));
        } catch (...) {
        }
        if (cfg.logging.console) {
            sinks.push_back(quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "e2echat_console_sink"));
        }
        if (sinks.empty()) {
            sinks.push_back(quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "e2echat_fallback_sink"));
        }
        logger = quill::Frontend::create_or_get_logger("E2EChat", std::move(sinks));
        logger->set_log_level(quill::LogLevel::Info);
    }

    bool initImpl(E2EChatConfig in_cfg)
    {
        if (initialized) {
            return true;
        }
        cfg = std::move(in_cfg);
        cfg.app_id = envOr(cfg.app_id_env, cfg.app_id);
        cfg.access_key = envOr(cfg.access_key_env, cfg.access_key);
        if (cfg.connect_id.empty()) {
            cfg.connect_id = makeRandomHex(16);
        }
        if (cfg.input_sample_rate <= 0) {
            cfg.input_sample_rate = 16000;
        }
        if (cfg.input_channels <= 0) {
            cfg.input_channels = 1;
        }
        if (cfg.input_chunk_ms <= 0) {
            cfg.input_chunk_ms = 20;
        }
        if (cfg.output_sample_rate <= 0) {
            cfg.output_sample_rate = 24000;
        }
        if (cfg.output_channels <= 0) {
            cfg.output_channels = 1;
        }

        setupLogger();
        if (cfg.app_id.empty() || cfg.access_key.empty()) {
            LOG_ERROR(logger,
                      "E2EChat credentials missing: set [e2echat].app_id/access_key or env {} / {}",
                      cfg.app_id_env, cfg.access_key_env);
            return false;
        }

        ix::initNetSystem();
        net_inited = true;
        // ws is constructed fresh on every startImpl() so we don't carry
        // any stale parser state across reconnects.
        initialized = true;
        setState(ChatState::kIdle);
        LOG_INFO(logger, "E2EChat initialized endpoint={} model={} speaker={}",
                 cfg.endpoint, cfg.model, cfg.speaker);
        return true;
    }

    void shutdownImpl()
    {
        stopImpl();
        if (net_inited) {
            ix::uninitNetSystem();
            net_inited = false;
        }
        initialized = false;
        if (logger) {
            LOG_INFO(logger, "E2EChat shutdown complete");
            logger->flush_log();
        }
    }

    bool startImpl()
    {
        if (!initialized) {
            return false;
        }
        if (running.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }

        resetRuntimeState();

        // Tear down any leftover WS from a previous failed session and
        // build a fresh instance.  Reusing a half-dead ix::WebSocket has
        // bitten us with 1002 "unexpected reserved bits 0x50/0x60" closes
        // on the very next handshake — its frame parser still holds
        // bytes from the prior connection.
        if (ws) {
            ws->stop();
            ws.reset();
        }
        ws = std::make_unique<ix::WebSocket>();
        ws->disableAutomaticReconnection();
        ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            onMessage(msg);
        });

        ix::WebSocketHttpHeaders headers;
        headers["X-Api-App-ID"] = cfg.app_id;
        headers["X-Api-Access-Key"] = cfg.access_key;
        headers["X-Api-Resource-Id"] = cfg.resource_id;
        headers["X-Api-App-Key"] = cfg.app_key;
        headers["X-Api-Connect-Id"] = connect_id;
        ws->setUrl(cfg.endpoint);
        ws->setExtraHeaders(headers);
        ws->setHandshakeTimeout(std::max(1, static_cast<int>(cfg.connect_timeout.count() / 1000)));

        ix::SocketTLSOptions tls;
        tls.disable_hostname_validation = !cfg.verify_ssl;
        tls.caFile = cfg.verify_ssl ? "SYSTEM" : "NONE";
        ws->setTLSOptions(tls);

        sender_running.store(true, std::memory_order_release);
        sender_thread = std::thread(&Impl::senderLoop, this);

        setState(ChatState::kConnecting);
        ws->start();

        const auto deadline = std::chrono::steady_clock::now() + cfg.connect_timeout;
        std::unique_lock<std::mutex> lk(state_mtx);
        const bool ok = state_cv.wait_until(lk, deadline, [this] {
            return session_started || failed || stop_requested.load(std::memory_order_acquire);
        });
        if (!ok || !session_started || failed) {
            const std::string message =
                failure_message.empty() ? "E2EChat start timeout" : failure_message;
            lk.unlock();
            LOG_ERROR(logger, "E2EChat start failed: {}", message);
            stopImpl();
            return false;
        }

        LOG_INFO(logger, "E2EChat session started session_id={} dialog_id={}",
                 session_id, dialog_id);
        return true;
    }

    void stopImpl()
    {
        if (!running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        stop_requested.store(true, std::memory_order_release);
        setState(ChatState::kClosing);

        if (sessionStartedSnapshot()) {
            sendJsonEvent(kFinishSession, true, "{}");
        }
        sendJsonEvent(kFinishConnection, false, "{}");

        sender_running.store(false, std::memory_order_release);
        audio_cv.notify_all();
        if (sender_thread.joinable()) {
            sender_thread.join();
        }

        if (ws) ws->stop();
        {
            std::lock_guard<std::mutex> lk(audio_mtx);
            audio_queue.clear();
        }
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            session_started = false;
            connection_started = false;
        }
        setState(ChatState::kClosed);
    }

    bool finishSessionImpl()
    {
        if (!running.load(std::memory_order_acquire)) {
            return false;
        }
        return sendJsonEvent(kFinishSession, true, "{}");
    }

    bool finishConnectionImpl()
    {
        if (!running.load(std::memory_order_acquire)) {
            return false;
        }
        return sendJsonEvent(kFinishConnection, false, "{}");
    }

    bool sendAudioImpl(const std::int16_t* samples,
                       std::size_t sample_count,
                       int channels,
                       int sample_rate)
    {
        if (!running.load(std::memory_order_acquire) ||
            !sessionStartedSnapshot() || !samples || sample_count == 0) {
            return false;
        }

        std::vector<std::int16_t> mono = toMono(samples, sample_count, channels);
        if (sample_rate != cfg.input_sample_rate) {
            mono = resampleNearest(mono, sample_rate, cfg.input_sample_rate);
        }
        if (mono.empty()) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(audio_mtx);
            audio_queue.insert(audio_queue.end(), mono.begin(), mono.end());
        }
        audio_cv.notify_one();
        return true;
    }

    bool endInputImpl()
    {
        if (!running.load(std::memory_order_acquire) || !sessionStartedSnapshot()) {
            return false;
        }
        return sendJsonEvent(kEndASR, true, "{}");
    }

    bool interruptImpl()
    {
        if (!running.load(std::memory_order_acquire) || !sessionStartedSnapshot()) {
            return false;
        }
        return sendJsonEvent(kClientInterrupt, true, "{}");
    }

    bool sayHelloImpl(const std::string& text)
    {
        if (!running.load(std::memory_order_acquire) || !sessionStartedSnapshot()) {
            return false;
        }
        return sendJsonEvent(kSayHello, true,
                             "{\"content\":\"" + jsonEscape(text) + "\"}");
    }

    bool chatTextQueryImpl(const std::string& text)
    {
        if (!running.load(std::memory_order_acquire) || !sessionStartedSnapshot()) {
            return false;
        }
        return sendJsonEvent(kChatTextQuery, true,
                             "{\"content\":\"" + jsonEscape(text) + "\"}");
    }

    void resetRuntimeState()
    {
        connect_id = cfg.connect_id.empty() ? makeRandomHex(16) : cfg.connect_id;
        session_id = makeRandomHex(16);
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            connection_started = false;
            session_started = false;
            failed = false;
            failure_message.clear();
            dialog_id = cfg.dialog_id;
            current_question_id.clear();
            current_reply_id.clear();
        }
        {
            std::lock_guard<std::mutex> lk(audio_mtx);
            audio_queue.clear();
        }
        stop_requested.store(false, std::memory_order_release);
    }

    void setState(ChatState state)
    {
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            chat_state = state;
        }
        state_cv.notify_all();
    }

    bool sessionStartedSnapshot() const
    {
        std::lock_guard<std::mutex> lk(state_mtx);
        return session_started;
    }

    std::string sessionIdSnapshot() const
    {
        std::lock_guard<std::mutex> lk(state_mtx);
        return session_id;
    }

    std::string dialogIdSnapshot() const
    {
        std::lock_guard<std::mutex> lk(state_mtx);
        return dialog_id;
    }

    static std::vector<std::uint8_t> buildFrame(std::uint8_t message_type,
                                                std::uint8_t serialization,
                                                std::int32_t event,
                                                const std::string* session,
                                                const std::vector<std::uint8_t>& payload)
    {
        std::vector<std::uint8_t> out;
        out.reserve(payload.size() + (session ? session->size() : 0) + 24);
        out.push_back(static_cast<std::uint8_t>((kProtocolVersion << 4) | kHeaderSize));
        out.push_back(static_cast<std::uint8_t>((message_type << 4) | kFlagEvent));
        out.push_back(static_cast<std::uint8_t>((serialization << 4) | kCompNone));
        out.push_back(0x00);
        writeBE32(out, static_cast<std::uint32_t>(event));
        if (session) {
            writeBE32(out, static_cast<std::uint32_t>(session->size()));
            out.insert(out.end(), session->begin(), session->end());
        }
        writeBE32(out, static_cast<std::uint32_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    bool sendJsonEvent(std::int32_t event, bool include_session, const std::string& json)
    {
        if (cfg.debug) {
            std::ostringstream oss;
            oss << "send event=" << event
                << " session=" << (include_session ? "yes" : "no")
                << " payload_len=" << json.size()
                << " payload=";
            // Cap at 1024 chars so a long system_prompt doesn't blow up logs.
            if (json.size() > 1024) {
                oss << json.substr(0, 1024) << "...[+"
                    << (json.size() - 1024) << " more]";
            } else {
                oss << json;
            }
            rawCallbackLog("E2EChat", oss.str());
        }
        const std::vector<std::uint8_t> payload(json.begin(), json.end());
        const std::string sid = sessionIdSnapshot();
        const std::string* sid_ptr = include_session ? &sid : nullptr;
        const auto frame = buildFrame(kMsgFullClient, kSerJson, event, sid_ptr, payload);
        return sendBinaryFrame(frame, "json event " + std::to_string(event));
    }

    bool sendAudioEvent(const std::vector<std::uint8_t>& payload)
    {
        const std::string sid = sessionIdSnapshot();
        const auto frame = buildFrame(kMsgAudioClient, kSerRaw, kTaskRequest, &sid, payload);
        return sendBinaryFrame(frame, "audio event");
    }

    bool sendBinaryFrame(const std::vector<std::uint8_t>& frame, const std::string& label)
    {
        std::lock_guard<std::mutex> lk(send_mtx);
        if (!ws) {
            markFailed("ws not constructed for " + label);
            return false;
        }
        const bool ok = ws->sendBinary(std::string(frame.begin(), frame.end())).success;
        if (!ok) {
            markFailed("failed to send " + label);
        }
        return ok;
    }

    std::string startSessionPayload() const
    {
        std::ostringstream oss;
        oss << "{"
            << "\"asr\":{"
            <<   "\"audio_info\":{"
            <<     "\"format\":\"" << jsonEscape(cfg.input_format) << "\","
            <<     "\"sample_rate\":" << cfg.input_sample_rate << ","
            <<     "\"channel\":" << cfg.input_channels
            <<   "},"
            <<   "\"extra\":{"
            <<     "\"end_smooth_window_ms\":" << cfg.end_smooth_window_ms << ","
            <<     "\"enable_custom_vad\":" << (cfg.enable_custom_vad ? "true" : "false") << ","
            <<     "\"enable_asr_twopass\":" << (cfg.enable_asr_twopass ? "true" : "false")
            <<   "}"
            << "},"
            << "\"tts\":{"
            <<   "\"speaker\":\"" << jsonEscape(cfg.speaker) << "\","
            <<   "\"audio_config\":{"
            <<     "\"channel\":" << cfg.output_channels << ","
            <<     "\"format\":\"" << jsonEscape(cfg.output_format) << "\","
            <<     "\"sample_rate\":" << cfg.output_sample_rate << ","
            <<     "\"speech_rate\":" << cfg.speech_rate << ","
            <<     "\"loudness_rate\":" << cfg.loudness_rate
            <<   "},"
            <<   "\"extra\":{}"
            << "},"
            << "\"dialog\":{"
            <<   "\"bot_name\":\"" << jsonEscape(cfg.bot_name) << "\","
            <<   "\"system_role\":\"" << jsonEscape(cfg.system_role) << "\","
            <<   "\"speaking_style\":\"" << jsonEscape(cfg.speaking_style) << "\","
            <<   "\"dialog_id\":\"" << jsonEscape(cfg.dialog_id) << "\","
            <<   "\"extra\":{"
            <<     "\"strict_audit\":" << (cfg.strict_audit ? "true" : "false") << ","
            <<     "\"input_mod\":\"" << jsonEscape(cfg.input_mod) << "\","
            <<     "\"enable_loudness_norm\":" << (cfg.enable_loudness_norm ? "true" : "false") << ","
            <<     "\"enable_conversation_truncate\":" << (cfg.enable_conversation_truncate ? "true" : "false") << ","
            <<     "\"enable_user_query_exit\":" << (cfg.enable_user_query_exit ? "true" : "false") << ","
            <<     "\"model\":\"" << jsonEscape(cfg.model) << "\""
            <<   "}"
            << "}"
            << "}";
        return oss.str();
    }

    void onMessage(const ix::WebSocketMessagePtr& msg)
    {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            rawCallbackLog("E2EChat",
                           std::string("E2EChat WebSocket opened connect_id=") +
                           connect_id);
            sendJsonEvent(kStartConnection, false, "{}");
            break;
        case ix::WebSocketMessageType::Close:
            {
                std::ostringstream oss;
                oss << "E2EChat WebSocket closed code="
                    << msg->closeInfo.code << " reason="
                    << msg->closeInfo.reason;
                rawCallbackLog("E2EChat", oss.str());
            }
            if (running.load(std::memory_order_acquire) &&
                !stop_requested.load(std::memory_order_acquire)) {
                markFailed(msg->closeInfo.reason.empty()
                               ? "E2EChat WebSocket closed"
                               : msg->closeInfo.reason);
            }
            break;
        case ix::WebSocketMessageType::Error:
            rawCallbackLog("E2EChat",
                           std::string("E2EChat WebSocket error: ") +
                           msg->errorInfo.reason);
            markFailed(msg->errorInfo.reason);
            break;
        case ix::WebSocketMessageType::Message:
            if (msg->binary) {
                handleServerFrame(reinterpret_cast<const std::uint8_t*>(msg->str.data()),
                                  msg->str.size());
            } else if (!msg->str.empty()) {
                rawCallbackLog("E2EChat",
                               std::string("E2EChat text message: ") + msg->str);
            }
            break;
        default:
            break;
        }
    }

    bool parseServerFrame(const std::uint8_t* data, std::size_t size, ParsedFrame& out)
    {
        if (!data || size < 4) {
            return false;
        }
        const std::size_t header_bytes = static_cast<std::size_t>(data[0] & 0x0f) * 4;
        if (header_bytes < 4 || size < header_bytes) {
            return false;
        }
        out.message_type = static_cast<std::uint8_t>((data[1] >> 4) & 0x0f);
        out.flags = static_cast<std::uint8_t>(data[1] & 0x0f);
        out.serialization = static_cast<std::uint8_t>((data[2] >> 4) & 0x0f);
        out.compression = static_cast<std::uint8_t>(data[2] & 0x0f);

        std::size_t offset = header_bytes;
        if (out.message_type == kMsgError) {
            if (offset + 4 > size) {
                return false;
            }
            out.error_code = static_cast<std::int32_t>(readBE32(data + offset));
            offset += 4;
        } else if ((out.flags & kFlagEvent) == kFlagEvent) {
            if (offset + 4 > size) {
                return false;
            }
            out.event = static_cast<std::int32_t>(readBE32(data + offset));
            offset += 4;
        }

        if (out.message_type != kMsgError && offset + 8 <= size) {
            const std::size_t id_offset = offset;
            const std::uint32_t id_size = readBE32(data + offset);
            if (id_size > 0 && id_size <= 256 && offset + 4 + id_size + 4 <= size) {
                const std::size_t payload_size_offset = offset + 4 + id_size;
                const std::uint32_t payload_size = readBE32(data + payload_size_offset);
                if (payload_size_offset + 4 + payload_size <= size) {
                    offset += 4;
                    out.id.assign(reinterpret_cast<const char*>(data + offset), id_size);
                    offset = payload_size_offset;
                } else {
                    offset = id_offset;
                }
            }
        }

        if (offset + 4 > size) {
            return false;
        }
        const std::uint32_t payload_size = readBE32(data + offset);
        offset += 4;
        if (offset + payload_size > size) {
            return false;
        }

        out.payload.assign(data + offset, data + offset + payload_size);
        if (out.compression == kCompGzip) {
            std::vector<std::uint8_t> decoded;
            if (!gzipDecompress(out.payload.data(), out.payload.size(), decoded)) {
                return false;
            }
            out.payload.swap(decoded);
        } else if (out.compression != kCompNone) {
            return false;
        }
        return true;
    }

    void handleServerFrame(const std::uint8_t* data, std::size_t size)
    {
        ParsedFrame frame;
        if (!parseServerFrame(data, size, frame)) {
            markFailed("failed to parse E2EChat server frame");
            return;
        }

        if (frame.message_type == kMsgError) {
            const std::string payload(frame.payload.begin(), frame.payload.end());
            std::ostringstream oss;
            oss << "E2EChat server error code=" << frame.error_code
                << " payload=" << payload;
            rawCallbackLog("E2EChat", oss.str());
            emitError(frame.event, std::to_string(frame.error_code), payload);
            markFailed(payload.empty() ? "server error" : payload);
            return;
        }

        if (frame.event != kTTSResponse && cfg.debug) {
            const std::string payload(frame.payload.begin(), frame.payload.end());
            std::ostringstream oss;
            oss << "E2EChat event=" << frame.event
                << " type=" << static_cast<int>(frame.message_type)
                << " payload=" << payload;
            rawCallbackLog("E2EChat", oss.str());
        }

        if (frame.event == kTTSResponse || frame.message_type == kMsgAudioServer) {
            if (!frame.payload.empty()) {
                AudioChunk chunk;
                chunk.bytes = std::move(frame.payload);
                chunk.sample_rate = cfg.output_sample_rate;
                chunk.channels = cfg.output_channels;
                chunk.format = cfg.output_format;
                chunk.timestamp_us = nowMicros();
                {
                    std::lock_guard<std::mutex> lk(state_mtx);
                    chunk.question_id = current_question_id;
                    chunk.reply_id = current_reply_id;
                }
                emitAudio(chunk);
            }
            return;
        }

        const std::string json(frame.payload.begin(), frame.payload.end());
        handleJsonEvent(frame, json);
    }

    void handleJsonEvent(const ParsedFrame& frame, const std::string& json)
    {
        switch (frame.event) {
        case kConnectionStarted:
            {
                std::lock_guard<std::mutex> lk(state_mtx);
                connection_started = true;
            }
            setState(ChatState::kConnected);
            emitSimple(ChatEventType::kConnectionStarted, frame.event, json);
            sendJsonEvent(kStartSession, true, startSessionPayload());
            break;
        case kConnectionFailed:
            emitError(frame.event, "", json);
            markFailed(json.empty() ? "connection failed" : json);
            break;
        case kConnectionFinished:
            emitSimple(ChatEventType::kConnectionFinished, frame.event, json);
            break;
        case kSessionStarted:
            {
                std::string parsed_dialog_id;
                extractJsonString(json, "dialog_id", parsed_dialog_id);
                std::lock_guard<std::mutex> lk(state_mtx);
                if (!parsed_dialog_id.empty()) {
                    dialog_id = parsed_dialog_id;
                }
                session_started = true;
            }
            setState(ChatState::kSessionStarted);
            emitSessionStarted(frame.event, json);
            if (cfg.send_opening_line && !cfg.opening_line.empty()) {
                sendJsonEvent(kSayHello, true,
                              "{\"content\":\"" + jsonEscape(cfg.opening_line) + "\"}");
            }
            break;
        case kSessionFinished:
            {
                std::lock_guard<std::mutex> lk(state_mtx);
                session_started = false;
            }
            emitSimple(ChatEventType::kSessionFinished, frame.event, json);
            break;
        case kSessionFailed:
            emitError(frame.event, "", json);
            markFailed(json.empty() ? "session failed" : json);
            break;
        case kUsageResponse:
            emitSimple(ChatEventType::kUsage, frame.event, json);
            break;
        case kConfigUpdated:
            emitSimple(ChatEventType::kConfigUpdated, frame.event, json);
            break;
        case kTTSSentenceStart:
            emitTtsSentenceStart(frame.event, json);
            break;
        case kTTSSentenceEnd:
            emitTtsSentenceEnd(frame.event, json);
            break;
        case kTTSEnded:
            emitTtsEnded(frame.event, json);
            break;
        case kASRInfo:
            emitAsrInfo(frame.event, json);
            break;
        case kASRResponse:
            emitAsrResponse(frame.event, json);
            break;
        case kASREnded:
            emitSimple(ChatEventType::kASREnded, frame.event, json);
            break;
        case kChatResponse:
            emitChatResponse(frame.event, json);
            break;
        case kChatEnded:
            emitChatEnded(frame.event, json);
            break;
        case kDialogCommonError:
            {
                std::string status_code;
                std::string message;
                extractJsonString(json, "status_code", status_code);
                extractJsonString(json, "message", message);
                emitError(frame.event, status_code, message.empty() ? json : message);
            }
            break;
        default:
            emitSimple(ChatEventType::kRawJson, frame.event, json);
            break;
        }
    }

    void senderLoop()
    {
        const std::size_t chunk_samples =
            std::max<std::size_t>(1,
                static_cast<std::size_t>(cfg.input_sample_rate) *
                static_cast<std::size_t>(cfg.input_chunk_ms) / 1000);
        std::vector<std::int16_t> chunk;
        chunk.reserve(chunk_samples);

        {
            std::unique_lock<std::mutex> lk(state_mtx);
            state_cv.wait(lk, [this] {
                return session_started ||
                       failed ||
                       !sender_running.load(std::memory_order_acquire);
            });
        }

        while (sender_running.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lk(audio_mtx);
                audio_cv.wait_for(lk, std::chrono::milliseconds(20), [this, chunk_samples] {
                    return audio_queue.size() >= chunk_samples ||
                           !sender_running.load(std::memory_order_acquire);
                });
                while (chunk.size() < chunk_samples && !audio_queue.empty()) {
                    chunk.push_back(audio_queue.front());
                    audio_queue.pop_front();
                }
            }

            if (!sender_running.load(std::memory_order_acquire)) {
                break;
            }
            if (chunk.size() >= chunk_samples) {
                const auto bytes = pcm16ToBytesLE(chunk.data(), chunk.size());
                sendAudioEvent(bytes);
                chunk.clear();
            }
        }
    }

    void emitSimple(ChatEventType type, int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = type;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        event.timestamp_us = nowMicros();
        emitEvent(event);
    }

    void emitSessionStarted(int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = ChatEventType::kSessionStarted;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        event.timestamp_us = nowMicros();
        emitEvent(event);
    }

    void emitTtsSentenceStart(int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = ChatEventType::kTTSSentenceStart;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        extractJsonString(json, "text", event.text);
        extractJsonString(json, "question_id", event.question_id);
        extractJsonString(json, "reply_id", event.reply_id);
        extractJsonString(json, "tts_type", event.tts_type);
        event.timestamp_us = nowMicros();
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            current_question_id = event.question_id;
            current_reply_id = event.reply_id;
        }
        emitEvent(event);
    }

    void emitTtsSentenceEnd(int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = ChatEventType::kTTSSentenceEnd;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        extractJsonString(json, "question_id", event.question_id);
        extractJsonString(json, "reply_id", event.reply_id);
        event.timestamp_us = nowMicros();
        emitEvent(event);
    }

    void emitTtsEnded(int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = ChatEventType::kTTSEnded;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        extractJsonString(json, "question_id", event.question_id);
        extractJsonString(json, "reply_id", event.reply_id);
        extractJsonString(json, "status_code", event.status_code);
        event.exit_intent = !cfg.exit_status_code.empty() &&
                            event.status_code == cfg.exit_status_code;
        event.timestamp_us = nowMicros();
        emitEvent(event);
        if (event.exit_intent) {
            event.type = ChatEventType::kExitIntent;
            event.message = "user exit intent detected";
            emitEvent(event);
        }
    }

    void emitAsrInfo(int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = ChatEventType::kASRInfo;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        extractJsonString(json, "question_id", event.question_id);
        event.timestamp_us = nowMicros();
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            current_question_id = event.question_id;
        }
        emitEvent(event);
    }

    void emitAsrResponse(int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = ChatEventType::kASRResponse;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        extractJsonString(json, "text", event.text);
        extractJsonBool(json, "is_interim", event.is_interim);
        event.timestamp_us = nowMicros();
        emitEvent(event);
    }

    void emitChatResponse(int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = ChatEventType::kChatResponse;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        extractJsonString(json, "content", event.text);
        extractJsonString(json, "question_id", event.question_id);
        extractJsonString(json, "reply_id", event.reply_id);
        event.timestamp_us = nowMicros();
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            current_question_id = event.question_id;
            current_reply_id = event.reply_id;
        }
        emitEvent(event);
    }

    void emitChatEnded(int event_id, const std::string& json)
    {
        ChatEvent event;
        event.type = ChatEventType::kChatEnded;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.raw_json = json;
        extractJsonString(json, "question_id", event.question_id);
        extractJsonString(json, "reply_id", event.reply_id);
        event.timestamp_us = nowMicros();
        emitEvent(event);
    }

    void emitError(int event_id, const std::string& status_code, const std::string& message)
    {
        ChatEvent event;
        event.type = ChatEventType::kError;
        event.event_id = event_id;
        event.session_id = sessionIdSnapshot();
        event.dialog_id = dialogIdSnapshot();
        event.status_code = status_code;
        event.message = message;
        event.raw_json = message;
        event.timestamp_us = nowMicros();
        emitEvent(event);
    }

    void emitEvent(const ChatEvent& event)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lk(callback_mtx);
            callback = event_callback;
        }
        if (callback) {
            try {
                callback(event);
            } catch (...) {
            }
        }
    }

    void emitAudio(const AudioChunk& chunk)
    {
        AudioCallback callback;
        {
            std::lock_guard<std::mutex> lk(callback_mtx);
            callback = audio_callback;
        }
        if (callback) {
            try {
                callback(chunk);
            } catch (...) {
            }
        }
    }

    void markFailed(const std::string& message)
    {
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            failed = true;
            failure_message = message;
            chat_state = ChatState::kFailed;
        }
        state_cv.notify_all();
        if (!message.empty()) {
            emitError(0, "", message);
        }
    }
};

E2EChat::E2EChat() : impl_(std::make_unique<Impl>()) {}

E2EChat::~E2EChat()
{
    shutdown();
}

bool E2EChat::init(const std::string& config_path)
{
    try {
        return impl_->initImpl(loadConfig(config_path));
    } catch (const std::exception& ex) {
        return false;
    }
}

bool E2EChat::init(const E2EChatConfig& config)
{
    return impl_->initImpl(config);
}

void E2EChat::shutdown()
{
    if (impl_) {
        impl_->shutdownImpl();
    }
}

bool E2EChat::start()
{
    return impl_->startImpl();
}

void E2EChat::stop()
{
    impl_->stopImpl();
}

bool E2EChat::finishSession()
{
    return impl_->finishSessionImpl();
}

bool E2EChat::finishConnection()
{
    return impl_->finishConnectionImpl();
}

bool E2EChat::sendAudio(const std::int16_t* samples,
                        std::size_t sample_count,
                        int channels,
                        int sample_rate)
{
    return impl_->sendAudioImpl(samples, sample_count, channels, sample_rate);
}

bool E2EChat::sendAudio(const std::vector<std::int16_t>& samples,
                        int channels,
                        int sample_rate)
{
    return sendAudio(samples.data(), samples.size(), channels, sample_rate);
}

bool E2EChat::endInput()
{
    return impl_->endInputImpl();
}

bool E2EChat::interrupt()
{
    return impl_->interruptImpl();
}

bool E2EChat::sayHello(const std::string& text)
{
    return impl_->sayHelloImpl(text);
}

bool E2EChat::sendChatTextQuery(const std::string& text)
{
    return impl_->chatTextQueryImpl(text);
}

void E2EChat::setEventCallback(EventCallback callback)
{
    std::lock_guard<std::mutex> lk(impl_->callback_mtx);
    impl_->event_callback = std::move(callback);
}

void E2EChat::setAudioCallback(AudioCallback callback)
{
    std::lock_guard<std::mutex> lk(impl_->callback_mtx);
    impl_->audio_callback = std::move(callback);
}

ChatState E2EChat::state() const
{
    std::lock_guard<std::mutex> lk(impl_->state_mtx);
    return impl_->chat_state;
}

bool E2EChat::isRunning() const
{
    return impl_->running.load(std::memory_order_acquire);
}

std::string E2EChat::sessionId() const
{
    return impl_->sessionIdSnapshot();
}

std::string E2EChat::dialogId() const
{
    return impl_->dialogIdSnapshot();
}

const E2EChatConfig& E2EChat::config() const
{
    return impl_->cfg;
}

E2EChatConfig E2EChat::loadConfig(const std::string& config_path)
{
    E2EChatConfig cfg;
    toml::table table = toml::parse_file(config_path);

    const toml::node_view<toml::node> root = table["e2echat"];
    const toml::node_view<toml::node> online = root["online"];
    cfg.endpoint = root["endpoint"].value_or(online["endpoint"].value_or(cfg.endpoint));
    cfg.app_id = root["app_id"].value_or(online["app_id"].value_or(cfg.app_id));
    cfg.access_key = root["access_key"].value_or(online["access_key"].value_or(cfg.access_key));
    cfg.resource_id = root["resource_id"].value_or(online["resource_id"].value_or(cfg.resource_id));
    cfg.app_key = root["app_key"].value_or(online["app_key"].value_or(cfg.app_key));
    cfg.app_id_env = root["app_id_env"].value_or(online["app_id_env"].value_or(cfg.app_id_env));
    cfg.access_key_env = root["access_key_env"].value_or(
        online["access_key_env"].value_or(cfg.access_key_env));
    cfg.connect_id = root["connect_id"].value_or(online["connect_id"].value_or(cfg.connect_id));

    cfg.model = root["model"].value_or(cfg.model);
    cfg.speaker = root["speaker"].value_or(cfg.speaker);
    cfg.bot_name = root["bot_name"].value_or(cfg.bot_name);
    cfg.system_role = root["system_role"].value_or(cfg.system_role);
    cfg.speaking_style = root["speaking_style"].value_or(cfg.speaking_style);
    cfg.opening_line = root["opening_line"].value_or(cfg.opening_line);
    cfg.dialog_id = root["dialog_id"].value_or(cfg.dialog_id);
    cfg.input_mod = root["input_mod"].value_or(cfg.input_mod);
    cfg.end_smooth_window_ms = static_cast<int>(
        root["end_smooth_window_ms"].value_or(static_cast<int64_t>(cfg.end_smooth_window_ms)));
    cfg.enable_custom_vad = root["enable_custom_vad"].value_or(cfg.enable_custom_vad);
    cfg.enable_asr_twopass = root["enable_asr_twopass"].value_or(cfg.enable_asr_twopass);

    const toml::node_view<toml::node> input = root["input"];
    cfg.input_sample_rate = input["sample_rate"].value_or(cfg.input_sample_rate);
    cfg.input_channels = input["channels"].value_or(cfg.input_channels);
    cfg.input_format = input["format"].value_or(cfg.input_format);
    cfg.input_chunk_ms = input["chunk_ms"].value_or(cfg.input_chunk_ms);

    const toml::node_view<toml::node> output = root["output"];
    cfg.output_sample_rate = output["sample_rate"].value_or(cfg.output_sample_rate);
    cfg.output_channels = output["channels"].value_or(cfg.output_channels);
    cfg.output_format = output["format"].value_or(cfg.output_format);
    cfg.speech_rate = output["speech_rate"].value_or(cfg.speech_rate);
    cfg.loudness_rate = output["loudness_rate"].value_or(cfg.loudness_rate);

    cfg.strict_audit = root["strict_audit"].value_or(cfg.strict_audit);
    cfg.enable_loudness_norm = root["enable_loudness_norm"].value_or(cfg.enable_loudness_norm);
    cfg.enable_conversation_truncate =
        root["enable_conversation_truncate"].value_or(cfg.enable_conversation_truncate);
    cfg.enable_user_query_exit =
        root["enable_user_query_exit"].value_or(cfg.enable_user_query_exit);
    cfg.exit_status_code = root["exit_status_code"].value_or(cfg.exit_status_code);
    cfg.send_opening_line = root["send_opening_line"].value_or(cfg.send_opening_line);
    cfg.verify_ssl = root["verify_ssl"].value_or(online["verify_ssl"].value_or(cfg.verify_ssl));
    cfg.debug = root["debug"].value_or(cfg.debug);
    cfg.connect_timeout = std::chrono::milliseconds(
        root["connect_timeout_ms"].value_or(
            online["connect_timeout_ms"].value_or(static_cast<int64_t>(cfg.connect_timeout.count()))));
    cfg.close_timeout = std::chrono::milliseconds(
        root["close_timeout_ms"].value_or(static_cast<int64_t>(cfg.close_timeout.count())));

    const toml::node_view<toml::node> logging = root["logging"];
    cfg.logging.log_dir = logging["log_dir"].value_or(cfg.logging.log_dir);
    cfg.logging.log_file = logging["log_file"].value_or(cfg.logging.log_file);
    cfg.logging.console = logging["console"].value_or(cfg.logging.console);
    cfg.logging.rotation_max_bytes = static_cast<std::size_t>(
        logging["rotation_max_bytes"].value_or(
            static_cast<int64_t>(cfg.logging.rotation_max_bytes)));
    cfg.logging.rotation_max_files =
        logging["rotation_max_files"].value_or(cfg.logging.rotation_max_files);

    const toml::node_view<toml::node> asr_online = table["asr"]["online"];
    if (cfg.app_id.empty()) {
        cfg.app_id = asr_online["app_id"].value_or(
            asr_online["app_key"].value_or(cfg.app_id));
    }
    if (cfg.access_key.empty()) {
        cfg.access_key = asr_online["access_key"].value_or(cfg.access_key);
    }
    return cfg;
}

const char* E2EChat::toString(ChatState state)
{
    switch (state) {
    case ChatState::kIdle: return "Idle";
    case ChatState::kConnecting: return "Connecting";
    case ChatState::kConnected: return "Connected";
    case ChatState::kSessionStarted: return "SessionStarted";
    case ChatState::kClosing: return "Closing";
    case ChatState::kClosed: return "Closed";
    case ChatState::kFailed: return "Failed";
    }
    return "Unknown";
}

const char* E2EChat::toString(ChatEventType type)
{
    switch (type) {
    case ChatEventType::kConnectionStarted: return "ConnectionStarted";
    case ChatEventType::kConnectionFinished: return "ConnectionFinished";
    case ChatEventType::kSessionStarted: return "SessionStarted";
    case ChatEventType::kSessionFinished: return "SessionFinished";
    case ChatEventType::kASRInfo: return "ASRInfo";
    case ChatEventType::kASRResponse: return "ASRResponse";
    case ChatEventType::kASREnded: return "ASREnded";
    case ChatEventType::kTTSSentenceStart: return "TTSSentenceStart";
    case ChatEventType::kTTSSentenceEnd: return "TTSSentenceEnd";
    case ChatEventType::kTTSEnded: return "TTSEnded";
    case ChatEventType::kExitIntent: return "ExitIntent";
    case ChatEventType::kChatResponse: return "ChatResponse";
    case ChatEventType::kChatEnded: return "ChatEnded";
    case ChatEventType::kUsage: return "Usage";
    case ChatEventType::kConfigUpdated: return "ConfigUpdated";
    case ChatEventType::kError: return "Error";
    case ChatEventType::kRawJson: return "RawJson";
    }
    return "Unknown";
}

} // namespace e2echat
