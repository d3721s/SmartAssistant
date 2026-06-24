#include "ASRManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <sherpa-onnx/c-api/c-api.h>

#include <zlib.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessageType.h>

#include "AudioManager.h"

namespace asr {

// ============================================================================
// Helpers
// ============================================================================
namespace {

constexpr float kInt16ToFloat = 1.0f / 32768.0f;

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

// Convert interleaved int16 PCM to mono float32 by channel averaging.
std::vector<float> toMonoFloat(const std::vector<std::int16_t>& interleaved, int channels)
{
    if (channels <= 0) {
        channels = 1;
    }
    const std::size_t frames = interleaved.size() / static_cast<std::size_t>(channels);
    std::vector<float> out(frames, 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        int sum = 0;
        for (int c = 0; c < channels; ++c) {
            sum += interleaved[i * channels + c];
        }
        out[i] = (static_cast<float>(sum) / channels) * kInt16ToFloat;
    }
    return out;
}

// Convert interleaved int16 PCM to mono int16 by channel averaging.
std::vector<std::int16_t> toMonoInt16(const std::vector<std::int16_t>& interleaved, int channels)
{
    if (channels <= 0) {
        channels = 1;
    }
    const std::size_t frames = interleaved.size() / static_cast<std::size_t>(channels);
    std::vector<std::int16_t> out(frames, 0);
    for (std::size_t i = 0; i < frames; ++i) {
        int sum = 0;
        for (int c = 0; c < channels; ++c) {
            sum += interleaved[i * channels + c];
        }
        out[i] = static_cast<std::int16_t>(sum / channels);
    }
    return out;
}

// Tiny gzip compressor backed by zlib. Doubao expects gzip wrapping.
bool gzipCompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out)
{
    out.clear();
    static const std::uint8_t kEmpty = 0;
    if (!data && size == 0) {
        data = &kEmpty;
    }
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    stream.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(size);

    std::vector<std::uint8_t> buffer(std::max<std::size_t>(64, size + 64));
    int code = Z_OK;
    do {
        if (stream.total_out >= buffer.size()) {
            buffer.resize(buffer.size() * 2);
        }
        stream.next_out  = buffer.data() + stream.total_out;
        stream.avail_out = static_cast<uInt>(buffer.size() - stream.total_out);
        code = deflate(&stream, Z_FINISH);
    } while (code == Z_OK);

    if (code != Z_STREAM_END) {
        deflateEnd(&stream);
        return false;
    }
    out.assign(buffer.begin(), buffer.begin() + stream.total_out);
    deflateEnd(&stream);
    return true;
}

// gzip decompress (used for server replies the protocol marks as gzip-compressed).
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
    stream.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(size);

    std::vector<std::uint8_t> buffer(std::max<std::size_t>(128, size * 4));
    int code = Z_OK;
    do {
        if (stream.total_out >= buffer.size()) {
            buffer.resize(buffer.size() * 2);
        }
        stream.next_out  = buffer.data() + stream.total_out;
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

// Minimal JSON string extractor (no need to depend on a full parser when we only
// need a handful of keys from the Doubao response). Returns the value for a given
// "key":"..." pair if it appears in the buffer.
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
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;  // skip escaped character (basic handling)
        }
        out.push_back(json[pos]);
        ++pos;
    }
    return true;
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
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) {
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

bool extractJsonInt64(const std::string& json, const std::string& key, std::int64_t& out)
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
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) {
        ++pos;
    }
    bool negative = false;
    if (pos < json.size() && json[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') {
        return false;
    }
    std::int64_t value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        value = value * 10 + (json[pos] - '0');
        ++pos;
    }
    out = negative ? -value : value;
    return true;
}

bool extractLastJsonInt64(const std::string& json, const std::string& key, std::int64_t& out)
{
    const std::string needle = "\"" + key + "\"";
    auto pos = json.rfind(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) {
        ++pos;
    }
    bool negative = false;
    if (pos < json.size() && json[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') {
        return false;
    }
    std::int64_t value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        value = value * 10 + (json[pos] - '0');
        ++pos;
    }
    out = negative ? -value : value;
    return true;
}

struct JsonUtterance {
    std::string  text;
    std::int64_t start_time = -1;
    std::int64_t end_time   = -1;
    bool         definite   = false;
};

std::vector<JsonUtterance> extractJsonUtterances(const std::string& json)
{
    std::vector<JsonUtterance> out;
    const std::string needle = "\"utterances\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return out;
    }
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) {
        return out;
    }

    bool in_string = false;
    bool escaped = false;
    int array_depth = 1;
    int object_depth = 0;
    std::size_t object_start = std::string::npos;

    for (++pos; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '[') {
            ++array_depth;
            continue;
        }
        if (ch == ']') {
            --array_depth;
            if (array_depth == 0) {
                break;
            }
            continue;
        }
        if (ch == '{') {
            if (array_depth == 1 && object_depth == 0) {
                object_start = pos;
            }
            ++object_depth;
            continue;
        }
        if (ch == '}') {
            if (object_depth <= 0) {
                continue;
            }
            --object_depth;
            if (array_depth == 1 && object_depth == 0 &&
                object_start != std::string::npos) {
                const std::string obj = json.substr(object_start, pos - object_start + 1);
                JsonUtterance u;
                extractJsonString(obj, "text", u.text);
                extractJsonInt64(obj, "start_time", u.start_time);
                extractJsonInt64(obj, "end_time", u.end_time);
                extractJsonBool(obj, "definite", u.definite);
                if (!u.text.empty()) {
                    out.push_back(std::move(u));
                }
                object_start = std::string::npos;
            }
        }
    }
    return out;
}

// JSON string escaper for building config payloads.
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
        default:   out += c;      break;
        }
    }
    return out;
}

// Random hex id (used as connect-id / request-id for Doubao session).
std::string makeRandomHex(int bytes)
{
    static thread_local std::mt19937_64 rng{
        static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    std::string out;
    out.reserve(static_cast<std::size_t>(bytes) * 2);
    for (int i = 0; i < bytes; ++i) {
        const auto v = static_cast<std::uint8_t>(rng() & 0xff);
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", v);
        out += buf;
    }
    return out;
}

} // namespace

// ============================================================================
// IAsrEngine — internal contract shared by online & offline engines.
// ============================================================================
class IAsrEngine {
public:
    virtual ~IAsrEngine() = default;

    virtual EngineType  type() const = 0;
    virtual bool        isAvailable() const = 0;

    virtual bool start(ASRMode mode) = 0;
    virtual void stop() = 0;
    virtual void cancelStart() { stop(); }
    virtual void feed(const std::int16_t* samples, std::size_t n, int channels, int sample_rate) = 0;
    virtual void hintEnd() = 0;
    virtual void finalize() = 0;

    void setCallbacks(PartialCallback p, FinalCallback f, ErrorCallback e)
    {
        std::lock_guard<std::mutex> lk(cb_mtx_);
        partial_cb_ = std::move(p);
        final_cb_   = std::move(f);
        error_cb_   = std::move(e);
    }

protected:
    void emitPartial(const ASRPartialResult& r)
    {
        PartialCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx_);
            cb = partial_cb_;
        }
        if (cb) {
            try { cb(r); } catch (...) {}
        }
    }
    void emitFinal(const ASRFinalResult& r)
    {
        FinalCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx_);
            cb = final_cb_;
        }
        if (cb) {
            try { cb(r); } catch (...) {}
        }
    }
    void emitError(const ASRError& e)
    {
        ErrorCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx_);
            cb = error_cb_;
        }
        if (cb) {
            try { cb(e); } catch (...) {}
        }
    }

private:
    std::mutex      cb_mtx_;
    PartialCallback partial_cb_;
    FinalCallback   final_cb_;
    ErrorCallback   error_cb_;
};

// ============================================================================
// OfflineSherpaEngine — wraps sherpa-onnx streaming OnlineRecognizer.
// ============================================================================
class OfflineSherpaEngine : public IAsrEngine {
public:
    OfflineSherpaEngine(OfflineAsrConfig cfg, quill::Logger* logger)
        : cfg_(std::move(cfg)), logger_(logger) {}

    ~OfflineSherpaEngine() override
    {
        shutdown();
    }

