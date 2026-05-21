#include "NetworkManager.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <toml++/toml.hpp>

namespace network_manager {
namespace {
constexpr int kUnavailableRtt = -1;
constexpr int kDefaultHttpsPort = 443;
constexpr int kDefaultHttpPort = 80;

int clampPositive(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

std::chrono::milliseconds positiveMilliseconds(int value, int fallback)
{
    return std::chrono::milliseconds(clampPositive(value, fallback));
}

std::string stripPath(std::string value)
{
    const auto slash = value.find('/');
    if (slash != std::string::npos) {
        value.erase(slash);
    }
    return value;
}

void closeSocket(int fd)
{
    if (fd >= 0) {
        ::close(fd);
    }
}
}

struct NetworkManager::ParsedEndpoint {
    std::string host;
    std::string port;
};

NetworkManager::NetworkManager()
    : config_(defaultConfig())
{
}

NetworkManager::NetworkManager(NetworkManagerConfig config)
    : config_(std::move(config))
{
    if (config_.endpoints.empty()) {
        config_.endpoints = defaultConfig().endpoints;
    }
}

NetworkManager::~NetworkManager()
{
    shutdown();
}

bool NetworkManager::init(const std::string& config_path)
{
    quill::Backend::start();
    logger_ = quill::Frontend::create_or_get_logger(
        "NetworkManager",
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>("network_manager_console_sink"));

    try {
        return init(loadConfig(config_path));
    } catch (const std::exception& ex) {
        LOG_ERROR(logger_, "NetworkManager init failed: {}", ex.what());
        return false;
    }
}

bool NetworkManager::init(const NetworkManagerConfig& config)
{
    quill::Backend::start();
    logger_ = quill::Frontend::create_or_get_logger(
        "NetworkManager",
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>("network_manager_console_sink"));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        if (config_.endpoints.empty()) {
            config_.endpoints = defaultConfig().endpoints;
        }
        config_.consecutive_timeout_limit = clampPositive(config_.consecutive_timeout_limit, 3);
        if (config_.monitor_interval.count() <= 0) {
            config_.monitor_interval = std::chrono::milliseconds(2000);
        }
        if (config_.rtt_threshold.count() <= 0) {
            config_.rtt_threshold = std::chrono::milliseconds(800);
        }
        if (config_.connect_timeout.count() <= 0) {
            config_.connect_timeout = std::chrono::milliseconds(3000);
        }
        if (config_.recovery_stable_duration.count() <= 0) {
            config_.recovery_stable_duration = std::chrono::milliseconds(10000);
        }
        state_ = force_offline_ ? NetworkState::Offline : NetworkState::Offline;
        consecutive_timeouts_ = 0;
        recovery_started_at_ = {};
    }

    LOG_INFO(logger_, "NetworkManager initialized with {} endpoint(s)", config_.endpoints.size());

    if (config_.start_monitor_on_init) {
        startMonitoring();
    } else {
        evaluateOnce();
    }

    return true;
}

const NetworkManagerConfig& NetworkManager::config() const
{
    return config_;
}

void NetworkManager::shutdown()
{
    stopMonitoring();
}

void NetworkManager::startMonitoring()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    monitor_thread_ = std::thread(&NetworkManager::monitorLoop, this);
}

void NetworkManager::stopMonitoring()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    wakeup_.notify_all();
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

NetworkState NetworkManager::currentState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void NetworkManager::subscribe(NetworkStateCallback callback)
{
    if (!callback) {
        return;
    }

    NetworkState snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.push_back(callback);
        snapshot = state_;
    }
    callback(snapshot);
}

void NetworkManager::forceOffline(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (force_offline_ == enabled) {
            return;
        }
        force_offline_ = enabled;
        recovery_started_at_ = {};
    }

    LOG_INFO(logger_, "NetworkManager forceOffline set to {}", enabled);

    if (enabled) {
        transitionTo(NetworkState::Offline);
    } else {
        evaluateOnce();
        wakeup_.notify_all();
    }
}

int NetworkManager::measureRtt(const std::string& endpoint)
{
    try {
        return measureTcpConnectRtt(parseEndpoint(endpoint));
    } catch (const std::exception& ex) {
        if (logger_) {
            LOG_WARNING(logger_, "RTT measurement skipped for '{}': {}", endpoint, ex.what());
        }
        return kUnavailableRtt;
    }
}

const char* NetworkManager::toString(NetworkState state)
{
    switch (state) {
    case NetworkState::OnlineGood:
        return "ONLINE_GOOD";
    case NetworkState::OnlinePoor:
        return "ONLINE_POOR";
    case NetworkState::Offline:
        return "OFFLINE";
    }
    return "OFFLINE";
}

