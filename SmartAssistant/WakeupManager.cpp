#include "WakeupManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
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

#include <sherpa-onnx/c-api/c-api.h>

#include "AudioManager.h"
#include "TTSManager.h"

namespace wakeup {

namespace {

constexpr int    kSvSampleRate  = 16000;
constexpr float  kInt16ToFloat  = 1.0f / 32768.0f;

inline std::int64_t nowMicros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Convert int16 interleaved samples to mono float32 (averaging channels) and
// also push to an int16 mono ring buffer for SV.
std::vector<float> toMonoFloat(const std::vector<std::int16_t>& interleaved,
                               int channels) {
    if (channels <= 0) channels = 1;
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

float cosineSimilarity(const float* a, const float* b, int dim) {
    double dot = 0.0;
    double na  = 0.0;
    double nb  = 0.0;
    for (int i = 0; i < dim; ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        na  += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        nb  += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    if (na <= 0.0 || nb <= 0.0) return 0.0f;
    return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
}

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

} // namespace

// ============================================================================
// Implementation
// ============================================================================

struct WakeupManager::Impl {
    // Borrowed reference (AudioManager outlives WakeupManager).
    audio::AudioManager* audio = nullptr;

    WakeupConfig         cfg;
    quill::Logger*       logger = nullptr;

    // sherpa-onnx KWS handles
    const SherpaOnnxKeywordSpotter* kws_spotter = nullptr;
    const SherpaOnnxOnlineStream*   kws_stream  = nullptr;

    // sherpa-onnx SV handles
    const SherpaOnnxSpeakerEmbeddingExtractor* sv_extractor = nullptr;
    int sv_dim = 0;

    // Mono int16 ring buffer to support SV's lookback window. Sized to hold
    // (analyze_window_ms + small slack) at AudioFrame sample_rate, but we
    // store at the *audio frame* sample-rate so sherpa-onnx will resample.
    std::deque<std::int16_t>   sv_ring;
    int                        sv_ring_sample_rate = 0;
    std::size_t                sv_ring_capacity    = 0;
    std::mutex                 sv_ring_mtx;

    // Voiceprint storage (kept in-memory for fast lookup).
    struct VoiceEntry {
        std::string         user_id;
        std::vector<float>  embedding;
    };
    std::vector<VoiceEntry>    voiceprints;
    std::mutex                 voiceprints_mtx;

    // FrameDispatcher consumer registration handle.
    audio::ConsumerHandle      consumer_handle = audio::kInvalidConsumerHandle;

    // Callback.
    WakeCallback               wake_cb;
    std::mutex                 wake_cb_mtx;

    // Optional borrowed TTS manager. WakeupManager only triggers an async
    // acknowledgement; ownership and lifecycle stay with the application.
    tts::TTSManager*           tts_manager = nullptr;
    std::unique_ptr<tts::TTSManager> owned_tts_manager;
    std::mutex                 tts_mtx;
    std::mt19937               ack_rng{std::random_device{}()};

    // State flags.
    std::atomic<bool>          listening{false};
    std::atomic<bool>          muted{false};
    std::atomic<bool>          initialized{false};

    // Cooldown after a successful wakeup, to avoid bursting.
    std::int64_t               last_trigger_us = 0;
    std::mutex                 cooldown_mtx;

    // ------------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------------
    bool initImpl(audio::AudioManager* am, WakeupConfig in_cfg);
    void shutdownImpl();

    // ------------------------------------------------------------------------
    // Engine creation
    // ------------------------------------------------------------------------
    bool createKws();
    bool createSv();

    // ------------------------------------------------------------------------
    // FrameDispatcher consumer
    // ------------------------------------------------------------------------
    void onFrame(const audio::AudioFrame& frame);
    void appendToSvRing(const std::vector<std::int16_t>& interleaved,
                        int channels);
    void feedKws(const std::vector<float>& mono, int sample_rate);
    void resetKwsStream();
    void playWakeAckAsync(const WakeupEvent& ev);
    std::string pickWakeAckPhrase();

    // ------------------------------------------------------------------------
    // SV evaluation
    // ------------------------------------------------------------------------
    bool runSvOnRing(std::vector<float>& embedding_out, float& best_score_out,
                     std::string& matched_user_out);

    // ------------------------------------------------------------------------
    // Logger setup
    // ------------------------------------------------------------------------
    void setupLogger();
};

// ============================================================================
// Logger
// ============================================================================
void WakeupManager::Impl::setupLogger() {
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
        // fall through: if file sink creation fails we still have console
    }

    if (cfg.logging.console) {
        auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "wakeup_console_sink");
        sinks.push_back(console_sink);
    }

