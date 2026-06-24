#include "SherpaOnnxTtsBridge.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <utility>
#include <string>

#include "sherpa-onnx/c-api/cxx-api.h"

namespace tts::sherpa_bridge {

namespace {

void setError(char* error, std::size_t error_size, const std::string& message)
{
    if (!error || error_size == 0) {
        return;
    }
    std::snprintf(error, error_size, "%s", message.c_str());
}

int32_t progressCallback(const float*, int32_t, float, void* arg)
{
    auto* ctx = static_cast<std::pair<ShouldStopFn, void*>*>(arg);
    if (!ctx || !ctx->first) {
        return 1;
    }
    return ctx->first(ctx->second) ? 0 : 1;
}

} // namespace

struct Handle {
    explicit Handle(sherpa_onnx::cxx::OfflineTts&& t) : tts(std::move(t)) {}
    sherpa_onnx::cxx::OfflineTts tts;
};

Handle* create(const Config& config, char* error, std::size_t error_size)
{
    try {
        sherpa_onnx::cxx::OfflineTtsConfig sherpa_config;
        sherpa_config.model.kokoro.model = config.model ? config.model : "";
        sherpa_config.model.kokoro.voices = config.voices ? config.voices : "";
        sherpa_config.model.kokoro.tokens = config.tokens ? config.tokens : "";
        sherpa_config.model.kokoro.data_dir = config.data_dir ? config.data_dir : "";
        sherpa_config.model.kokoro.lexicon = config.lexicon ? config.lexicon : "";
        sherpa_config.model.num_threads = std::max<int32_t>(1, config.num_threads);
        sherpa_config.model.debug = config.debug;
        sherpa_config.rule_fsts = config.rule_fsts ? config.rule_fsts : "";

        auto handle = std::make_unique<Handle>(
            sherpa_onnx::cxx::OfflineTts::Create(sherpa_config));
        if (!handle->tts.Get()) {
            setError(error, error_size, "sherpa-onnx OfflineTts::Create returned null");
            return nullptr;
        }
        return handle.release();
    } catch (const std::exception& e) {
        setError(error, error_size, e.what());
        return nullptr;
    } catch (...) {
        setError(error, error_size, "unknown sherpa-onnx OfflineTts::Create failure");
        return nullptr;
    }
}

void destroy(Handle* handle)
{
    delete handle;
}

bool generate(Handle* handle,
              const char* text,
              int32_t sid,
              float speed,
              ShouldStopFn should_stop,
              void* stop_arg,
              Audio* audio,
              char* error,
              std::size_t error_size)
{
    if (!handle || !handle->tts.Get()) {
        setError(error, error_size, "sherpa-onnx OfflineTts is not initialized");
        return false;
    }
    if (!text || !*text || !audio) {
        setError(error, error_size, "empty TTS text or output audio pointer");
        return false;
    }

    try {
        sherpa_onnx::cxx::GenerationConfig gen_config;
        gen_config.sid = sid;
        gen_config.speed = speed;

        std::pair<ShouldStopFn, void*> ctx{should_stop, stop_arg};
        sherpa_onnx::cxx::GeneratedAudio generated =
            handle->tts.Generate(text, gen_config, progressCallback, &ctx);

        if (generated.samples.empty() || generated.sample_rate <= 0) {
            setError(error, error_size, "sherpa-onnx generated no audio");
            return false;
        }

        const std::size_t bytes = generated.samples.size() * sizeof(float);
        auto* samples = static_cast<float*>(std::malloc(bytes));
        if (!samples) {
            setError(error, error_size, "failed to allocate generated audio buffer");
            return false;
        }

        std::memcpy(samples, generated.samples.data(), bytes);
        audio->samples = samples;
        audio->num_samples = static_cast<int32_t>(generated.samples.size());
        audio->sample_rate = generated.sample_rate;
        return true;
    } catch (const std::exception& e) {
        setError(error, error_size, e.what());
        return false;
    } catch (...) {
        setError(error, error_size, "unknown sherpa-onnx OfflineTts::Generate failure");
        return false;
    }
}

void freeAudio(Audio* audio)
{
    if (!audio) {
        return;
    }
    std::free(audio->samples);
    audio->samples = nullptr;
    audio->num_samples = 0;
    audio->sample_rate = 0;
}

} // namespace tts::sherpa_bridge
