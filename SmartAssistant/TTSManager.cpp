#include "TTSManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cmath>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
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

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessageType.h>

#if defined(SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS) && SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS
#include "SherpaOnnxTtsBridge.h"
#define SMARTASSISTANT_HAS_SHERPA_ONNX_TTS 1
#else
#define SMARTASSISTANT_HAS_SHERPA_ONNX_TTS 0
#endif

namespace tts {

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

inline float clampFloat(float v, float lo, float hi)
{
    return std::max(lo, std::min(v, hi));
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
        default:   out += c;      break;
        }
    }
    return out;
}

std::string shellQuote(const std::string& v)
{
    std::string out = "'";
    for (char c : v) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
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
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", v);
        out += buf;
    }
    return out;
}

std::string joinComma(const std::vector<std::string>& values)
{
    std::string out;
    for (const auto& value : values) {
        if (value.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ",";
        }
        out += value;
    }
    return out;
}

bool pathLike(const std::string& value)
{
    return value.find('/') != std::string::npos ||
           value.find('\\') != std::string::npos;
}

bool readBinaryFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool parseIntStrict(const std::string& value, int& out)
{
    if (value.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

void writeBE32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

std::uint32_t readBE32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
            static_cast<std::uint32_t>(p[3]);
}

std::uint32_t readLE32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[3]) << 24) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
            static_cast<std::uint32_t>(p[0]);
}

std::uint16_t readLE16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[1]) << 8) |
         static_cast<std::uint16_t>(p[0]));
}

std::vector<std::int16_t> bytesToPcm16LE(const std::vector<std::uint8_t>& bytes)
{
    std::vector<std::int16_t> pcm(bytes.size() / 2, 0);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const std::uint16_t lo = bytes[i * 2];
        const std::uint16_t hi = bytes[i * 2 + 1];
        pcm[i] = static_cast<std::int16_t>((hi << 8) | lo);
    }
    return pcm;
}

std::vector<std::int16_t> floatSamplesToPcm16(const std::vector<float>& samples)
{
    std::vector<std::int16_t> pcm;
    pcm.reserve(samples.size());
    for (float sample : samples) {
        sample = clampFloat(sample, -1.0f, 1.0f);
        pcm.push_back(static_cast<std::int16_t>(
            sample >= 0.0f ? sample * 32767.0f : sample * 32768.0f));
    }
    return pcm;
}

void addPcmSilence(std::vector<std::int16_t>& pcm,
                   int sample_rate,
                   int channels,
                   int leading_ms,
                   int trailing_ms)
{
    if (pcm.empty() || sample_rate <= 0 || channels <= 0) {
        return;
    }
    const auto silenceSamples = [sample_rate, channels](int ms) -> std::size_t {
        if (ms <= 0) {
            return 0;
        }
        return static_cast<std::size_t>(sample_rate) *
               static_cast<std::size_t>(channels) *
               static_cast<std::size_t>(ms) / 1000;
    };

    const std::size_t leading = silenceSamples(leading_ms);
    const std::size_t trailing = silenceSamples(trailing_ms);
    if (leading == 0 && trailing == 0) {
        return;
    }

    std::vector<std::int16_t> padded;
    padded.reserve(leading + pcm.size() + trailing);
    padded.insert(padded.end(), leading, 0);
    padded.insert(padded.end(), pcm.begin(), pcm.end());
    padded.insert(padded.end(), trailing, 0);
    pcm.swap(padded);
}

struct DecodedAudio {
    std::vector<std::int16_t> pcm;
    int sample_rate = 0;
    int channels    = 0;
};

DecodedAudio parseWavOrPcm(const std::vector<std::uint8_t>& bytes,
                           int fallback_rate,
                           int fallback_channels)
{
    DecodedAudio out;
    out.sample_rate = fallback_rate;
    out.channels    = fallback_channels;

    if (bytes.size() > 44 &&
        std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
        std::memcmp(bytes.data() + 8, "WAVE", 4) == 0) {
        std::size_t pos = 12;
        std::size_t data_offset = 0;
        std::size_t data_size = 0;
        while (pos + 8 <= bytes.size()) {
            const char* id = reinterpret_cast<const char*>(bytes.data() + pos);
            const std::uint32_t size = readLE32(bytes.data() + pos + 4);
            pos += 8;
            if (pos + size > bytes.size()) {
                break;
            }
            if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
                const std::uint16_t format = readLE16(bytes.data() + pos);
                const std::uint16_t channels = readLE16(bytes.data() + pos + 2);
                const std::uint32_t rate = readLE32(bytes.data() + pos + 4);
                const std::uint16_t bits = readLE16(bytes.data() + pos + 14);
                if (format == 1 && bits == 16 && channels > 0 && rate > 0) {
                    out.channels = channels;
                    out.sample_rate = static_cast<int>(rate);
                }
            } else if (std::memcmp(id, "data", 4) == 0) {
                data_offset = pos;
                data_size = size;
                break;
            }
            pos += size + (size & 1U);
        }
        if (data_offset != 0 && data_offset + data_size <= bytes.size()) {
            std::vector<std::uint8_t> pcm_bytes(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
                                                bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + data_size));
            out.pcm = bytesToPcm16LE(pcm_bytes);
            return out;
        }
    }

    out.pcm = bytesToPcm16LE(bytes);
    return out;
}

int speedToDoubaoRate(float speed)
{
    const float clamped = clampFloat(speed, 0.5f, 2.0f);
    return static_cast<int>(std::lround((clamped - 1.0f) * 100.0f));
}

int volumeToDoubaoRate(float volume)
{
    const float clamped = clampFloat(volume, 0.0f, 1.0f);
    return static_cast<int>(std::lround(clampFloat((clamped - 1.0f) * 100.0f, -50.0f, 100.0f)));
}

bool isLikelyDoubaoSpeakerResourceCompatible(const std::string& resource_id,
                                             const std::string& speaker)
{
    if (resource_id == "seed-tts-2.0") {
        return speaker.find("_uranus_bigtts") != std::string::npos;
    }
    if (resource_id == "seed-tts-1.0" ||
        resource_id == "seed-tts-1.0-concurr") {
        return speaker.find("_uranus_bigtts") == std::string::npos;
    }
    if (resource_id == "seed-icl-2.0") {
        return speaker.rfind("S_", 0) == 0 ||
               speaker.rfind("icl_", 0) == 0 ||
               speaker.find("_uranus_bigtts") != std::string::npos;
    }
    if (resource_id == "seed-icl-1.0" ||
        resource_id == "seed-icl-1.0-concurr") {
        return speaker.rfind("S_", 0) == 0 ||
               speaker.rfind("icl_", 0) == 0;
    }
    return true;
}