    if (sinks.empty()) {
        auto fallback = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "wakeup_fallback_sink");
        sinks.push_back(fallback);
    }

    logger = quill::Frontend::create_or_get_logger("WakeupManager", std::move(sinks));
    logger->set_log_level(quill::LogLevel::Info);
}

// ============================================================================
// KWS engine creation
// ============================================================================
bool WakeupManager::Impl::createKws() {
    const auto& k = cfg.kws;
    if (!fileExists(k.encoder) || !fileExists(k.decoder) ||
        !fileExists(k.joiner)  || !fileExists(k.tokens)  ||
        !fileExists(k.keywords_file)) {
        LOG_ERROR(logger,
                  "KWS model files missing (encoder={}, decoder={}, joiner={}, "
                  "tokens={}, keywords={})",
                  k.encoder, k.decoder, k.joiner, k.tokens, k.keywords_file);
        return false;
    }

    SherpaOnnxKeywordSpotterConfig kc;
    std::memset(&kc, 0, sizeof(kc));

    kc.feat_config.sample_rate           = kSvSampleRate; // KWS model is 16k
    kc.feat_config.feature_dim           = k.feature_dim;

    kc.model_config.transducer.encoder   = k.encoder.c_str();
    kc.model_config.transducer.decoder   = k.decoder.c_str();
    kc.model_config.transducer.joiner    = k.joiner.c_str();
    kc.model_config.tokens               = k.tokens.c_str();
    kc.model_config.num_threads          = k.num_threads;
    kc.model_config.provider             = k.provider.c_str();
    kc.model_config.debug                = k.debug ? 1 : 0;

    kc.max_active_paths                  = k.max_active_paths;
    kc.num_trailing_blanks               = k.num_trailing_blanks;
    kc.keywords_score                    = k.keywords_score;
    kc.keywords_threshold                = k.keywords_threshold;
    kc.keywords_file                     = k.keywords_file.c_str();
    kc.keywords_buf                      = nullptr;
    kc.keywords_buf_size                 = 0;

    kws_spotter = SherpaOnnxCreateKeywordSpotter(&kc);
    if (!kws_spotter) {
        LOG_ERROR(logger, "SherpaOnnxCreateKeywordSpotter failed");
        return false;
    }
    kws_stream = SherpaOnnxCreateKeywordStream(kws_spotter);
    if (!kws_stream) {
        LOG_ERROR(logger, "SherpaOnnxCreateKeywordStream failed");
        SherpaOnnxDestroyKeywordSpotter(kws_spotter);
        kws_spotter = nullptr;
        return false;
    }

    LOG_INFO(logger,
             "KWS engine ready: keywords={}, threshold={}, score={}",
             k.keywords_file, k.keywords_threshold, k.keywords_score);
    return true;
}

// ============================================================================
// SV engine creation
// ============================================================================
bool WakeupManager::Impl::createSv() {
    if (!cfg.sv.enabled) {
        LOG_INFO(logger, "Speaker verification disabled by config");
        return true;
    }
    if (!fileExists(cfg.sv.model)) {
        LOG_ERROR(logger, "SV model file missing: {}", cfg.sv.model);
        return false;
    }

    SherpaOnnxSpeakerEmbeddingExtractorConfig sc;
    std::memset(&sc, 0, sizeof(sc));
    sc.model       = cfg.sv.model.c_str();
    sc.num_threads = cfg.sv.num_threads;
    sc.debug       = cfg.sv.debug ? 1 : 0;
    sc.provider    = cfg.sv.provider.c_str();

    sv_extractor = SherpaOnnxCreateSpeakerEmbeddingExtractor(&sc);
    if (!sv_extractor) {
        LOG_ERROR(logger, "SherpaOnnxCreateSpeakerEmbeddingExtractor failed");
        return false;
    }
    sv_dim = SherpaOnnxSpeakerEmbeddingExtractorDim(sv_extractor);
    LOG_INFO(logger, "SV engine ready: model={}, dim={}, threshold={:.3f}",
             cfg.sv.model, sv_dim, cfg.sv.threshold);
    return true;
}

