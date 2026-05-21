# 智能语音音响 — 整体架构设计

---

## 1. 系统定位

本系统是一套运行在嵌入式 Linux 设备上的**商用智能语音音响框架**，目标对标小米小爱音箱 / 天猫精灵级别的语音交互体验。

核心能力：
- 低功耗持续唤醒词检测 + 说话人验证
- 在线 / 离线双模 ASR / NLU / TTS 引擎，网络故障自动降级
- 多轮对话上下文管理
- 插件化技能系统（IoT 控制、媒体播放、AI 对话等由插件实现）
- 7×24 h 无人工干预稳定运行

---

## 2. 模块一览

| 模块 | 定位 | 关键依赖 |
|------|------|----------|
| **SmartAssistant** | 顶层编排器，对话状态机 | 全部模块 |
| **AudioManager** | 音频输入/输出与实时链路管理 | toml++ 配置表, quill logger |
| **WakeupManager** | KWS 唤醒词检测 + SV 声纹初筛 | AudioManager, UserManager, toml++, quill |
| **UserManager** | 用户身份、声纹注册、偏好与账号管理 | toml++, quill |
| **ASRManager** | 语音识别（在线/离线引擎切换） | AudioManager, NetworkManager, toml++, quill |
| **IntentManager** | 意图识别 + 槽位提取（在线/离线引擎切换） | NetworkManager, toml++, quill |
| **SessionManager** | 多轮对话上下文状态管理 | toml++, quill |
| **PluginManager** | 插件系统（注册/发现/生命周期/路由） | toml++, quill |
| **TTSManager** | 文本转语音（在线/离线引擎切换） | AudioManager, NetworkManager, toml++, quill |
| **NetworkManager** | 网络状态监听，在线/离线切换通知 | toml++, quill |

> 不再设计 `ConfigManager` 和 `LogManager` 组件。配置和运行期状态由各模块在源文件中直接使用 toml++（tomlplusplus）读取/写入 TOML；日志由各模块直接使用 quill 的 logger 与日志宏。

---

## 3. 总体架构图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          SmartAssistant                                      │
│                      （对话状态机 / 全流程编排）                               │
│                                                                               │
│  IDLE ──► WAKEUP_DETECTED ──► LISTENING ──► UNDERSTANDING ──► EXECUTING     │
│    ▲                                                              │           │
│    └──────────────────── RESPONDING ◄────────────────────────────┘           │
└────┬──────────┬──────────┬──────────┬──────────┬──────────┬─────────────────┘
     │          │          │          │          │          │
     ▼          ▼          ▼          ▼          ▼          ▼
 Wakeup      ASR       Intent    Session    Plugin       TTS
 Manager    Manager    Manager   Manager    Manager     Manager
     │                                        │
     ▼                                        ▼
 User                                    [Plugin A] [Plugin B] [Plugin N]
 Manager                                  (技能实现，不在本框架内)

─────────────────────────── 音频基础设施 ────────────────────────────────────

┌─────────────────────────────────────────────────────────────────────────────┐
│                            AudioManager                                      │
│                   （麦克风采集 / 音频预处理 / 播放 / AEC）                    │
│                                                                               │
│   CaptureManager → ProcessingChain → FrameDispatcher                         │
│   PlaybackManager ← FocusManager(焦点) ← AudioRouter  ← 内部组件，与业务层    │
│                                                           SessionManager无关   │
│   VolumeManager   DeviceManager   Diagnostics                                 │
└─────────────────────────────────────────────────────────────────────────────┘

─────────────────────────── 横切关注点 ──────────────────────────────────────

 NetworkManager          toml++                 quill
 （在线/离线切换）         （TOML配置/状态）        （异步日志）
