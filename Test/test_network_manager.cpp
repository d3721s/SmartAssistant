#include "NetworkManager.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <toml++/toml.hpp>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
struct EndpointCheck {
    std::string group;
    std::string name;
    std::string url;
    std::vector<int> attempts;
    int rtt_ms{-1};
};

constexpr const char* kConfigPath = "Config/config.toml";

bool hasReachableEndpoint(const std::vector<EndpointCheck>& checks, const std::string& group)
{
    for (const auto& check : checks) {
        if (check.group == group && check.rtt_ms >= 0) {
            return true;
        }
    }
    return false;
}

std::string formatAttempts(const EndpointCheck& check)
{
    std::string result;
    for (std::size_t index = 0; index < check.attempts.size(); ++index) {
        if (!result.empty()) {
            result += ", ";
        }
        result += check.attempts[index] >= 0 ? std::to_string(check.attempts[index]) + "ms" : "FAIL";
    }
    return result;
}

void printCheck(const EndpointCheck& check)
{
    std::cout << std::left << std::setw(8) << check.group
              << std::setw(32) << check.name
              << std::setw(12) << (check.rtt_ms >= 0 ? std::to_string(check.rtt_ms) + " ms" : "FAILED")
              << std::setw(32) << formatAttempts(check)
              << check.url << '\n';
}

int readPositiveInt(const toml::node_view<toml::node>& table, const char* key, int fallback)
{
    const int value = static_cast<int>(table[key].value_or(fallback));
    return value > 0 ? value : fallback;
}

std::vector<EndpointCheck> buildChecks(const network_manager::NetworkManagerConfig& config)
{
    std::vector<EndpointCheck> checks;
    checks.reserve(config.endpoints.size());
    for (const auto& endpoint : config.endpoints) {
        EndpointCheck check;
        check.group = endpoint.group.empty() ? "UNKNOWN" : endpoint.group;
        check.name = endpoint.name;
        check.url = endpoint.url;
        checks.push_back(std::move(check));
    }
    return checks;
}
}

int main()
{
    using namespace network_manager;

    quill::Backend::start();
    quill::Logger* logger = quill::Frontend::create_or_get_logger(
        "NetworkManagerConnectivityTest",
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>("network_manager_connectivity_test_console_sink"));

    toml::table table;
    try {
        table = toml::parse_file(kConfigPath);
    } catch (const toml::parse_error& ex) {
        LOG_ERROR(logger, "Failed to parse config '{}': {}", kConfigPath, ex.description());
        return EXIT_FAILURE;
    }

    const toml::node_view connectivity_test = table["connectivity_test"];
    const int attempt_count = readPositiveInt(connectivity_test, "attempt_count", 4);
    const int state_wait_ms = readPositiveInt(connectivity_test, "state_wait_ms", 1200);

    NetworkManager manager;
    if (!manager.init(kConfigPath)) {
        LOG_ERROR(logger, "NetworkManager init failed");
        return EXIT_FAILURE;
    }

    std::vector<EndpointCheck> checks = buildChecks(manager.config());

    for (auto& check : checks) {
        check.attempts.reserve(static_cast<std::size_t>(attempt_count));
        for (int attempt = 0; attempt < attempt_count; ++attempt) {
            const int rtt_ms = manager.measureRtt(check.url);
            check.attempts.push_back(rtt_ms);
            if (rtt_ms >= 0) {
                check.rtt_ms = rtt_ms;
                break;
            }
        }
    }

    std::cout << "Doubao ASR/TTS endpoint connectivity\n";
    std::cout << std::left << std::setw(8) << "GROUP"
              << std::setw(32) << "ENDPOINT"
              << std::setw(12) << "BEST_RTT"
              << std::setw(32) << "ATTEMPTS"
              << "URL" << '\n';
    std::cout << std::string(142, '-') << '\n';

    for (const auto& check : checks) {
        printCheck(check);
    }

    const bool asr_available = hasReachableEndpoint(checks, "ASR");
    const bool tts_available = hasReachableEndpoint(checks, "TTS");

    manager.startMonitoring();
    std::this_thread::sleep_for(std::chrono::milliseconds(state_wait_ms));
    const NetworkState monitored_state = manager.currentState();

    std::cout << "\nASR connectivity: " << (asr_available ? "AVAILABLE" : "UNAVAILABLE") << '\n';
    std::cout << "TTS connectivity: " << (tts_available ? "AVAILABLE" : "UNAVAILABLE") << '\n';
    std::cout << "NetworkManager state: " << NetworkManager::toString(monitored_state) << '\n';

    manager.shutdown();

    return asr_available && tts_available ? EXIT_SUCCESS : EXIT_FAILURE;
}