    // Eagerly load the sherpa model so engine switches are zero-latency.
    bool init()
    {
        if (!cfg_.enabled) {
            LOG_INFO(logger_, "Offline ASR disabled by config");
            return false;
        }
        if (!fileExists(cfg_.tokens)) {
            LOG_WARNING(logger_, "Offline ASR tokens file missing: {}", cfg_.tokens);
            return false;
        }
        if (cfg_.model_type == "transducer") {
            if (!fileExists(cfg_.encoder) || !fileExists(cfg_.decoder) || !fileExists(cfg_.joiner)) {
                LOG_WARNING(logger_,
                            "Offline ASR transducer model files missing (encoder={}, decoder={}, joiner={})",
                            cfg_.encoder, cfg_.decoder, cfg_.joiner);
                return false;
            }
        } else if (cfg_.model_type == "paraformer") {
            if (!fileExists(cfg_.paraformer_encoder) || !fileExists(cfg_.paraformer_decoder)) {
                LOG_WARNING(logger_, "Offline ASR paraformer model files missing");
                return false;
            }
        } else if (cfg_.model_type == "zipformer2_ctc") {
            if (!fileExists(cfg_.zipformer2_ctc_model)) {
                LOG_WARNING(logger_, "Offline ASR zipformer2_ctc model file missing");
                return false;
            }
        } else {
            LOG_WARNING(logger_, "Offline ASR unknown model_type: {}", cfg_.model_type);
            return false;
        }

        SherpaOnnxOnlineRecognizerConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));

        cfg.feat_config.sample_rate = cfg_.sample_rate;
        cfg.feat_config.feature_dim = cfg_.feature_dim;

        if (cfg_.model_type == "transducer") {
            cfg.model_config.transducer.encoder = cfg_.encoder.c_str();
            cfg.model_config.transducer.decoder = cfg_.decoder.c_str();
            cfg.model_config.transducer.joiner  = cfg_.joiner.c_str();
        } else if (cfg_.model_type == "paraformer") {
            cfg.model_config.paraformer.encoder = cfg_.paraformer_encoder.c_str();
            cfg.model_config.paraformer.decoder = cfg_.paraformer_decoder.c_str();
        } else if (cfg_.model_type == "zipformer2_ctc") {
            cfg.model_config.zipformer2_ctc.model = cfg_.zipformer2_ctc_model.c_str();
        }

        cfg.model_config.tokens       = cfg_.tokens.c_str();
        cfg.model_config.num_threads  = cfg_.num_threads;
        cfg.model_config.provider     = cfg_.provider.c_str();
        cfg.model_config.debug        = cfg_.debug ? 1 : 0;
        if (!cfg_.model_architecture.empty()) {
            cfg.model_config.model_type = cfg_.model_architecture.c_str();
        }

        cfg.decoding_method            = cfg_.decoding_method.c_str();
        cfg.max_active_paths           = cfg_.max_active_paths;
        cfg.enable_endpoint            = cfg_.enable_endpoint ? 1 : 0;
        cfg.rule1_min_trailing_silence = cfg_.rule1_min_trailing_silence;
        cfg.rule2_min_trailing_silence = cfg_.rule2_min_trailing_silence;
        cfg.rule3_min_utterance_length = cfg_.rule3_min_utterance_length;
        if (!cfg_.hotwords_file.empty()) {
            cfg.hotwords_file  = cfg_.hotwords_file.c_str();
            cfg.hotwords_score = cfg_.hotwords_score;
        }

        recognizer_ = SherpaOnnxCreateOnlineRecognizer(&cfg);
        if (!recognizer_) {
            LOG_ERROR(logger_, "SherpaOnnxCreateOnlineRecognizer failed");
            return false;
        }
        available_ = true;
        LOG_INFO(logger_, "Offline ASR engine ready: model_type={}, architecture={}, tokens={}",
                 cfg_.model_type,
                 cfg_.model_architecture.empty() ? "auto" : cfg_.model_architecture,
                 cfg_.tokens);
        return true;
    }

    void shutdown()
    {
        stop();
        if (recognizer_) {
            SherpaOnnxDestroyOnlineRecognizer(recognizer_);
            recognizer_ = nullptr;
        }
        available_ = false;
    }

    EngineType type() const override { return EngineType::kOffline; }
    bool       isAvailable() const override { return available_; }

    bool start(ASRMode /*mode*/) override
    {
        if (!available_) {
            return false;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        if (running_) {
            return true;
        }
        if (stream_) {
            SherpaOnnxDestroyOnlineStream(stream_);
            stream_ = nullptr;
        }
        stream_ = SherpaOnnxCreateOnlineStream(recognizer_);
        if (!stream_) {
            LOG_ERROR(logger_, "Offline ASR CreateOnlineStream failed");
            return false;
        }
        last_text_.clear();
        last_emit_us_ = 0;
        running_      = true;
        LOG_INFO(logger_, "Offline ASR session started");
        return true;
    }

    void stop() override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) {
            return;
        }
        running_ = false;
        if (stream_) {
            SherpaOnnxOnlineStreamInputFinished(stream_);
        }
        // Drain any final result before tearing down the stream.
        if (recognizer_ && stream_) {
            while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_)) {
                SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
            }
            const auto* r = SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
            if (r && r->text && r->text[0] != '\0') {
                ASRFinalResult fin;
                fin.text         = r->text;
                fin.confidence   = 1.0f;
                fin.engine       = EngineType::kOffline;
                fin.timestamp_us = nowMicros();
                emitFinal(fin);
            }
            if (r) {
                SherpaOnnxDestroyOnlineRecognizerResult(r);
            }
        }
        if (stream_) {
            SherpaOnnxDestroyOnlineStream(stream_);
            stream_ = nullptr;
        }
        LOG_INFO(logger_, "Offline ASR session stopped");
    }

    void feed(const std::int16_t* samples, std::size_t n, int channels, int sample_rate) override
    {
        if (!available_ || !running_ || !samples || n == 0 || channels <= 0 || sample_rate <= 0) {
            return;
        }
        // Convert to mono float in-place.
        const std::size_t frames = n / static_cast<std::size_t>(channels);
        if (frames == 0) {
            return;
        }
        std::vector<float> mono(frames, 0.0f);
        for (std::size_t i = 0; i < frames; ++i) {
            int sum = 0;
            for (int c = 0; c < channels; ++c) {
                sum += samples[i * channels + c];
            }
            mono[i] = (static_cast<float>(sum) / channels) * kInt16ToFloat;
        }

        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_ || !stream_) {
            return;
        }
        SherpaOnnxOnlineStreamAcceptWaveform(stream_, sample_rate, mono.data(),
                                             static_cast<std::int32_t>(mono.size()));
        decodeAndPublish();
    }

    void hintEnd() override
    {
        // In streaming mode the model decides EOS on its own via endpoint
        // detection. We do nothing here.
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_ || !stream_) {
            return;
        }
        LOG_DEBUG(logger_, "Offline ASR hintEnd (no-op, endpoint detection drives EOS)");
    }

    void finalize() override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_ || !stream_ || !recognizer_) {
            return;
        }
        SherpaOnnxOnlineStreamInputFinished(stream_);
        while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_)) {
            SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
        }
        publishCurrentResult(true);
    }

