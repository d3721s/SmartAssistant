#include "IntentManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>

namespace intent {

// ============================================================================
// Helpers
// ============================================================================
namespace {

inline std::int64_t nowMicros()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

inline int timeoutSecondsCeil(int timeout_ms)
{
    if (timeout_ms <= 0) {
        return 1;
    }
    return std::max(1, (timeout_ms + 999) / 1000);
}

inline std::string envOr(const std::string& name, const std::string& fallback)
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

inline bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

const char* engineLabel(EngineType e)
{
    return e == EngineType::kOnline ? "online" : "offline";
}

bool containsAny(const std::string& text, std::initializer_list<const char*> needles)
{
    for (const char* needle : needles) {
        if (needle && *needle && text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string armActionFromText(const std::string& text)
{
    if (containsAny(text, {"向前", "往前", "前进", "前移", "伸出", "伸长"})) {
        return "extend";
    }
    if (containsAny(text, {"向后", "往后", "后退", "后移", "缩回", "收回"})) {
        return "retract";
    }
    if (containsAny(text, {"向上", "往上", "上升", "抬起", "举起", "升起"})) {
        return "raise";
    }
    if (containsAny(text, {"向下", "往下", "下降", "下移", "放下", "降下"})) {
        return "lower";
    }
    if (containsAny(text, {"停止", "停下", "复位", "归位"})) {
        return "reset";
    }
    return "";
}

// ---- Tiny JSON helpers (matching ASRManager's approach) --------------------

bool extractJsonString(const std::string& json, const std::string& key, std::string& out)
{
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            const char esc = json[pos + 1];
            switch (esc) {
                case 'n': out.push_back('\n'); pos += 2; continue;
                case 't': out.push_back('\t'); pos += 2; continue;
                case 'r': out.push_back('\r'); pos += 2; continue;
                case '"': out.push_back('"');  pos += 2; continue;
                case '\\': out.push_back('\\'); pos += 2; continue;
                default: ++pos; break;
            }
        }
        out.push_back(json[pos]);
        ++pos;
    }
    return true;
}

bool extractJsonNumber(const std::string& json, const std::string& key, double& out)
{
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) {
        ++pos;
    }
    const std::size_t start = pos;
    if (pos < json.size() && (json[pos] == '-' || json[pos] == '+')) ++pos;
    while (pos < json.size() &&
           ((json[pos] >= '0' && json[pos] <= '9') || json[pos] == '.' ||
            json[pos] == 'e' || json[pos] == 'E' || json[pos] == '-' || json[pos] == '+')) {
        ++pos;
    }
    if (pos == start) return false;
    try {
        out = std::stod(json.substr(start, pos - start));
        return true;
    } catch (...) {
        return false;
    }
}

std::string jsonEscape(const std::string& v)
{
    std::string out;
    out.reserve(v.size() + 2);
    for (char c : v) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xff);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Parse the contents of a top-level JSON object value, e.g. extract every
// "k":"v" pair inside "slots":{ ... }.  Lightweight; assumes well-formed slots.
bool extractObjectBlock(const std::string& json, const std::string& key, std::string& out)
{
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '{') return false;
    int depth = 0;
    const std::size_t start = pos;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '{') {
            ++depth;
        } else if (json[pos] == '}') {
            if (--depth == 0) {
                out = json.substr(start, pos - start + 1);
                return true;
            }
        } else if (json[pos] == '"') {
            ++pos;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
                ++pos;
            }
        }
    }
    return false;
}

void parseFlatStringMap(const std::string& obj, std::map<std::string, std::string>& out)
{
    out.clear();
    if (obj.size() < 2) return;
    std::size_t pos = 1;  // skip leading '{'
    while (pos < obj.size()) {
        while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t' ||
                                    obj[pos] == '\n' || obj[pos] == ',')) {
            ++pos;
        }
        if (pos >= obj.size() || obj[pos] == '}') break;
        if (obj[pos] != '"') break;
        ++pos;
        std::string key;
        while (pos < obj.size() && obj[pos] != '"') {
            if (obj[pos] == '\\' && pos + 1 < obj.size()) ++pos;
            key.push_back(obj[pos]);
            ++pos;
        }
        if (pos >= obj.size()) break;
        ++pos;  // closing quote
        while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t' || obj[pos] == ':')) {
            ++pos;
        }
        std::string value;
        if (pos < obj.size() && obj[pos] == '"') {
            ++pos;
            while (pos < obj.size() && obj[pos] != '"') {
                if (obj[pos] == '\\' && pos + 1 < obj.size()) ++pos;
                value.push_back(obj[pos]);
                ++pos;
            }
            if (pos < obj.size()) ++pos;
        } else {
            // unquoted scalar: read until comma or '}'
            while (pos < obj.size() && obj[pos] != ',' && obj[pos] != '}') {
                value.push_back(obj[pos]);
                ++pos;
            }
            // trim trailing whitespace
            while (!value.empty() &&
                   (value.back() == ' ' || value.back() == '\t' || value.back() == '\n')) {
                value.pop_back();
            }
        }
        if (!key.empty()) {
            out.emplace(std::move(key), std::move(value));
        }
    }
}

std::string buildUserMessage(const IntentRequest& req, bool include_context)
{
    std::ostringstream os;
    os << "user_text: " << req.text;
    if (include_context && !req.previous_intent.empty()) {
        os << "\nprevious_intent: " << req.previous_intent;
    }
    if (include_context && !req.context_slots.empty()) {
        os << "\ncontext_slots: {";
        bool first = true;
        for (const auto& kv : req.context_slots) {
            if (!first) os << ", ";
            first = false;
            os << kv.first << "=" << kv.second;
        }
        os << "}";
    }
    return os.str();
}

