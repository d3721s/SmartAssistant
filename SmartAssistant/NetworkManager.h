#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <quill/Logger.h>

namespace network_manager {

enum class NetworkState {
    OnlineGood,
    OnlinePoor,
    Offline
};

using NetworkStateCallback = std::function<void(NetworkState)>;

struct NetworkEndpoint {
    std::string group;
    std::string name;
    std::string url;
};

struct NetworkManagerConfig {
    std::vector<NetworkEndpoint> endpoints;
    std::chrono::milliseconds monitor_interval{2000};
    std::chrono::milliseconds rtt_threshold{800};
    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds recovery_stable_duration{10000};
    int consecutive_timeout_limit{3};
    bool degrade_on_poor_network{true};
    bool start_monitor_on_init{true};
};

class NetworkManager {
public:
    NetworkManager();
    explicit NetworkManager(NetworkManagerConfig config);
    ~NetworkManager();

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    bool init(const std::string& config_path = "Config/config.toml");
    bool init(const NetworkManagerConfig& config);

    const NetworkManagerConfig& config() const;
    void shutdown();

    void startMonitoring();
    void stopMonitoring();

    NetworkState currentState() const;
    void subscribe(NetworkStateCallback callback);
    void forceOffline(bool enabled);
    int measureRtt(const std::string& endpoint);

    static const char* toString(NetworkState state);

private:
    struct ParsedEndpoint;

    void monitorLoop();
    void evaluateOnce();
    void transitionTo(NetworkState next_state);
    void notifySubscribers(NetworkState state);
    int measureTcpConnectRtt(const ParsedEndpoint& endpoint) const;
    NetworkManagerConfig loadConfig(const std::string& config_path) const;
    static NetworkManagerConfig defaultConfig();
    static ParsedEndpoint parseEndpoint(const std::string& endpoint);
    static bool isTimeoutRtt(int rtt_ms);

    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    NetworkManagerConfig config_;
    std::vector<NetworkStateCallback> callbacks_;
    std::thread monitor_thread_;
    quill::Logger* logger_{nullptr};
    std::chrono::steady_clock::time_point recovery_started_at_{};
    NetworkState state_{NetworkState::Offline};
    int consecutive_timeouts_{0};
    bool force_offline_{false};
    bool running_{false};
};

}
