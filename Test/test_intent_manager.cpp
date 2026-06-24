#include "IntentManager.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int)
{
    g_stop.store(true, std::memory_order_release);
}

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string findConfig()
{
    const char* env = std::getenv("INTENT_CONFIG");
    if (env && *env && fileExists(env)) {
        return env;
    }
    const char* candidates[] = {
        "Config/config.toml",
        "../Config/config.toml",
        "../../Config/config.toml",
        "../../../Config/config.toml",
    };
    for (const char* path : candidates) {
        if (fileExists(path)) {
            return path;
        }
    }
    return "Config/config.toml";
}

const char* engineName(intent::EngineType e)
{
    return e == intent::EngineType::kOnline ? "online" : "offline";
}

const char* errorName(intent::IntentErrorCode c)
{
    switch (c) {
        case intent::IntentErrorCode::kOk:                return "ok";
        case intent::IntentErrorCode::kNotInitialized:    return "not_initialized";
        case intent::IntentErrorCode::kEngineUnavailable: return "engine_unavailable";
        case intent::IntentErrorCode::kNetworkFailure:    return "network_failure";
        case intent::IntentErrorCode::kProtocolError:     return "protocol_error";
        case intent::IntentErrorCode::kAuthFailure:       return "auth_failure";
        case intent::IntentErrorCode::kTimeout:           return "timeout";
        case intent::IntentErrorCode::kInvalidRequest:    return "invalid_request";
        case intent::IntentErrorCode::kInternalError:     return "internal_error";
    }
    return "unknown";
}

void printResult(const intent::IntentResult& r)
{
    std::cout << "[INTENT " << engineName(r.engine) << "] "
              << r.intent_name << " (confidence=" << r.confidence << ")";
    if (r.requires_confirmation) {
        std::cout << " [need_confirm]";
    }
    if (!r.slots.empty()) {
        std::cout << " slots={";
        bool first = true;
        for (const auto& kv : r.slots) {
            if (!first) std::cout << ", ";
            first = false;
            std::cout << kv.first << "=\"" << kv.second << "\"";
        }
        std::cout << "}";
    }
    std::cout << "\n";
}