private:
    void decodeAndPublish()
    {
        // Assumes mtx_ held.
        if (!recognizer_ || !stream_) {
            return;
        }
        while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_)) {
            SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
        }
        const bool is_endpoint =
            SherpaOnnxOnlineStreamIsEndpoint(recognizer_, stream_) != 0;
        publishCurrentResult(is_endpoint);
        if (is_endpoint) {
            SherpaOnnxOnlineStreamReset(recognizer_, stream_);
            last_text_.clear();
        }
    }

    void publishCurrentResult(bool is_final)
    {
        // Assumes mtx_ held.
        const auto* r = SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
        if (!r) {
            return;
        }
        std::string text = (r->text != nullptr) ? r->text : "";
        SherpaOnnxDestroyOnlineRecognizerResult(r);

        // An endpoint with no decoded text just means trailing silence — never
        // surface an empty result, partial or final, to the upper layer.
        if (text.empty()) {
            return;
        }

        const std::int64_t now = nowMicros();
        // Throttle partials: at most one every 100 ms when text unchanged.
        if (!is_final && text == last_text_ && (now - last_emit_us_) < 100'000) {
            return;
        }
        last_text_    = text;
        last_emit_us_ = now;

        if (is_final) {
            ASRFinalResult fin;
            fin.text         = std::move(text);
            fin.confidence   = 1.0f;
            fin.engine       = EngineType::kOffline;
            fin.timestamp_us = now;
            emitFinal(fin);
        } else {
            ASRPartialResult par;
            par.text         = std::move(text);
            par.is_final     = false;
            par.confidence   = 1.0f;
            par.engine       = EngineType::kOffline;
            par.timestamp_us = now;
            emitPartial(par);
        }
    }

    OfflineAsrConfig                cfg_;
    quill::Logger*                  logger_   = nullptr;
    const SherpaOnnxOnlineRecognizer* recognizer_ = nullptr;
    const SherpaOnnxOnlineStream*   stream_   = nullptr;
    std::mutex                      mtx_;
    std::atomic<bool>               running_{false};
    std::atomic<bool>               available_{false};
    std::string                     last_text_;
    std::int64_t                    last_emit_us_ = 0;
};

// ============================================================================
// OnlineDoubaoEngine — Doubao (Bytedance) bigmodel WebSocket ASR.
// Protocol reference: https://www.volcengine.com/docs/6561/1354869
// ============================================================================
class OnlineDoubaoEngine : public IAsrEngine {
public:
    OnlineDoubaoEngine(OnlineAsrConfig cfg, quill::Logger* logger)
        : cfg_(std::move(cfg)), logger_(logger)
    {
        cfg_.api_key    = envOr(cfg_.api_key_env,    cfg_.api_key);
        cfg_.app_key    = envOr(cfg_.app_key_env,    cfg_.app_key);
        cfg_.access_key = envOr(cfg_.access_key_env, cfg_.access_key);
    }

    ~OnlineDoubaoEngine() override
    {
        shutdown();
    }

    bool init()
    {
        if (!cfg_.enabled) {
            LOG_INFO(logger_, "Online ASR disabled by config");
            return false;
        }
        const bool has_new_console_key = !cfg_.api_key.empty();
        const bool has_old_console_key = !cfg_.app_key.empty() && !cfg_.access_key.empty();
        if (!has_new_console_key && !has_old_console_key) {
            LOG_WARNING(logger_,
                        "Online ASR credentials missing (env {} or {} / {}); online engine unavailable",
                        cfg_.api_key_env, cfg_.app_key_env, cfg_.access_key_env);
            credentials_ok_ = false;
        } else {
            credentials_ok_ = true;
        }

        ix::initNetSystem();
        net_inited_ = true;

        ws_.setUrl(cfg_.endpoint);
        ws_.disableAutomaticReconnection();
        ws_.setHandshakeTimeout(std::max(1, cfg_.connect_timeout_ms / 1000));
        // Stay quiet: every message will be routed through onMessage().
        ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            onMessage(msg);
        });

        LOG_INFO(logger_, "Online ASR engine ready: endpoint={}, credentials={}",
                 cfg_.endpoint, credentials_ok_ ? "present" : "missing");
        return true;
    }

    void shutdown()
    {
        stop();
        if (net_inited_) {
            ix::uninitNetSystem();
            net_inited_ = false;
        }
    }

    EngineType type() const override { return EngineType::kOnline; }
    bool       isAvailable() const override
    {
        return cfg_.enabled && credentials_ok_;
    }

    bool start(ASRMode /*mode*/) override
    {
        if (!isAvailable()) {
            LOG_WARNING(logger_, "Online ASR start refused: engine unavailable");
            return false;
        }
        if (running_.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }
        seq_ = 0;
        connect_id_ = makeRandomHex(16);
        ws_open_.store(false, std::memory_order_release);
        ws_failed_.store(false, std::memory_order_release);
        last_final_text_.clear();
        last_final_key_.clear();
        last_partial_text_.clear();
        emitted_final_utterance_keys_.clear();

        // Doubao expects authentication headers on the WebSocket upgrade.
        ix::WebSocketHttpHeaders headers;
        if (!cfg_.api_key.empty()) {
            headers["X-Api-Key"] = cfg_.api_key;
        } else {
            headers["X-Api-App-Key"]    = cfg_.app_key;
            headers["X-Api-Access-Key"] = cfg_.access_key;
        }
        headers["X-Api-Resource-Id"] = cfg_.resource_id;
        headers["X-Api-Connect-Id"]  = connect_id_;
        headers["X-Api-Request-Id"]  = connect_id_;
        headers["X-Api-Sequence"]    = "-1";
        ws_.setExtraHeaders(headers);

        // Optionally bypass cert verification when running on dev hosts with
        // no CA bundle — the toml config exposes this knob.
        ix::SocketTLSOptions tls;
        tls.disable_hostname_validation = !cfg_.verify_ssl;
        tls.caFile = cfg_.verify_ssl ? "SYSTEM" : "NONE";
        ws_.setTLSOptions(tls);

        send_thread_running_ = true;
        send_thread_ = std::thread(&OnlineDoubaoEngine::senderLoop, this);

        ws_.start();

        // ixwebsocket's start() is asynchronous — it dispatches the connect
        // on a background thread and returns immediately.  Block here until
        // the handshake either completes (Open) or fails (Error/Close),
        // capped at connect_timeout_ms.  Without this, every start() would
        // return true regardless of outcome and the retry-then-fallback
        // logic in runOnce would never engage on transient handshake
        // failures (e.g. "Failed reading HTTP status line").
        const int wait_ms =
            cfg_.connect_timeout_ms > 0 ? cfg_.connect_timeout_ms : 5000;
        {
            std::unique_lock<std::mutex> lk(audio_mtx_);
            audio_cv_.wait_for(
                lk, std::chrono::milliseconds(wait_ms), [this] {
                    return ws_open_.load(std::memory_order_acquire) ||
                           ws_failed_.load(std::memory_order_acquire) ||
                           !running_.load(std::memory_order_acquire) ||
                           !send_thread_running_.load(std::memory_order_acquire);
                });
        }

        if (!ws_open_.load(std::memory_order_acquire)) {
            LOG_WARNING(logger_,
                        "Online ASR handshake did not complete: "
                        "connect_id={}, failed={}, waited_ms={}",
                        connect_id_,
                        ws_failed_.load(std::memory_order_acquire),
                        wait_ms);
            send_thread_running_ = false;
            audio_cv_.notify_all();
            if (send_thread_.joinable()) {
                send_thread_.join();
            }
            ws_.stop();
            ws_open_.store(false, std::memory_order_release);
            ws_failed_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(audio_mtx_);
                audio_queue_.clear();
                audio_finalize_ = false;
            }
            running_.store(false, std::memory_order_release);
            return false;
        }

        LOG_INFO(logger_, "Online ASR session started: connect_id={}", connect_id_);
        return true;
    }

    void stop() override
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        // Ask the server to finalize before closing the socket. This gives the
        // receive callback a chance to observe a final result during normal
        // teardown instead of silently dropping the tail of the utterance.
        {
            std::unique_lock<std::mutex> lk(audio_mtx_);
            audio_finalize_ = true;
            audio_cv_.notify_all();
            if (ws_open_.load(std::memory_order_acquire) &&
                send_thread_running_.load(std::memory_order_acquire)) {
                audio_cv_.wait_for(lk, std::chrono::milliseconds(800), [this] {
                    return !audio_finalize_ ||
                           ws_failed_.load(std::memory_order_acquire);
                });
            }
        }
        send_thread_running_ = false;
        audio_cv_.notify_all();
        if (send_thread_.joinable()) {
            send_thread_.join();
        }
        ws_.stop();
        ws_open_.store(false, std::memory_order_release);
        ws_failed_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(audio_mtx_);
            audio_queue_.clear();
            audio_finalize_ = false;
        }
        LOG_INFO(logger_, "Online ASR session stopped");
    }

    void cancelStart() override
    {
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
        running_.store(false, std::memory_order_release);
        send_thread_running_.store(false, std::memory_order_release);
        ws_failed_.store(true, std::memory_order_release);
        audio_cv_.notify_all();
    }

    void feed(const std::int16_t* samples, std::size_t n, int channels, int sample_rate) override
    {
        if (!running_ || !samples || n == 0 || channels <= 0 || sample_rate <= 0) {
            return;
        }
        std::vector<std::int16_t> mono =
            channels == 1 ? std::vector<std::int16_t>(samples, samples + n)
                          : toMonoInt16(std::vector<std::int16_t>(samples, samples + n), channels);
        if (sample_rate != cfg_.sample_rate) {
            // Cheap nearest-neighbour resampling to the target rate.  In a real
            // deployment AudioManager's processing chain already negotiates the
            // ASR-expected rate, but this fallback keeps the engine resilient
            // to mismatched configurations.
            mono = resampleNearest(mono, sample_rate, cfg_.sample_rate);
        }

        std::lock_guard<std::mutex> lk(audio_mtx_);
        audio_queue_.insert(audio_queue_.end(), mono.begin(), mono.end());
        audio_cv_.notify_one();
    }

    void hintEnd() override
    {
        // Doubao streams accept additional audio chunks even after VAD-end on
        // the client; we treat the hint as a soft EOS marker the server can
        // use to finalize the partial result.
        std::lock_guard<std::mutex> lk(audio_mtx_);
        audio_finalize_ = true;
        audio_cv_.notify_all();
    }

    void finalize() override
    {
        hintEnd();
    }