```

---

## 4. SmartAssistant — 对话状态机

SmartAssistant 是系统的唯一编排器，持有所有 Manager 引用，不做任何具体的音频/AI 处理。

### 4.1 状态定义

```
                      ┌─────────────────────────────────────────────────┐
                      │                   SmartAssistant                 │
                      │                                                   │
         wakeup_confirmed                                                 │
  IDLE ──────────────► WAKEUP_DETECTED                                   │
   ▲                        │ sv_passed / sv_skipped                     │
   │                        ▼                                            │
   │              ┌──── LISTENING ◄────────────────────────┐            │
   │              │         │ asr_final /                  │            │
   │              │         │ vad_end_hint(非流式触发)      │            │
   │   speech_    │         ▼                         multi_turn        │
   │   detected   │   UNDERSTANDING                   _continue         │
   │              │         │ intent_result                │            │
   │              │         ▼                              │            │
   │              └── MULTI_TURN_WAIT              EXECUTING ──────────►┘
   │    (多轮等待用户              ▲                    │ plugin_result  │
   │     说话，不回IDLE)           └────────────────────┘                │
   │                                                    ▼                │
   │                                               RESPONDING            │
   │                                                    │ tts_completed  │
   └────────────────────────────────────────────────────┘ (无追问)       │
                                                                         │
  任意状态 ──► ERROR ──────────────────────────────────────────► IDLE   │
  任意状态 ──► IDLE  (用户说"取消" / 超时 / 主动打断)                    │
                      └─────────────────────────────────────────────────┘
```

### 4.2 各状态职责

| 状态 | 进入动作 | 退出条件 |
|------|----------|----------|
| `IDLE` | 通知 WakeupManager 开始监听；AudioManager 采集保持运行 | WakeupManager 上报 `WakeupEvent` |
| `WAKEUP_DETECTED` | 播放唤醒提示音；查询 UserManager 确认用户身份 | SV 通过/跳过 → `LISTENING` |
| `LISTENING` | 通知 ASRManager 开始识别；订阅 AudioManager VAD 事件 | `asr_final` 或超时 → `UNDERSTANDING` |
| `UNDERSTANDING` | 将 ASR 文本 + SessionManager 上下文 → IntentManager | `intent_result` → `EXECUTING` |
| `EXECUTING` | 通过 PluginManager 路由到目标插件执行 | `plugin_result` → `RESPONDING` |
| `RESPONDING` | 将 plugin_result 文本 → TTSManager 合成并播放 | `tts_completed`：无追问 → `IDLE`；有追问 → `MULTI_TURN_WAIT` |
| `MULTI_TURN_WAIT` | 静默等待用户语音；WakeupManager 保持静默；超时计时器启动 | `VAD_SPEECH_START` → `LISTENING`；超时 → `IDLE` |
| `ERROR` | 记录错误；播放兜底提示音 | 无条件 → `IDLE` |

### 4.3 VAD 事件处理策略

AudioManager 发布 `VAD_SPEECH_START` / `VAD_SPEECH_END` 事件，SmartAssistant 在 `LISTENING` 状态下的处理：

```
VAD_SPEECH_END 收到：
  if ASRManager.mode == STREAMING:
      ASRManager.hintSpeechEnd()   // 提示可以结束，引擎自主决定
  else:
      ASRManager.finalize()        // 非流式：触发识别
```

流式 ASR 不依赖 VAD 事件结束识别，由引擎内部端点检测（EOS）驱动。

### 4.4 超时策略

| 超时场景 | 时限 | 处理 |
|----------|------|------|
| 唤醒后无语音输入 | 5 s | → `IDLE`，播放超时提示音 |
| ASR 识别无结果 | 10 s | → `ERROR` |
| Intent 解析无响应 | 5 s | → `ERROR` |
| Plugin 执行无响应 | 30 s | → `ERROR` |
| 多轮等待用户回复 | 30 s | → `IDLE` |

---

## 5. 模块职责详述

### 5.1 AudioManager

**职责：** 音频输入/输出与实时链路管理层，是整个系统的音频基础设施。详细设计见 [AudioManager 架构设计](./AUDIO_MANAGER_ARCHITECTURE.md)，此处仅描述与上层模块的交互契约。

**负责：**
- 麦克风采集、多通道预处理（AEC / NS / AGC / Beamforming）
- 向上层提供处理后的 16 kHz 单通道 PCM 流（FrameDispatcher 消费者模型）
- VAD 事件发布（`VAD_SPEECH_START` / `VAD_SPEECH_END`，投递到 ControlThread，非实时回调）
- 接收 PCM 流播放（TTS 合成结果、提示音、媒体音频），内部管理音频焦点与 ducking
- 软件音量、设备热插拔恢复

**不负责：** 唤醒词识别、ASR 识别、NLU、TTS 文本合成、对话状态。

**向上层暴露的关键接口（本文档视角）：**

```cpp
class AudioManager {
public:
    bool   init(const Config&);
    void   shutdown();