// ============================================================================
// Frame handling
// ============================================================================
void WakeupManager::Impl::appendToSvRing(const std::vector<std::int16_t>& interleaved,
                                         int channels) {
    if (!cfg.sv.enabled || channels <= 0) return;

    std::lock_guard<std::mutex> lk(sv_ring_mtx);
    const std::size_t frames = interleaved.size() / static_cast<std::size_t>(channels);
    for (std::size_t i = 0; i < frames; ++i) {
        int sum = 0;
        for (int c = 0; c < channels; ++c) {
            sum += interleaved[i * channels + c];
        }
        sv_ring.push_back(static_cast<std::int16_t>(sum / channels));
    }
    while (sv_ring.size() > sv_ring_capacity) {
        sv_ring.pop_front();
    }
}

void WakeupManager::Impl::resetKwsStream() {
    if (kws_spotter && kws_stream) {
        SherpaOnnxResetKeywordStream(kws_spotter, kws_stream);
    }
}

std::string WakeupManager::Impl::pickWakeAckPhrase() {
    if (cfg.tts_ack.phrases.empty()) {
        return {};
    }
    std::uniform_int_distribution<std::size_t> dist(0, cfg.tts_ack.phrases.size() - 1);
    return cfg.tts_ack.phrases[dist(ack_rng)];
}

void WakeupManager::Impl::playWakeAckAsync(const WakeupEvent& ev) {
    if (!cfg.tts_ack.enabled || !ev.sv_passed) {
        return;
    }

    std::lock_guard<std::mutex> lk(tts_mtx);
    if (!tts_manager) {
        return;
    }
    const std::string phrase = pickWakeAckPhrase();
    if (phrase.empty()) {
        return;
    }

    tts::TTSOptions opts;
    opts.voice    = cfg.tts_ack.voice.empty() ? opts.voice : cfg.tts_ack.voice;
    opts.language = cfg.tts_ack.language;
    opts.speed    = cfg.tts_ack.speed;
    opts.volume   = cfg.tts_ack.volume;

    LOG_INFO(logger, "playing wake TTS ack: '{}'", phrase);
    const bool started = tts_manager->synthesizeAndPlay(
        phrase, opts,
        [logger = logger, phrase](const tts::TTSResult& result) {
            if (!logger) {
                return;
            }
            if (result.success) {
                LOG_INFO(logger, "wake TTS ack completed: '{}'", phrase);
            } else {
                LOG_WARNING(logger, "wake TTS ack failed: '{}' code={} message={}",
                            phrase, static_cast<int>(result.code), result.message);
            }
        });
    if (!started) {
        LOG_WARNING(logger, "wake TTS ack start rejected: '{}'", phrase);
    }
}