private:
    // ------------------------------------------------------------------------
    // Doubao binary framing
    // ------------------------------------------------------------------------
    enum : std::uint8_t {
        kProtocolVersion = 0x1,
        kHeaderSize      = 0x1,    // 1 * 4 bytes
        kMsgFullClient   = 0x1,
        kMsgAudioClient  = 0x2,
        kMsgServerFull   = 0x9,
        kMsgServerAck    = 0xb,
        kMsgServerError  = 0xf,
        kSerJson         = 0x1,
        kSerRaw          = 0x0,
        kCompGzip        = 0x1,
        kCompNone        = 0x0,
        kFlagNoSeq       = 0x0,
        kFlagHasSeq      = 0x1,
        kFlagLastNoSeq   = 0x2,
        kFlagLastWithSeq = 0x3,
    };

    static void writeBE32(std::vector<std::uint8_t>& out, std::uint32_t v)
    {
        out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(v & 0xff));
    }

    static std::uint32_t readBE32(const std::uint8_t* p)
    {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8)  |
                static_cast<std::uint32_t>(p[3]);
    }

    // Build a Doubao binary frame.
    static std::vector<std::uint8_t> buildFrame(std::uint8_t msg_type,
                                                std::uint8_t flags,
                                                std::uint8_t serialization,
                                                std::uint8_t compression,
                                                std::int32_t seq,
                                                bool include_seq,
                                                const std::vector<std::uint8_t>& payload)
    {
        std::vector<std::uint8_t> out;
        out.reserve(payload.size() + 16);
        out.push_back(static_cast<std::uint8_t>((kProtocolVersion << 4) | kHeaderSize));
        out.push_back(static_cast<std::uint8_t>((msg_type << 4) | (flags & 0x0f)));
        out.push_back(static_cast<std::uint8_t>((serialization << 4) | (compression & 0x0f)));
        out.push_back(0x00);
        if (include_seq) {
            const auto useq = static_cast<std::uint32_t>(seq);
            writeBE32(out, useq);
        }
        writeBE32(out, static_cast<std::uint32_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    bool sendInitial()
    {
        std::ostringstream oss;
        oss << "{"
            << "\"user\":{\"uid\":\"" << jsonEscape(cfg_.uid) << "\"},"
            << "\"audio\":{"
            <<   "\"format\":\"" << jsonEscape(cfg_.format) << "\","
            <<   "\"codec\":\""  << jsonEscape(cfg_.codec)  << "\","
            <<   "\"rate\":"     << cfg_.sample_rate << ","
            <<   "\"bits\":"     << cfg_.bits        << ","
            <<   "\"channel\":"  << cfg_.channels
            << "},"
            << "\"request\":{"
            <<   "\"model_name\":\"bigmodel\","
            <<   "\"enable_punc\":" << (cfg_.enable_punctuation ? "true" : "false") << ","
            <<   "\"enable_itn\":"  << (cfg_.enable_itn ? "true" : "false") << ","
            <<   "\"enable_ddc\":"  << (cfg_.enable_ddc ? "true" : "false") << ","
            <<   "\"show_utterances\":" << (cfg_.show_utterances ? "true" : "false") << ","
            <<   "\"result_type\":\"" << jsonEscape(cfg_.result_type) << "\","
            <<   "\"enable_nonstream\":" << (cfg_.enable_nonstream ? "true" : "false") << ","
            <<   "\"enable_accelerate_text\":"
            <<       (cfg_.enable_accelerate_text ? "true" : "false");
        if (cfg_.enable_accelerate_text) {
            oss << ",\"accelerate_score\":"
                << std::clamp(cfg_.accelerate_score, 0, 20);
        }
        if (cfg_.end_window_size_ms > 0) {
            oss << ",\"end_window_size\":"
                << std::max(200, cfg_.end_window_size_ms);
        }
        if (cfg_.force_to_speech_time_ms > 0) {
            oss << ",\"force_to_speech_time\":"
                << std::max(1, cfg_.force_to_speech_time_ms);
        }
        if (cfg_.vad_segment_duration_ms > 0 && cfg_.end_window_size_ms <= 0) {
            oss << ",\"vad_segment_duration\":"
                << cfg_.vad_segment_duration_ms;
        }
        oss << "}}";
        const std::string raw = oss.str();
        std::vector<std::uint8_t> compressed;
        if (!gzipCompress(reinterpret_cast<const std::uint8_t*>(raw.data()),
                          raw.size(), compressed)) {
            return false;
        }
        ++seq_;
        const auto frame = buildFrame(kMsgFullClient, kFlagHasSeq,
                                      kSerJson, kCompGzip,
                                      seq_, true, compressed);
        return ws_.sendBinary(std::string(frame.begin(), frame.end())).success;
    }

    bool sendAudio(const std::int16_t* data, std::size_t samples, bool is_last)
    {
        std::vector<std::uint8_t> raw;
        if (data && samples > 0) {
            const auto* first = reinterpret_cast<const std::uint8_t*>(data);
            raw.assign(first, first + samples * sizeof(std::int16_t));
        }
        std::vector<std::uint8_t> compressed;
        if (!gzipCompress(raw.empty() ? nullptr : raw.data(), raw.size(), compressed)) {
            return false;
        }
        const std::uint8_t flag = is_last ? kFlagLastWithSeq : kFlagHasSeq;
        if (is_last) {
            seq_ = -std::abs(seq_ + 1);  // negative seq marks the last frame
        } else {
            ++seq_;
        }
        const auto frame = buildFrame(kMsgAudioClient, flag,
                                      kSerRaw, kCompGzip,
                                      seq_, true, compressed);
        return ws_.sendBinary(std::string(frame.begin(), frame.end())).success;
    }

    void onMessage(const ix::WebSocketMessagePtr& msg)
    {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            rawCallbackLog("ASRManager", "Online ASR WebSocket opened");
            ws_open_.store(true, std::memory_order_release);
            audio_cv_.notify_all();
            if (!sendInitial()) {
                emitError({AsrErrorCode::kProtocolError,
                           "failed to send Doubao initial config",
                           EngineType::kOnline});
            }
            break;
        case ix::WebSocketMessageType::Close:
            {
                std::ostringstream oss;
                oss << "Online ASR WebSocket closed: code="
                    << msg->closeInfo.code << ", reason="
                    << msg->closeInfo.reason;
                rawCallbackLog("ASRManager", oss.str());
            }
            ws_open_.store(false, std::memory_order_release);
            ws_failed_.store(true, std::memory_order_release);
            audio_cv_.notify_all();
            break;
        case ix::WebSocketMessageType::Error:
            rawCallbackLog("ASRManager",
                           std::string("Online ASR WebSocket error: ") +
                           msg->errorInfo.reason);
            {
                // If we hadn't yet observed Open, this Error is a handshake
                // failure that start() is synchronously waiting for — let
                // the retry loop in runOnce decide what to surface, instead
                // of leaking a transient failure to the user callback for
                // every retry attempt.
                const bool was_open =
                    ws_open_.exchange(false, std::memory_order_acq_rel);
                ws_failed_.store(true, std::memory_order_release);
                audio_cv_.notify_all();
                if (was_open) {
                    emitError({AsrErrorCode::kNetworkFailure,
                               msg->errorInfo.reason, EngineType::kOnline});
                }
            }
            break;
        case ix::WebSocketMessageType::Message:
            if (msg->binary) {
                handleServerFrame(reinterpret_cast<const std::uint8_t*>(msg->str.data()),
                                  msg->str.size());
            }
            break;
        default:
            break;
        }
    }

    void handleServerFrame(const std::uint8_t* buf, std::size_t size)
    {
        if (size < 4) {
            return;
        }
        const std::uint8_t header0 = buf[0];
        const std::uint8_t hdr_size = (header0 & 0x0f);
        const std::size_t hdr_bytes = static_cast<std::size_t>(hdr_size) * 4;
        if (size < hdr_bytes) {
            return;
        }
        const std::uint8_t msg_type = (buf[1] >> 4) & 0x0f;
        const std::uint8_t flags    =  buf[1] & 0x0f;
        const std::uint8_t compress =  buf[2] & 0x0f;

        std::size_t offset = hdr_bytes;
        if (flags & kFlagHasSeq) {
            if (offset + 4 > size) return;
            offset += 4;  // seq number; we don't currently use it
        }
        if (offset + 4 > size) {
            return;
        }
        const std::uint32_t payload_size = readBE32(buf + offset);
        offset += 4;
        if (offset + payload_size > size) {
            return;
        }

        std::vector<std::uint8_t> payload(buf + offset, buf + offset + payload_size);
        std::vector<std::uint8_t> decoded = payload;
        if (compress == kCompGzip) {
            if (!gzipDecompress(payload.data(), payload.size(), decoded)) {
                rawCallbackLog("ASRManager", "Online ASR failed to gunzip server frame");
                return;
            }
        }

        const std::string json(decoded.begin(), decoded.end());
        if (cfg_.debug) {
            std::ostringstream oss;
            oss << "Online ASR server frame type=" << static_cast<int>(msg_type)
                << " flags=" << static_cast<int>(flags)
                << " json=" << json;
            rawCallbackLog("ASRManager", oss.str());
        }
        if (msg_type == kMsgServerError) {
            rawCallbackLog("ASRManager",
                           std::string("Online ASR server error frame: ") + json);
            emitError({AsrErrorCode::kProtocolError, json, EngineType::kOnline});
            return;
        }

        std::string text;
        bool is_final = (flags == kFlagLastNoSeq || flags == kFlagLastWithSeq);
        bool explicit_is_final = false;
        if (!extractJsonString(json, "text", text)) {
            extractJsonString(json, "result", text);
        }
        if (extractJsonBool(json, "is_final", explicit_is_final) && explicit_is_final) {
            is_final = true;
        }

        const auto utterances = extractJsonUtterances(json);
        const std::int64_t now = nowMicros();
        bool emitted_final = false;
        for (const JsonUtterance& u : utterances) {
            if (!u.definite) {
                continue;
            }
            const std::string key =
                std::to_string(u.start_time) + "#" +
                std::to_string(u.end_time) + "#" + u.text;
            if (!emitted_final_utterance_keys_.insert(key).second) {
                continue;
            }
            ASRFinalResult fin;
            fin.text         = u.text;
            fin.confidence   = 1.0f;
            fin.engine       = EngineType::kOnline;
            fin.timestamp_us = now;
            emitFinal(fin);
            emitted_final = true;
        }
        if (emitted_final) {
            last_partial_text_.clear();
        }

        if (text.empty()) {
            return;
        }

        if (is_final && utterances.empty()) {
            std::int64_t end_time_ms = -1;
            std::string final_key = text;
            if (extractLastJsonInt64(json, "end_time", end_time_ms)) {
                final_key += "#" + std::to_string(end_time_ms);
            }
            if (final_key == last_final_key_) {
                return;
            }
            std::string final_text = text;
            if (!last_final_text_.empty() &&
                text.rfind(last_final_text_, 0) == 0 &&
                text.size() > last_final_text_.size()) {
                final_text = text.substr(last_final_text_.size());
            }
            last_final_text_ = text;
            last_final_key_ = std::move(final_key);
            last_partial_text_.clear();
            if (final_text.empty()) {
                return;
            }
            ASRFinalResult fin;
            fin.text         = std::move(final_text);
            fin.confidence   = 1.0f;
            fin.engine       = EngineType::kOnline;
            fin.timestamp_us = now;
            emitFinal(fin);
        } else {
            std::string partial_text;
            if (!utterances.empty()) {
                for (auto it = utterances.rbegin(); it != utterances.rend(); ++it) {
                    if (!it->definite) {
                        partial_text = it->text;
                        break;
                    }
                }
                if (partial_text.empty()) {
                    return;
                }
            } else {
                partial_text = std::move(text);
            }

            if (partial_text == last_partial_text_) {
                return;
            }
            last_partial_text_ = partial_text;
            ASRPartialResult par;
            par.text         = std::move(partial_text);
            par.is_final     = false;
            par.confidence   = 1.0f;
            par.engine       = EngineType::kOnline;
            par.timestamp_us = now;
            emitPartial(par);
        }
    }

    void senderLoop()
    {
        const std::size_t chunk_samples =
            static_cast<std::size_t>(cfg_.sample_rate) *
            static_cast<std::size_t>(cfg_.segment_duration_ms) / 1000;
        std::vector<std::int16_t> chunk;
        chunk.reserve(chunk_samples);

        // Wait until the WS handshake either succeeds or fails. Audio frames
        // queued during the handshake are buffered and flushed once the
        // connection opens; if the handshake fails we drop everything quietly
        // and exit instead of flooding the log with sendBinary failures.
        {
            std::unique_lock<std::mutex> lk(audio_mtx_);
            audio_cv_.wait(lk, [this] {
                return ws_open_.load(std::memory_order_acquire) ||
                       ws_failed_.load(std::memory_order_acquire) ||
                       !send_thread_running_;
            });
        }

        if (ws_failed_.load(std::memory_order_acquire) || !send_thread_running_) {
            std::lock_guard<std::mutex> lk(audio_mtx_);
            audio_queue_.clear();
            audio_finalize_ = false;
            return;
        }

        while (send_thread_running_) {
            bool finalize_pending = false;
            {
                std::unique_lock<std::mutex> lk(audio_mtx_);
                audio_cv_.wait_for(lk, std::chrono::milliseconds(50), [this] {
                    return audio_queue_.size() >=
                               (static_cast<std::size_t>(cfg_.sample_rate) *
                                static_cast<std::size_t>(cfg_.segment_duration_ms) / 1000) ||
                           audio_finalize_ ||
                           !send_thread_running_ ||
                           ws_failed_.load(std::memory_order_acquire);
                });
                while (chunk.size() < chunk_samples && !audio_queue_.empty()) {
                    chunk.push_back(audio_queue_.front());
                    audio_queue_.pop_front();
                }
                finalize_pending = audio_finalize_;
            }

            if (ws_failed_.load(std::memory_order_acquire)) {
                // Server hung up or never accepted us; drain and exit so we
                // don't spam sendBinary failures.
                std::lock_guard<std::mutex> lk(audio_mtx_);
                audio_queue_.clear();
                audio_finalize_ = false;
                return;
            }
            if (!ws_open_.load(std::memory_order_acquire)) {
                continue;
            }

            if (chunk.size() >= chunk_samples ||
                (finalize_pending && !chunk.empty())) {
                if (!sendAudio(chunk.data(), chunk.size(), false)) {
                    LOG_ERROR(logger_, "Online ASR send chunk failed");
                }
                chunk.clear();
            }

            if (finalize_pending) {
                {
                    std::lock_guard<std::mutex> lk(audio_mtx_);
                    audio_finalize_ = false;
                }
                if (!sendAudio(nullptr, 0, true)) {
                    LOG_ERROR(logger_, "Online ASR send last frame failed");
                }
                audio_cv_.notify_all();
            }
        }
    }

    static std::vector<std::int16_t> resampleNearest(const std::vector<std::int16_t>& in,
                                                      int from_rate, int to_rate)
    {
        if (from_rate == to_rate || in.empty() || from_rate <= 0 || to_rate <= 0) {
            return in;
        }
        const std::size_t out_size = (in.size() * static_cast<std::size_t>(to_rate)) /
                                     static_cast<std::size_t>(from_rate);
        std::vector<std::int16_t> out(out_size, 0);
        for (std::size_t i = 0; i < out_size; ++i) {
            const std::size_t src = (i * static_cast<std::size_t>(from_rate)) /
                                    static_cast<std::size_t>(to_rate);
            out[i] = in[std::min(src, in.size() - 1)];
        }
        return out;
    }

    OnlineAsrConfig cfg_;
    quill::Logger*  logger_ = nullptr;

    ix::WebSocket   ws_;
    std::string     connect_id_;
    std::int32_t    seq_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> send_thread_running_{false};
    std::thread       send_thread_;

    std::mutex                 audio_mtx_;
    std::condition_variable    audio_cv_;
    std::deque<std::int16_t>   audio_queue_;
    bool                       audio_finalize_ = false;

    // Set once the WebSocket handshake succeeds. Until then the sender loop
    // must not call ws_.sendBinary() — IXWebSocket would return failure on
    // every call and flood the log.
    std::atomic<bool>          ws_open_{false};
    std::atomic<bool>          ws_failed_{false};

    std::string                last_final_text_;
    std::string                last_final_key_;
    std::string                last_partial_text_;
    std::unordered_set<std::string> emitted_final_utterance_keys_;

    bool credentials_ok_ = false;
    bool net_inited_     = false;
};