std::string normalizeDoubaoLanguage(std::string language)
{
    for (char& c : language) {
        if (c == '_') {
            c = '-';
        } else {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (language == "zh" || language == "zh-cn" || language == "zh-hans") {
        return "zh-cn";
    }
    if (language == "en" || language == "en-us" || language == "en-gb") {
        return "en";
    }
    return language;
}

std::string buildDoubaoAdditions(const OnlineTtsConfig& cfg,
                                 const std::string& explicit_language)
{
    std::vector<std::string> fields;
    if (!explicit_language.empty()) {
        fields.push_back("\"explicit_language\":\"" +
                         jsonEscape(explicit_language) + "\"");
    }

    if (cfg.use_cache || cfg.use_segment_cache) {
        std::vector<std::string> cache_fields;
        cache_fields.push_back("\"text_type\":" +
                               std::to_string(std::max(1, cfg.cache_text_type)));
        if (cfg.use_cache) {
            cache_fields.push_back("\"use_cache\":true");
        }
        if (cfg.use_segment_cache) {
            cache_fields.push_back("\"use_segment_cache\":true");
        }
        fields.push_back("\"cache_config\":{" + joinComma(cache_fields) + "}");
    }

    if (fields.empty()) {
        return "{}";
    }
    return "{" + joinComma(fields) + "}";
}

#if SMARTASSISTANT_HAS_SHERPA_ONNX_TTS
int32_t sherpaTtsShouldStop(void* arg)
{
    const auto* stop_requested = static_cast<const std::atomic<bool>*>(arg);
    if (stop_requested && stop_requested->load(std::memory_order_acquire)) {
        return 1;
    }
    return 0;
}
#endif

} // namespace

// ============================================================================
// Internal engine contract
// ============================================================================

struct SynthesisOutput {
    std::vector<std::int16_t> pcm;
    int sample_rate = 0;
    int channels    = 0;
    std::string message;
};

class ITtsEngine {
public:
    virtual ~ITtsEngine() = default;

    virtual EngineType type() const = 0;
    virtual bool       isAvailable() const = 0;
    virtual bool       synthesize(const std::string& text,
                                  const TTSOptions& opts,
                                  SynthesisOutput& out) = 0;
    virtual void       stop() = 0;
};

// ============================================================================
// OfflineSherpaOnnxTtsEngine - sherpa-onnx C++ API Kokoro adapter
// ============================================================================

class OfflineSherpaOnnxTtsEngine : public ITtsEngine {
public:
    OfflineSherpaOnnxTtsEngine(OfflineTtsConfig cfg, quill::Logger* logger)
        : cfg_(std::move(cfg)), logger_(logger) {}

    ~OfflineSherpaOnnxTtsEngine() override
    {
#if SMARTASSISTANT_HAS_SHERPA_ONNX_TTS
        if (tts_) {
            sherpa_bridge::destroy(tts_);
            tts_ = nullptr;
        }
#endif
    }

    EngineType type() const override { return EngineType::kOffline; }

    bool init()
    {
        if (!cfg_.enabled) {
            LOG_INFO(logger_, "Offline TTS disabled by config");
            return false;
        }

        if (cfg_.model_type != "kokoro") {
            LOG_WARNING(logger_,
                        "Offline TTS unavailable: unsupported sherpa-onnx model_type={}",
                        cfg_.model_type);
            available_ = false;
            return false;
        }

#if !SMARTASSISTANT_HAS_SHERPA_ONNX_TTS
        if (cfg_.executable.empty()) {
            LOG_WARNING(logger_, "Offline TTS unavailable: sherpa-onnx executable is empty");
            available_ = false;
            return false;
        }
        if (pathLike(cfg_.executable) && !fileExists(cfg_.executable)) {
            LOG_WARNING(logger_, "Offline TTS executable missing: {}", cfg_.executable);
            available_ = false;
            return false;
        }
#endif

        if (!fileExists(cfg_.kokoro_model)) {
            LOG_WARNING(logger_, "Offline TTS Kokoro model missing: {}", cfg_.kokoro_model);
            available_ = false;
            return false;
        }
        if (!fileExists(cfg_.kokoro_voices)) {
            LOG_WARNING(logger_, "Offline TTS Kokoro voices missing: {}", cfg_.kokoro_voices);
            available_ = false;
            return false;
        }
        if (!fileExists(cfg_.kokoro_tokens)) {
            LOG_WARNING(logger_, "Offline TTS Kokoro tokens missing: {}", cfg_.kokoro_tokens);
            available_ = false;
            return false;
        }
        if (!cfg_.kokoro_data_dir.empty() && !fileExists(cfg_.kokoro_data_dir)) {
            LOG_WARNING(logger_, "Offline TTS Kokoro espeak data dir missing: {}",
                        cfg_.kokoro_data_dir);
            available_ = false;
            return false;
        }
        for (const auto& path : cfg_.kokoro_lexicon) {
            if (!path.empty() && !fileExists(path)) {
                LOG_WARNING(logger_, "Offline TTS Kokoro lexicon missing: {}", path);
                available_ = false;
                return false;
            }
        }
        for (const auto& path : cfg_.rule_fsts) {
            if (!path.empty() && !fileExists(path)) {
                LOG_WARNING(logger_, "Offline TTS rule FST missing: {}", path);
                available_ = false;
                return false;
            }
        }

#if SMARTASSISTANT_HAS_SHERPA_ONNX_TTS
        lexicon_ = joinComma(cfg_.kokoro_lexicon);
        rule_fsts_ = joinComma(cfg_.rule_fsts);

        sherpa_bridge::Config bridge_cfg;
        bridge_cfg.model = cfg_.kokoro_model.c_str();
        bridge_cfg.voices = cfg_.kokoro_voices.c_str();
        bridge_cfg.tokens = cfg_.kokoro_tokens.c_str();
        bridge_cfg.data_dir = cfg_.kokoro_data_dir.c_str();
        bridge_cfg.lexicon = lexicon_.c_str();
        bridge_cfg.rule_fsts = rule_fsts_.c_str();
        bridge_cfg.num_threads = std::max(1, cfg_.num_threads);
        bridge_cfg.debug = cfg_.debug ? 1 : 0;

        char error[512] = {};
        tts_ = sherpa_bridge::create(bridge_cfg, error, sizeof(error));
        if (!tts_) {
            LOG_ERROR(logger_, "Offline TTS sherpa-onnx create failed: {}",
                      error[0] ? error : "unknown error");
            available_ = false;
            return false;
        }
#endif

        available_ = true;
        LOG_INFO(logger_,
                 "Offline TTS sherpa-onnx Kokoro ready: model={} sid={} api={}",
                 cfg_.kokoro_model, cfg_.sid,
#if SMARTASSISTANT_HAS_SHERPA_ONNX_TTS
                 "cxx"
#else
                 "cli"
#endif
        );
        if (cfg_.debug) {
            LOG_INFO(logger_,
                     "Offline TTS Kokoro assets: voices={} tokens={} data_dir={} lexicon={} rule_fsts={}",
                     cfg_.kokoro_voices, cfg_.kokoro_tokens, cfg_.kokoro_data_dir,
                     joinComma(cfg_.kokoro_lexicon), joinComma(cfg_.rule_fsts));
        }
        return true;
    }

    bool isAvailable() const override
    {
        return available_.load(std::memory_order_acquire);
    }

    bool synthesize(const std::string& text, const TTSOptions& opts, SynthesisOutput& out) override
    {
        if (!isAvailable() || text.empty()) {
            return false;
        }

        stop_requested_.store(false, std::memory_order_release);

#if SMARTASSISTANT_HAS_SHERPA_ONNX_TTS
        if (!tts_) {
            out.message = "offline sherpa-onnx TTS is not initialized";
            return false;
        }

        int sid = cfg_.sid;
        int sid_from_voice = 0;
        if (parseIntStrict(opts.voice, sid_from_voice)) {
            sid = sid_from_voice;
        }

        float speed = cfg_.speed;
        if (std::abs(opts.speed - 1.0f) > 0.001f) {
            speed = opts.speed;
        }
        speed = clampFloat(speed, 0.5f, 2.0f);

        sherpa_bridge::Audio audio;
        char error[512] = {};
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(tts_mtx_);
            ok = sherpa_bridge::generate(tts_, text.c_str(), sid, speed,
                                         sherpaTtsShouldStop, &stop_requested_,
                                         &audio, error, sizeof(error));
        }
        if (!ok) {
            out.message = std::string("sherpa-onnx offline TTS failed: ") +
                          (error[0] ? error : "unknown error");
            return false;
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
            sherpa_bridge::freeAudio(&audio);
            out.message = "offline TTS canceled";
            return false;
        }
        if (!audio.samples || audio.num_samples <= 0 || audio.sample_rate <= 0) {
            sherpa_bridge::freeAudio(&audio);
            out.message = "sherpa-onnx offline TTS produced no audio";
            return false;
        }

        out.pcm.reserve(static_cast<std::size_t>(audio.num_samples));
        for (int32_t i = 0; i < audio.num_samples; ++i) {
            float sample = clampFloat(audio.samples[i], -1.0f, 1.0f);
            out.pcm.push_back(static_cast<std::int16_t>(
                sample >= 0.0f ? sample * 32767.0f : sample * 32768.0f));
        }
        out.sample_rate = audio.sample_rate;
        out.channels = 1;
        out.message = "ok";
        sherpa_bridge::freeAudio(&audio);
        return !out.pcm.empty();
#else
        std::error_code ec;
        const std::filesystem::path tmp_path =
            std::filesystem::temp_directory_path(ec) /
            ("smartassistant-tts-" + makeRandomHex(8) + ".wav");
        if (ec) {
            out.message = "failed to resolve temporary directory";
            return false;
        }

        const std::string command = buildCommand(text, opts, tmp_path.string());
        if (command.empty()) {
            out.message = "offline sherpa-onnx TTS command is empty";
            return false;
        }

        const int rc = std::system(command.c_str());
        if (stop_requested_.load(std::memory_order_acquire)) {
            std::filesystem::remove(tmp_path, ec);
            out.message = "offline TTS canceled";
            return false;
        }
        if (rc != 0) {
            std::filesystem::remove(tmp_path, ec);
            out.message = "sherpa-onnx-offline-tts failed with code " + std::to_string(rc);
            return false;
        }

        std::vector<std::uint8_t> bytes;
        if (!readBinaryFile(tmp_path, bytes)) {
            std::filesystem::remove(tmp_path, ec);
            out.message = "failed to read sherpa-onnx TTS output";
            return false;
        }
        std::filesystem::remove(tmp_path, ec);

        if (bytes.empty()) {
            out.message = "sherpa-onnx-offline-tts produced no audio";
            return false;
        }

        DecodedAudio decoded = parseWavOrPcm(bytes, cfg_.sample_rate, cfg_.channels);
        out.pcm = std::move(decoded.pcm);
        out.sample_rate = decoded.sample_rate;
        out.channels = decoded.channels;
        out.message = "ok";
        return !out.pcm.empty() && out.sample_rate > 0 && out.channels > 0;
#endif
    }

    void stop() override
    {
        stop_requested_.store(true, std::memory_order_release);
    }

private:
    std::string buildCommand(const std::string& text,
                             const TTSOptions& opts,
                             const std::string& output_path) const
    {
        const int timeout_sec = cfg_.timeout_ms > 0 ? (cfg_.timeout_ms + 999) / 1000 : 0;
        int sid = cfg_.sid;
        int sid_from_voice = 0;
        if (parseIntStrict(opts.voice, sid_from_voice)) {
            sid = sid_from_voice;
        }

        float speed = cfg_.speed;
        if (std::abs(opts.speed - 1.0f) > 0.001f) {
            speed = opts.speed;
        }
        speed = clampFloat(speed, 0.5f, 2.0f);

        std::ostringstream oss;
        if (timeout_sec > 0) {
            oss << "timeout --kill-after=2s " << timeout_sec << "s ";
        }

        oss << shellQuote(cfg_.executable)
            << " --kokoro-model=" << shellQuote(cfg_.kokoro_model)
            << " --kokoro-voices=" << shellQuote(cfg_.kokoro_voices)
            << " --kokoro-tokens=" << shellQuote(cfg_.kokoro_tokens);

        if (!cfg_.kokoro_data_dir.empty()) {
            oss << " --kokoro-data-dir=" << shellQuote(cfg_.kokoro_data_dir);
        }

        const std::string lexicon = joinComma(cfg_.kokoro_lexicon);
        if (!lexicon.empty()) {
            oss << " --kokoro-lexicon=" << shellQuote(lexicon);
        }

        const std::string rule_fsts = joinComma(cfg_.rule_fsts);
        if (!rule_fsts.empty()) {
            oss << " --tts-rule-fsts=" << shellQuote(rule_fsts);
        }

        oss << " --sid=" << sid
            << " --speed=" << speed
            << " --num-threads=" << std::max(1, cfg_.num_threads)
            << " --output-filename=" << shellQuote(output_path)
            << " " << shellQuote(text);

        return oss.str();
    }

    OfflineTtsConfig cfg_;
    quill::Logger*   logger_ = nullptr;
#if SMARTASSISTANT_HAS_SHERPA_ONNX_TTS
    sherpa_bridge::Handle* tts_ = nullptr;
    std::string lexicon_;
    std::string rule_fsts_;
    std::mutex tts_mtx_;
#endif
    std::atomic<bool> available_{false};
    std::atomic<bool> stop_requested_{false};
};