std::string buildChatCompletionsBody(const OnlineIntentConfig& cfg,
                                     const IntentRequest&      req)
{
    std::ostringstream os;
    os << "{\"model\":\"" << jsonEscape(cfg.model) << "\","
       << "\"messages\":[";
    if (!cfg.system_prompt.empty()) {
        os << "{\"role\":\"system\",\"content\":\""
           << jsonEscape(cfg.system_prompt) << "\"},";
    }
    os << "{\"role\":\"user\",\"content\":\""
       << jsonEscape(buildUserMessage(req, cfg.include_context)) << "\"}],"
       << "\"temperature\":" << cfg.temperature
       << ",\"max_tokens\":" << cfg.max_tokens;
    if (cfg.response_format_json) {
        os << ",\"response_format\":{\"type\":\"json_object\"}";
    }
    if (!cfg.thinking_type.empty()) {
        os << ",\"thinking\":{\"type\":\""
           << jsonEscape(cfg.thinking_type) << "\"}";
    }
    if (!cfg.reasoning_effort.empty() && cfg.thinking_type != "disabled") {
        os << ",\"reasoning_effort\":\""
           << jsonEscape(cfg.reasoning_effort) << "\"";
    }
    os << "}";
    return os.str();
}

std::string stripCodeFences(const std::string& s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return s;
    auto end = s.find_last_not_of(" \t\r\n");
    std::string trimmed = s.substr(begin, end - begin + 1);
    if (trimmed.size() >= 3 && trimmed.compare(0, 3, "```") == 0) {
        auto first_nl = trimmed.find('\n');
        if (first_nl != std::string::npos) {
            trimmed.erase(0, first_nl + 1);
        }
        if (trimmed.size() >= 3 &&
            trimmed.compare(trimmed.size() - 3, 3, "```") == 0) {
            trimmed.erase(trimmed.size() - 3);
        }
    }
    auto lo = trimmed.find('{');
    auto hi = trimmed.rfind('}');
    if (lo != std::string::npos && hi != std::string::npos && hi > lo) {
        return trimmed.substr(lo, hi - lo + 1);
    }
    return trimmed;
}

} // namespace

// ============================================================================
// Engine abstraction
// ============================================================================

class IIntentEngine {
public:
    virtual ~IIntentEngine() = default;

    virtual EngineType type() const = 0;
    virtual bool       isAvailable() const = 0;

    virtual bool init() = 0;
    virtual void shutdown() = 0;

    // Synchronous parse.  On success returns true and fills `result`.
    // On failure returns false and fills `error`.
    virtual bool parse(const IntentRequest& req,
                       IntentResult&        result,
                       IntentError&         error) = 0;
};

// ----------------------------------------------------------------------------
// Offline rule-based engine.  Uses std::regex to match against a list of
// configured patterns.  Each capture group is exposed as a slot.
// ----------------------------------------------------------------------------
class OfflineRuleEngine : public IIntentEngine {
public:
    OfflineRuleEngine(const OfflineIntentConfig& cfg, quill::Logger* logger)
        : cfg_(cfg), logger_(logger) {}

    ~OfflineRuleEngine() override { shutdown(); }

    EngineType type() const override { return EngineType::kOffline; }
    bool       isAvailable() const override { return available_; }

    bool init() override
    {
        if (!cfg_.enabled) {
            if (logger_) {
                LOG_INFO(logger_, "Offline intent engine disabled by configuration");
            }
            available_ = false;
            return false;
        }

        compiled_.clear();
        for (const auto& rule : cfg_.rules) {
            if (rule.intent_name.empty() || rule.pattern.empty()) {
                continue;
            }
            try {
                compiled_.push_back(CompiledRule{
                    rule.intent_name,
                    std::regex(rule.pattern,
                               std::regex::ECMAScript | std::regex::icase),
                    rule.slot_names,
                    rule.confidence,
                });
            } catch (const std::regex_error& e) {
                if (logger_) {
                    LOG_WARNING(logger_,
                                "Offline intent rule '{}' has invalid pattern '{}': {}",
                                rule.intent_name, rule.pattern, e.what());
                }
            }
        }

        available_ = !compiled_.empty();
        if (logger_) {
            LOG_INFO(logger_,
                     "Offline intent engine initialized: {} rules compiled",
                     compiled_.size());
        }
        // Even with zero compiled rules we report available so that the
        // fallback intent is always returnable; this matches the architecture
        // requirement of an always-on offline path.
        available_ = true;
        return true;
    }

    void shutdown() override
    {
        compiled_.clear();
        available_ = false;
    }

    bool parse(const IntentRequest& req,
               IntentResult&        result,
               IntentError&         error) override
    {
        for (const auto& rule : compiled_) {
            std::smatch m;
            if (std::regex_search(req.text, m, rule.regex)) {
                result.intent_name  = rule.intent_name;
                result.confidence   = rule.confidence;
                result.engine       = EngineType::kOffline;
                result.timestamp_us = nowMicros();
                result.slots.clear();
                for (std::size_t i = 1; i < m.size(); ++i) {
                    const std::size_t slot_idx = i - 1;
                    if (slot_idx < rule.slot_names.size() &&
                        !rule.slot_names[slot_idx].empty()) {
                        result.slots[rule.slot_names[slot_idx]] = m[i].str();
                    }
                }
                std::ostringstream os;
                os << "{\"intent\":\"" << jsonEscape(result.intent_name)
                   << "\",\"confidence\":" << result.confidence
                   << ",\"engine\":\"offline\"}";
                result.raw_response = os.str();
                if (cfg_.debug && logger_) {
                    LOG_DEBUG(logger_, "Offline matched intent='{}' on text='{}'",
                              result.intent_name, req.text);
                }
                return true;
            }
        }

        // No rule matched — return the configured fallback intent.
        result.intent_name  = cfg_.fallback_intent;
        result.confidence   = cfg_.fallback_confidence;
        result.engine       = EngineType::kOffline;
        result.timestamp_us = nowMicros();
        result.slots.clear();
        std::ostringstream os;
        os << "{\"intent\":\"" << jsonEscape(result.intent_name)
           << "\",\"confidence\":" << result.confidence
           << ",\"engine\":\"offline\",\"fallback\":true}";
        result.raw_response = os.str();
        (void)error;
        return true;
    }

private:
    struct CompiledRule {
        std::string              intent_name;
        std::regex               regex;
        std::vector<std::string> slot_names;
        float                    confidence;
    };