    // 采集
    bool   startCapture();
    void   stopCapture();

    // FrameDispatcher：上层消费者（WakeupManager / ASRManager）注册 PCM 帧回调
    ConsumerHandle addFrameConsumer(FrameCallback, size_t max_queue_depth);
    void           removeFrameConsumer(ConsumerHandle);

    // 播放（TTSManager / PluginManager 通过此接口送入 PCM）
    PlaybackHandle play(const PlaybackRequest&);
    void           stop(PlaybackHandle);
    void           stopAll();

    // 音量
    void  setMasterVolume(float);   // [0.0, 1.0]
    float masterVolume() const;

    // 事件订阅（SmartAssistant 订阅 VAD / 设备事件）
    void subscribe(AudioEventCallback);

    // 诊断
    HealthStatus health() const;
};
```

**AudioManager 发布的事件（SmartAssistant 关注）：**

```
VAD_SPEECH_START          // 检测到语音开始，MULTI_TURN_WAIT 状态下触发转入 LISTENING
VAD_SPEECH_END            // 检测到语音结束，LISTENING 状态下提示 ASR
PlaybackCompleted(handle) // TTSManager 监听，播放结束后回调 SmartAssistant
DeviceFailed              // 设备不可恢复，SmartAssistant 转入 ERROR
```

**消费者优先级约定：**

| 消费者 | 工作时段 | 队列深度 |
|--------|----------|----------|
| WakeupManager | 全程（IDLE 等所有状态） | 50 帧 |
| ASRManager | LISTENING 状态期间 | 200 帧 |
| Diagnostics | 按需开启 | 100 帧 |

WakeupManager 与 ASRManager 共享同一路 PCM 流，互不干扰；SmartAssistant 在进入 `LISTENING` 时调用 `ASRManager.startRecognition()`，ASRManager 内部向 FrameDispatcher 注册消费者，退出 `LISTENING` 时注销。

---

### 5.2 WakeupManager

**职责：** KWS 持续检测 + 声纹初筛（SV），触发唤醒事件。

**不负责：** 用户身份持久化、个性化内容。

```
AudioManager.FrameDispatcher (PCM 16kHz)
  → KWS Engine（低功耗，MFCC + DNN/MDTC）
  → 触发候选
  → SV Engine（ECAPA-TDNN 或等效）
  → 与 UserManager 注册声纹比对
  → WakeupEvent { user_id, confidence, keyword }
  → SmartAssistant
```

**关键接口：**
```cpp
class WakeupManager {
public:
    bool   init(const Config&);
    void   startListening();
    void   stopListening();
    void   setWakeCallback(WakeCallback);  // SmartAssistant 注册
    void   muteWakeup(bool);               // LISTENING/RESPONDING 期间静默，避免误触发
};
```

**与 UserManager 交互：** SV 得分 + 声纹 embedding 交给 UserManager 做最终身份决策，WakeupManager 只关心「是否通过阈值」。

---

### 5.3 UserManager

**职责：** 用户身份管理、声纹注册/更新、账号绑定、用户偏好存取。

```
用户实体：
  - user_id       (本地唯一标识)
  - display_name
  - voiceprint[]  (声纹 embedding 列表，支持多条)
  - account_token (云端账号 OAuth token，可为空)
  - preferences   (音量偏好、语言、个性化开关等)