// ============================================================================
// ASRManager::Impl
// ============================================================================
struct ASRManager::Impl {
    audio::AudioManager* audio = nullptr;
    AsrConfig            cfg;

    quill::Logger*       logger = nullptr;

    std::unique_ptr<OfflineSherpaEngine> offline_engine;
    std::unique_ptr<OnlineDoubaoEngine>  online_engine;
    IAsrEngine*                          active_engine = nullptr;

    audio::ConsumerHandle consumer_handle = audio::kInvalidConsumerHandle;

    PartialCallback partial_cb;
    FinalCallback   final_cb;
    ErrorCallback   error_cb;
    std::mutex      cb_mtx;

    std::atomic<bool> initialized{false};
    std::atomic<bool> recognizing{false};
    std::atomic<bool> force_offline{false};
    std::atomic<EngineType> engine_pref{EngineType::kOnline};
    ASRMode active_mode = ASRMode::kStreaming;

    // Cancellation primitives for startRecognition's blocking retry loop.
    // cancel_start_requested is checked between attempts and during the
    // backoff sleep (which uses cancel_cv.wait_for instead of sleep_for).
    // cancelPendingStart() flips the flag, notifies the cv, and calls
    // active_engine->stop() to break the engine's in-flight handshake wait.
    std::atomic<bool>       cancel_start_requested{false};
    std::mutex              cancel_mtx;
    std::condition_variable cancel_cv;