void WakeupManager::Impl::feedKws(const std::vector<float>& mono, int sample_rate) {
    if (!kws_spotter || !kws_stream || mono.empty()) return;

    SherpaOnnxOnlineStreamAcceptWaveform(
        kws_stream, sample_rate, mono.data(),
        static_cast<std::int32_t>(mono.size()));

    while (SherpaOnnxIsKeywordStreamReady(kws_spotter, kws_stream)) {
        SherpaOnnxDecodeKeywordStream(kws_spotter, kws_stream);
    }

    const SherpaOnnxKeywordResult* r =
        SherpaOnnxGetKeywordResult(kws_spotter, kws_stream);
    if (!r || !r->keyword || r->keyword[0] == '\0') {
        if (r) SherpaOnnxDestroyKeywordResult(r);
        return;
    }

    // Make a local copy of the keyword string before resetting the stream
    // (the underlying memory belongs to r and is freed by Destroy).
    const std::string keyword = r->keyword;
    const float       start_time = r->start_time;
    SherpaOnnxDestroyKeywordResult(r);

    // Always reset after a trigger so the next match can fire cleanly.
    resetKwsStream();

    // Cooldown gate.
    const std::int64_t now_us = nowMicros();
    {
        std::lock_guard<std::mutex> lk(cooldown_mtx);
        if (last_trigger_us != 0 &&
            (now_us - last_trigger_us) < static_cast<std::int64_t>(cfg.cooldown_ms) * 1000) {
            LOG_DEBUG(logger, "wakeup '{}' suppressed by cooldown", keyword);
            return;
        }
        last_trigger_us = now_us;
    }

    if (muted.load(std::memory_order_acquire)) {
        LOG_INFO(logger, "wakeup '{}' detected but muted (dialog in progress)", keyword);
        return;
    }

    LOG_INFO(logger, "KWS triggered: keyword='{}', start_time={:.3f}s",
             keyword, start_time);

    // Build the public WakeupEvent. SV happens synchronously inside the
    // FrameDispatcher worker thread (one of audio_consumer threads), which is
    // already off the realtime path.
    WakeupEvent ev;
    ev.keyword        = keyword;
    ev.kws_confidence = 1.0f; // sherpa KWS gives no scalar confidence
    ev.sv_enabled     = cfg.sv.enabled;
    ev.timestamp_us   = now_us;

    if (cfg.sv.enabled && sv_extractor) {
        std::vector<float> embedding;
        float              best_score = 0.0f;
        std::string        matched_user;
        const bool ok = runSvOnRing(embedding, best_score, matched_user);

        ev.embedding = std::move(embedding);
        ev.sv_score  = best_score;
        ev.user_id   = matched_user;
        ev.sv_passed = ok ? (best_score >= cfg.sv.threshold ||
                             !cfg.sv.require_enrolled_user)
                          : !cfg.sv.require_enrolled_user;

        LOG_INFO(logger, "SV result: best_score={:.3f}, threshold={:.3f}, "
                          "matched_user='{}', passed={}",
                 best_score, cfg.sv.threshold, matched_user, ev.sv_passed);
    }

    playWakeAckAsync(ev);

    WakeCallback cb;
    {
        std::lock_guard<std::mutex> lk(wake_cb_mtx);
        cb = wake_cb;
    }
    if (cb) {
        try {
            cb(ev);
        } catch (const std::exception& e) {
            LOG_ERROR(logger, "wake callback threw: {}", e.what());
        } catch (...) {
            LOG_ERROR(logger, "wake callback threw unknown exception");
        }
    } else {
        LOG_WARNING(logger, "wakeup '{}' fired but no callback registered", keyword);
    }
}

void WakeupManager::Impl::onFrame(const audio::AudioFrame& frame) {
    if (!listening.load(std::memory_order_acquire)) return;
    if (frame.samples.empty() || frame.channels <= 0 || frame.sample_rate <= 0) {
        return;
    }

    // Always buffer raw mono int16 audio for SV regardless of mute, so that on
    // unmute we don't lose the lookback window. But never feed KWS while muted
    // (KWS is only useful when actively listening for wake words).
    if (cfg.sv.enabled) {
        if (sv_ring_sample_rate != frame.sample_rate) {
            std::lock_guard<std::mutex> lk(sv_ring_mtx);
            sv_ring.clear();
            sv_ring_sample_rate = frame.sample_rate;
            const std::size_t target =
                static_cast<std::size_t>(cfg.sv.analyze_window_ms) *
                static_cast<std::size_t>(frame.sample_rate) / 1000;
            sv_ring_capacity = target + static_cast<std::size_t>(frame.sample_rate / 10);
        }
        appendToSvRing(frame.samples, frame.channels);
    }

    if (muted.load(std::memory_order_acquire)) {
        return;
    }

    // Convert to mono float32 and let sherpa-onnx handle the (input_rate ->
    // model_rate) resampling internally.
    auto mono = toMonoFloat(frame.samples, frame.channels);
    feedKws(mono, frame.sample_rate);
}