// ============================================================================
// OnlineDoubaoTtsEngine - Doubao V3 bidirectional WebSocket TTS
// ============================================================================

class OnlineDoubaoTtsEngine : public ITtsEngine {
public:
    OnlineDoubaoTtsEngine(OnlineTtsConfig cfg, quill::Logger* logger)
        : cfg_(std::move(cfg)), logger_(logger)
    {
        cfg_.api_key = envOr(cfg_.api_key_env, cfg_.api_key);
    }

    ~OnlineDoubaoTtsEngine() override
    {
        shutdown();
    }

    bool init()
    {
        if (!cfg_.enabled) {
            LOG_INFO(logger_, "Online TTS disabled by config");
            return false;
        }
        if (cfg_.api_key.empty()) {
            LOG_WARNING(logger_,
                        "Online TTS credentials missing (env {}); online engine unavailable",
                        cfg_.api_key_env);
            credentials_ok_ = false;
        } else {
            credentials_ok_ = true;
        }

        ix::initNetSystem();
        net_inited_ = true;

        ws_.disableAutomaticReconnection();
        ws_.setHandshakeTimeout(std::max(1, cfg_.connect_timeout_ms / 1000));
        ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            onMessage(msg);
        });

        LOG_INFO(logger_,
                 "Online TTS engine ready: endpoint={}, resource_id={}, credentials={}, reuse_connection={}, preconnect={}, cache={}, segment_cache={}",
                 cfg_.endpoint, cfg_.resource_id, credentials_ok_ ? "present" : "missing",
                 cfg_.reuse_connection, cfg_.preconnect, cfg_.use_cache,
                 cfg_.use_segment_cache);
        if (cfg_.reuse_connection && cfg_.preconnect && credentials_ok_) {
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                start_session_after_connect_ = false;
            }
            startWebSocketConnection();
        }
        return true;
    }

    void shutdown()
    {
        closeWebSocketConnection(true);
        if (net_inited_) {
            ix::uninitNetSystem();
            net_inited_ = false;
        }
    }

    EngineType type() const override { return EngineType::kOnline; }

    bool isAvailable() const override
    {
        return cfg_.enabled && credentials_ok_;
    }

    bool synthesize(const std::string& text, const TTSOptions& opts, SynthesisOutput& out) override
    {
        if (!isAvailable()) {
            out.message = "online TTS unavailable";
            return false;
        }
        if (text.empty()) {
            out.message = "empty text";
            return false;
        }
        if (cfg_.audio_format != "pcm") {
            out.message = "only pcm output is supported by TTSManager playback path";
            return false;
        }
        if (!isLikelyDoubaoSpeakerResourceCompatible(cfg_.resource_id, opts.voice)) {
            out.message =
                "Doubao TTS resource_id/speaker mismatch: resource_id=" +
                cfg_.resource_id + ", speaker=" + opts.voice +
                ". Use a seed-tts-2.0 speaker such as zh_female_vv_uranus_bigtts "
                "with seed-tts-2.0, or switch resource_id to seed-tts-1.0 for "
                "1.0 speakers.";
            LOG_ERROR(logger_, "{}", out.message);
            return false;
        }

        std::unique_lock<std::mutex> session_lock(session_mtx_);
        resetSessionState(text, opts);
        prepareSessionTransport();

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(std::max(1000, cfg_.receive_timeout_ms));
        {
            rawCallbackLog("TTSManager", "Online TTS synthesize wait begin");
            std::unique_lock<std::mutex> lk(state_mtx_);
            state_cv_.wait_until(lk, deadline, [this] {
                return done_ || stop_requested_;
            });
            if (!done_ && !stop_requested_) {
                error_message_ = "online TTS receive timeout";
                error_code_ = TTSErrorCode::kTimeout;
                done_ = true;
                success_ = false;
            }
        }

        if (!cfg_.reuse_connection) {
            closeWebSocketConnection(true);
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
            session_lock.unlock();
            out.message = "online TTS canceled";
            return false;
        }

        bool ok = false;
        std::string message;
        TTSErrorCode code = TTSErrorCode::kOk;
        std::vector<std::uint8_t> audio_bytes;
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            ok = success_;
            message = error_message_.empty() ? "ok" : error_message_;
            code = error_code_;
            audio_bytes = audio_bytes_;
        }
        session_lock.unlock();

        if (!ok) {
            if (cfg_.reuse_connection && code != TTSErrorCode::kSynthesisFailed) {
                closeWebSocketConnection(false);
            }
            out.message = message;
            return false;
        }
        if (audio_bytes.empty()) {
            out.message = "online TTS returned no audio";
            return false;
        }

        out.pcm = bytesToPcm16LE(audio_bytes);
        out.sample_rate = cfg_.sample_rate;
        out.channels = 1;
        out.message = message;
        return !out.pcm.empty();
    }

    void stop() override
    {
        stop_requested_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            done_ = true;
            success_ = false;
            error_code_ = TTSErrorCode::kCanceled;
            error_message_ = "online TTS canceled";
            connection_ready_ = false;
            websocket_started_ = false;
            connection_error_ = "online TTS canceled";
            start_session_after_connect_ = false;
        }
        state_cv_.notify_all();
        // Make cancellation synchronous with the transport teardown.  If this
        // only queues an async close, the old connection's Close event can race
        // with the next synthesize() call and mark that new session as failed
        // with "Normal closure".
        rawCallbackLog("TTSManager", "Online TTS external stop close");
        ws_.stop();
    }