    OfflineIntentConfig         cfg_;
    quill::Logger*              logger_ = nullptr;
    std::vector<CompiledRule>   compiled_;
    bool                        available_ = false;
};

// ----------------------------------------------------------------------------
// Online HTTP engine.  POSTs JSON to the configured endpoint via ixwebsocket's
// HttpClient and parses an {intent, confidence, slots} reply.
// ----------------------------------------------------------------------------
class OnlineHttpEngine : public IIntentEngine {
public:
    OnlineHttpEngine(const OnlineIntentConfig& cfg, quill::Logger* logger)
        : cfg_(cfg), logger_(logger) {}

    ~OnlineHttpEngine() override { shutdown(); }

    EngineType type() const override { return EngineType::kOnline; }
    bool       isAvailable() const override { return available_; }

    bool init() override
    {
        if (!cfg_.enabled) {
            if (logger_) {
                LOG_INFO(logger_, "Online intent engine disabled by configuration");
            }
            available_ = false;
            return false;
        }
        if (cfg_.endpoint.empty()) {
            if (logger_) {
                LOG_WARNING(logger_,
                            "Online intent engine: endpoint is empty — disabling");
            }
            available_ = false;
            return false;
        }

        // Resolve the API key: prefer explicitly configured value, then env var.
        if (cfg_.api_key.empty()) {
            cfg_.api_key = envOr(cfg_.api_key_env, std::string());
        }

        ix::initNetSystem();
        client_ = std::make_unique<ix::HttpClient>(false);
        ix::SocketTLSOptions tls;
        tls.disable_hostname_validation = !cfg_.verify_ssl;
        client_->setTLSOptions(tls);

        available_ = true;
        if (logger_) {
            LOG_INFO(logger_,
                     "Online intent engine initialized: provider={}, "
                     "endpoint={}, model={}, has_key={}, json={}, "
                     "thinking={}, max_tokens={}, request_timeout_ms={}, "
                     "max_retries={}",
                     cfg_.provider.empty() ? "(unset)" : cfg_.provider,
                     cfg_.endpoint, cfg_.model, !cfg_.api_key.empty(),
                     cfg_.response_format_json,
                     cfg_.thinking_type.empty() ? "(omit)" : cfg_.thinking_type,
                     cfg_.max_tokens, cfg_.request_timeout_ms,
                     cfg_.max_retries);
        }
        return true;
    }

    void shutdown() override
    {
        client_.reset();
        available_ = false;
    }