void runSyncCase(intent::IntentManager& mgr,
                 const std::string&     text,
                 const std::map<std::string, std::string>& context = {})
{
    intent::IntentRequest req;
    req.text          = text;
    req.user_id       = "test-user";
    req.session_id    = "test-session";
    req.context_slots = context;

    intent::IntentResult result;
    intent::IntentError  error;
    std::cout << "\n> recognize: \"" << text << "\"\n";
    if (mgr.recognize(req, result, &error)) {
        printResult(result);
    } else {
        std::cerr << "  [ERROR " << engineName(error.engine) << "] "
                  << errorName(error.code) << ": " << error.message << "\n";
    }
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    quill::Backend::start();
    (void)quill::Frontend::create_or_get_logger(
        "IntentTest",
        quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
            "intent_test_console_sink"));

    const std::string cfg_path = findConfig();
    intent::IntentConfig cfg = intent::IntentManager::loadConfig(cfg_path);

    if (std::getenv("INTENT_FORCE_OFFLINE")) {
        cfg.preferred_engine = intent::EngineType::kOffline;
    } else if (std::getenv("INTENT_FORCE_ONLINE")) {
        cfg.preferred_engine = intent::EngineType::kOnline;
    }

    std::cout << "Intent test driver\n"
              << "config: " << cfg_path << "\n"
              << "preferred engine: " << engineName(cfg.preferred_engine) << "\n"
              << "offline rules loaded: " << cfg.offline.rules.size() << "\n"
              << "online endpoint: "
              << (cfg.online.endpoint.empty() ? "(not configured)" : cfg.online.endpoint)
              << "\n\n";

    intent::IntentManager mgr;
    if (!mgr.init(cfg)) {
        std::cerr << "IntentManager::init failed\n";
        return EXIT_FAILURE;
    }

    std::atomic<int> async_count{0};
    std::mutex       cout_mtx;

    mgr.setResultCallback([&](const intent::IntentResult& r) {
        async_count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(cout_mtx);
        std::cout << "[ASYNC] ";
        printResult(r);
    });
    mgr.setErrorCallback([&](const intent::IntentError& e) {
        std::lock_guard<std::mutex> lk(cout_mtx);
        std::cerr << "[ASYNC ERROR " << engineName(e.engine) << "] "
                  << errorName(e.code) << ": " << e.message << "\n";
    });

    std::cout << "==== synchronous cases ====\n";
    // 1. 轮椅移动控制（前进/后退/左/右 → 需要确认；停止 → 豁免）
    runSyncCase(mgr, "向前走");
    runSyncCase(mgr, "向后退一点");
    runSyncCase(mgr, "左转");
    runSyncCase(mgr, "右转");
    runSyncCase(mgr, "停下来");      // exempt: action_hint=停下 / action=stop
    runSyncCase(mgr, "刹车");         // exempt: action_hint=刹车 / action=stop
    // 2. 播放音乐
    runSyncCase(mgr, "播放周杰伦的稻香");
    runSyncCase(mgr, "暂停音乐");
    runSyncCase(mgr, "下一首");
    // 3. 机械臂末端控制 + 夹爪控制
    runSyncCase(mgr, "把机械臂抬起来");
    runSyncCase(mgr, "机械臂抓取一下");
    runSyncCase(mgr, "机械臂松开");
    // 4. 音量控制
    runSyncCase(mgr, "声音大一点");
    runSyncCase(mgr, "把音量调小");
    runSyncCase(mgr, "静音");
    runSyncCase(mgr, "音量调到五十");
    // 5. 自主导航（navigation.navigate）— target 槽位
    runSyncCase(mgr, "导航到厨房");
    runSyncCase(mgr, "带我去客厅");
    runSyncCase(mgr, "前往卧室那里");
    runSyncCase(mgr, "走到桌子旁边");
    // 6. 其他（聊天 / unknown）
    runSyncCase(mgr, "今天天气怎么样");
    runSyncCase(mgr, "你叫什么名字");
    runSyncCase(mgr, "");  // empty — should yield kInvalidRequest

    std::cout << "\n==== asynchronous cases ====\n";
    const std::vector<std::string> async_texts = {
        "前进一米",          // wheelchair.move
        "继续播放",          // music.control
        "机械臂伸出",        // arm.control
        "音量调到八十",      // volume.control
        "讲个笑话听听",      // chat
    };
    for (const auto& text : async_texts) {
        intent::IntentRequest req;
        req.text       = text;
        req.user_id    = "test-user";
        req.session_id = "test-session";
        std::cout << "> recognizeAsync: \"" << text << "\"\n";
        if (!mgr.recognizeAsync(req)) {
            std::cerr << "  enqueue failed\n";
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!g_stop.load(std::memory_order_acquire) &&
           async_count.load(std::memory_order_relaxed) <
               static_cast<int>(async_texts.size()) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }

    std::cout << "\n==== network state transitions ====\n";
    std::cout << "→ Offline\n";
    mgr.onNetworkStateChanged(network_manager::NetworkState::Offline);
    std::cout << "current engine: " << engineName(mgr.currentEngine()) << "\n";
    runSyncCase(mgr, "向前走一点");
    runSyncCase(mgr, "音量调小");
    runSyncCase(mgr, "今天星期几");  // → fallback chat

    std::cout << "→ OnlineGood\n";
    mgr.onNetworkStateChanged(network_manager::NetworkState::OnlineGood);
    std::cout << "current engine: " << engineName(mgr.currentEngine()) << "\n";

    mgr.shutdown();
    std::cout << "\nDone. async_results=" << async_count.load(std::memory_order_relaxed) << "\n";
    return EXIT_SUCCESS;
}