    network_manager::NetworkState last_net_state = network_manager::NetworkState::Offline;
    std::mutex                    state_mtx;

    // Logger setup
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
            // fall through: console sink will keep us observable
        }
        if (cfg.logging.console) {
            auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "asr_console_sink");
            sinks.push_back(console_sink);
        }
        if (sinks.empty()) {
            auto fallback = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "asr_fallback_sink");
            sinks.push_back(fallback);
        }
        logger = quill::Frontend::create_or_get_logger("ASRManager", std::move(sinks));
        logger->set_log_level(quill::LogLevel::Info);
    }

    bool initImpl(audio::AudioManager* am, AsrConfig in_cfg)
    {
        if (initialized.load(std::memory_order_acquire)) {
            return true;
        }
        audio = am;
        cfg   = std::move(in_cfg);

        setupLogger();
        LOG_INFO(logger,
                 "ASRManager init: preferred={}, fallback_to_offline={}, frame_queue_depth={}",
                 cfg.preferred_engine == EngineType::kOnline ? "online" : "offline",
                 cfg.fallback_to_offline, cfg.frame_queue_depth);

        if (!audio) {
            LOG_ERROR(logger, "AudioManager pointer is null");
            return false;
        }

        engine_pref.store(cfg.preferred_engine, std::memory_order_release);
        active_mode = cfg.default_mode == ASRMode::kAuto ? ASRMode::kStreaming : cfg.default_mode;

        // Construct engines (failure is non-fatal — switch table simply marks
        // the engine unavailable and we lean on the other one).
        offline_engine = std::make_unique<OfflineSherpaEngine>(cfg.offline, logger);
        online_engine  = std::make_unique<OnlineDoubaoEngine>(cfg.online, logger);

        const bool offline_ok = offline_engine->init();
        const bool online_ok  = online_engine->init();

        if (!offline_ok && !online_ok) {
            LOG_ERROR(logger, "ASRManager init: neither online nor offline engine is available");
        }

        // Pick initial active engine according to preference + availability.
        active_engine = selectEngine();
        broadcastCallbacks();

        initialized.store(true, std::memory_order_release);
        LOG_INFO(logger, "ASRManager initialized (active engine: {})",
                 active_engine ? (active_engine->type() == EngineType::kOnline ? "online" : "offline")
                               : "none");
        return true;
    }

    IAsrEngine* selectEngine()
    {
        const bool prefer_online = engine_pref.load(std::memory_order_acquire) == EngineType::kOnline;
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

    void broadcastCallbacks()
    {
        PartialCallback p;
        FinalCallback   f;
        ErrorCallback   e;
        {
            std::lock_guard<std::mutex> lk(cb_mtx);
            p = partial_cb;
            f = final_cb;
            e = error_cb;
        }
        if (offline_engine) offline_engine->setCallbacks(p, f, e);
        if (online_engine)  online_engine->setCallbacks(p, f, e);
    }

    void shutdownImpl()
    {
        if (!initialized.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        stopRecognitionImpl();
        if (offline_engine) offline_engine->shutdown();
        if (online_engine)  online_engine->shutdown();
        offline_engine.reset();
        online_engine.reset();
        active_engine = nullptr;
        if (logger) {
            LOG_INFO(logger, "ASRManager shutdown complete");
            logger->flush_log();
        }
        audio = nullptr;
    }

    bool startRecognitionImpl(ASRMode mode)
    {
        if (!initialized.load(std::memory_order_acquire)) {
            if (logger) {
                LOG_WARNING(logger, "startRecognition called before init");
            }
            return false;
        }
        if (recognizing.load(std::memory_order_acquire)) {
            if (logger) {
                LOG_DEBUG(logger, "startRecognition: already recognizing");
            }
            return true;
        }
        if (mode != ASRMode::kAuto) {
            active_mode = mode;
        }
        IAsrEngine* eng = selectEngine();
        if (!eng) {
            LOG_ERROR(logger, "startRecognition: no engine is available");
            ASRError err{AsrErrorCode::kEngineUnavailable,
                         "no ASR engine available", EngineType::kOffline};
            ErrorCallback cb;
            {
                std::lock_guard<std::mutex> lk(cb_mtx);
                cb = error_cb;
            }
            if (cb) cb(err);
            return false;
        }
        active_engine = eng;
        cancel_start_requested.store(false, std::memory_order_release);

        // Online engine: retry session-start up to cfg.online.max_retries times
        // before falling back to the offline engine.  Each call to start()
        // performs a fresh connect, so retrying covers transient network /
        // auth-handshake failures.  Offline engine isn't retried — it's local.
        bool started = false;
        if (active_engine->type() == EngineType::kOnline) {
            const int max_attempts = std::max(1, cfg.online.max_retries);
            for (int attempt = 1; attempt <= max_attempts; ++attempt) {
                if (cancel_start_requested.load(std::memory_order_acquire)) {
                    LOG_INFO(logger,
                             "startRecognition canceled before attempt {}",
                             attempt);
                    break;
                }
                if (active_engine->start(active_mode)) {
                    started = true;
                    break;
                }
                if (cancel_start_requested.load(std::memory_order_acquire)) {
                    LOG_INFO(logger,
                             "startRecognition canceled after attempt {}",
                             attempt);
                    break;
                }
                LOG_WARNING(logger,
                            "Online ASR start attempt {}/{} failed",
                            attempt, max_attempts);
                if (attempt < max_attempts && cfg.online.retry_backoff_ms > 0) {
                    // cancellable backoff: wake immediately if cancel arrives.
                    std::unique_lock<std::mutex> lk(cancel_mtx);
                    cancel_cv.wait_for(
                        lk,
                        std::chrono::milliseconds(cfg.online.retry_backoff_ms),
                        [this] {
                            return cancel_start_requested.load(
                                std::memory_order_acquire);
                        });
                }
            }
        } else {
            started = active_engine->start(active_mode);
        }

        // If a cancel arrived (during retries or right after a successful
        // start), bail out cleanly.  Tear down any session we managed to
        // open so the engine doesn't leak.
        if (cancel_start_requested.load(std::memory_order_acquire)) {
            if (started && active_engine) {
                active_engine->stop();
            }
            LOG_INFO(logger, "startRecognition aborted by cancelPendingStart");
            return false;
        }

        if (!started) {
            LOG_ERROR(logger, "startRecognition: engine start failed; trying fallback");
            // Try the other engine once.
            IAsrEngine* other = (active_engine == online_engine.get())
                                    ? static_cast<IAsrEngine*>(offline_engine.get())
                                    : static_cast<IAsrEngine*>(online_engine.get());
            if (other && other->isAvailable() && other->start(active_mode)) {
                active_engine = other;
            } else {
                return false;
            }
        }

        // Register the AudioManager FrameDispatcher consumer.
        if (consumer_handle == audio::kInvalidConsumerHandle && audio) {
            consumer_handle = audio->addFrameConsumer(
                [this](const audio::AudioFrame& f) { onFrame(f); },
                cfg.frame_queue_depth);
        }

        recognizing.store(true, std::memory_order_release);
        LOG_INFO(logger, "ASR recognition started: engine={}, mode={}",
                 active_engine->type() == EngineType::kOnline ? "online" : "offline",
                 active_mode == ASRMode::kStreaming ? "streaming" : "batch");
        return true;
    }

    // Wake any in-flight startRecognition out of its retry/backoff loop and
    // out of the engine's connect-timeout wait_for.  Safe to call from any
    // thread, including audio / wake-callback threads.  No-op if no start is
    // pending.  After this returns, startRecognitionImpl is guaranteed to
    // return false within a few milliseconds.
    void cancelPendingStartImpl()
    {
        cancel_start_requested.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(cancel_mtx);
            cancel_cv.notify_all();
        }
        // Nudge the engine that's currently in start()'s wait_for without
        // tearing down an already-recognizing session from the wake callback
        // thread. The owning ASR turn will call stopRecognition() after it
        // observes the preempt flag.
        if (!recognizing.load(std::memory_order_acquire)) {
            if (online_engine)  online_engine->cancelStart();
            if (offline_engine) offline_engine->cancelStart();
        }
    }

    void stopRecognitionImpl()
    {
        if (!recognizing.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (audio && consumer_handle != audio::kInvalidConsumerHandle) {
            audio->removeFrameConsumer(consumer_handle);
            consumer_handle = audio::kInvalidConsumerHandle;
        }
        if (active_engine) {
            active_engine->stop();
        }
        LOG_INFO(logger, "ASR recognition stopped");
    }

    void onFrame(const audio::AudioFrame& frame)
    {
        if (!recognizing.load(std::memory_order_acquire) || !active_engine) {
            return;
        }
        if (frame.samples.empty() || frame.channels <= 0 || frame.sample_rate <= 0) {
            return;
        }
        active_engine->feed(frame.samples.data(), frame.samples.size(),
                            frame.channels, frame.sample_rate);
    }

    void onNetworkChangedImpl(network_manager::NetworkState s)
    {
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            last_net_state = s;
        }

        const bool was_offline = force_offline.load(std::memory_order_acquire);
        const bool should_force_offline =
            (s == network_manager::NetworkState::Offline);

        if (should_force_offline == was_offline) {
            return;
        }
        force_offline.store(should_force_offline, std::memory_order_release);
        if (!logger) return;
        LOG_INFO(logger, "Network state changed to {} → preferred engine switching",
                 network_manager::NetworkManager::toString(s));

        // Hot-swap the engine, but do not interrupt the current request: we
        // tear down on the next session boundary (stop/start).  If no session
        // is active right now we eagerly update active_engine for the next one.
        if (!recognizing.load(std::memory_order_acquire)) {
            active_engine = selectEngine();
            LOG_INFO(logger, "Active ASR engine pre-selected: {}",
                     active_engine ? (active_engine->type() == EngineType::kOnline ? "online" : "offline")
                                   : "none");
        }
    }
};

