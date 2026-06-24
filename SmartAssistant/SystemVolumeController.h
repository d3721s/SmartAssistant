#pragma once

#include <string>

namespace system_volume {

class SystemVolumeController {
public:
    SystemVolumeController();
    ~SystemVolumeController();

    SystemVolumeController(const SystemVolumeController&) = delete;
    SystemVolumeController& operator=(const SystemVolumeController&) = delete;

    bool init();
    void shutdown();
    bool isInitialized() const;

    bool volume(float& out_volume);
    bool setVolume(float volume);
    bool muted(bool& out_muted);
    bool setMuted(bool muted);

    const std::string& lastError() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace system_volume