```

**关键接口：**
```cpp
class UserManager {
public:
    UserId   identifyUser(const VoiceEmbedding&, float& score);
    bool     enrollUser(UserId, const VoiceEmbedding&);
    UserProfile getProfile(UserId);
    void     updatePreference(UserId, const Preference&);
    string   getAccountToken(UserId);   // 供在线 ASR/TTS/Intent 引擎鉴权使用
};
```

**持久化：** 声纹 embedding 由 UserManager 直接使用 toml++ 写入 `state/users/{user_id}.toml`；账号 token 加密后再落盘。

---

### 5.4 ASRManager

**职责：** 语音识别引擎管理，支持在线/离线引擎切换，向 SmartAssistant 提供 ASR 结果。

**引擎切换策略：** 由 NetworkManager 的 `OnlineStateChanged` 事件驱动，SmartAssistant 无需感知。

```
在线引擎（默认）: WebSocket 流式 ASR（阿里云 / 讯飞 / 自建）
离线引擎（降级）: Vosk / Whisper.cpp / Sherpa-ONNX
```

```
AudioManager.FrameDispatcher (PCM 16kHz)
  → ASRManager（接管 FrameDispatcher 消费者）
  → 在线/离线引擎
  → 中间结果: ASRPartialResult { text, is_final=false }
  → 最终结果: ASRFinalResult  { text, confidence }
  → SmartAssistant
```

**关键接口：**
```cpp
class ASRManager : public IEngineSwitch {
public:
    bool  init(const Config&);
    void  startRecognition(ASRMode mode = ASRMode::kAuto);
    void  stopRecognition();
    void  hintSpeechEnd();    // 流式 ASR 软提示
    void  finalize();         // 非流式 ASR 触发识别

    void  setPartialCallback(PartialCallback);  // 中间结果（可用于 UI 实时展示）
    void  setFinalCallback(FinalCallback);      // 最终结果 → SmartAssistant
    void  setErrorCallback(ErrorCallback);

    EngineType currentEngine() const override;  // kOnline | kOffline
    void onNetworkStateChanged(NetworkState) override;
    bool isOnlineAvailable() const override;
};
```

---

### 5.5 IntentManager

**职责：** 将 ASR 文本 + 上下文 → 意图 + 槽位，支持在线/离线引擎切换。**不负责多轮对话状态**（由 SessionManager 管）。

```
在线引擎: 云端 NLU（RESTful / gRPC）
离线引擎: Rasa NLU / 本地规则引擎 / 轻量 BERT 模型
```

**输入：**
```
IntentRequest {
    text        : string          // ASR 识别文本
    context     : SessionContext  // 由 SessionManager 提供，含历史意图/槽位
    user_id     : UserId
}
```

**输出：**
```
IntentResult {
    intent_name : string          // "play_music", "set_alarm", "query_weather"...
    slots       : map<string,any> // {"song": "稻香", "artist": "周杰伦"}
    confidence  : float
    raw_response: string          // 原始云端/本地引擎响应（调试用）
}
```

**关键接口：**
```cpp
class IntentManager : public IEngineSwitch {
public:
    void recognize(const IntentRequest&, IntentCallback);
    EngineType currentEngine() const override;
    void onNetworkStateChanged(NetworkState) override;
    bool isOnlineAvailable() const override;
};
```

---

### 5.6 SessionManager

**职责：** 多轮对话上下文状态管理。存储每轮的意图、槽位、ASR 文本，判断是否需要追问，为下一轮 IntentManager 提供上下文。

**不负责：** 音频焦点（AudioManager 内部管理）、插件执行。

**持久化：** SessionManager 直接使用 toml++ 写入 `state/sessions/{session_id}.toml`；系统关闭时由 `closeAllSessions()` 统一 flush，重启后可恢复未完成的多轮会话（可配置关闭）。

```
Session 生命周期：
  创建（WakeupEvent）→ 活跃（每轮对话追加）→ 关闭（多轮超时 / 主动结束）
```

```
SessionContext {
    session_id    : UUID
    user_id       : UserId
    turns         : Turn[]          // 历史轮次
    pending_slots : map<string,any> // 当前意图仍缺失的槽位
    current_intent: string          // 当前正在进行的意图
    created_at    : timestamp
    last_active   : timestamp
}