    bool parse(const IntentRequest& req,
               IntentResult&        result,
               IntentError&         error) override
    {
        if (!available_ || !client_) {
            error.code   = IntentErrorCode::kEngineUnavailable;
            error.engine = EngineType::kOnline;
            error.message = "online engine not available";
            return false;
        }

        const std::string body = buildChatCompletionsBody(cfg_, req);

        auto args = client_->createRequest(cfg_.endpoint, ix::HttpClient::kPost);
        args->extraHeaders["Content-Type"] = "application/json";
        args->extraHeaders["Accept"]       = "application/json";
        if (!cfg_.api_key.empty() && !cfg_.auth_header.empty()) {
            std::string value = cfg_.auth_scheme.empty()
                                    ? cfg_.api_key
                                    : (cfg_.auth_scheme + " " + cfg_.api_key);
            args->extraHeaders[cfg_.auth_header] = value;
        }
        args->connectTimeout  = timeoutSecondsCeil(cfg_.connect_timeout_ms);
        args->transferTimeout = timeoutSecondsCeil(cfg_.request_timeout_ms);

        if (cfg_.debug && logger_) {
            LOG_DEBUG(logger_,
                      "Online intent POST {} connect_timeout_s={} "
                      "transfer_timeout_s={} body={}",
                      cfg_.endpoint, args->connectTimeout,
                      args->transferTimeout, body);
        }

        ix::HttpResponsePtr resp = client_->post(cfg_.endpoint, body, args);
        if (!resp) {
            error.code    = IntentErrorCode::kInternalError;
            error.engine  = EngineType::kOnline;
            error.message = "null HTTP response";
            return false;
        }
        if (resp->errorCode != ix::HttpErrorCode::Ok) {
            error.code    = (resp->errorCode == ix::HttpErrorCode::Timeout)
                                ? IntentErrorCode::kTimeout
                                : IntentErrorCode::kNetworkFailure;
            error.engine  = EngineType::kOnline;
            error.message = "HTTP error: " + resp->errorMsg;
            if (logger_) {
                LOG_WARNING(logger_, "Online intent HTTP failure: {} ({})",
                            resp->errorMsg, static_cast<int>(resp->errorCode));
            }
            return false;
        }
        if (resp->statusCode == 401 || resp->statusCode == 403) {
            error.code    = IntentErrorCode::kAuthFailure;
            error.engine  = EngineType::kOnline;
            error.message = "auth failure (status " + std::to_string(resp->statusCode) + ")";
            return false;
        }
        if (resp->statusCode < 200 || resp->statusCode >= 300) {
            error.code    = IntentErrorCode::kProtocolError;
            error.engine  = EngineType::kOnline;
            error.message = "non-2xx status " + std::to_string(resp->statusCode) +
                            ": " + resp->body;
            return false;
        }

        const std::string& payload = resp->body;

        std::string content;
        if (!extractJsonString(payload, "content", content)) {
            error.code    = IntentErrorCode::kProtocolError;
            error.engine  = EngineType::kOnline;
            error.message = "missing 'content' field in chat completions reply";
            if (logger_) {
                LOG_WARNING(logger_, "Online intent reply missing content: {}", payload);
            }
            return false;
        }

        const std::string inner = stripCodeFences(content);
        std::string intent_name;
        if (!extractJsonString(inner, "intent", intent_name) &&
            !extractJsonString(inner, "intent_name", intent_name)) {
            error.code    = IntentErrorCode::kProtocolError;
            error.engine  = EngineType::kOnline;
            error.message = "model output has no 'intent' field: " + inner;
            if (logger_) {
                LOG_WARNING(logger_, "Model intent JSON malformed: {}", inner);
            }
            return false;
        }

        double confidence = 0.0;
        extractJsonNumber(inner, "confidence", confidence);

        std::string slots_block;
        std::map<std::string, std::string> slots;
        if (extractObjectBlock(inner, "slots", slots_block)) {
            parseFlatStringMap(slots_block, slots);
        }

        result.intent_name  = std::move(intent_name);
        result.confidence   = static_cast<float>(confidence);
        result.slots        = std::move(slots);
        result.raw_response = inner;
        result.engine       = EngineType::kOnline;
        result.timestamp_us = nowMicros();
        return true;
    }

private:
    OnlineIntentConfig            cfg_;
    quill::Logger*                logger_ = nullptr;
    std::unique_ptr<ix::HttpClient> client_;
    bool                          available_ = false;
};

// ============================================================================
// IntentManager::Impl
// ============================================================================
struct IntentManager::Impl {
    IntentConfig cfg;

    quill::Logger* logger = nullptr;

    std::unique_ptr<OfflineRuleEngine> offline_engine;
    std::unique_ptr<OnlineHttpEngine>  online_engine;

    IntentCallback      result_cb;
    IntentErrorCallback error_cb;
    std::mutex          cb_mtx;

    std::atomic<bool>           initialized{false};
    std::atomic<bool>           force_offline{false};
    std::atomic<EngineType>     engine_pref{EngineType::kOnline};
    std::atomic<EngineType>     active_engine_type{EngineType::kOffline};

    network_manager::NetworkState last_net_state = network_manager::NetworkState::Offline;
    std::mutex                    state_mtx;

    // Async dispatch worker
    struct PendingRequest {
        IntentRequest  request;
        IntentCallback per_call_cb;
    };

    std::deque<PendingRequest>    queue;
    std::mutex                    queue_mtx;
    std::condition_variable       queue_cv;
    std::thread                   worker;
    std::atomic<bool>             worker_stop{false};