private:
    enum : std::uint8_t {
        kProtocolVersion = 0x1,
        kHeaderSize      = 0x1,
        kMsgFullClient   = 0x1,
        kMsgServerFull   = 0x9,
        kMsgServerAudio  = 0xb,
        kMsgServerError  = 0xf,
        kSerRaw          = 0x0,
        kSerJson         = 0x1,
        kCompNone        = 0x0,
        kFlagEvent       = 0x4,
    };

    enum EventCode : std::int32_t {
        kStartConnection    = 1,
        kFinishConnection   = 2,
        kConnectionStarted  = 50,
        kConnectionFailed   = 51,
        kConnectionFinished = 52,
        kStartSession       = 100,
        kCancelSession      = 101,
        kFinishSession      = 102,
        kSessionStarted     = 150,
        kSessionCanceled    = 151,
        kSessionFinished    = 152,
        kSessionFailed      = 153,
        kTaskRequest        = 200,
        kTtsSentenceStart   = 350,
        kTtsSentenceEnd     = 351,
        kTtsResponse        = 352,
    };

    struct ParsedFrame {
        std::uint8_t msg_type = 0;
        std::uint8_t serialization = 0;
        std::uint8_t compression = 0;
        std::int32_t event = 0;
        std::int32_t error_code = 0;
        std::string id;
        std::vector<std::uint8_t> payload;
    };

    void resetSessionState(const std::string& text, const TTSOptions& opts)
    {
        connect_id_ = makeRandomHex(16);
        session_id_ = makeRandomHex(16);
        request_text_ = text;
        request_opts_ = opts;
        stop_requested_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            done_ = false;
            success_ = false;
            session_started_ = false;
            error_code_ = TTSErrorCode::kOk;
            error_message_.clear();
            audio_bytes_.clear();
        }
    }

    static std::vector<std::uint8_t> buildClientFrame(std::int32_t event,
                                                       const std::string* session_id,
                                                       const std::string& payload)
    {
        std::vector<std::uint8_t> out;
        out.reserve(payload.size() + (session_id ? session_id->size() : 0) + 16);
        out.push_back(static_cast<std::uint8_t>((kProtocolVersion << 4) | kHeaderSize));
        out.push_back(static_cast<std::uint8_t>((kMsgFullClient << 4) | kFlagEvent));
        out.push_back(static_cast<std::uint8_t>((kSerJson << 4) | kCompNone));
        out.push_back(0x00);
        writeBE32(out, static_cast<std::uint32_t>(event));
        if (session_id) {
            writeBE32(out, static_cast<std::uint32_t>(session_id->size()));
            out.insert(out.end(), session_id->begin(), session_id->end());
        }
        writeBE32(out, static_cast<std::uint32_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    bool sendFrame(std::int32_t event,
                   const std::string* session_id,
                   const std::string& payload,
                   bool report_error = true)
    {
        const auto frame = buildClientFrame(event, session_id, payload);
        const bool ok = ws_.sendBinary(std::string(frame.begin(), frame.end())).success;
        if (!ok && report_error) {
            complete(false, TTSErrorCode::kNetworkFailure, "failed to send TTS WebSocket frame");
        }
        return ok;
    }

    bool sendStartConnection()
    {
        return sendFrame(kStartConnection, nullptr, "{}");
    }

    bool sendFinishConnection()
    {
        return sendFrame(kFinishConnection, nullptr, "{}");
    }

    bool sendStartSession()
    {
        const int speech_rate = speedToDoubaoRate(request_opts_.speed);
        const int loudness_rate = volumeToDoubaoRate(request_opts_.volume);
        const std::string explicit_language = normalizeDoubaoLanguage(request_opts_.language);
        const std::string additions = buildDoubaoAdditions(cfg_, explicit_language);

        LOG_INFO(logger_,
                 "Online TTS StartSession: resource_id={}, speaker={}, model={}, language={}, cache={}, segment_cache={}, reuse_connection={}",
                 cfg_.resource_id, request_opts_.voice, cfg_.model, explicit_language,
                 cfg_.use_cache, cfg_.use_segment_cache, cfg_.reuse_connection);

        std::ostringstream oss;
        oss << "{"
            << "\"user\":{\"uid\":\"" << jsonEscape(cfg_.uid) << "\"},"
            << "\"event\":100,"
            << "\"namespace\":\"BidirectionalTTS\","
            << "\"req_params\":{"
            <<   "\"model\":\"" << jsonEscape(cfg_.model) << "\","
            <<   "\"speaker\":\"" << jsonEscape(request_opts_.voice) << "\","
            <<   "\"audio_params\":{"
            <<     "\"format\":\"" << jsonEscape(cfg_.audio_format) << "\","
            <<     "\"sample_rate\":" << cfg_.sample_rate << ","
            <<     "\"speech_rate\":" << speech_rate << ","
            <<     "\"loudness_rate\":" << loudness_rate
            <<   "},"
            <<   "\"additions\":\"" << jsonEscape(additions) << "\""
            << "}"
            << "}";
        return sendFrame(kStartSession, &session_id_, oss.str());
    }

    bool sendTaskRequest()
    {
        std::ostringstream oss;
        oss << "{"
            << "\"event\":200,"
            << "\"namespace\":\"BidirectionalTTS\","
            << "\"req_params\":{"
            <<   "\"text\":\"" << jsonEscape(request_text_) << "\""
            << "}"
            << "}";
        return sendFrame(kTaskRequest, &session_id_, oss.str());
    }

    bool sendFinishSession()
    {
        return sendFrame(kFinishSession, &session_id_, "{}");
    }

    bool sendCancelSession()
    {
        return sendFrame(kCancelSession, &session_id_, "{}");
    }

    void startWebSocketConnection()
    {
        connect_id_ = makeRandomHex(16);

        ix::WebSocketHttpHeaders headers;
        headers["X-Api-Key"] = cfg_.api_key;
        headers["X-Api-Resource-Id"] = cfg_.resource_id;
        headers["X-Api-Connect-Id"] = connect_id_;
        if (cfg_.require_usage_tokens) {
            headers["X-Control-Require-Usage-Tokens-Return"] = "text_words";
        }
        ws_.setUrl(cfg_.endpoint);
        ws_.setExtraHeaders(headers);

        ix::SocketTLSOptions tls;
        tls.disable_hostname_validation = !cfg_.verify_ssl;
        tls.caFile = cfg_.verify_ssl ? "SYSTEM" : "NONE";
        ws_.setTLSOptions(tls);

        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            connection_ready_ = false;
            websocket_started_ = true;
            connection_error_.clear();
        }

        rawCallbackLog("TTSManager",
                       std::string("Online TTS ws start connect_id=") +
                       connect_id_);
        ws_.start();
    }

    void closeWebSocketConnection(bool graceful)
    {
        bool should_finish = false;
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            should_finish = connection_ready_;
            connection_ready_ = false;
            websocket_started_ = false;
            start_session_after_connect_ = false;
        }

        if (graceful && should_finish) {
            sendFrame(kFinishConnection, nullptr, "{}", false);
        }

        rawCallbackLog("TTSManager", "Online TTS ws stop begin");
        ws_.stop();
        rawCallbackLog("TTSManager", "Online TTS ws stop end");
    }

    void prepareSessionTransport()
    {
        bool send_session_now = false;
        bool start_new_connection = false;
        bool restart_failed_connection = false;

        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            if (cfg_.reuse_connection && connection_ready_) {
                send_session_now = true;
                start_session_after_connect_ = false;
            } else {
                start_session_after_connect_ = true;
                if (!cfg_.reuse_connection) {
                    websocket_started_ = false;
                    connection_ready_ = false;
                    connection_error_.clear();
                    start_new_connection = true;
                } else if (!connection_error_.empty()) {
                    restart_failed_connection = true;
                    start_new_connection = true;
                    websocket_started_ = false;
                    connection_ready_ = false;
                    connection_error_.clear();
                } else if (!websocket_started_) {
                    start_new_connection = true;
                }
            }
        }

        if (send_session_now) {
            sendStartSession();
            return;
        }
        if (restart_failed_connection) {
            ws_.stop();
        }
        if (start_new_connection) {
            startWebSocketConnection();
        }
    }

    void onMessage(const ix::WebSocketMessagePtr& msg)
    {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            rawCallbackLog("TTSManager",
                           std::string("Online TTS WebSocket opened: connect_id=") +
                           connect_id_);
            sendStartConnection();
            break;
        case ix::WebSocketMessageType::Close:
            {
                std::ostringstream oss;
                oss << "Online TTS WebSocket closed: code="
                    << msg->closeInfo.code << ", reason="
                    << msg->closeInfo.reason;
                rawCallbackLog("TTSManager", oss.str());
            }
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                connection_ready_ = false;
                websocket_started_ = false;
                connection_error_ = msg->closeInfo.reason.empty()
                    ? "Online TTS WebSocket closed"
                    : msg->closeInfo.reason;
                start_session_after_connect_ = false;
            }
            if (!isDone()) {
                complete(false, TTSErrorCode::kNetworkFailure, msg->closeInfo.reason);
            }
            break;
        case ix::WebSocketMessageType::Error:
            rawCallbackLog("TTSManager",
                           std::string("Online TTS WebSocket error: ") +
                           msg->errorInfo.reason);
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                connection_ready_ = false;
                websocket_started_ = false;
                connection_error_ = msg->errorInfo.reason.empty()
                    ? "Online TTS WebSocket error"
                    : msg->errorInfo.reason;
                start_session_after_connect_ = false;
            }
            complete(false, TTSErrorCode::kNetworkFailure, msg->errorInfo.reason);
            break;
        case ix::WebSocketMessageType::Message:
            if (msg->binary) {
                handleServerFrame(reinterpret_cast<const std::uint8_t*>(msg->str.data()),
                                  msg->str.size());
            } else if (!msg->str.empty()) {
                rawCallbackLog("TTSManager",
                               std::string("Online TTS text frame: ") + msg->str);
            }
            break;
        default:
            break;
        }
    }

    bool parseServerFrame(const std::uint8_t* buf, std::size_t size, ParsedFrame& out)
    {
        if (!buf || size < 4) {
            return false;
        }
        const std::size_t header_bytes = static_cast<std::size_t>(buf[0] & 0x0f) * 4;
        if (header_bytes < 4 || size < header_bytes) {
            return false;
        }
        out.msg_type = static_cast<std::uint8_t>((buf[1] >> 4) & 0x0f);
        const std::uint8_t flags = static_cast<std::uint8_t>(buf[1] & 0x0f);
        out.serialization = static_cast<std::uint8_t>((buf[2] >> 4) & 0x0f);
        out.compression = static_cast<std::uint8_t>(buf[2] & 0x0f);

        if (out.compression != kCompNone) {
            std::ostringstream oss;
            oss << "Online TTS compressed response is unsupported: "
                << static_cast<int>(out.compression);
            rawCallbackLog("TTSManager", oss.str());
            return false;
        }

        std::size_t offset = header_bytes;
        if (out.msg_type == kMsgServerError) {
            if (offset + 4 > size) {
                return false;
            }
            out.error_code = static_cast<std::int32_t>(readBE32(buf + offset));
            offset += 4;
            if (offset + 4 <= size) {
                const std::uint32_t maybe_size = readBE32(buf + offset);
                if (offset + 4 + maybe_size <= size) {
                    offset += 4;
                    out.payload.assign(buf + offset, buf + offset + maybe_size);
                    return true;
                }
            }
            out.payload.assign(buf + offset, buf + size);
            return true;
        }

        if ((flags & kFlagEvent) == kFlagEvent) {
            if (offset + 4 > size) {
                return false;
            }
            out.event = static_cast<std::int32_t>(readBE32(buf + offset));
            offset += 4;
        }

        const bool event_normally_has_id =
            out.event == kConnectionStarted ||
            out.event == kConnectionFailed ||
            out.event == kConnectionFinished ||
            out.event == kSessionStarted ||
            out.event == kSessionCanceled ||
            out.event == kSessionFinished ||
            out.event == kSessionFailed ||
            out.event == kTtsSentenceStart ||
            out.event == kTtsSentenceEnd ||
            out.event == kTtsResponse;

        if (event_normally_has_id && offset + 4 <= size) {
            const std::size_t id_offset = offset;
            const std::uint32_t id_size = readBE32(buf + offset);
            offset += 4;
            if (offset + id_size + 4 <= size) {
                const std::size_t payload_size_offset = offset + id_size;
                const std::uint32_t payload_size = readBE32(buf + payload_size_offset);
                if (payload_size_offset + 4 + payload_size <= size) {
                    out.id.assign(reinterpret_cast<const char*>(buf + offset), id_size);
                    offset = payload_size_offset;
                } else {
                    offset = id_offset;
                }
            } else {
                offset = id_offset;
            }
        }

        if (offset + 4 > size) {
            return false;
        }
        const std::uint32_t payload_size = readBE32(buf + offset);
        offset += 4;
        if (offset + payload_size > size) {
            return false;
        }
        out.payload.assign(buf + offset, buf + offset + payload_size);
        return true;
    }

    void handleServerFrame(const std::uint8_t* buf, std::size_t size)
    {
        ParsedFrame frame;
        if (!parseServerFrame(buf, size, frame)) {
            complete(false, TTSErrorCode::kProtocolError, "failed to parse TTS server frame");
            return;
        }

        if (frame.msg_type == kMsgServerError) {
            const std::string payload(frame.payload.begin(), frame.payload.end());
            std::ostringstream oss;
            oss << "Online TTS server error: code=" << frame.error_code
                << " payload=" << payload;
            rawCallbackLog("TTSManager", oss.str());
            complete(false, TTSErrorCode::kProtocolError, payload.empty() ? "server error" : payload);
            return;
        }

        if (cfg_.debug && frame.event != kTtsResponse) {
            const std::string payload(frame.payload.begin(), frame.payload.end());
            std::ostringstream oss;
            oss << "Online TTS event=" << frame.event
                << " type=" << static_cast<int>(frame.msg_type)
                << " payload=" << payload;
            rawCallbackLog("TTSManager", oss.str());
        }

        switch (frame.event) {
        case kConnectionStarted:
            {
                bool should_start_session = false;
                {
                    std::lock_guard<std::mutex> lk(state_mtx_);
                    connection_ready_ = true;
                    websocket_started_ = true;
                    connection_error_.clear();
                    should_start_session = start_session_after_connect_;
                    start_session_after_connect_ = false;
                }
                if (should_start_session) {
                    sendStartSession();
                }
            }
            break;
        case kConnectionFailed:
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                connection_ready_ = false;
                websocket_started_ = false;
                connection_error_ = std::string(frame.payload.begin(), frame.payload.end());
                start_session_after_connect_ = false;
            }
            complete(false, TTSErrorCode::kAuthFailure,
                     std::string(frame.payload.begin(), frame.payload.end()));
            break;
        case kSessionStarted:
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                session_started_ = true;
            }
            if (sendTaskRequest()) {
                sendFinishSession();
            }
            break;
        case kTtsResponse:
            if (!frame.payload.empty()) {
                std::lock_guard<std::mutex> lk(state_mtx_);
                audio_bytes_.insert(audio_bytes_.end(), frame.payload.begin(), frame.payload.end());
            }
            break;
        case kSessionFinished:
            complete(true, TTSErrorCode::kOk, "ok");
            break;
        case kSessionCanceled:
            complete(false, TTSErrorCode::kCanceled, "TTS session canceled");
            break;
        case kSessionFailed:
            complete(false, TTSErrorCode::kSynthesisFailed,
                     std::string(frame.payload.begin(), frame.payload.end()));
            break;
        case kConnectionFinished:
            {
                std::lock_guard<std::mutex> lk(state_mtx_);
                connection_ready_ = false;
                websocket_started_ = false;
                start_session_after_connect_ = false;
            }
            break;
        case kTtsSentenceStart:
        case kTtsSentenceEnd:
            break;
        default:
            if (frame.msg_type == kMsgServerAudio && !frame.payload.empty()) {
                std::lock_guard<std::mutex> lk(state_mtx_);
                audio_bytes_.insert(audio_bytes_.end(), frame.payload.begin(), frame.payload.end());
            }
            break;
        }
    }

    void complete(bool success, TTSErrorCode code, std::string message)
    {
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            if (done_) {
                return;
            }
            success_ = success;
            error_code_ = code;
            error_message_ = std::move(message);
            done_ = true;
        }
        state_cv_.notify_all();
    }

    bool isDone() const
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        return done_;
    }

    OnlineTtsConfig cfg_;
    quill::Logger*  logger_ = nullptr;

    ix::WebSocket   ws_;
    std::string     connect_id_;
    std::string     session_id_;
    std::string     request_text_;
    TTSOptions      request_opts_;

    mutable std::mutex      session_mtx_;
    mutable std::mutex      state_mtx_;
    std::condition_variable state_cv_;
    bool                    done_ = false;
    bool                    success_ = false;
    bool                    connection_ready_ = false;
    bool                    websocket_started_ = false;
    bool                    start_session_after_connect_ = false;
    bool                    session_started_ = false;
    TTSErrorCode            error_code_ = TTSErrorCode::kOk;
    std::string             error_message_;
    std::string             connection_error_;
    std::vector<std::uint8_t> audio_bytes_;
    std::atomic<bool>       stop_requested_{false};

    bool credentials_ok_ = false;
    bool net_inited_ = false;
};