Turn {
    asr_text    : string
    intent      : IntentResult
    plugin_resp : string            // 插件返回给用户的文本
}
```

**关键接口：**
```cpp
class SessionManager {
public:
    SessionId   createSession(UserId);
    void        appendTurn(SessionId, const Turn&);
    SessionContext getContext(SessionId);
    bool        needsClarification(SessionId);  // 是否有缺失槽位需要追问
    string      buildClarificationPrompt(SessionId);
    void        closeSession(SessionId);
    void        closeAllSessions();
};
```

---

### 5.7 PluginManager

**职责：** 插件系统框架，负责插件的注册、发现、生命周期管理、意图路由。**不实现任何具体技能**（音乐、IoT、AI 对话等均为独立插件）。

**插件契约（IPlugin 接口）：**
```cpp
struct PluginContext {
    const toml::table*       app_config;        // 全局 TOML 配置的只读视图
    std::filesystem::path    plugin_state_dir;  // 插件自行用 toml++ 读写状态
    quill::Logger*           logger;            // 插件专属 quill logger
    AudioManager*            audio;             // 需要播放音频的插件（如音乐插件）可持有此引用
};

class IPlugin {
public:
    virtual string       name() const = 0;
    virtual string       version() const = 0;
    virtual vector<string> intentNames() const = 0;  // 声明处理哪些意图
    virtual int          priority() const { return 0; }

    virtual void         onLoad(PluginContext&) = 0;
    virtual void         onUnload() = 0;
    virtual PluginResult execute(const IntentResult&,
                                 const SessionContext&) = 0;
};

struct PluginResult {
    string   response_text;     // 返回给用户的文字
    bool     need_follow_up;    // 是否需要多轮追问
    string   follow_up_prompt;  // 追问提示文字（need_follow_up=true 时有效）
    any      data;              // 插件自定义数据（供 SmartAssistant 扩展）
};
```

**意图路由：** 按 `intentNames()` 注册路由表，同一意图多个插件时按 `priority()` 降序选择。

**关键接口：**
```cpp
class PluginManager {
public:
    bool   loadPlugin(const string& so_path);
    bool   unloadPlugin(const string& plugin_name);
    void   reloadPlugin(const string& plugin_name);
    bool   routeIntent(const IntentResult&,
                       const SessionContext&,
                       PluginResultCallback);
    vector<PluginInfo> listPlugins() const;
};
```

---

### 5.8 TTSManager

**职责：** 文本转语音引擎管理，输出 PCM 数据交给 AudioManager 播放。

```
在线引擎: 云端 TTS（阿里云 / 讯飞 / Azure TTS）
离线引擎: piper / Coqui TTS / MNN-TTS
```

**与 AudioManager 交互：**
```
TTSManager.synthesize(text)
  → 引擎合成 PCM 数据
  → AudioManager.play(PlaybackRequest{ pcm, priority=TTS })
  → PlaybackHandle
  → 监听 PlaybackCompleted 事件
  → 回调 SmartAssistant TTSCompleted
```

**关键接口：**
```cpp
class TTSManager : public IEngineSwitch {
public:
    void synthesizeAndPlay(const string& text,
                           TTSOptions opts,
                           TTSCallback on_complete);
    void stop();
    EngineType currentEngine() const override;
    void onNetworkStateChanged(NetworkState) override;
    bool isOnlineAvailable() const override;
};

struct TTSOptions {
    string   voice;        // 音色
    float    speed;        // 语速 [0.5, 2.0]
    float    volume;       // 相对音量 [0.0, 1.0]
    string   language;     // "zh-CN", "en-US"
};
```

---

### 5.9 NetworkManager

**职责：** 监听网络连接状态，通知 ASRManager / IntentManager / TTSManager 切换引擎；维护在线引擎的连接健康检查。

```
网络状态：
  ONLINE_GOOD    → 延迟 < 阈值，在线引擎可用
  ONLINE_POOR    → 延迟 > 阈值，降级策略（可配置是否切换）
  OFFLINE        → 无连接，强制切换离线引擎
```

**切换触发条件（可配置）：**

| 条件 | 默认阈值 | 动作 |
|------|----------|------|
| 网络不可达 | — | 立即切换离线 |
| 在线引擎连续超时 | 3 次 | 切换离线 |
| RTT > 阈值 | 800 ms | 切换离线（可配置关闭） |
| 网络恢复 | 稳定 10 s | 切回在线 |

**关键接口：**
```cpp
class NetworkManager {
public:
    NetworkState currentState() const;
    void   subscribe(NetworkStateCallback);   // ASR/Intent/TTS Manager 注册
    void   forceOffline(bool);                // 调试 / 用户手动设置
    int    measureRtt(const string& endpoint);
};
```

---

### 5.10 配置与状态（toml++ / tomlplusplus）

**职责：** 不再由独立配置管理组件统一代理。启动入口和各 Manager 在源文件中直接使用 toml++ 解析 TOML，并把需要的子表转换为本模块自己的 `Config` 结构。

#### 静态配置（TOML）

启动时加载，运行期只读。存放硬件参数、引擎 endpoint、功能开关等不在运行时频繁变更的配置。

```toml
# config/assistant.toml
[audio]
backend = "alsa"