    void setupLogger()
    {
        quill::Backend::start();
        std::vector<std::shared_ptr<quill::Sink>> sinks;
        try {
            std::error_code ec;
            std::filesystem::create_directories(cfg.logging.log_dir, ec);
            const std::string full_path = cfg.logging.log_dir + "/" + cfg.logging.log_file;
            quill::RotatingFileSinkConfig rotation;
            rotation.set_rotation_max_file_size(cfg.logging.rotation_max_bytes);
            rotation.set_max_backup_files(static_cast<std::uint32_t>(
                std::max(1, cfg.logging.rotation_max_files)));
            rotation.set_open_mode('a');
            auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
                full_path, rotation);
            sinks.push_back(file_sink);
        } catch (...) {
            // fall back to console only
        }
        if (cfg.logging.console) {
            auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "intent_console_sink");
            sinks.push_back(console_sink);
        }
        if (sinks.empty()) {
            auto fallback = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "intent_fallback_sink");
            sinks.push_back(fallback);
        }
        logger = quill::Frontend::create_or_get_logger("IntentManager", std::move(sinks));
        logger->set_log_level(quill::LogLevel::Info);
    }

    bool initImpl(IntentConfig in_cfg)
    {
        if (initialized.load(std::memory_order_acquire)) {
            return true;
        }
        cfg = std::move(in_cfg);

        setupLogger();
        LOG_INFO(logger,
                 "IntentManager init: preferred={}, fallback_to_offline={}, queue_depth={}",
                 engineLabel(cfg.preferred_engine), cfg.fallback_to_offline,
                 cfg.request_queue_depth);

        engine_pref.store(cfg.preferred_engine, std::memory_order_release);

        // Install the config-supplied unknown-reply phrases so callers of the
        // free-function pickUnknownReplyPhrase() pick from this pool.  Empty
        // input leaves the built-in defaults in place (handled in the setter).
        setUnknownReplyPhrases(cfg.unknown_reply.phrases);

        offline_engine = std::make_unique<OfflineRuleEngine>(cfg.offline, logger);
        online_engine  = std::make_unique<OnlineHttpEngine>(cfg.online, logger);

        const bool offline_ok = offline_engine->init();
        const bool online_ok  = online_engine->init();
        if (!offline_ok && !online_ok) {
            LOG_ERROR(logger,
                      "IntentManager init: neither online nor offline engine is available");
        }

        active_engine_type.store(currentSelectedType(), std::memory_order_release);

        worker_stop.store(false, std::memory_order_release);
        worker = std::thread(&Impl::workerLoop, this);

        initialized.store(true, std::memory_order_release);
        LOG_INFO(logger, "IntentManager initialized (active engine: {})",
                 engineLabel(active_engine_type.load(std::memory_order_acquire)));
        return true;
    }

    EngineType currentSelectedType() const
    {
        const bool prefer_online =
            engine_pref.load(std::memory_order_acquire) == EngineType::kOnline;
        const bool forced_offline = force_offline.load(std::memory_order_acquire);

        if (!forced_offline && prefer_online && online_engine && online_engine->isAvailable()) {
            return EngineType::kOnline;
        }
        if (offline_engine && offline_engine->isAvailable()) {
            return EngineType::kOffline;
        }
        if (online_engine && online_engine->isAvailable()) {
            return EngineType::kOnline;
        }
        return EngineType::kOffline;
    }

    IIntentEngine* selectEngine()
    {
        const bool prefer_online =
            engine_pref.load(std::memory_order_acquire) == EngineType::kOnline;
        const bool forced_offline = force_offline.load(std::memory_order_acquire);

        if (!forced_offline && prefer_online && online_engine && online_engine->isAvailable()) {
            return online_engine.get();
        }
        if (offline_engine && offline_engine->isAvailable()) {
            return offline_engine.get();
        }
        if (online_engine && online_engine->isAvailable()) {
            return online_engine.get();
        }
        return nullptr;
    }

    void applyConfirmationPolicy(IntentResult& result) const
    {
        result.requires_confirmation = false;
        if (!cfg.confirmation.enabled) {
            return;
        }
        for (const auto& rule : cfg.confirmation.rules) {
            if (rule.intent_name != result.intent_name) {
                continue;
            }
            for (const auto& kv : rule.exempt_when_slot_equals) {
                auto it = result.slots.find(kv.first);
                if (it == result.slots.end()) {
                    continue;
                }
                for (const auto& allowed : kv.second) {
                    if (it->second == allowed) {
                        return;
                    }
                }
            }
            result.requires_confirmation = true;
            return;
        }
    }

    void normalizeSubjectIntent(const IntentRequest& req, IntentResult& result) const
    {
        const bool mentions_arm =
            containsAny(req.text, {"机械臂", "手臂", "胳膊"});
        if (!mentions_arm || result.intent_name != "wheelchair.move") {
            return;
        }

        const std::string action = armActionFromText(req.text);
        result.intent_name = "arm.control";
        if (!action.empty()) {
            result.slots["action"] = action;
        }
        if (result.slots.find("action_hint") == result.slots.end()) {
            result.slots["action_hint"] = req.text;
        }
        result.confidence = std::max(result.confidence, 0.93f);
        result.raw_response += "\n{\"normalized\":\"arm_subject_over_wheelchair\"}";
        if (logger) {
            LOG_INFO(logger,
                     "Intent normalized: arm subject overrides wheelchair.move for text='{}'",
                     req.text);
        }
    }

    void shutdownImpl()
    {
        if (!initialized.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        worker_stop.store(true, std::memory_order_release);
        queue_cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            queue.clear();
        }
        if (offline_engine) offline_engine->shutdown();
        if (online_engine)  online_engine->shutdown();
        offline_engine.reset();
        online_engine.reset();
        if (logger) {
            LOG_INFO(logger, "IntentManager shutdown complete");
            logger->flush_log();
        }
    }

    bool runOnce(const IntentRequest& req,
                 IntentResult&        result,
                 IntentError&         error)
    {
        if (req.text.empty()) {
            error.code    = IntentErrorCode::kInvalidRequest;
            error.engine  = active_engine_type.load(std::memory_order_acquire);
            error.message = "empty intent text";
            return false;
        }

        IIntentEngine* eng = selectEngine();
        if (!eng) {
            error.code    = IntentErrorCode::kEngineUnavailable;
            error.engine  = active_engine_type.load(std::memory_order_acquire);
            error.message = "no intent engine available";
            return false;
        }
        active_engine_type.store(eng->type(), std::memory_order_release);

        // Online engine: try up to `cfg.online.max_retries` times with a small
        // backoff between attempts.  Only after every retry has failed do we
        // fall through to the offline branch below.  Offline engine isn't
        // retried — it's deterministic regex matching, retrying buys nothing.
        bool first_ok = false;
        if (eng->type() == EngineType::kOnline) {
            const int max_attempts = std::max(1, cfg.online.max_retries);
            for (int attempt = 1; attempt <= max_attempts; ++attempt) {
                IntentError attempt_error{};
                if (eng->parse(req, result, attempt_error)) {
                    first_ok = true;
                    break;
                }
                error = attempt_error;
                if (logger) {
                    LOG_WARNING(logger,
                                "Online intent attempt {}/{} failed: {}",
                                attempt, max_attempts, error.message);
                }
                if (attempt < max_attempts && cfg.online.retry_backoff_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(cfg.online.retry_backoff_ms));
                }
            }
        } else {
            first_ok = eng->parse(req, result, error);
        }

        if (first_ok) {
            normalizeSubjectIntent(req, result);
            applyConfirmationPolicy(result);
            return true;
        }

        // Fallback to the other engine when allowed.
        if (cfg.fallback_to_offline && eng->type() == EngineType::kOnline &&
            offline_engine && offline_engine->isAvailable()) {
            if (logger) {
                LOG_INFO(logger,
                         "Online intent exhausted retries ({}); falling back to offline",
                         error.message);
            }
            IntentError offline_error{};
            if (offline_engine->parse(req, result, offline_error)) {
                active_engine_type.store(EngineType::kOffline,
                                         std::memory_order_release);
                normalizeSubjectIntent(req, result);
                applyConfirmationPolicy(result);
                return true;
            }
            error = offline_error;
        }
        return false;
    }

    void deliver(const IntentRequest& /*req*/,
                 const IntentResult&  result,
                 const IntentCallback& per_call_cb)
    {
        IntentCallback cb;
        if (per_call_cb) {
            cb = per_call_cb;
        } else {
            std::lock_guard<std::mutex> lk(cb_mtx);
            cb = result_cb;
        }
        if (cb) {
            try {
                cb(result);
            } catch (const std::exception& e) {
                if (logger) {
                    LOG_WARNING(logger,
                                "Intent result callback threw: {}", e.what());
                }
            } catch (...) {
                if (logger) {
                    LOG_WARNING(logger, "Intent result callback threw unknown exception");
                }
            }
        }
    }

    void deliverError(const IntentError& err)
    {
        IntentErrorCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx);
            cb = error_cb;
        }
        if (cb) {
            try {
                cb(err);
            } catch (...) {
                if (logger) {
                    LOG_WARNING(logger, "Intent error callback threw");
                }
            }
        }
    }

    bool enqueue(IntentRequest request, IntentCallback per_call_cb)
    {
        if (!initialized.load(std::memory_order_acquire)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            if (queue.size() >= cfg.request_queue_depth) {
                if (logger) {
                    LOG_WARNING(logger,
                                "Intent request queue full ({}), dropping",
                                queue.size());
                }
                return false;
            }
            queue.push_back(PendingRequest{std::move(request), std::move(per_call_cb)});
        }
        queue_cv.notify_one();
        return true;
    }

    void workerLoop()
    {
        while (true) {
            PendingRequest pending;
            {
                std::unique_lock<std::mutex> lk(queue_mtx);
                queue_cv.wait(lk, [this] {
                    return worker_stop.load(std::memory_order_acquire) || !queue.empty();
                });
                if (worker_stop.load(std::memory_order_acquire) && queue.empty()) {
                    return;
                }
                pending = std::move(queue.front());
                queue.pop_front();
            }

            IntentResult result{};
            IntentError  error{};
            const bool ok = runOnce(pending.request, result, error);
            if (ok) {
                deliver(pending.request, result, pending.per_call_cb);
            } else {
                deliverError(error);
            }
        }
    }

    void onNetworkChangedImpl(network_manager::NetworkState s)
    {
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            last_net_state = s;
        }
        const bool was_offline = force_offline.load(std::memory_order_acquire);
        const bool should_force_offline = (s == network_manager::NetworkState::Offline);
        if (should_force_offline == was_offline) {
            return;
        }
        force_offline.store(should_force_offline, std::memory_order_release);
        if (!logger) return;
        LOG_INFO(logger,
                 "Network state changed to {} → preferred intent engine switching",
                 network_manager::NetworkManager::toString(s));

        active_engine_type.store(currentSelectedType(), std::memory_order_release);
        LOG_INFO(logger, "Active intent engine pre-selected: {}",
                 engineLabel(active_engine_type.load(std::memory_order_acquire)));
    }
};