// ============================================================================
// TTSManager::Impl
// ============================================================================

struct TTSManager::Impl {
    audio::AudioManager* audio = nullptr;
    TtsConfig            cfg;

    quill::Logger*       logger = nullptr;

    std::unique_ptr<OfflineSherpaOnnxTtsEngine> offline_engine;
    std::unique_ptr<OnlineDoubaoTtsEngine> online_engine;
    ITtsEngine* active_engine = nullptr;

    std::atomic<bool> initialized{false};
    std::atomic<bool> speaking{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> force_offline{false};
    std::atomic<EngineType> engine_pref{EngineType::kOnline};

    std::thread worker;
    std::mutex  worker_mtx;
    std::mutex  engine_mtx;

    struct PlaybackState {
        std::mutex              mtx;
        std::condition_variable cv;
        audio::PlaybackHandle   handle = audio::kInvalidPlaybackHandle;
        audio::AudioEventType   terminal = audio::AudioEventType::PlaybackCompleted;
        bool                    finished = false;
    };
    std::shared_ptr<PlaybackState> playback_state = std::make_shared<PlaybackState>();

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
            // Console sink below keeps the module observable.
        }

        if (cfg.logging.console) {
            auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "tts_console_sink");
            sinks.push_back(console_sink);
        }
        if (sinks.empty()) {
            auto fallback = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "tts_fallback_sink");
            sinks.push_back(fallback);
        }

        logger = quill::Frontend::create_or_get_logger("TTSManager", std::move(sinks));
        logger->set_log_level(quill::LogLevel::Info);
    }

    bool initImpl(audio::AudioManager* am, TtsConfig in_cfg)
    {
        if (initialized.load(std::memory_order_acquire)) {
            return true;
        }
        audio = am;
        cfg = std::move(in_cfg);

        setupLogger();
        LOG_INFO(logger,
                 "TTSManager init: preferred={}, fallback_to_offline={}",
                 cfg.preferred_engine == EngineType::kOnline ? "online" : "offline",
                 cfg.fallback_to_offline);

        if (!audio) {
            LOG_ERROR(logger, "AudioManager pointer is null");
            return false;
        }

        engine_pref.store(cfg.preferred_engine, std::memory_order_release);

        offline_engine = std::make_unique<OfflineSherpaOnnxTtsEngine>(cfg.offline, logger);
        online_engine = std::make_unique<OnlineDoubaoTtsEngine>(cfg.online, logger);

        const bool offline_ok = offline_engine->init();
        const bool online_ok = online_engine->init();

        if (!offline_ok && !online_ok) {
            LOG_ERROR(logger, "TTSManager init: neither online nor offline engine is available");
        }

        active_engine = selectEngine();

        std::weak_ptr<PlaybackState> weak_state = playback_state;
        audio->subscribe([weak_state](const audio::AudioEvent& ev) {
            auto state = weak_state.lock();
            if (!state) {
                return;
            }
            if (ev.type != audio::AudioEventType::PlaybackCompleted &&
                ev.type != audio::AudioEventType::PlaybackInterrupted &&
                ev.type != audio::AudioEventType::PlaybackError) {
                return;
            }
            std::lock_guard<std::mutex> lk(state->mtx);
            if (state->handle != audio::kInvalidPlaybackHandle &&
                state->handle == ev.handle) {
                state->terminal = ev.type;
                state->finished = true;
                state->cv.notify_all();
            }
        });

        initialized.store(true, std::memory_order_release);
        LOG_INFO(logger, "TTSManager initialized (active engine: {})",
                 active_engine ? (active_engine->type() == EngineType::kOnline ? "online" : "offline")
                               : "none");
        return true;
    }

    ITtsEngine* selectEngine()
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

    void shutdownImpl()
    {
        if (!initialized.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        stopImpl(true);
        offline_engine.reset();
        online_engine.reset();
        active_engine = nullptr;
        if (logger) {
            LOG_INFO(logger, "TTSManager shutdown complete");
            logger->flush_log();
        }
        audio = nullptr;
    }

    bool synthesizeAndPlayImpl(std::string text, TTSOptions opts, TTSCallback cb)
    {
        if (!initialized.load(std::memory_order_acquire)) {
            if (logger) {
                LOG_WARNING(logger, "synthesizeAndPlay called before init");
            }
            if (cb) {
                cb({false, TTSErrorCode::kNotInitialized,
                    "TTSManager not initialized", EngineType::kOffline,
                    audio::kInvalidPlaybackHandle, nowMicros()});
            }
            return false;
        }
        if (text.empty()) {
            if (cb) {
                cb({false, TTSErrorCode::kSynthesisFailed,
                    "empty text", currentEngineImpl(),
                    audio::kInvalidPlaybackHandle, nowMicros()});
            }
            return false;
        }

        stopImpl();

        {
            std::lock_guard<std::mutex> lk(playback_state->mtx);
            playback_state->handle = audio::kInvalidPlaybackHandle;
            playback_state->finished = false;
            playback_state->terminal = audio::AudioEventType::PlaybackCompleted;
        }

        stop_requested.store(false, std::memory_order_release);
        speaking.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> lk(worker_mtx);
        worker = std::thread(&Impl::workerLoop, this, std::move(text), std::move(opts), std::move(cb));
        return true;
    }

    void stopImpl(bool force_engine_stop = false)
    {
        const bool was_speaking = speaking.load(std::memory_order_acquire);
        stop_requested.store(true, std::memory_order_release);
        rawCallbackLog("TTSManager", "stopImpl begin");

        ITtsEngine* eng = nullptr;
        {
            std::lock_guard<std::mutex> lk(engine_mtx);
            eng = active_engine;
        }
        if (eng && (was_speaking || force_engine_stop)) {
            rawCallbackLog("TTSManager", "stopImpl engine stop begin");
            eng->stop();
            rawCallbackLog("TTSManager", "stopImpl engine stop end");
        }

        audio::PlaybackHandle h = audio::kInvalidPlaybackHandle;
        {
            std::lock_guard<std::mutex> lk(playback_state->mtx);
            h = playback_state->handle;
            playback_state->finished = true;
            playback_state->terminal = audio::AudioEventType::PlaybackInterrupted;
        }
        playback_state->cv.notify_all();

        if (audio && h != audio::kInvalidPlaybackHandle) {
            rawCallbackLog("TTSManager", "stopImpl audio stop begin");
            audio->stop(h);
            rawCallbackLog("TTSManager", "stopImpl audio stop end");
        }

        std::lock_guard<std::mutex> lk(worker_mtx);
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            rawCallbackLog("TTSManager", "stopImpl worker join begin");
            worker.join();
            rawCallbackLog("TTSManager", "stopImpl worker join end");
        }
        speaking.store(false, std::memory_order_release);
        rawCallbackLog("TTSManager", "stopImpl complete");
    }

    EngineType currentEngineImpl() const
    {
        if (active_engine) {
            return active_engine->type();
        }
        return engine_pref.load(std::memory_order_acquire);
    }

    void onNetworkChangedImpl(network_manager::NetworkState s)
    {
        const bool was_offline = force_offline.load(std::memory_order_acquire);
        const bool should_force_offline = (s == network_manager::NetworkState::Offline);
        if (was_offline == should_force_offline) {
            return;
        }

        force_offline.store(should_force_offline, std::memory_order_release);
        if (logger) {
            LOG_INFO(logger, "Network state changed to {} -> TTS engine switching",
                     network_manager::NetworkManager::toString(s));
        }

        if (!speaking.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(engine_mtx);
            active_engine = selectEngine();
            if (logger) {
                LOG_INFO(logger, "Active TTS engine pre-selected: {}",
                         active_engine ? (active_engine->type() == EngineType::kOnline ? "online" : "offline")
                                       : "none");
            }
        }
    }

    void workerLoop(std::string text, TTSOptions opts, TTSCallback cb)
    {
        TTSResult result;
        result.timestamp_us = nowMicros();

        ITtsEngine* eng = nullptr;
        {
            std::lock_guard<std::mutex> lk(engine_mtx);
            active_engine = selectEngine();
            eng = active_engine;
        }

        if (!eng) {
            finishCallback(cb, {false, TTSErrorCode::kEngineUnavailable,
                                "no TTS engine available", EngineType::kOffline,
                                audio::kInvalidPlaybackHandle, nowMicros()});
            speaking.store(false, std::memory_order_release);
            return;
        }

        SynthesisOutput audio_out;
        result.engine = eng->type();
        bool ok = false;

        // Online engine: retry up to cfg.online.max_retries times before
        // falling back to the offline engine.  Offline engine isn't retried —
        // it's deterministic local synthesis.
        if (eng->type() == EngineType::kOnline) {
            const int max_attempts = std::max(1, cfg.online.max_retries);
            for (int attempt = 1; attempt <= max_attempts; ++attempt) {
                if (stop_requested.load(std::memory_order_acquire)) break;
                audio_out = {};
                ok = eng->synthesize(text, opts, audio_out);
                if (ok) break;
                LOG_WARNING(logger,
                            "Online TTS attempt {}/{} failed: {}",
                            attempt, max_attempts, audio_out.message);
                if (attempt < max_attempts && cfg.online.retry_backoff_ms > 0 &&
                    !stop_requested.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(cfg.online.retry_backoff_ms));
                }
            }
        } else {
            ok = eng->synthesize(text, opts, audio_out);
        }

        if (!ok && cfg.fallback_to_offline && eng->type() == EngineType::kOnline &&
            offline_engine && offline_engine->isAvailable() &&
            !stop_requested.load(std::memory_order_acquire)) {
            LOG_WARNING(logger, "Online TTS exhausted retries ({}); trying offline fallback",
                        audio_out.message);
            {
                std::lock_guard<std::mutex> lk(engine_mtx);
                active_engine = offline_engine.get();
                eng = active_engine;
            }
            audio_out = {};
            result.engine = EngineType::kOffline;
            ok = eng->synthesize(text, opts, audio_out);
        }

        if (stop_requested.load(std::memory_order_acquire)) {
            finishCallback(cb, {false, TTSErrorCode::kCanceled,
                                "TTS canceled", result.engine,
                                audio::kInvalidPlaybackHandle, nowMicros()});
            speaking.store(false, std::memory_order_release);
            return;
        }

        if (!ok || audio_out.pcm.empty() || audio_out.sample_rate <= 0 || audio_out.channels <= 0) {
            finishCallback(cb, {false, TTSErrorCode::kSynthesisFailed,
                                audio_out.message.empty() ? "TTS synthesis failed" : audio_out.message,
                                result.engine, audio::kInvalidPlaybackHandle, nowMicros()});
            speaking.store(false, std::memory_order_release);
            return;
        }

        addPcmSilence(audio_out.pcm,
                      audio_out.sample_rate,
                      audio_out.channels,
                      cfg.leading_silence_ms,
                      cfg.trailing_silence_ms);

        audio::PlaybackRequest req;
        req.pcm_data = std::move(audio_out.pcm);
        req.sample_rate = audio_out.sample_rate;
        req.channels = audio_out.channels;
        req.priority = audio::PlaybackPriority::TTS;
        req.stream_gain = clampFloat(opts.volume, 0.0f, 1.0f);
        req.loop = false;

        const audio::PlaybackHandle handle = audio ? audio->play(req) : audio::kInvalidPlaybackHandle;
        if (handle == audio::kInvalidPlaybackHandle) {
            finishCallback(cb, {false, TTSErrorCode::kPlaybackFailed,
                                "AudioManager refused TTS playback",
                                result.engine, audio::kInvalidPlaybackHandle, nowMicros()});
            speaking.store(false, std::memory_order_release);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(playback_state->mtx);
            playback_state->handle = handle;
            playback_state->finished = false;
            playback_state->terminal = audio::AudioEventType::PlaybackCompleted;
        }

        bool playback_ok = false;
        {
            std::unique_lock<std::mutex> lk(playback_state->mtx);
            playback_state->cv.wait(lk, [this] {
                return playback_state->finished ||
                       stop_requested.load(std::memory_order_acquire);
            });
            playback_ok = playback_state->finished &&
                          playback_state->terminal == audio::AudioEventType::PlaybackCompleted &&
                          !stop_requested.load(std::memory_order_acquire);
            playback_state->handle = audio::kInvalidPlaybackHandle;
        }

        if (playback_ok) {
            finishCallback(cb, {true, TTSErrorCode::kOk, "ok",
                                result.engine, handle, nowMicros()});
        } else {
            finishCallback(cb, {false,
                                stop_requested.load(std::memory_order_acquire)
                                    ? TTSErrorCode::kCanceled
                                    : TTSErrorCode::kPlaybackFailed,
                                stop_requested.load(std::memory_order_acquire)
                                    ? "TTS canceled"
                                    : "TTS playback interrupted",
                                result.engine, handle, nowMicros()});
        }

        speaking.store(false, std::memory_order_release);
    }

    static void finishCallback(const TTSCallback& cb, const TTSResult& result)
    {
        if (cb) {
            try {
                cb(result);
            } catch (...) {
            }
        }
    }
};

