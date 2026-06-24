#include "MusicPlayer.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace music {

namespace {

quill::Logger* logger()
{
    static quill::Logger* lg = quill::Frontend::get_logger("MusicPlayer");
    if (!lg) {
        quill::Backend::start();
        lg = quill::Frontend::create_or_get_logger(
            "MusicPlayer",
            quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "music_player_console_sink"));
        lg->set_log_level(quill::LogLevel::Info);
    }
    return lg;
}

std::string trim(const std::string& s)
{
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool fileExists(const std::string& p)
{
    if (p.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

bool executableInPath(const char* name)
{
    if (!name || !*name) return false;
    if (std::strchr(name, '/')) {
        return ::access(name, X_OK) == 0;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return false;

    std::string path(path_env);
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t next = path.find(':', pos);
        std::string dir = path.substr(
            pos, next == std::string::npos ? std::string::npos : next - pos);
        if (dir.empty()) dir = ".";
        const std::filesystem::path candidate =
            std::filesystem::path(dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return false;
}

float clampGain(float value, float lo, float hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

}  // namespace

// ----------------------------------------------------------------------------
// Config loader
// ----------------------------------------------------------------------------

MusicConfig MusicPlayer::loadConfig(const std::string& config_path)
{
    MusicConfig cfg;
    toml::table tbl;
    try {
        tbl = toml::parse_file(config_path);
    } catch (const std::exception& e) {
        LOG_WARNING(logger(), "loadConfig: parse failed: {}", e.what());
        return cfg;
    }
    auto music = tbl["music"];
    if (!music) {
        return cfg;
    }
    cfg.enabled     = music["enabled"].value_or(true);
    cfg.library_dir = music["library_dir"].value_or<std::string>("");
    cfg.stream_gain = static_cast<float>(music["stream_gain"].value_or(1.0));
    cfg.ducking_gain = static_cast<float>(
        tbl["audio"]["volume"]["ducking_gain"].value_or(
            static_cast<double>(cfg.ducking_gain)));
    cfg.ducking_gain = static_cast<float>(
        music["ducking_gain"].value_or(static_cast<double>(cfg.ducking_gain)));
    cfg.stream_gain = clampGain(cfg.stream_gain, 0.0f, 2.0f);
    cfg.ducking_gain = clampGain(cfg.ducking_gain, 0.0f, 1.0f);

    if (auto arr = music["tracks"].as_array()) {
        for (auto&& el : *arr) {
            auto* t = el.as_table();
            if (!t) continue;
            Track track;
            track.title  = (*t)["title"].value_or<std::string>("");
            track.artist = (*t)["artist"].value_or<std::string>("");
            track.file   = (*t)["file"].value_or<std::string>("");
            if (auto aa = (*t)["aliases"].as_array()) {
                for (auto&& a : *aa) {
                    if (auto sv = a.value<std::string>()) {
                        track.aliases.push_back(*sv);
                    }
                }
            }
            if (track.file.empty() && track.title.empty()) continue;
            cfg.tracks.push_back(std::move(track));
        }
    }
    return cfg;
}

// ----------------------------------------------------------------------------
// MusicPlayer lifecycle
// ----------------------------------------------------------------------------

MusicPlayer::MusicPlayer()  = default;
MusicPlayer::~MusicPlayer() { shutdown(); }

bool MusicPlayer::init(audio::AudioManager* audio, const MusicConfig& cfg)
{
    if (initialized_) return true;
    (void)audio;
    if (!executableInPath("mpg123")) {
        LOG_ERROR(logger(), "init: mpg123 executable not found in PATH");
        return false;
    }
    std::signal(SIGPIPE, SIG_IGN);
    cfg_ = cfg;
    initialized_ = true;
    LOG_INFO(logger(), "init ok: tracks={} library_dir={}",
             cfg_.tracks.size(), cfg_.library_dir);
    return true;
}

void MusicPlayer::shutdown()
{
    if (!initialized_) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        terminatePlayerLocked();
        ducking_depth_ = 0;
    }
    initialized_ = false;
}

bool MusicPlayer::isInitialized() const { return initialized_; }

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

std::string MusicPlayer::resolveTrackPath(const Track& t) const
{
    if (t.file.empty()) return "";
    std::filesystem::path p(t.file);
    if (p.is_absolute() || cfg_.library_dir.empty()) {
        return t.file;
    }
    return (std::filesystem::path(cfg_.library_dir) / p).string();
}

int MusicPlayer::findTrack(const std::string& query) const
{
    if (cfg_.tracks.empty()) return -1;
    if (query.empty()) {
        return current_idx_ >= 0 ? current_idx_ : 0;
    }
    const std::string q = trim(query);
    if (q.empty()) {
        return current_idx_ >= 0 ? current_idx_ : 0;
    }
    auto contains = [&](const std::string& hay) {
        return !hay.empty() && hay.find(q) != std::string::npos;
    };
    auto titleLikeMatches = [&](const std::string& hay) {
        return !hay.empty() &&
               (hay.find(q) != std::string::npos ||
                q.find(hay) != std::string::npos);
    };
    // Title/alias matching accepts both "稻香" and natural ASR/NLU tails such
    // as "播放稻香", "周杰伦的稻香", or "稻香。".
    for (std::size_t i = 0; i < cfg_.tracks.size(); ++i) {
        if (titleLikeMatches(cfg_.tracks[i].title)) return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < cfg_.tracks.size(); ++i) {
        if (contains(cfg_.tracks[i].artist)) return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < cfg_.tracks.size(); ++i) {
        for (const auto& a : cfg_.tracks[i].aliases) {
            if (titleLikeMatches(a)) return static_cast<int>(i);
        }
    }
    return -1;
}

bool MusicPlayer::isPlayingLocked() const
{
    return playback_active_ && !paused_ && isPlayerRunningLocked();
}

bool MusicPlayer::isPlayerRunningLocked() const
{
    if (player_pid_ <= 0) return false;

    int status = 0;
    const auto pid = static_cast<pid_t>(player_pid_);
    const pid_t rc = ::waitpid(pid, &status, WNOHANG);
    if (rc == 0) {
        return true;
    }
    if (rc == pid || (rc == -1 && errno == ECHILD)) {
        player_pid_ = -1;
        const_cast<MusicPlayer*>(this)->closeCommandPipeLocked();
        paused_ = false;
        playback_active_ = false;
        return false;
    }
    if (rc == -1 && errno != EINTR) {
        LOG_WARNING(logger(), "waitpid failed for mpg123 pid {}: {}",
                    player_pid_, std::strerror(errno));
    }
    return true;
}

void MusicPlayer::closeCommandPipeLocked()
{
    if (command_fd_ >= 0) {
        while (::close(command_fd_) == -1 && errno == EINTR) {
        }
        command_fd_ = -1;
    }
}

bool MusicPlayer::writeCommandLocked(const std::string& command)
{
    if (command_fd_ < 0) return false;

    std::string line = command;
    if (line.empty() || line.back() != '\n') {
        line.push_back('\n');
    }

    const char* data = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        const ssize_t n = ::write(command_fd_, data, remaining);
        if (n > 0) {
            data += n;
            remaining -= static_cast<std::size_t>(n);
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        LOG_WARNING(logger(), "write mpg123 command failed: {}",
                    std::strerror(errno));
        closeCommandPipeLocked();
        player_pid_ = -1;
        paused_ = false;
        playback_active_ = false;
        return false;
    }
    return true;
}

bool MusicPlayer::ensurePlayerLocked()
{
    if (isPlayerRunningLocked() && command_fd_ >= 0) {
        return true;
    }

    closeCommandPipeLocked();

    int pipe_fd[2] = {-1, -1};
    if (::pipe(pipe_fd) == -1) {
        LOG_ERROR(logger(), "pipe for mpg123 failed: {}", std::strerror(errno));
        return false;
    }

    const pid_t pid = ::fork();
    if (pid == -1) {
        LOG_ERROR(logger(), "fork mpg123 failed: {}", std::strerror(errno));
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        return false;
    }

    if (pid == 0) {
        ::dup2(pipe_fd[0], STDIN_FILENO);
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);

        const int dev_null = ::open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            ::dup2(dev_null, STDOUT_FILENO);
            ::dup2(dev_null, STDERR_FILENO);
            ::close(dev_null);
        }

        ::execlp("mpg123", "mpg123", "-R", "-q", "--no-control",
                 static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(pipe_fd[0]);
    command_fd_ = pipe_fd[1];
    player_pid_ = static_cast<int>(pid);
    paused_ = false;
    playback_active_ = false;
    writeCommandLocked("SILENCE");
    applyVolumeLocked();
    LOG_INFO(logger(), "mpg123 remote player started pid={}", player_pid_);
    return true;
}

void MusicPlayer::applyVolumeLocked()
{
    if (command_fd_ < 0) return;
    const float duck_gain = ducking_depth_ > 0 ? cfg_.ducking_gain : 1.0f;
    const float percent = clampGain(cfg_.stream_gain * duck_gain * 100.0f,
                                    0.0f, 200.0f);
    char command[64];
    std::snprintf(command, sizeof(command), "VOLUME %.2f", percent);
    writeCommandLocked(command);
}

void MusicPlayer::stopPlaybackLocked()
{
    if (isPlayerRunningLocked() && command_fd_ >= 0) {
        writeCommandLocked("STOP");
    }
    paused_ = false;
    playback_active_ = false;
}

void MusicPlayer::terminatePlayerLocked()
{
    if (player_pid_ <= 0) {
        closeCommandPipeLocked();
        paused_ = false;
        playback_active_ = false;
        return;
    }

    const auto pid = static_cast<pid_t>(player_pid_);
    if (command_fd_ >= 0) {
        writeCommandLocked("STOP");
    }
    closeCommandPipeLocked();
    if (::kill(pid, SIGTERM) == -1 && errno != ESRCH) {
        LOG_WARNING(logger(), "SIGTERM mpg123 pid {} failed: {}",
                    player_pid_, std::strerror(errno));
    }

    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t rc = ::waitpid(pid, &status, WNOHANG);
        if (rc == pid || (rc == -1 && errno == ECHILD)) {
            player_pid_ = -1;
            paused_ = false;
            return;
        }
        if (rc == -1 && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    if (::kill(pid, SIGKILL) == -1 && errno != ESRCH) {
        LOG_WARNING(logger(), "SIGKILL mpg123 pid {} failed: {}",
                    player_pid_, std::strerror(errno));
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    player_pid_ = -1;
    paused_ = false;
    playback_active_ = false;
}

bool MusicPlayer::isPlaying() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return isPlayingLocked();
}

bool MusicPlayer::isPaused() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return paused_;
}

std::string MusicPlayer::currentTitle() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (current_idx_ < 0) return "";
    return cfg_.tracks[static_cast<std::size_t>(current_idx_)].title;
}

std::string MusicPlayer::currentArtist() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (current_idx_ < 0) return "";
    return cfg_.tracks[static_cast<std::size_t>(current_idx_)].artist;
}

// ----------------------------------------------------------------------------
// Core dispatch
// ----------------------------------------------------------------------------

PlayResult MusicPlayer::startInternal(std::size_t idx)
{
    PlayResult res;
    if (!initialized_) {
        res.message = "music player not initialized";
        return res;
    }
    if (idx >= cfg_.tracks.size()) {
        res.message = "曲目索引越界";
        return res;
    }

    const std::string path = resolveTrackPath(cfg_.tracks[idx]);
    if (!fileExists(path)) {
        LOG_ERROR(logger(), "track missing on disk: idx={} path='{}'", idx, path);
        res.message = "曲目文件不存在";
        return res;
    }

    if (!ensurePlayerLocked()) {
        res.message = "启动播放器失败";
        return res;
    }

    writeCommandLocked("STOP");
    applyVolumeLocked();
    if (!writeCommandLocked("LOAD " + path)) {
        res.message = "启动播放器失败";
        return res;
    }

    current_idx_     = static_cast<int>(idx);
    paused_          = false;
    playback_active_ = true;

    res.ok      = true;
    res.title   = cfg_.tracks[idx].title;
    res.artist  = cfg_.tracks[idx].artist;
    res.message = res.title.empty() ? std::string("正在播放")
                                    : "正在播放 " + res.title;
    LOG_INFO(logger(), "mpg123 remote play idx={} title='{}' pid={} path='{}'",
             idx, res.title, player_pid_, path);
    return res;
}

// ----------------------------------------------------------------------------
// Public commands
// ----------------------------------------------------------------------------

PlayResult MusicPlayer::play(const std::string& query)
{
    std::lock_guard<std::mutex> lk(mtx_);
    PlayResult res;
    if (!initialized_) {
        res.message = "music player not initialized";
        return res;
    }
    if (cfg_.tracks.empty()) {
        res.message = "曲库为空，请稍后再试";
        return res;
    }

    if (query.empty() && paused_ && current_idx_ >= 0) {
        if (ensurePlayerLocked() && writeCommandLocked("PAUSE")) {
            paused_ = false;
            applyVolumeLocked();
            res.ok = true;
            res.title = cfg_.tracks[static_cast<std::size_t>(current_idx_)].title;
            res.artist = cfg_.tracks[static_cast<std::size_t>(current_idx_)].artist;
            res.message = res.title.empty() ? std::string("继续播放")
                                            : "继续播放 " + res.title;
            return res;
        }
        return startInternal(static_cast<std::size_t>(current_idx_));
    }

    const int idx = findTrack(query);
    if (idx < 0) {
        res.message = query.empty() ? std::string("没有可播放的曲目")
                                    : "曲库中暂时没有 " + query;
        return res;
    }
    return startInternal(static_cast<std::size_t>(idx));
}

bool MusicPlayer::pause()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!initialized_ || current_idx_ < 0 || paused_) return false;
    if (!isPlayingLocked()) return false;
    if (!writeCommandLocked("PAUSE")) {
        return false;
    }
    paused_ = true;
    LOG_INFO(logger(), "pause mpg123 pid {}", player_pid_);
    return true;
}

