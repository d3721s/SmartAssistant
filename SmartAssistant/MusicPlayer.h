#pragma once

#include "AudioManager.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace music {

// ============================================================================
// Configuration (parsed from [music] in TOML)
// ============================================================================

struct Track {
    std::string              title;     // e.g. "稻香"
    std::string              artist;    // e.g. "周杰伦"
    std::string              file;      // absolute, or relative to library_dir
    std::vector<std::string> aliases;   // additional search keys
};

struct MusicConfig {
    bool                 enabled      = true;
    std::string          library_dir;       // base dir prepended to non-absolute Track.file
    float                stream_gain  = 1.0f;
    float                ducking_gain = 0.35f;
    std::vector<Track>   tracks;
};

struct PlayResult {
    bool        ok    = false;
    std::string message;     // user-facing reason on failure / acknowledgement on success
    std::string title;
    std::string artist;
};

// ============================================================================
// MusicPlayer — plays local MP3 files through the mpg123 executable.  The
// AudioManager parameter is kept for API compatibility with the assistant demo,
// but music playback does not pass through AudioManager.
// ============================================================================

class MusicPlayer {
public:
    MusicPlayer();
    ~MusicPlayer();

    MusicPlayer(const MusicPlayer&)            = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // audio is unused and may be null; it is kept only for call-site compatibility.
    bool init(audio::AudioManager* audio, const MusicConfig& cfg);
    void shutdown();
    bool isInitialized() const;

    // Empty query → resume current selection (or play first track if none).
    // Non-empty query → match by title / artist / alias (case-sensitive substring).
    PlayResult play(const std::string& query = "");

    bool pause();        // false if nothing was playing
    bool resume();       // false if not paused
    std::string stop();  // returns previous track title (or "")
    void beginDucking();
    void endDucking();

    PlayResult next();
    PlayResult previous();

    bool        isPlaying() const;   // wall-clock estimate
    bool        isPaused()  const;
    std::string currentTitle()  const;
    std::string currentArtist() const;

    static MusicConfig loadConfig(const std::string& config_path);

private:
    int  findTrack(const std::string& query) const;
    PlayResult startInternal(std::size_t idx);
    bool isPlayingLocked() const;
    bool isPlayerRunningLocked() const;
    bool ensurePlayerLocked();
    bool writeCommandLocked(const std::string& command);
    void closeCommandPipeLocked();
    void stopPlaybackLocked();
    void applyVolumeLocked();
    void terminatePlayerLocked();
    std::string resolveTrackPath(const Track& t) const;

    MusicConfig cfg_;
    bool        initialized_ = false;

    mutable std::mutex mtx_;
    int                current_idx_ = -1;
    mutable int        player_pid_  = -1;
    int                command_fd_  = -1;
    mutable bool       paused_      = false;
    mutable bool       playback_active_ = false;
    int                ducking_depth_ = 0;
};

}  // namespace music