[audio.capture]
device = "hw:0,0"
sample_rate = 48000
channels = 4
channel_map = [0, 1, 2, 3]
format = "s16"
frame_ms = 10

[audio.playback]
device = "hw:0,0"
sample_rate = 48000
channels = 2
format = "s16"
frame_ms = 10

[engines.asr.online]
provider = "aliyun"
endpoint = "wss://nls-gateway.aliyun.com/ws/v1"
app_key_env = "ASR_APP_KEY"        # 从环境变量注入，不硬编码密钥

[engines.asr.offline]
model_path = "/opt/models/vosk-cn"

[engines.tts.online]
provider = "aliyun"
app_key_env = "TTS_APP_KEY"

[engines.tts.offline]
model_path = "/opt/models/piper-zh"

[engines.intent.online]
endpoint = "https://nlu.example.com/v1"
api_key_env = "NLU_API_KEY"

[engines.intent.offline]
model_path = "/opt/models/rasa"

[wakeup]
keyword = "小智"
kws_model = "/opt/models/kws-xiaozhi"
sv_threshold = 0.75

[network]
rtt_threshold_ms = 800
online_stable_seconds = 10
max_consecutive_timeout = 3
```

模块内直接读取：

```cpp
#include <toml++/toml.hpp>

toml::table cfg = toml::parse_file(config_path);

auto sample_rate = cfg["audio"]["capture"]["sample_rate"].value_or(48000);
auto endpoint = cfg["engines"]["asr"]["online"]["endpoint"].value_or("");
auto app_key_env = cfg["engines"]["asr"]["online"]["app_key_env"].value_or("ASR_APP_KEY");
```

#### 运行期状态（TOML 持久化）

运行时可持久化状态不再是中心化键值存储。各模块拥有自己的状态文件，并在状态变更、会话关闭或系统关闭时用 toml++ 写回；写入采用临时文件 + 原子 rename，避免掉电留下半文件。

```
state/
  system.toml
  users/{user_id}.toml
  sessions/{session_id}.toml
  plugins/{name}.toml
```

状态文件示例：

```toml
# state/users/{user_id}.toml
user_id = "local-user-001"
display_name = "家庭用户"
voiceprints = ["base64-float-array-0", "base64-float-array-1"]
account_token = "aes256:ciphertext"

[preferences]
volume = 0.75
language = "zh-CN"
personalization = true
```

```cpp
#include <fstream>
#include <toml++/toml.hpp>

toml::table user_state;
user_state["user_id"] = user_id;
user_state["display_name"] = display_name;
user_state["preferences"]["volume"] = volume;

std::ofstream out(tmp_path);
out << user_state;
```

---

### 5.11 日志（quill）

**职责：** 不再由独立日志管理组件统一代理。进程启动时初始化 quill 后端和 sinks；各模块在自己的源文件中直接获取或创建 `quill::Logger*`，并调用 quill 日志宏。

**设计原则：**
- 实时线程（AudioManager 的 CaptureThread / PlaybackThread）**禁止**执行日志格式化和磁盘 I/O；只把结构化事件写入 lock-free queue，由普通线程消费后调用 quill。
- 其余模块直接使用 quill logger，不再包一层日志管理组件。

```cpp
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/RotatingFileSink.h"

quill::Backend::start();

auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
    "logs/assistant.log",
    quill::RotatingFileSinkConfig{}
);
quill::Logger* logger =
    quill::Frontend::create_or_get_logger("assistant.asr", std::move(file_sink));