// ============================================================================
// SV evaluation
// ============================================================================
bool WakeupManager::Impl::runSvOnRing(std::vector<float>& embedding_out,
                                      float& best_score_out,
                                      std::string& matched_user_out) {
    embedding_out.clear();
    best_score_out   = 0.0f;
    matched_user_out.clear();

    if (!sv_extractor) return false;

    std::vector<std::int16_t> snapshot;
    int snapshot_rate = 0;
    {
        std::lock_guard<std::mutex> lk(sv_ring_mtx);
        snapshot.assign(sv_ring.begin(), sv_ring.end());
        snapshot_rate = sv_ring_sample_rate;
    }
    if (snapshot.empty() || snapshot_rate <= 0) {
        LOG_DEBUG(logger, "SV: ring buffer empty, skipping");
        return false;
    }

    // Trim to analyze_window_ms (keep the most recent portion).
    const std::size_t want = static_cast<std::size_t>(cfg.sv.analyze_window_ms) *
                             static_cast<std::size_t>(snapshot_rate) / 1000;
    if (snapshot.size() > want) {
        snapshot.erase(snapshot.begin(),
                       snapshot.begin() + (snapshot.size() - want));
    }

    std::vector<float> samples_f(snapshot.size());
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        samples_f[i] = static_cast<float>(snapshot[i]) * kInt16ToFloat;
    }

    const SherpaOnnxOnlineStream* stream =
        SherpaOnnxSpeakerEmbeddingExtractorCreateStream(sv_extractor);
    if (!stream) {
        LOG_ERROR(logger, "SV: failed to create extractor stream");
        return false;
    }

    SherpaOnnxOnlineStreamAcceptWaveform(stream, snapshot_rate, samples_f.data(),
                                         static_cast<std::int32_t>(samples_f.size()));
    SherpaOnnxOnlineStreamInputFinished(stream);

    if (!SherpaOnnxSpeakerEmbeddingExtractorIsReady(sv_extractor, stream)) {
        LOG_WARNING(logger, "SV: extractor not ready after {} samples ({} ms)",
                    samples_f.size(),
                    samples_f.size() * 1000ULL / static_cast<std::size_t>(snapshot_rate));
        SherpaOnnxDestroyOnlineStream(stream);
        return false;
    }

    const float* emb = SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(
        sv_extractor, stream);
    if (!emb) {
        LOG_ERROR(logger, "SV: ComputeEmbedding returned null");
        SherpaOnnxDestroyOnlineStream(stream);
        return false;
    }

    embedding_out.assign(emb, emb + sv_dim);
    SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(emb);
    SherpaOnnxDestroyOnlineStream(stream);

    // Compare against known voiceprints.
    {
        std::lock_guard<std::mutex> lk(voiceprints_mtx);
        for (const auto& vp : voiceprints) {
            if (static_cast<int>(vp.embedding.size()) != sv_dim) continue;
            const float s = cosineSimilarity(embedding_out.data(),
                                             vp.embedding.data(), sv_dim);
            if (s > best_score_out) {
                best_score_out   = s;
                matched_user_out = vp.user_id;
            }
        }
    }
    return true;
}

// ============================================================================
// initImpl / shutdownImpl
// ============================================================================
bool WakeupManager::Impl::initImpl(audio::AudioManager* am, WakeupConfig in_cfg) {
    if (initialized.load(std::memory_order_acquire)) {
        return true;
    }
    audio = am;
    cfg   = std::move(in_cfg);

    setupLogger();
    LOG_INFO(logger, "WakeupManager init: keywords=[{}], frame_queue_depth={}",
             [&]() {
                 std::string s;
                 for (std::size_t i = 0; i < cfg.keywords.size(); ++i) {
                     if (i) s += ",";
                     s += cfg.keywords[i];
                 }
                 return s;
             }(),
             cfg.frame_queue_depth);

    if (!audio) {
        LOG_ERROR(logger, "AudioManager pointer is null");
        return false;
    }

    if (!createKws()) {
        return false;
    }
    if (!createSv()) {
        LOG_WARNING(logger, "SV engine failed to initialize; continuing without SV");
        cfg.sv.enabled = false;
    }

    muted.store(cfg.mute_on_init, std::memory_order_release);

    initialized.store(true, std::memory_order_release);
    LOG_INFO(logger, "WakeupManager initialized (mute_on_init={})", cfg.mute_on_init);
    return true;
}