// ============================================================================
// TTSManager facade
// ============================================================================

TTSManager::TTSManager() : impl_(std::make_unique<Impl>()) {}

TTSManager::~TTSManager()
{
    shutdown();
}

bool TTSManager::init(audio::AudioManager* audio_manager, const std::string& config_path)
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

bool TTSManager::init(audio::AudioManager* audio_manager, const TtsConfig& cfg)
{
    return impl_->initImpl(audio_manager, cfg);
}

void TTSManager::shutdown()
{
    if (impl_) {
        impl_->shutdownImpl();
    }
}

bool TTSManager::synthesizeAndPlay(const std::string& text,
                                   const TTSOptions& opts,
                                   TTSCallback on_complete)
{
    return impl_->synthesizeAndPlayImpl(text, opts, std::move(on_complete));
}

void TTSManager::stop()
{
    impl_->stopImpl();
}

EngineType TTSManager::currentEngine() const
{
    return impl_->currentEngineImpl();
}

void TTSManager::onNetworkStateChanged(network_manager::NetworkState s)
{
    impl_->onNetworkChangedImpl(s);
}

bool TTSManager::isOnlineAvailable() const
{
    return impl_->online_engine && impl_->online_engine->isAvailable() &&
           !impl_->force_offline.load(std::memory_order_acquire);
}