// ============================================================================
// IntentManager facade
// ============================================================================
IntentManager::IntentManager() : impl_(std::make_unique<Impl>()) {}
IntentManager::~IntentManager() { shutdown(); }

bool IntentManager::init(const std::string& config_path)
{
    try {
        return impl_->initImpl(loadConfig(config_path));
    } catch (const std::exception& e) {
        if (impl_->logger) {
            LOG_ERROR(impl_->logger, "init failed: {}", e.what());
        }
        return false;
    }
}

bool IntentManager::init(const IntentConfig& cfg)
{
    return impl_->initImpl(cfg);
}

void IntentManager::shutdown()
{
    if (impl_) impl_->shutdownImpl();
}

bool IntentManager::recognize(const IntentRequest& request,
                              IntentResult&        out_result,
                              IntentError*         out_error)
{
    if (!impl_->initialized.load(std::memory_order_acquire)) {
        if (out_error) {
            out_error->code    = IntentErrorCode::kNotInitialized;
            out_error->message = "IntentManager not initialized";
        }
        return false;
    }
    IntentError local_err{};
    const bool ok = impl_->runOnce(request, out_result, local_err);
    if (!ok) {
        if (out_error) *out_error = local_err;
        impl_->deliverError(local_err);
    }
    return ok;
}

bool IntentManager::recognizeAsync(const IntentRequest& request,
                                   IntentCallback       per_call_callback)
{
    return impl_->enqueue(request, std::move(per_call_callback));
}

void IntentManager::setResultCallback(IntentCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->result_cb = std::move(cb);
}

void IntentManager::setErrorCallback(IntentErrorCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mtx);
    impl_->error_cb = std::move(cb);
}

EngineType IntentManager::currentEngine() const
{
    return impl_->active_engine_type.load(std::memory_order_acquire);
}

void IntentManager::onNetworkStateChanged(network_manager::NetworkState s)
{
    impl_->onNetworkChangedImpl(s);
}

bool IntentManager::isOnlineAvailable() const
{
    return impl_->online_engine && impl_->online_engine->isAvailable() &&
           !impl_->force_offline.load(std::memory_order_acquire);
}

bool IntentManager::isInitialized() const
{
    return impl_->initialized.load(std::memory_order_acquire);
}

