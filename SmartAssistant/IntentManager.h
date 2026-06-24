#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "NetworkManager.h"

namespace intent {

// ============================================================================
// Public enums
// ============================================================================

enum class EngineType {
    kOffline = 0,
    kOnline  = 1,
};

enum class IntentErrorCode {
    kOk                = 0,
    kNotInitialized    = 1,
    kEngineUnavailable = 2,
    kNetworkFailure    = 3,
    kProtocolError     = 4,
    kAuthFailure       = 5,
    kTimeout           = 6,
    kInvalidRequest    = 7,
    kInternalError     = 99,
};

// ============================================================================
// Public request / result / event types
// ============================================================================

// Caller-supplied context.  Per architecture, multi-turn slot tracking is
// owned by SessionManager — IntentManager treats `context_slots` and
// `previous_intent` as read-only hints to forward to the engine.
struct IntentRequest {
    std::string                        text;
    std::string                        user_id;
    std::string                        session_id;
    std::map<std::string, std::string> context_slots;
    std::string                        previous_intent;
};

struct IntentResult {
    std::string                        intent_name;
    std::map<std::string, std::string> slots;
    float                              confidence   = 0.0f;
    std::string                        raw_response;
    EngineType                         engine       = EngineType::kOffline;
    std::int64_t                       timestamp_us = 0;
    // Set by IntentManager from [intent.confirmation]; downstream
    // (SessionManager) must re-prompt the user before executing.
    bool                               requires_confirmation = false;
};

struct IntentError {
    IntentErrorCode code = IntentErrorCode::kOk;
    std::string     message;
    EngineType      engine = EngineType::kOffline;
};

using IntentCallback      = std::function<void(const IntentResult&)>;
using IntentErrorCallback = std::function<void(const IntentError&)>;

// ============================================================================
// Configuration (parsed from TOML [intent.*])
// ============================================================================

// Offline rule entry: a regex matched against the input plus a list of slot
// names corresponding to its capture groups.
struct OfflineIntentRule {
    std::string              intent_name;
    std::string              pattern;       // ECMAScript regex, case-insensitive
    std::vector<std::string> slot_names;    // names for capture groups, in order
    float                    confidence = 0.85f;
};

struct OfflineIntentConfig {
    bool                            enabled            = true;
    std::vector<OfflineIntentRule>  rules;
    std::string                     fallback_intent    = "unknown";
    float                           fallback_confidence = 0.0f;
    bool                            debug              = false;
};

// Online engine: LLM-driven NLU via an OpenAI-compatible chat completions
// endpoint.  Provider-agnostic — works with any service that speaks the
// OpenAI ChatCompletions wire format (Volcengine Ark / Doubao, DeepSeek,
// Moonshot, etc.).  The `provider` selector picks a named preset block
// from TOML; see [intent.online.providers.*] in Config/config.toml.
//
// The model is asked to return a JSON object
// {"intent":"...","confidence":0.x,"slots":{...}} as its message content;
// surrounding prose / markdown fences are tolerated.
struct OnlineIntentConfig {
    bool        enabled            = true;
    std::string provider;                       // "doubao" | "deepseek" | ...
    std::string endpoint           = "https://ark.cn-beijing.volces.com/api/v3/chat/completions";
    std::string api_key;
    std::string api_key_env        = "ARK_API_KEY";
    std::string auth_header        = "Authorization";
    std::string auth_scheme        = "Bearer";
    std::string model              = "doubao-seed-2-0-mini-260428";
    std::string system_prompt;
    double      temperature        = 0.1;
    int         max_tokens         = 256;
    bool        response_format_json = false;
    std::string thinking_type;                 // DeepSeek: "enabled" | "disabled"; empty = omit
    std::string reasoning_effort;              // DeepSeek thinking mode: "high" | "max"; empty = omit
    bool        include_context     = false;   // include previous_intent/context_slots in prompt
    int         connect_timeout_ms = 3000;
    int         request_timeout_ms = 8000;
    int         max_retries        = 3;     // total online attempts before falling back to offline
    int         retry_backoff_ms   = 200;   // delay between retries (ms)
    bool        verify_ssl         = true;
    bool        debug              = false;
};

struct IntentLoggingConfig {
    std::string log_dir            = "logs";
    std::string log_file           = "intent.log";
    bool        console            = true;
    std::size_t rotation_max_bytes = 10 * 1024 * 1024;
    int         rotation_max_files = 7;
};

// Per-intent confirmation rule.  An intent listed here is flagged with
// requires_confirmation=true unless one of `exempt_when_slot_equals` matches
// the resolved slot value (e.g. wheelchair.move with action=stop is allowed
// to execute immediately as an emergency-stop short-circuit).
struct ConfirmationRule {
    std::string                                  intent_name;
    std::map<std::string, std::vector<std::string>> exempt_when_slot_equals;
};

struct ConfirmationPolicy {
    bool                          enabled = true;
    std::vector<ConfirmationRule> rules;
};

// Phrases the upper layer reads via pickUnknownReplyPhrase() when the
// recognized intent is neither one of the supported control intents nor
// the explicit `chat` entry.  Loaded from [intent.unknown_reply].phrases.
struct UnknownReplyConfig {
    std::vector<std::string> phrases;
};

struct IntentConfig {
    OfflineIntentConfig  offline;
    OnlineIntentConfig   online;
    IntentLoggingConfig  logging;
    ConfirmationPolicy   confirmation;
    UnknownReplyConfig   unknown_reply;
    EngineType           preferred_engine    = EngineType::kOnline;
    bool                 fallback_to_offline = true;
    std::size_t          request_queue_depth = 64;
    int                  request_timeout_ms  = 5000;
};

// ============================================================================
// IEngineSwitch (shared by ASR / Intent / TTS managers — declared per-namespace
// so each module can be linked independently)
// ============================================================================

class IEngineSwitch {
public:
    virtual ~IEngineSwitch() = default;