LOG_DEBUG(logger, "engine switched to offline, rtt={}", rtt_ms);
LOG_INFO(logger, "state: {} -> {}", from_state, to_state);
LOG_WARN(logger, "SV score below threshold: {:.2f}", score);
LOG_ERROR(logger, "ALSA xrun on device {}, count={}", device_name, xrun_cnt);
```

**Sink：**

| Sink | 说明 |
|------|------|
| quill file sink | 按日期或大小滚动，最大保留 7 天 |
| quill console sink | 开发调试用，可通过 TOML 配置关闭 |
| metrics bridge | 普通线程从结构化事件队列消费后上报可观测性系统 |

---

## 6. 关键数据流

### 6.1 完整对话链路（在线模式）

```
麦克风
  → AudioManager（AEC / NS / AGC / Beamforming）
  → WakeupManager（KWS 检测）
  ─── 触发唤醒 ──────────────────────────────────────────►
                                              SmartAssistant
                                              [IDLE → WAKEUP_DETECTED]
  ◄── 唤醒提示音（AudioManager.play）─────────────────────
  ─── SV 声纹比对（UserManager）───────────────────────────
  ◄── user_id confirmed ─────────────────────────────────
                                              [→ LISTENING]
  ─── ASRManager.startRecognition() ──────────────────────
  (用户说话)
  ── VAD_SPEECH_END ──────────────────────────────────────►
                                              ASRManager.hintSpeechEnd()
  ── ASRFinalResult{text} ────────────────────────────────►
                                              [→ UNDERSTANDING]
  ── IntentManager.recognize(text, context) ──────────────►
  ── IntentResult{intent, slots} ─────────────────────────►
                                              SessionManager.appendTurn()
                                              [→ EXECUTING]
  ── PluginManager.routeIntent() ─────────────────────────►
  ── PluginResult{response_text} ─────────────────────────►
                                              [→ RESPONDING]
  ── TTSManager.synthesizeAndPlay(response_text) ─────────►
  ── AudioManager.play(PCM) ──────────────────────────────►
  ◄── PlaybackCompleted ─────────────────────────────────
                                              [→ IDLE / LISTENING(多轮)]
```

### 6.2 网络故障降级链路

```
NetworkManager 检测到连接断开
  → 广播 NetworkStateChanged(OFFLINE)
  → ASRManager:    切换至 Vosk/Sherpa 离线引擎
  → IntentManager: 切换至本地规则/BERT 离线引擎
  → TTSManager:    切换至 piper/Coqui 离线引擎
  → quill:         记录降级事件
网络恢复（稳定 10 s）
  → 广播 NetworkStateChanged(ONLINE_GOOD)
  → 三个 Manager 切回在线引擎
```

### 6.3 多轮对话链路

```
PluginResult.need_follow_up = true
  → SmartAssistant [EXECUTING → RESPONDING]
  → TTSManager.synthesizeAndPlay(follow_up_prompt)
  → PlaybackCompleted
  → SmartAssistant [RESPONDING → LISTENING]  ← 不回 IDLE
  → WakeupManager.muteWakeup(true)           ← 多轮期间禁止误唤醒
  → ASRManager.startRecognition()
  (用户回答)
  → ASRFinalResult → IntentManager(携带 SessionContext)
  → PluginManager 继续执行（已有完整槽位）
```

### 6.4 VAD 事件流

```
AudioManager.ProcessingChain（VAD 检测）
  → AudioManager 发布事件（投递到 ControlThread，非实时线程）
  → SmartAssistant.subscribe(AudioEvent)

SmartAssistant 处理：
  VAD_SPEECH_START：
    当前状态 == MULTI_TURN_WAIT → 转入 LISTENING
  VAD_SPEECH_END：
    当前状态 == LISTENING：
      ASRMode == STREAMING  → ASRManager.hintSpeechEnd()
      ASRMode == BATCH      → ASRManager.finalize()
```

---

## 7. 模块间依赖关系

```
       toml++（直接解析 TOML）        quill（直接记录日志）
              ▲                              ▲
              │                              │
              └──────── 所有模块按需直接 include ────────┘

        NetworkManager           AudioManager
              │                    ┌─────┴──────┐
    ┌─────────┴──┐                 │WakeupManager│
    │ ASRManager │                 └─────┬──────┘
    │IntentManager│                       │
    │ TTSManager │                  UserManager
    └─────┬──────┘
          │
          └──────────────┐
                         │
                  SmartAssistant
                  ├── WakeupManager
                  ├── ASRManager
                  ├── IntentManager
                  ├── SessionManager
                  ├── PluginManager
                  ├── TTSManager
                  ├── UserManager
                  ├── AudioManager
                  └── NetworkManager