// ============================================================================
// ASRManager facade
// ============================================================================
ASRManager::ASRManager() : impl_(std::make_unique<Impl>()) {}
ASRManager::~ASRManager() { shutdown(); }

bool ASRManager::init(audio::AudioManager* audio_manager, const std::string& config_path)
{
    try {
        return impl_->initImpl(audio_manager, loadConfig(config_path));
    } catch (const std::exception& e) {
        if (impl_->logger) {
            LOG_ERROR(impl_->logger, "init failed: {}", e.what());
        }
        return false;
    }
}

bool ASRManager::init(audio::AudioManager* audio_manager, const AsrConfig& cfg)
{
    return impl_->initImpl(audio_manager, cfg);
}

void ASRManager::shutdown()
{
    if (impl_) impl_->shutdownImpl();
}

bool ASRManager::startRecognition(ASRMode mode)
{
    return impl_->startRecognitionImpl(mode);
}

void ASRManager::stopRecognition()
{
    impl_->stopRecognitionImpl();
}

void ASRManager::cancelPendingStart()
{
    if (impl_) impl_->cancelPendingStartImpl();
}

void ASRManager::hintSpeechEnd()
{
    if (!impl_->recognizing.load(std::memory_order_acquire) || !impl_->active_engine) {
        return;
    }
    impl_->active_engine->hintEnd();
}

void ASRManager::finalize()
{
    if (!impl_->recognizing.load(std::memory_order_acquire) || !impl_->active_engine) {
        return;
    }
    impl_->active_engine->finalize();
}

void ASRManager::setPartialCallback(PartialCallback cb)
{
    {
        std::lock_guard<std::mutex> lk(impl_->cb_mtx);
        impl_->partial_cb = std::move(cb);
    }
    impl_->broadcastCallbacks();
}

void ASRManager::setFinalCallback(FinalCallback cb)
{
    {
        std::lock_guard<std::mutex> lk(impl_->cb_mtx);
        impl_->final_cb = std::move(cb);
    }
    impl_->broadcastCallbacks();
}

void ASRManager::setErrorCallback(ErrorCallback cb)
{
    {
        std::lock_guard<std::mutex> lk(impl_->cb_mtx);
        impl_->error_cb = std::move(cb);
    }
    impl_->broadcastCallbacks();
}

EngineType ASRManager::currentEngine() const
{
    if (impl_->active_engine) {
        return impl_->active_engine->type();
    }
    return impl_->engine_pref.load(std::memory_order_acquire);
}

void ASRManager::onNetworkStateChanged(network_manager::NetworkState s)
{
    impl_->onNetworkChangedImpl(s);
}

bool ASRManager::isOnlineAvailable() const
{
    return impl_->online_engine && impl_->online_engine->isAvailable() &&
           !impl_->force_offline.load(std::memory_order_acquire);
}

bool ASRManager::isRecognizing() const
{
    return impl_->recognizing.load(std::memory_order_acquire);
}

ASRMode ASRManager::activeMode() const
{
    return impl_->active_mode;
}

const AsrConfig& ASRManager::config() const
{
    return impl_->cfg;
}