void NetworkManager::monitorLoop()
{
    while (true) {
        evaluateOnce();

        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_) {
            break;
        }
        const auto interval = config_.monitor_interval;
        wakeup_.wait_for(lock, interval, [this] { return !running_; });
        if (!running_) {
            break;
        }
    }
}

void NetworkManager::evaluateOnce()
{
    NetworkManagerConfig config_snapshot;
    bool force_offline_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_snapshot = config_;
        force_offline_snapshot = force_offline_;
    }

    if (force_offline_snapshot) {
        transitionTo(NetworkState::Offline);
        return;
    }

    int best_rtt = std::numeric_limits<int>::max();
    bool reachable = false;

    for (const auto& endpoint : config_snapshot.endpoints) {
        const int rtt = measureRtt(endpoint.url);
        if (rtt >= 0) {
            reachable = true;
            best_rtt = std::min(best_rtt, rtt);
            LOG_DEBUG(logger_, "Network endpoint '{}' RTT {} ms", endpoint.name, rtt);
        } else {
            LOG_DEBUG(logger_, "Network endpoint '{}' unavailable", endpoint.name);
        }
    }

    if (!reachable) {
        bool should_go_offline = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++consecutive_timeouts_;
            recovery_started_at_ = {};
            should_go_offline = consecutive_timeouts_ >= config_snapshot.consecutive_timeout_limit;
        }
        if (should_go_offline) {
            transitionTo(NetworkState::Offline);
        }
        return;
    }

    NetworkState candidate = NetworkState::OnlineGood;
    if (best_rtt > config_snapshot.rtt_threshold.count()) {
        candidate = config_snapshot.degrade_on_poor_network ? NetworkState::Offline : NetworkState::OnlinePoor;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        consecutive_timeouts_ = 0;
    }

    if (candidate == NetworkState::OnlineGood) {
        const auto now = std::chrono::steady_clock::now();
        bool stable = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == NetworkState::OnlineGood) {
                stable = true;
            } else {
                if (recovery_started_at_ == std::chrono::steady_clock::time_point{}) {
                    recovery_started_at_ = now;
                }
                stable = now - recovery_started_at_ >= config_snapshot.recovery_stable_duration;
            }
        }
        if (stable) {
            transitionTo(NetworkState::OnlineGood);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recovery_started_at_ = {};
    }
    transitionTo(candidate);
}

void NetworkManager::transitionTo(NetworkState next_state)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == next_state) {
            return;
        }
        state_ = next_state;
        changed = true;
    }

    if (changed) {
        LOG_INFO(logger_, "Network state changed to {}", toString(next_state));
        notifySubscribers(next_state);
    }
}

void NetworkManager::notifySubscribers(NetworkState state)
{
    std::vector<NetworkStateCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks = callbacks_;
    }

    for (const auto& callback : callbacks) {
        try {
            callback(state);
        } catch (const std::exception& ex) {
            LOG_ERROR(logger_, "Network state callback failed: {}", ex.what());
        } catch (...) {
            LOG_ERROR(logger_, "Network state callback failed with unknown exception");
        }
    }
}

int NetworkManager::measureTcpConnectRtt(const ParsedEndpoint& endpoint) const
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* result = nullptr;
    const int gai = ::getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &result);
    if (gai != 0) {
        return kUnavailableRtt;
    }

    const auto cleanup = [result] { ::freeaddrinfo(result); };
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = config_.connect_timeout;

    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        const int fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            continue;
        }

        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            closeSocket(fd);
            continue;
        }

        const int connect_result = ::connect(fd, item->ai_addr, item->ai_addrlen);
        if (connect_result == 0) {
            closeSocket(fd);
            cleanup();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            return static_cast<int>(elapsed.count());
        }

        if (errno != EINPROGRESS) {
            closeSocket(fd);
            continue;
        }

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);

        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

        const int selected = ::select(fd + 1, nullptr, &write_set, nullptr, &tv);
        if (selected > 0 && FD_ISSET(fd, &write_set)) {
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && socket_error == 0) {
                closeSocket(fd);
                cleanup();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
                return static_cast<int>(elapsed.count());
            }
        }

        closeSocket(fd);
    }

    cleanup();
    return kUnavailableRtt;
}

