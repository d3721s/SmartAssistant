#pragma once

#include <cstddef>
#include <cstdint>

namespace tts::sherpa_bridge {

struct Config {
    const char* model = nullptr;
    const char* voices = nullptr;
    const char* tokens = nullptr;
    const char* data_dir = nullptr;
    const char* lexicon = nullptr;
    const char* rule_fsts = nullptr;
    int32_t num_threads = 1;
    int32_t debug = 0;
};

struct Audio {
    float* samples = nullptr;
    int32_t num_samples = 0;
    int32_t sample_rate = 0;
};

struct Handle;

using ShouldStopFn = int32_t (*)(void* arg);

Handle* create(const Config& config, char* error, std::size_t error_size);
void destroy(Handle* handle);

bool generate(Handle* handle,
              const char* text,
              int32_t sid,
              float speed,
              ShouldStopFn should_stop,
              void* stop_arg,
              Audio* audio,
              char* error,
              std::size_t error_size);

void freeAudio(Audio* audio);

} // namespace tts::sherpa_bridge
