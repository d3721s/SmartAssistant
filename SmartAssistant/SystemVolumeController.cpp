#include "SystemVolumeController.h"

#include <algorithm>
#include <memory>
#include <string>

#include <pulse/pulseaudio.h>

namespace system_volume {
namespace {

float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

struct CallbackState {
    explicit CallbackState(pa_threaded_mainloop* loop) : mainloop(loop) {}

    pa_threaded_mainloop* mainloop = nullptr;
    bool done = false;
    bool success = false;
    std::string sink_name;
    float volume = 0.0f;
    bool muted = false;
    uint8_t channels = 2;
};

void contextStateCallback(pa_context*, void* userdata)
{
    auto* loop = static_cast<pa_threaded_mainloop*>(userdata);
    pa_threaded_mainloop_signal(loop, 0);
}

void serverInfoCallback(pa_context*, const pa_server_info* info, void* userdata)
{
    auto* state = static_cast<CallbackState*>(userdata);
    if (info && info->default_sink_name) {
        state->sink_name = info->default_sink_name;
        state->success = true;
    }
    state->done = true;
    pa_threaded_mainloop_signal(state->mainloop, 0);
}

void sinkInfoCallback(pa_context*, const pa_sink_info* info, int eol, void* userdata)
{
    auto* state = static_cast<CallbackState*>(userdata);
    if (eol > 0) {
        state->done = true;
        pa_threaded_mainloop_signal(state->mainloop, 0);
        return;
    }
    if (!info) {
        return;
    }

    const pa_cvolume* cv = &info->volume;
    const pa_volume_t avg = pa_cvolume_avg(cv);
    state->volume = static_cast<float>(avg) / static_cast<float>(PA_VOLUME_NORM);
    state->muted = info->mute != 0;
    state->channels = cv->channels > 0 ? cv->channels : 2;
    state->success = true;
}

void successCallback(pa_context*, int success, void* userdata)
{
    auto* state = static_cast<CallbackState*>(userdata);
    state->success = success != 0;
    state->done = true;
    pa_threaded_mainloop_signal(state->mainloop, 0);
}

}  // namespace

struct SystemVolumeController::Impl {
    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    bool initialized = false;
    std::string last_error;

    ~Impl()
    {
        shutdown();
    }

    void setError(const std::string& message)
    {
        last_error = message;
    }

    bool waitForOperationLocked(pa_operation* op, CallbackState& state)
    {
        if (!op) {
            setError("PulseAudio operation creation failed");
            return false;
        }
        while (!state.done &&
               pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(mainloop);
        }
        pa_operation_unref(op);
        if (!state.success) {
            setError("PulseAudio operation failed");
        }
        return state.success;
    }

    bool defaultSinkLocked(std::string& out)
    {
        CallbackState state(mainloop);
        pa_operation* op = pa_context_get_server_info(
            context, serverInfoCallback, &state);
        if (!waitForOperationLocked(op, state)) {
            if (last_error.empty()) {
                setError("Failed to query PulseAudio default sink");
            }
            return false;
        }
        if (state.sink_name.empty()) {
            setError("PulseAudio default sink is empty");
            return false;
        }
        out = std::move(state.sink_name);
        return true;
    }

    bool init()
    {
        if (initialized) return true;

        mainloop = pa_threaded_mainloop_new();
        if (!mainloop) {
            setError("pa_threaded_mainloop_new failed");
            return false;
        }

        pa_mainloop_api* api = pa_threaded_mainloop_get_api(mainloop);
        context = pa_context_new(api, "SmartAssistantSystemVolume");
        if (!context) {
            setError("pa_context_new failed");
            shutdown();
            return false;
        }

        pa_context_set_state_callback(context, contextStateCallback, mainloop);
        if (pa_threaded_mainloop_start(mainloop) < 0) {
            setError("pa_threaded_mainloop_start failed");
            shutdown();
            return false;
        }

        pa_threaded_mainloop_lock(mainloop);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            setError("pa_context_connect failed");
            pa_threaded_mainloop_unlock(mainloop);
            shutdown();
            return false;
        }

        bool ok = false;
        for (;;) {
            const pa_context_state_t state = pa_context_get_state(context);
            if (state == PA_CONTEXT_READY) {
                ok = true;
                break;
            }
            if (!PA_CONTEXT_IS_GOOD(state)) {
                setError("PulseAudio context failed");
                break;
            }
            pa_threaded_mainloop_wait(mainloop);
        }
        pa_threaded_mainloop_unlock(mainloop);