const IntentConfig& IntentManager::config() const
{
    return impl_->cfg;
}

// ============================================================================
// TOML loader
// ============================================================================
IntentConfig IntentManager::loadConfig(const std::string& config_path)
{
    IntentConfig cfg;
    toml::table table = toml::parse_file(config_path);
    const toml::node_view in = table["intent"];

    cfg.request_queue_depth = static_cast<std::size_t>(
        in["request_queue_depth"].value_or(static_cast<int64_t>(cfg.request_queue_depth)));
    cfg.request_timeout_ms  = in["request_timeout_ms"].value_or(cfg.request_timeout_ms);
    cfg.fallback_to_offline = in["fallback_to_offline"].value_or(cfg.fallback_to_offline);

    const std::string pref_str = in["preferred_engine"].value_or(std::string("online"));
    cfg.preferred_engine = (pref_str == "offline") ? EngineType::kOffline : EngineType::kOnline;

    // -- offline ------------------------------------------------------------
    const toml::node_view off = in["offline"];
    cfg.offline.enabled             = off["enabled"].value_or(cfg.offline.enabled);
    cfg.offline.fallback_intent     = off["fallback_intent"].value_or(cfg.offline.fallback_intent);
    cfg.offline.fallback_confidence = static_cast<float>(
        off["fallback_confidence"].value_or(static_cast<double>(cfg.offline.fallback_confidence)));
    cfg.offline.debug               = off["debug"].value_or(cfg.offline.debug);

    cfg.offline.rules.clear();
    if (auto rules = off["rules"].as_array()) {
        for (auto& node : *rules) {
            if (auto entry = node.as_table()) {
                OfflineIntentRule rule;
                rule.intent_name = (*entry)["intent"].value_or(std::string());
                if (rule.intent_name.empty()) {
                    rule.intent_name = (*entry)["name"].value_or(std::string());
                }
                rule.pattern    = (*entry)["pattern"].value_or(std::string());
                rule.confidence = static_cast<float>(
                    (*entry)["confidence"].value_or(static_cast<double>(rule.confidence)));
                if (auto slots = (*entry)["slots"].as_array()) {
                    for (auto& s : *slots) {
                        if (auto str = s.value<std::string>()) {
                            rule.slot_names.push_back(*str);
                        }
                    }
                }
                if (!rule.intent_name.empty() && !rule.pattern.empty()) {
                    cfg.offline.rules.push_back(std::move(rule));
                }
            }
        }
    }

    // -- online -------------------------------------------------------------
    // Step 1: load the base [intent.online] keys (these act as defaults).
    // Step 2: if [intent.online].provider is set, look up
    //         [intent.online.providers.<provider>] and override the
    //         provider-specific keys (endpoint / model / api_key /
    //         api_key_env / auth_header / auth_scheme).  This lets the user
    //         flip provider with a single line in config.toml, e.g.
    //             [intent.online]
    //             provider = "deepseek"     # or "doubao"
    const toml::node_view on = in["online"];
    cfg.online.enabled            = on["enabled"].value_or(cfg.online.enabled);
    cfg.online.provider           = on["provider"].value_or(cfg.online.provider);
    cfg.online.endpoint           = on["endpoint"].value_or(cfg.online.endpoint);
    cfg.online.api_key            = on["api_key"].value_or(cfg.online.api_key);
    cfg.online.api_key_env        = on["api_key_env"].value_or(cfg.online.api_key_env);
    cfg.online.auth_header        = on["auth_header"].value_or(cfg.online.auth_header);
    cfg.online.auth_scheme        = on["auth_scheme"].value_or(cfg.online.auth_scheme);
    cfg.online.model              = on["model"].value_or(cfg.online.model);
    cfg.online.system_prompt      = on["system_prompt"].value_or(cfg.online.system_prompt);
    cfg.online.temperature        = on["temperature"].value_or(cfg.online.temperature);
    cfg.online.max_tokens         = on["max_tokens"].value_or(cfg.online.max_tokens);
    cfg.online.response_format_json =
        on["response_format_json"].value_or(cfg.online.response_format_json);
    cfg.online.thinking_type =
        on["thinking_type"].value_or(cfg.online.thinking_type);
    cfg.online.reasoning_effort =
        on["reasoning_effort"].value_or(cfg.online.reasoning_effort);
    cfg.online.include_context =
        on["include_context"].value_or(cfg.online.include_context);
    cfg.online.connect_timeout_ms = on["connect_timeout_ms"].value_or(cfg.online.connect_timeout_ms);
    cfg.online.request_timeout_ms = on["request_timeout_ms"].value_or(cfg.online.request_timeout_ms);
    cfg.online.max_retries        = on["max_retries"].value_or(cfg.online.max_retries);
    cfg.online.retry_backoff_ms   = on["retry_backoff_ms"].value_or(cfg.online.retry_backoff_ms);
    cfg.online.verify_ssl         = on["verify_ssl"].value_or(cfg.online.verify_ssl);
    cfg.online.debug              = on["debug"].value_or(cfg.online.debug);

    if (!cfg.online.provider.empty()) {
        const toml::node_view prov =
            on["providers"][cfg.online.provider];
        if (prov.is_table()) {
            cfg.online.endpoint     = prov["endpoint"].value_or(cfg.online.endpoint);
            cfg.online.api_key      = prov["api_key"].value_or(cfg.online.api_key);
            cfg.online.api_key_env  = prov["api_key_env"].value_or(cfg.online.api_key_env);
            cfg.online.auth_header  = prov["auth_header"].value_or(cfg.online.auth_header);
            cfg.online.auth_scheme  = prov["auth_scheme"].value_or(cfg.online.auth_scheme);
            cfg.online.model        = prov["model"].value_or(cfg.online.model);
            cfg.online.system_prompt =
                prov["system_prompt"].value_or(cfg.online.system_prompt);
            cfg.online.temperature =
                prov["temperature"].value_or(cfg.online.temperature);
            cfg.online.max_tokens =
                prov["max_tokens"].value_or(cfg.online.max_tokens);
            cfg.online.response_format_json =
                prov["response_format_json"].value_or(cfg.online.response_format_json);
            cfg.online.thinking_type =
                prov["thinking_type"].value_or(cfg.online.thinking_type);
            cfg.online.reasoning_effort =
                prov["reasoning_effort"].value_or(cfg.online.reasoning_effort);
            cfg.online.include_context =
                prov["include_context"].value_or(cfg.online.include_context);
            cfg.online.connect_timeout_ms =
                prov["connect_timeout_ms"].value_or(cfg.online.connect_timeout_ms);
            cfg.online.request_timeout_ms =
                prov["request_timeout_ms"].value_or(cfg.online.request_timeout_ms);
            cfg.online.max_retries =
                prov["max_retries"].value_or(cfg.online.max_retries);
            cfg.online.retry_backoff_ms =
                prov["retry_backoff_ms"].value_or(cfg.online.retry_backoff_ms);
            cfg.online.verify_ssl =
                prov["verify_ssl"].value_or(cfg.online.verify_ssl);
            cfg.online.debug =
                prov["debug"].value_or(cfg.online.debug);
        }
    }

    // -- confirmation -------------------------------------------------------
    const toml::node_view conf = in["confirmation"];
    cfg.confirmation.enabled = conf["enabled"].value_or(cfg.confirmation.enabled);
    cfg.confirmation.rules.clear();
    if (auto rules = conf["rules"].as_array()) {
        for (auto& node : *rules) {
            if (auto entry = node.as_table()) {
                ConfirmationRule rule;
                rule.intent_name = (*entry)["intent"].value_or(std::string());
                if (rule.intent_name.empty()) {
                    continue;
                }
                if (auto exempt = (*entry)["exempt_when_slot_equals"].as_table()) {
                    for (auto&& [key, node2] : *exempt) {
                        std::vector<std::string> values;
                        if (auto arr = node2.as_array()) {
                            for (auto& v : *arr) {
                                if (auto s = v.value<std::string>()) {
                                    values.push_back(*s);
                                }
                            }
                        } else if (auto s = node2.value<std::string>()) {
                            values.push_back(*s);
                        }
                        if (!values.empty()) {
                            rule.exempt_when_slot_equals.emplace(
                                std::string(key.str()), std::move(values));
                        }
                    }
                }
                cfg.confirmation.rules.push_back(std::move(rule));
            }
        }
    }

    // -- unknown_reply ------------------------------------------------------
    const toml::node_view unk = in["unknown_reply"];
    cfg.unknown_reply.phrases.clear();
    if (auto arr = unk["phrases"].as_array()) {
        for (const auto& node : *arr) {
            if (auto s = node.value<std::string>()) {
                if (!s->empty()) {
                    cfg.unknown_reply.phrases.push_back(*s);
                }
            }
        }
    }

    // -- logging ------------------------------------------------------------
    const toml::node_view log = in["logging"];
    cfg.logging.log_dir            = log["log_dir"].value_or(cfg.logging.log_dir);
    cfg.logging.log_file           = log["log_file"].value_or(cfg.logging.log_file);
    cfg.logging.console            = log["console"].value_or(cfg.logging.console);
    cfg.logging.rotation_max_bytes = static_cast<std::size_t>(
        log["rotation_max_bytes"].value_or(static_cast<int64_t>(cfg.logging.rotation_max_bytes)));
    cfg.logging.rotation_max_files = log["rotation_max_files"].value_or(cfg.logging.rotation_max_files);

    return cfg;
}