    virtual EngineType currentEngine() const                                 = 0;
    virtual void       onNetworkStateChanged(network_manager::NetworkState)  = 0;
    virtual bool       isOnlineAvailable() const                             = 0;
};

// ============================================================================
// IntentManager
// ============================================================================

class IntentManager : public IEngineSwitch {
public:
    IntentManager();
    ~IntentManager() override;

    IntentManager(const IntentManager&)            = delete;
    IntentManager& operator=(const IntentManager&) = delete;

    // Two-phase init.  IntentManager has no AudioManager dependency.
    bool init(const std::string& config_path = "Config/config.toml");
    bool init(const IntentConfig& cfg);

    void shutdown();

    // Recognition lifecycle
    //
    // Synchronous:  recognize() returns the result directly.  Errors set
    // `out_error` (when non-null) and the function returns false.
    //
    // Asynchronous: recognizeAsync() enqueues the request and the per-call
    // callback (or the manager-wide callback) is invoked from the worker
    // thread.  Returns false if the queue is full or the manager is not
    // initialized.
    bool recognize(const IntentRequest& request,
                   IntentResult&        out_result,
                   IntentError*         out_error = nullptr);

    bool recognizeAsync(const IntentRequest& request,
                        IntentCallback       per_call_callback = {});

    // Manager-wide callbacks (invoked when no per-call callback is supplied).
    void setResultCallback(IntentCallback cb);
    void setErrorCallback(IntentErrorCallback cb);

    // IEngineSwitch
    EngineType currentEngine() const override;
    void       onNetworkStateChanged(network_manager::NetworkState s) override;
    bool       isOnlineAvailable() const override;

    // Introspection
    bool                isInitialized() const;
    const IntentConfig& config()        const;

    // Standalone TOML loader (also used internally by init(path)).
    static IntentConfig loadConfig(const std::string& config_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Fallback UX helpers
//
// When ASR text does not map to any supported control intent and is not an
// explicit "enter AI chat" command, callers should reply with a randomized
// "I don't understand" phrase rather than silently entering chat mode.  The
// phrase pool is loaded from [intent.unknown_reply].phrases and installed
// here by IntentManager::init() so all callers share the same wording.
// ============================================================================

// Install the active phrase pool for pickUnknownReplyPhrase().  Empty input
// keeps whatever was previously installed (or the built-in default pool if
// nothing has been installed yet).  Thread-safe.  Called automatically by
// IntentManager::init() from the loaded config.
void setUnknownReplyPhrases(const std::vector<std::string>& phrases);

// Returns a reference to a randomly-picked phrase from the active pool.
// If setUnknownReplyPhrases() was never called (or called with an empty
// list), falls back to a small built-in default pool so callers always get
// something speakable.  Thread-safe.
const std::string& pickUnknownReplyPhrase();

// True iff `intent_name` is one of the six business intents the assistant
// can actually execute (wheelchair / arm / gripper / music / volume /
// navigation).  `chat` is intentionally excluded — it is a separate mode,
// not a control.
bool isSupportedControlIntent(const std::string& intent_name);

} // namespace intent