NetworkManagerConfig NetworkManager::loadConfig(const std::string& config_path) const
{
    NetworkManagerConfig config = defaultConfig();
    toml::table table = toml::parse_file(config_path);
    const toml::node_view network = table["network"];

    config.monitor_interval = positiveMilliseconds(
        network["monitor_interval_ms"].value_or(static_cast<int64_t>(config.monitor_interval.count())),
        static_cast<int>(config.monitor_interval.count()));
    config.rtt_threshold = positiveMilliseconds(
        network["rtt_threshold_ms"].value_or(static_cast<int64_t>(config.rtt_threshold.count())),
        static_cast<int>(config.rtt_threshold.count()));
    config.connect_timeout = positiveMilliseconds(
        network["connect_timeout_ms"].value_or(static_cast<int64_t>(config.connect_timeout.count())),
        static_cast<int>(config.connect_timeout.count()));
    config.recovery_stable_duration = positiveMilliseconds(
        network["recovery_stable_duration_ms"].value_or(static_cast<int64_t>(config.recovery_stable_duration.count())),
        static_cast<int>(config.recovery_stable_duration.count()));
    config.consecutive_timeout_limit = clampPositive(
        static_cast<int>(network["consecutive_timeout_limit"].value_or(config.consecutive_timeout_limit)),
        config.consecutive_timeout_limit);
    config.degrade_on_poor_network = network["degrade_on_poor_network"].value_or(config.degrade_on_poor_network);
    config.start_monitor_on_init = network["start_monitor_on_init"].value_or(config.start_monitor_on_init);

    if (const toml::array* endpoints = network["endpoints"].as_array()) {
        std::vector<NetworkEndpoint> parsed;
        endpoints->for_each([&parsed](const toml::table& endpoint_table) {
            NetworkEndpoint endpoint;
            endpoint.group = endpoint_table["group"].value_or(std::string{});
            endpoint.name = endpoint_table["name"].value_or(std::string{});
            endpoint.url = endpoint_table["url"].value_or(std::string{});
            if (!endpoint.url.empty()) {
                if (endpoint.name.empty()) {
                    endpoint.name = endpoint.url;
                }
                parsed.push_back(std::move(endpoint));
            }
        });
        if (!parsed.empty()) {
            config.endpoints = std::move(parsed);
        }
    }

    return config;
}

NetworkManagerConfig NetworkManager::defaultConfig()
{
    NetworkManagerConfig config;
    config.endpoints = {
        {"ASR", "doubao_asr_stream_async", "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"},
        {"ASR", "doubao_asr_stream", "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel"},
        {"TTS", "doubao_tts_v3_unidirectional", "wss://openspeech.bytedance.com/api/v3/tts/unidirectional/stream"},
        {"TTS", "doubao_tts_v3_bidirection", "wss://openspeech.bytedance.com/api/v3/tts/bidirection"},
        {"TTS", "doubao_tts_v1_http", "https://openspeech.bytedance.com/api/v1/tts"}
    };
    return config;
}

NetworkManager::ParsedEndpoint NetworkManager::parseEndpoint(const std::string& endpoint)
{
    if (endpoint.empty()) {
        throw std::invalid_argument("endpoint is empty");
    }

    std::string remaining = endpoint;
    int default_port = kDefaultHttpsPort;
    const auto scheme = remaining.find("://");
    if (scheme != std::string::npos) {
        const std::string protocol = remaining.substr(0, scheme);
        remaining.erase(0, scheme + 3);
        default_port = protocol == "http" || protocol == "ws" ? kDefaultHttpPort : kDefaultHttpsPort;
    }

    remaining = stripPath(remaining);
    if (remaining.empty()) {
        throw std::invalid_argument("endpoint host is empty");
    }

    ParsedEndpoint parsed;
    if (remaining.front() == '[') {
        const auto close = remaining.find(']');
        if (close == std::string::npos) {
            throw std::invalid_argument("invalid IPv6 endpoint");
        }
        parsed.host = remaining.substr(1, close - 1);
        if (close + 1 < remaining.size() && remaining[close + 1] == ':') {
            parsed.port = remaining.substr(close + 2);
        }
    } else {
        const auto colon = remaining.rfind(':');
        if (colon != std::string::npos && remaining.find(':') == colon) {
            parsed.host = remaining.substr(0, colon);
            parsed.port = remaining.substr(colon + 1);
        } else {
            parsed.host = remaining;
        }
    }

    if (parsed.host.empty()) {
        throw std::invalid_argument("endpoint host is empty");
    }
    if (parsed.port.empty()) {
        parsed.port = std::to_string(default_port);
    }
    return parsed;
}

bool NetworkManager::isTimeoutRtt(int rtt_ms)
{
    return rtt_ms < 0;
}

}