void WakeupManager::Impl::shutdownImpl() {
    if (!initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (audio && consumer_handle != audio::kInvalidConsumerHandle) {
        audio->removeFrameConsumer(consumer_handle);
        consumer_handle = audio::kInvalidConsumerHandle;
    }
    listening.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(tts_mtx);
        tts_manager = nullptr;
        owned_tts_manager.reset();
    }

    if (kws_stream) {
        SherpaOnnxDestroyOnlineStream(kws_stream);
        kws_stream = nullptr;
    }
    if (kws_spotter) {
        SherpaOnnxDestroyKeywordSpotter(kws_spotter);
        kws_spotter = nullptr;
    }
    if (sv_extractor) {
        SherpaOnnxDestroySpeakerEmbeddingExtractor(sv_extractor);
        sv_extractor = nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(sv_ring_mtx);
        sv_ring.clear();
    }
    if (logger) {
        LOG_INFO(logger, "WakeupManager shutdown complete");
        logger->flush_log();
    }
    audio = nullptr;
}

// ============================================================================
// WakeupManager facade
// ============================================================================
WakeupManager::WakeupManager() : impl_(std::make_unique<Impl>()) {}
WakeupManager::~WakeupManager() { shutdown(); }

bool WakeupManager::init(audio::AudioManager* audio_manager,
                         const std::string& config_path) {
    try {
        WakeupConfig cfg = loadConfig(config_path);
        const bool ok = impl_->initImpl(audio_manager, cfg);
        if (!ok || !cfg.tts_ack.enabled) {
            return ok;
        }

        auto owned_tts = std::make_unique<tts::TTSManager>();
        if (owned_tts->init(audio_manager, tts::TTSManager::loadConfig(config_path))) {
            std::lock_guard<std::mutex> lk(impl_->tts_mtx);
            impl_->owned_tts_manager = std::move(owned_tts);
            impl_->tts_manager = impl_->owned_tts_manager.get();
            LOG_INFO(impl_->logger, "Wakeup TTS ack manager initialized from {}", config_path);
        } else if (impl_->logger) {
            LOG_WARNING(impl_->logger,
                        "Wakeup TTS ack is enabled but TTSManager init failed");
        }
        return true;
    } catch (const std::exception& e) {
        // Logger may not exist yet; fall back to stderr-style log via Frontend.
        if (impl_->logger) {
            LOG_ERROR(impl_->logger, "init failed: {}", e.what());
        }
        return false;
    }
}

bool WakeupManager::init(audio::AudioManager* audio_manager,
                         const WakeupConfig& cfg) {
    return impl_->initImpl(audio_manager, cfg);
}

void WakeupManager::shutdown() {
    if (impl_) impl_->shutdownImpl();
}

void WakeupManager::startListening() {
    if (!impl_->initialized.load(std::memory_order_acquire)) {
        return;
    }
    if (impl_->listening.exchange(true, std::memory_order_acq_rel)) {
        return; // already listening
    }
    impl_->resetKwsStream();

    if (impl_->consumer_handle == audio::kInvalidConsumerHandle && impl_->audio) {
        auto self = impl_.get();
        audio::FrameCallback cb = [self](const audio::AudioFrame& f) {
            self->onFrame(f);
        };
        impl_->consumer_handle =
            impl_->audio->addFrameConsumer(std::move(cb), impl_->cfg.frame_queue_depth);
        LOG_INFO(impl_->logger,
                 "WakeupManager listening (consumer_handle={})",
                 static_cast<unsigned>(impl_->consumer_handle));
    } else {
        LOG_INFO(impl_->logger, "WakeupManager listening resumed");
    }
}