bool MusicPlayer::resume()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!initialized_ || current_idx_ < 0 || !paused_) return false;
    if (!isPlayerRunningLocked()) return false;
    if (!writeCommandLocked("PAUSE")) {
        return false;
    }
    paused_ = false;
    applyVolumeLocked();
    LOG_INFO(logger(), "resume mpg123 pid {}", player_pid_);
    return true;
}

std::string MusicPlayer::stop()
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::string title;
    if (current_idx_ >= 0 &&
        static_cast<std::size_t>(current_idx_) < cfg_.tracks.size()) {
        title = cfg_.tracks[static_cast<std::size_t>(current_idx_)].title;
    }
    stopPlaybackLocked();
    return title;
}

void MusicPlayer::beginDucking()
{
    std::lock_guard<std::mutex> lk(mtx_);
    ++ducking_depth_;
    if (ducking_depth_ == 1) {
        applyVolumeLocked();
        LOG_INFO(logger(), "music ducking enabled gain={}", cfg_.ducking_gain);
    }
}

void MusicPlayer::endDucking()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (ducking_depth_ <= 0) {
        ducking_depth_ = 0;
        return;
    }
    --ducking_depth_;
    if (ducking_depth_ == 0) {
        applyVolumeLocked();
        LOG_INFO(logger(), "music ducking disabled");
    }
}

PlayResult MusicPlayer::next()
{
    std::lock_guard<std::mutex> lk(mtx_);
    PlayResult res;
    if (!initialized_ || cfg_.tracks.empty()) {
        res.message = "曲库为空";
        return res;
    }
    const std::size_t n = cfg_.tracks.size();
    const std::size_t idx =
        current_idx_ < 0 ? 0 : (static_cast<std::size_t>(current_idx_) + 1) % n;
    return startInternal(idx);
}

PlayResult MusicPlayer::previous()
{
    std::lock_guard<std::mutex> lk(mtx_);
    PlayResult res;
    if (!initialized_ || cfg_.tracks.empty()) {
        res.message = "曲库为空";
        return res;
    }
    const std::size_t n = cfg_.tracks.size();
    std::size_t idx;
    if (current_idx_ <= 0) {
        idx = n - 1;
    } else {
        idx = static_cast<std::size_t>(current_idx_) - 1;
    }
    return startInternal(idx);
}

}  // namespace music