bool TTSManager::isSpeaking() const
{
    return impl_->speaking.load(std::memory_order_acquire);
}

const TtsConfig& TTSManager::config() const
{
    return impl_->cfg;
}

// ============================================================================
// TOML loader
// ============================================================================

TtsConfig TTSManager::loadConfig(const std::string& config_path)
{
    TtsConfig cfg;
    toml::table table = toml::parse_file(config_path);

    toml::node_view<toml::node> root = table["tts"];
    if (!root) {
        root = table["engines"]["tts"];
    }

    const std::string pref_str = root["preferred_engine"].value_or(std::string("online"));
    cfg.preferred_engine = (pref_str == "offline") ? EngineType::kOffline : EngineType::kOnline;
    cfg.fallback_to_offline = root["fallback_to_offline"].value_or(cfg.fallback_to_offline);
    cfg.leading_silence_ms  = root["leading_silence_ms"].value_or(cfg.leading_silence_ms);
    cfg.trailing_silence_ms = root["trailing_silence_ms"].value_or(cfg.trailing_silence_ms);

    const toml::node_view off = root["offline"];
    const auto readStringArray = [](const toml::node_view<toml::node>& node,
                                    std::vector<std::string> fallback) {
        if (const auto* arr = node.as_array()) {
            std::vector<std::string> out;
            for (const auto& item : *arr) {
                if (auto value = item.value<std::string>()) {
                    out.push_back(*value);
                }
            }
            return out;
        }
        return fallback;
    };
    cfg.offline.enabled         = off["enabled"].value_or(cfg.offline.enabled);
    cfg.offline.executable      = off["executable"].value_or(cfg.offline.executable);
    cfg.offline.model_type      = off["model_type"].value_or(cfg.offline.model_type);
    cfg.offline.kokoro_model    = off["kokoro_model"].value_or(
        off["model_path"].value_or(cfg.offline.kokoro_model));
    cfg.offline.kokoro_voices   = off["kokoro_voices"].value_or(cfg.offline.kokoro_voices);
    cfg.offline.kokoro_tokens   = off["kokoro_tokens"].value_or(cfg.offline.kokoro_tokens);
    cfg.offline.kokoro_data_dir = off["kokoro_data_dir"].value_or(cfg.offline.kokoro_data_dir);
    cfg.offline.kokoro_lexicon  = readStringArray(off["kokoro_lexicon"], cfg.offline.kokoro_lexicon);
    cfg.offline.rule_fsts       = readStringArray(off["rule_fsts"], cfg.offline.rule_fsts);
    cfg.offline.sid             = off["sid"].value_or(cfg.offline.sid);
    cfg.offline.speed           = off["speed"].value_or(cfg.offline.speed);
    cfg.offline.num_threads     = off["num_threads"].value_or(cfg.offline.num_threads);
    cfg.offline.sample_rate     = off["sample_rate"].value_or(cfg.offline.sample_rate);
    cfg.offline.channels        = off["channels"].value_or(cfg.offline.channels);
    cfg.offline.timeout_ms      = off["timeout_ms"].value_or(cfg.offline.timeout_ms);
    cfg.offline.debug           = off["debug"].value_or(cfg.offline.debug);

    const toml::node_view on = root["online"];
    cfg.online.enabled             = on["enabled"].value_or(cfg.online.enabled);
    cfg.online.endpoint            = on["endpoint"].value_or(cfg.online.endpoint);
    cfg.online.api_key             = on["api_key"].value_or(
        on["app_key"].value_or(cfg.online.api_key));
    cfg.online.api_key_env         = on["api_key_env"].value_or(
        on["app_key_env"].value_or(cfg.online.api_key_env));
    cfg.online.resource_id         = on["resource_id"].value_or(cfg.online.resource_id);
    cfg.online.uid                 = on["uid"].value_or(cfg.online.uid);
    cfg.online.model               = on["model"].value_or(cfg.online.model);
    cfg.online.default_voice       = on["default_voice"].value_or(cfg.online.default_voice);
    cfg.online.audio_format        = on["audio_format"].value_or(
        on["format"].value_or(cfg.online.audio_format));
    cfg.online.sample_rate         = on["sample_rate"].value_or(cfg.online.sample_rate);
    cfg.online.bit_rate            = on["bit_rate"].value_or(cfg.online.bit_rate);
    cfg.online.connect_timeout_ms  = on["connect_timeout_ms"].value_or(cfg.online.connect_timeout_ms);
    cfg.online.receive_timeout_ms  = on["receive_timeout_ms"].value_or(cfg.online.receive_timeout_ms);
    cfg.online.max_retries         = on["max_retries"].value_or(cfg.online.max_retries);
    cfg.online.retry_backoff_ms    = on["retry_backoff_ms"].value_or(cfg.online.retry_backoff_ms);
    cfg.online.reuse_connection    = on["reuse_connection"].value_or(cfg.online.reuse_connection);
    cfg.online.preconnect          = on["preconnect"].value_or(cfg.online.preconnect);
    cfg.online.use_cache           = on["use_cache"].value_or(cfg.online.use_cache);
    cfg.online.use_segment_cache   = on["use_segment_cache"].value_or(cfg.online.use_segment_cache);
    cfg.online.cache_text_type     = on["cache_text_type"].value_or(cfg.online.cache_text_type);
    cfg.online.verify_ssl          = on["verify_ssl"].value_or(cfg.online.verify_ssl);
    cfg.online.require_usage_tokens = on["require_usage_tokens"].value_or(cfg.online.require_usage_tokens);
    cfg.online.debug               = on["debug"].value_or(cfg.online.debug);

    const toml::node_view log = root["logging"];
    cfg.logging.log_dir            = log["log_dir"].value_or(cfg.logging.log_dir);
    cfg.logging.log_file           = log["log_file"].value_or(cfg.logging.log_file);
    cfg.logging.console            = log["console"].value_or(cfg.logging.console);
    cfg.logging.rotation_max_bytes = static_cast<std::size_t>(
        log["rotation_max_bytes"].value_or(static_cast<int64_t>(cfg.logging.rotation_max_bytes)));
    cfg.logging.rotation_max_files = log["rotation_max_files"].value_or(cfg.logging.rotation_max_files);

    return cfg;
}

} // namespace tts