```

**初始化顺序（严格按序）：**

```
1. quill Backend::start()，创建进程级 file/console sinks
2. toml++ 解析 `config/assistant.toml`，或由各模块在 init 中直接解析
3. NetworkManager
4. AudioManager
5. UserManager
6. WakeupManager
7. ASRManager
8. IntentManager
9. TTSManager
10. SessionManager
11. PluginManager
12. SmartAssistant.init()  ← 全部就绪后启动状态机
```

**关闭顺序（初始化逆序）：**

```
1. SmartAssistant.shutdown()  ← 先停状态机
2. PluginManager（卸载所有插件）
3. SessionManager（用 toml++ 持久化未关闭 session）
4. TTSManager（停止合成/播放）
5. ASRManager
6. IntentManager
7. WakeupManager
8. UserManager
9. AudioManager
10. NetworkManager
11. 各 Manager flush 自己的 TOML 状态文件
12. flush quill 日志缓冲
```

---

## 8. 在线/离线引擎切换

三个支持双模的 Manager（ASR / Intent / TTS）遵循统一的引擎切换接口：

```cpp
class IEngineSwitch {
public:
    virtual void onNetworkStateChanged(NetworkState) = 0;
    virtual EngineType currentEngine() const = 0;
    virtual bool isOnlineAvailable() const = 0;
};
```

**切换原则：**
- 当前正在进行的请求**不打断**，本次请求完成后生效
- 离线引擎须在启动时完成模型加载（非懒加载），确保切换无延迟
- 切换事件通过 quill 记录，并上报 MetricsSink

---

## 9. 配置文件结构

```
/opt/assistant/
├── config/
│   └── assistant.toml        # 静态配置（只读）
├── state/
│   ├── system.toml           # 系统运行期状态
│   ├── users/                # 用户与偏好状态
│   ├── sessions/             # 可恢复会话状态
│   └── plugins/              # 插件状态
├── models/
│   ├── kws-keyword/          # 唤醒词模型
│   ├── sv-ecapa/             # 声纹模型
│   ├── asr-offline/          # 离线 ASR 模型
│   ├── intent-offline/       # 离线 NLU 模型
│   └── tts-offline/          # 离线 TTS 模型
├── plugins/
│   ├── plugin_music.so
│   ├── plugin_iot.so
│   └── plugin_aichat.so
└── logs/
    └── assistant.log
```

---

## 10. 可观测性指标汇总

在 AudioManager 已有指标基础上，追加：

```
# SmartAssistant
dialog_state_transitions_total{from, to}
dialog_session_duration_ms
dialog_error_total{reason}
wakeup_events_total{user_id}
wakeup_sv_reject_total

# ASRManager
asr_requests_total{engine=online|offline}
asr_latency_ms{engine}
asr_error_total{engine, reason}
asr_engine_switches_total{direction=online_to_offline|offline_to_online}

# IntentManager / TTSManager（同上模式）

# NetworkManager
network_state_changes_total{from, to}
network_rtt_ms{endpoint}
```

---

## 11. 部署

```ini
[Unit]
Description=Smart Voice Assistant
After=network.target sound.target

[Service]
User=voice
Group=audio
WorkingDirectory=/opt/assistant
ExecStart=/opt/assistant/bin/assistant --config /opt/assistant/config/assistant.toml
Restart=always
RestartSec=3
LimitRTPRIO=10          # 允许实时线程
LimitMEMLOCK=infinity   # 允许 mlockall（实时音频必须）

[Install]
WantedBy=multi-user.target
```

API 密钥通过 systemd `EnvironmentFile` 注入，不写入任何配置文件：

```ini
EnvironmentFile=/etc/assistant/secrets.env
```

```
# /etc/assistant/secrets.env（权限 600，只有 voice 用户可读）
ASR_APP_KEY=xxxxx
TTS_APP_KEY=xxxxx
NLU_API_KEY=xxxxx
```