// ============================================================================
// Fallback UX helpers (declared in IntentManager.h)
// ============================================================================

namespace {
// Built-in default pool, used when the config didn't supply phrases (yet).
// Kept short so the assistant never goes silent on an unknown intent.
const std::vector<std::string>& defaultUnknownReplyPhrases()
{
    static const std::vector<std::string> kDefaults = {
        "抱歉，我没听懂您的意思。",
        "这个我暂时还不会，可以换个说法吗？",
        "我没明白，请再说一次。",
    };
    return kDefaults;
}

std::mutex& unknownReplyMutex()
{
    static std::mutex m;
    return m;
}

std::vector<std::string>& unknownReplyPhrases()
{
    static std::vector<std::string> phrases;
    return phrases;
}
} // namespace

void setUnknownReplyPhrases(const std::vector<std::string>& phrases)
{
    if (phrases.empty()) {
        return; // ignore — keep whatever is currently installed
    }
    std::lock_guard<std::mutex> lk(unknownReplyMutex());
    unknownReplyPhrases() = phrases;
}

const std::string& pickUnknownReplyPhrase()
{
    static std::mt19937 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lk(unknownReplyMutex());
    const auto& pool = unknownReplyPhrases().empty()
                           ? defaultUnknownReplyPhrases()
                           : unknownReplyPhrases();
    std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
    return pool[dist(rng)];
}

bool isSupportedControlIntent(const std::string& intent_name)
{
    return intent_name == "wheelchair.move"     ||
           intent_name == "arm.control"         ||
           intent_name == "gripper.control"     ||
           intent_name == "music.control"       ||
           intent_name == "volume.control"      ||
           intent_name == "navigation.navigate";
}

} // namespace intent