// ============================================================================
// TOML loader
// ============================================================================
AsrConfig ASRManager::loadConfig(const std::string& config_path)
{
    AsrConfig cfg;
    toml::table table = toml::parse_file(config_path);
    const toml::node_view asr = table["asr"];

    cfg.frame_queue_depth   = static_cast<std::size_t>(
        asr["frame_queue_depth"].value_or(static_cast<int64_t>(cfg.frame_queue_depth)));
    cfg.max_session_ms      = asr["max_session_ms"].value_or(cfg.max_session_ms);
    cfg.no_result_timeout_ms = asr["no_result_timeout_ms"].value_or(cfg.no_result_timeout_ms);
    cfg.fallback_to_offline  = asr["fallback_to_offline"].value_or(cfg.fallback_to_offline);

    const std::string mode_str = asr["mode"].value_or(std::string("streaming"));
    if (mode_str == "batch")    cfg.default_mode = ASRMode::kBatch;
    else if (mode_str == "auto") cfg.default_mode = ASRMode::kAuto;
    else                         cfg.default_mode = ASRMode::kStreaming;

    const std::string pref_str = asr["preferred_engine"].value_or(std::string("online"));
    cfg.preferred_engine = (pref_str == "offline") ? EngineType::kOffline : EngineType::kOnline;

    // -- offline ------------------------------------------------------------
    const toml::node_view off = asr["offline"];
    cfg.offline.enabled               = off["enabled"].value_or(cfg.offline.enabled);
    cfg.offline.model_type            = off["model_type"].value_or(cfg.offline.model_type);
    cfg.offline.model_architecture    = off["model_architecture"].value_or(cfg.offline.model_architecture);
    cfg.offline.encoder               = off["encoder"].value_or(cfg.offline.encoder);
    cfg.offline.decoder               = off["decoder"].value_or(cfg.offline.decoder);
    cfg.offline.joiner                = off["joiner"].value_or(cfg.offline.joiner);
    cfg.offline.paraformer_encoder    = off["paraformer_encoder"].value_or(cfg.offline.paraformer_encoder);
    cfg.offline.paraformer_decoder    = off["paraformer_decoder"].value_or(cfg.offline.paraformer_decoder);
    cfg.offline.zipformer2_ctc_model  = off["zipformer2_ctc_model"].value_or(cfg.offline.zipformer2_ctc_model);
    cfg.offline.tokens                = off["tokens"].value_or(cfg.offline.tokens);
    cfg.offline.provider              = off["provider"].value_or(cfg.offline.provider);
    cfg.offline.decoding_method       = off["decoding_method"].value_or(cfg.offline.decoding_method);
    cfg.offline.num_threads           = off["num_threads"].value_or(cfg.offline.num_threads);
    cfg.offline.max_active_paths      = off["max_active_paths"].value_or(cfg.offline.max_active_paths);
    cfg.offline.sample_rate           = off["sample_rate"].value_or(cfg.offline.sample_rate);
    cfg.offline.feature_dim           = off["feature_dim"].value_or(cfg.offline.feature_dim);
    cfg.offline.enable_endpoint       = off["enable_endpoint"].value_or(cfg.offline.enable_endpoint);
    cfg.offline.rule1_min_trailing_silence = static_cast<float>(
        off["rule1_min_trailing_silence"].value_or(static_cast<double>(cfg.offline.rule1_min_trailing_silence)));
    cfg.offline.rule2_min_trailing_silence = static_cast<float>(
        off["rule2_min_trailing_silence"].value_or(static_cast<double>(cfg.offline.rule2_min_trailing_silence)));
    cfg.offline.rule3_min_utterance_length = static_cast<float>(
        off["rule3_min_utterance_length"].value_or(static_cast<double>(cfg.offline.rule3_min_utterance_length)));
    cfg.offline.hotwords_file         = off["hotwords_file"].value_or(cfg.offline.hotwords_file);
    cfg.offline.hotwords_score        = static_cast<float>(
        off["hotwords_score"].value_or(static_cast<double>(cfg.offline.hotwords_score)));
    cfg.offline.debug                 = off["debug"].value_or(cfg.offline.debug);

    // -- online -------------------------------------------------------------
    const toml::node_view on = asr["online"];
    cfg.online.enabled              = on["enabled"].value_or(cfg.online.enabled);
    cfg.online.endpoint             = on["endpoint"].value_or(cfg.online.endpoint);
    cfg.online.api_key              = on["api_key"].value_or(cfg.online.api_key);
    cfg.online.app_key              = on["app_key"].value_or(cfg.online.app_key);
    cfg.online.access_key           = on["access_key"].value_or(cfg.online.access_key);
    cfg.online.api_key_env          = on["api_key_env"].value_or(cfg.online.api_key_env);
    cfg.online.app_key_env          = on["app_key_env"].value_or(cfg.online.app_key_env);
    cfg.online.access_key_env       = on["access_key_env"].value_or(cfg.online.access_key_env);
    cfg.online.resource_id          = on["resource_id"].value_or(cfg.online.resource_id);
    cfg.online.uid                  = on["uid"].value_or(cfg.online.uid);
    cfg.online.format               = on["format"].value_or(cfg.online.format);
    cfg.online.codec                = on["codec"].value_or(cfg.online.codec);
    cfg.online.language             = on["language"].value_or(cfg.online.language);
    cfg.online.sample_rate          = on["sample_rate"].value_or(cfg.online.sample_rate);
    cfg.online.channels             = on["channels"].value_or(cfg.online.channels);
    cfg.online.bits                 = on["bits"].value_or(cfg.online.bits);
    cfg.online.segment_duration_ms  = on["segment_duration_ms"].value_or(cfg.online.segment_duration_ms);
    cfg.online.connect_timeout_ms   = on["connect_timeout_ms"].value_or(cfg.online.connect_timeout_ms);
    cfg.online.receive_timeout_ms   = on["receive_timeout_ms"].value_or(cfg.online.receive_timeout_ms);
    cfg.online.silence_finalize_ms  = on["silence_finalize_ms"].value_or(cfg.online.silence_finalize_ms);
    cfg.online.max_retries          = on["max_retries"].value_or(cfg.online.max_retries);
    cfg.online.retry_backoff_ms     = on["retry_backoff_ms"].value_or(cfg.online.retry_backoff_ms);
    cfg.online.enable_punctuation   = on["enable_punctuation"].value_or(cfg.online.enable_punctuation);
    cfg.online.enable_itn           = on["enable_itn"].value_or(cfg.online.enable_itn);
    cfg.online.enable_ddc           = on["enable_ddc"].value_or(cfg.online.enable_ddc);
    cfg.online.show_utterances      = on["show_utterances"].value_or(cfg.online.show_utterances);
    cfg.online.result_type          = on["result_type"].value_or(cfg.online.result_type);
    cfg.online.enable_nonstream     = on["enable_nonstream"].value_or(cfg.online.enable_nonstream);
    cfg.online.enable_accelerate_text =
        on["enable_accelerate_text"].value_or(cfg.online.enable_accelerate_text);
    cfg.online.accelerate_score     = on["accelerate_score"].value_or(cfg.online.accelerate_score);
    cfg.online.end_window_size_ms   = on["end_window_size_ms"].value_or(cfg.online.end_window_size_ms);
    cfg.online.force_to_speech_time_ms =
        on["force_to_speech_time_ms"].value_or(cfg.online.force_to_speech_time_ms);
    cfg.online.vad_segment_duration_ms =
        on["vad_segment_duration_ms"].value_or(cfg.online.vad_segment_duration_ms);
    cfg.online.verify_ssl           = on["verify_ssl"].value_or(cfg.online.verify_ssl);
    cfg.online.debug                = on["debug"].value_or(cfg.online.debug);

    // -- logging ------------------------------------------------------------
    const toml::node_view log = asr["logging"];
    cfg.logging.log_dir            = log["log_dir"].value_or(cfg.logging.log_dir);
    cfg.logging.log_file           = log["log_file"].value_or(cfg.logging.log_file);
    cfg.logging.console            = log["console"].value_or(cfg.logging.console);
    cfg.logging.rotation_max_bytes = static_cast<std::size_t>(
        log["rotation_max_bytes"].value_or(static_cast<int64_t>(cfg.logging.rotation_max_bytes)));
    cfg.logging.rotation_max_files = log["rotation_max_files"].value_or(cfg.logging.rotation_max_files);

    return cfg;
}

} // namespace asr