void WakeupManager::stopListening() {
    if (!impl_->listening.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (impl_->audio && impl_->consumer_handle != audio::kInvalidConsumerHandle) {
        impl_->audio->removeFrameConsumer(impl_->consumer_handle);
        impl_->consumer_handle = audio::kInvalidConsumerHandle;
    }
    LOG_INFO(impl_->logger, "WakeupManager stopped");
}

void WakeupManager::setWakeCallback(WakeCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->wake_cb_mtx);
    impl_->wake_cb = std::move(cb);
}

void WakeupManager::setTTSManager(tts::TTSManager* tts_manager) {
    std::lock_guard<std::mutex> lk(impl_->tts_mtx);
    if (tts_manager != impl_->owned_tts_manager.get()) {
        impl_->owned_tts_manager.reset();
    }
    impl_->tts_manager = tts_manager;
}

void WakeupManager::muteWakeup(bool mute) {
    const bool prev = impl_->muted.exchange(mute, std::memory_order_acq_rel);
    if (prev != mute && impl_->logger) {
        LOG_INFO(impl_->logger, "muteWakeup -> {}", mute);
        if (mute) {
            // Discard partially-decoded acoustic state so we don't fire when
            // unmuted on stale audio.
            impl_->resetKwsStream();
        }
    }
}

bool WakeupManager::isMuted() const {
    return impl_->muted.load(std::memory_order_acquire);
}

bool WakeupManager::addVoiceprint(const std::string& user_id,
                                  std::vector<float> embedding) {
    if (embedding.empty()) {
        return false;
    }
    if (impl_->sv_dim > 0 && static_cast<int>(embedding.size()) != impl_->sv_dim) {
        if (impl_->logger) {
            LOG_ERROR(impl_->logger,
                      "addVoiceprint dim mismatch: got {}, expected {}",
                      embedding.size(), impl_->sv_dim);
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->voiceprints_mtx);
        impl_->voiceprints.push_back({user_id, std::move(embedding)});
    }
    if (impl_->logger) {
        LOG_INFO(impl_->logger, "voiceprint added: user_id='{}'", user_id);
    }
    return true;
}

void WakeupManager::clearVoiceprints() {
    std::lock_guard<std::mutex> lk(impl_->voiceprints_mtx);
    impl_->voiceprints.clear();
}

int WakeupManager::embeddingDim() const {
    return impl_->sv_dim;
}

bool WakeupManager::isListening() const {
    return impl_->listening.load(std::memory_order_acquire);
}

const WakeupConfig& WakeupManager::config() const {
    return impl_->cfg;
}