        if (!ok) {
            shutdown();
            return false;
        }

        initialized = true;
        return true;
    }

    void shutdown()
    {
        initialized = false;
        if (context) {
            pa_context_disconnect(context);
            pa_context_unref(context);
            context = nullptr;
        }
        if (mainloop) {
            pa_threaded_mainloop_stop(mainloop);
            pa_threaded_mainloop_free(mainloop);
            mainloop = nullptr;
        }
    }

    bool volume(float& out_volume)
    {
        if (!initialized && !init()) return false;
        pa_threaded_mainloop_lock(mainloop);
        std::string sink;
        if (!defaultSinkLocked(sink)) {
            pa_threaded_mainloop_unlock(mainloop);
            return false;
        }
        CallbackState state(mainloop);
        pa_operation* op = pa_context_get_sink_info_by_name(
            context, sink.c_str(), sinkInfoCallback, &state);
        const bool ok = waitForOperationLocked(op, state);
        pa_threaded_mainloop_unlock(mainloop);
        if (ok) {
            out_volume = clamp01(state.volume);
        }
        return ok;
    }

    bool setVolume(float volume)
    {
        if (!initialized && !init()) return false;
        pa_threaded_mainloop_lock(mainloop);
        std::string sink;
        if (!defaultSinkLocked(sink)) {
            pa_threaded_mainloop_unlock(mainloop);
            return false;
        }

        CallbackState sink_info(mainloop);
        pa_operation* info_op = pa_context_get_sink_info_by_name(
            context, sink.c_str(), sinkInfoCallback, &sink_info);
        if (!waitForOperationLocked(info_op, sink_info)) {
            pa_threaded_mainloop_unlock(mainloop);
            return false;
        }

        pa_cvolume cv;
        pa_cvolume_set(&cv, sink_info.channels, static_cast<pa_volume_t>(
                                            clamp01(volume) * PA_VOLUME_NORM));
        CallbackState state(mainloop);
        pa_operation* op = pa_context_set_sink_volume_by_name(
            context, sink.c_str(), &cv, successCallback, &state);
        const bool ok = waitForOperationLocked(op, state);
        pa_threaded_mainloop_unlock(mainloop);
        return ok;
    }

    bool muted(bool& out_muted)
    {
        if (!initialized && !init()) return false;
        pa_threaded_mainloop_lock(mainloop);
        std::string sink;
        if (!defaultSinkLocked(sink)) {
            pa_threaded_mainloop_unlock(mainloop);
            return false;
        }
        CallbackState state(mainloop);
        pa_operation* op = pa_context_get_sink_info_by_name(
            context, sink.c_str(), sinkInfoCallback, &state);
        const bool ok = waitForOperationLocked(op, state);
        pa_threaded_mainloop_unlock(mainloop);
        if (ok) {
            out_muted = state.muted;
        }
        return ok;
    }

    bool setMuted(bool muted)
    {
        if (!initialized && !init()) return false;
        pa_threaded_mainloop_lock(mainloop);
        std::string sink;
        if (!defaultSinkLocked(sink)) {
            pa_threaded_mainloop_unlock(mainloop);
            return false;
        }
        CallbackState state(mainloop);
        pa_operation* op = pa_context_set_sink_mute_by_name(
            context, sink.c_str(), muted ? 1 : 0, successCallback, &state);
        const bool ok = waitForOperationLocked(op, state);
        pa_threaded_mainloop_unlock(mainloop);
        return ok;
    }
};

SystemVolumeController::SystemVolumeController()
    : impl_(new Impl)
{
}

SystemVolumeController::~SystemVolumeController()
{
    delete impl_;
}

bool SystemVolumeController::init()
{
    return impl_->init();
}

void SystemVolumeController::shutdown()
{
    impl_->shutdown();
}

bool SystemVolumeController::isInitialized() const
{
    return impl_->initialized;
}

bool SystemVolumeController::volume(float& out_volume)
{
    return impl_->volume(out_volume);
}

bool SystemVolumeController::setVolume(float volume)
{
    return impl_->setVolume(volume);
}

bool SystemVolumeController::muted(bool& out_muted)
{
    return impl_->muted(out_muted);
}

bool SystemVolumeController::setMuted(bool muted)
{
    return impl_->setMuted(muted);
}

const std::string& SystemVolumeController::lastError() const
{
    return impl_->last_error;
}

}  // namespace system_volume