// ============================================================================
// TOML loader
// ============================================================================
WakeupConfig WakeupManager::loadConfig(const std::string& config_path) {
    WakeupConfig cfg;

    toml::table table = toml::parse_file(config_path);
    const toml::node_view wakeup = table["wakeup"];

    if (const toml::array* kws_arr = wakeup["keywords"].as_array()) {
        std::vector<std::string> v;
        kws_arr->for_each([&v](const toml::value<std::string>& s) {
            v.push_back(*s);
        });
        if (!v.empty()) cfg.keywords = std::move(v);
    }
    cfg.sample_rate       = wakeup["sample_rate"].value_or(cfg.sample_rate);
    cfg.frame_queue_depth = static_cast<std::size_t>(
        wakeup["frame_queue_depth"].value_or(static_cast<int64_t>(cfg.frame_queue_depth)));
    cfg.mute_on_init      = wakeup["mute_on_init"].value_or(cfg.mute_on_init);
    cfg.cooldown_ms       = wakeup["cooldown_ms"].value_or(cfg.cooldown_ms);

    const toml::node_view ack_node = wakeup["tts_ack"];
    cfg.tts_ack.enabled = ack_node["enabled"].value_or(cfg.tts_ack.enabled);
    if (const toml::array* phrases = ack_node["phrases"].as_array()) {
        std::vector<std::string> parsed;
        phrases->for_each([&parsed](const toml::value<std::string>& s) {
            parsed.push_back(*s);
        });
        if (!parsed.empty()) {
            cfg.tts_ack.phrases = std::move(parsed);
        }
    }
    cfg.tts_ack.voice    = ack_node["voice"].value_or(cfg.tts_ack.voice);
    cfg.tts_ack.language = ack_node["language"].value_or(cfg.tts_ack.language);
    cfg.tts_ack.speed    = static_cast<float>(
        ack_node["speed"].value_or(static_cast<double>(cfg.tts_ack.speed)));
    cfg.tts_ack.volume   = static_cast<float>(
        ack_node["volume"].value_or(static_cast<double>(cfg.tts_ack.volume)));

    const toml::node_view kws_node = wakeup["kws"];
    cfg.kws.encoder            = kws_node["encoder"].value_or(cfg.kws.encoder);
    cfg.kws.decoder            = kws_node["decoder"].value_or(cfg.kws.decoder);
    cfg.kws.joiner             = kws_node["joiner"].value_or(cfg.kws.joiner);
    cfg.kws.tokens             = kws_node["tokens"].value_or(cfg.kws.tokens);
    cfg.kws.keywords_file      = kws_node["keywords_file"].value_or(cfg.kws.keywords_file);
    cfg.kws.provider           = kws_node["provider"].value_or(cfg.kws.provider);
    cfg.kws.num_threads        = kws_node["num_threads"].value_or(cfg.kws.num_threads);
    cfg.kws.max_active_paths   = kws_node["max_active_paths"].value_or(cfg.kws.max_active_paths);
    cfg.kws.keywords_score     = static_cast<float>(
        kws_node["keywords_score"].value_or(static_cast<double>(cfg.kws.keywords_score)));
    cfg.kws.keywords_threshold = static_cast<float>(
        kws_node["keywords_threshold"].value_or(static_cast<double>(cfg.kws.keywords_threshold)));
    cfg.kws.num_trailing_blanks = kws_node["num_trailing_blanks"].value_or(cfg.kws.num_trailing_blanks);
    cfg.kws.feature_dim        = kws_node["feature_dim"].value_or(cfg.kws.feature_dim);
    cfg.kws.debug              = kws_node["debug"].value_or(cfg.kws.debug);

    const toml::node_view sv_node = wakeup["sv"];
    cfg.sv.enabled               = sv_node["enabled"].value_or(cfg.sv.enabled);
    cfg.sv.model                 = sv_node["model"].value_or(cfg.sv.model);
    cfg.sv.provider              = sv_node["provider"].value_or(cfg.sv.provider);
    cfg.sv.num_threads           = sv_node["num_threads"].value_or(cfg.sv.num_threads);
    cfg.sv.threshold             = static_cast<float>(
        sv_node["threshold"].value_or(static_cast<double>(cfg.sv.threshold)));
    cfg.sv.sample_rate           = sv_node["sample_rate"].value_or(cfg.sv.sample_rate);
    cfg.sv.analyze_window_ms     = sv_node["analyze_window_ms"].value_or(cfg.sv.analyze_window_ms);
    cfg.sv.require_enrolled_user = sv_node["require_enrolled_user"].value_or(cfg.sv.require_enrolled_user);
    cfg.sv.debug                 = sv_node["debug"].value_or(cfg.sv.debug);

    const toml::node_view log_node = wakeup["logging"];
    cfg.logging.log_dir            = log_node["log_dir"].value_or(cfg.logging.log_dir);
    cfg.logging.log_file           = log_node["log_file"].value_or(cfg.logging.log_file);
    cfg.logging.console            = log_node["console"].value_or(cfg.logging.console);
    cfg.logging.rotation_max_bytes = static_cast<std::size_t>(
        log_node["rotation_max_bytes"].value_or(static_cast<int64_t>(cfg.logging.rotation_max_bytes)));
    cfg.logging.rotation_max_files = log_node["rotation_max_files"].value_or(cfg.logging.rotation_max_files);

    return cfg;
}

} // namespace wakeup
