# AudioManager 架构设计

## 1. 定位

`AudioManager` 是智能语音音箱的音频输入输出与实时链路管理层。

**外部依赖：**
- `toml++`（tomlplusplus）：在源文件中直接解析音频相关 TOML 配置
- `quill`：在源文件中直接记录日志；实时线程通过 lock-free 队列间接投递，不做日志格式化

**负责：**
- 麦克风采集、多通道预处理（AEC/NS/AGC/Beamforming）
- 向上层提供处理后的 PCM 流（唤醒词引擎、ASR 客户端消费）
- 接收 PCM 流播放（TTS 合成结果、提示音、媒体音频）
- 软件音量、会话优先级、设备管理、热插拔恢复

**不负责：**
- 唤醒词识别、ASR 识别、NLU、TTS 文本合成
- 对话状态机、业务逻辑

---

## 2. 性能指标

| 指标 | 目标 |
|------|------|
| 采集 → ASR 端到端延迟 | ≤ 100 ms |
| 播放请求 → 首帧输出 | ≤ 50 ms |
| AEC reference 对齐误差 | ≤ ±1 ms |
| 稳定运行 | 7×24 h 无人工干预 |

---

## 3. 总体架构

```
┌─────────────────────────────────────────────────────────┐
│                    业务层 (Voice Service)                 │
│  WakeWord Engine │ ASR Client │ TTS Player │ Media Player │
└────────┬─────────────────────────┬───────────────────────┘
         │ PCM 帧 (消费)            │ PlaybackRequest (生产)
         ▼                         ▼
┌─────────────────────────────────────────────────────────┐
│                      AudioManager                        │
│                                                          │
│  CaptureManager ──► ProcessingChain ──► FrameDispatcher  │
│                            ▲                             │
│                     AEC Reference                        │
│                            │                             │
│  PlaybackManager ◄── FocusManager ◄── AudioRouter        │
│       │                                                  │
│  VolumeManager   DeviceManager   Diagnostics             │
└────────┬─────────────────────────────────────────────────┘
         │ IAudioBackend
         ▼
┌─────────────────┐
│   ALSA Backend  │
└────────┬────────┘
         ▼
   Linux Audio Devices
```

---

## 4. 线程模型

| 线程 | 调度 | 职责 |
|------|------|------|
| CaptureThread | SCHED_FIFO | 读硬件，写 CaptureRingBuf |
| PlaybackThread | SCHED_FIFO | 拉 Mixer 输出，写硬件，抽 AEC ref |
| ProcessingThread | 普通 | DSP 链，分发给消费者 |
| ControlThread | 普通 | API 调用、状态机、设备事件串行处理 |
| DeviceThread | 普通 | udev 热插拔监听 |
| DiagnosticsThread | 普通 | PCM dump、指标上报 |

**实时线程硬性约束（CaptureThread / PlaybackThread）：**
无 malloc、无磁盘/网络 I/O、无 mutex 竞争、无日志格式化。线程间通信使用 lock-free queue。

---

## 5. 模块职责

### 5.1 CaptureManager

打开/关闭输入设备，维护 CaptureThread 和 CaptureRingBuf。

```
采样率: 48000 Hz
位深:   int16
通道:   由 channel_map 配置（1/2/4/6/8）
帧长:   10 ms
CaptureRingBuf 容量: 200 ms（20 帧）
```

### 5.2 AudioProcessingChain

运行在 ProcessingThread，从 CaptureRingBuf 消费，输出单通道 16kHz PCM。

```
Raw Mic PCM (N ch, 48kHz)
  → ChannelMapper
  → HighPassFilter
  → Per-channel AEC          ← 每通道独立消回声
  → Beamformer               → 单通道
  → NoiseSuppressor
  → AGC / VAD
  → Resampler (48k → 16k)    ← libsoxr
  → FrameDispatcher
```

依赖：WebRTC Audio Processing（AEC/NS/AGC/VAD）、libsoxr（重采样 + 时钟漂移补偿）。

**时钟漂移补偿：** ProcessingThread 每 30 s 测量采集与播放时钟累计差值，超过 ±0.5 ms 时通过 libsoxr 变速重采样对 AEC reference 做轻微拉伸/压缩。

### 5.3 FrameDispatcher

将处理后的 PCM 帧分发给多个消费者（唤醒词引擎、ASR 客户端、Diagnostics）。

每个消费者持有独立队列，队列满时**丢帧**（不阻塞 ProcessingThread），递增该消费者的 `dropped_frames` 计数。

```cpp
class FrameDispatcher {
public:
    ConsumerHandle addConsumer(FrameCallback cb, size_t max_queue_depth);
    void removeConsumer(ConsumerHandle);
    void dispatch(const AudioFrame&);  // called by ProcessingThread
};
```

### 5.4 PlaybackManager

接收业务层的 `PlaybackRequest`（PCM 流或文件路径），管理播放队列、暂停、恢复、停止、打断。

**Mixer：** 最多 4 路并发，float32 内部精度，混音后转 int16。溢出由 Limiter 处理，Mixer 不做削波。

**AEC Reference Tap：** 在 Limiter 之后、写入硬件之前抽取信号，写入 ReferenceRingBuf（500 ms）。

### 5.5 FocusManager

管理音频焦点、优先级抢占、ducking、恢复策略。不感知具体设备。

> **命名说明：** 此为 AudioManager 内部组件，与业务层的 `SessionManager`（多轮对话上下文）无关。

```
优先级: ALARM > SYSTEM > TTS > PROMPT > MEDIA > BLUETOOTH
```

| 场景 | 策略 |
|------|------|
| ALARM 触发 | 抢占所有，独占播放 |
| TTS 播放 | MEDIA ducking −12 dB |
| PROMPT 播放 | 与 MEDIA 混音 |
| TTS 结束 | 恢复 MEDIA |

焦点变更通知 AudioRouter，由 AudioRouter 决定路由目标。FocusManager 不感知设备。

### 5.6 AudioRouter

决定采集输入来源和播放输出目标。由 FocusManager 焦点变更事件驱动。

- 支持按播放类型路由到不同设备（如 ALARM 强制路由到内置扬声器）
- 外接声卡断开后自动降级到内置声卡

### 5.7 VolumeManager

软件音量，不修改系统音量。

```
final_gain = master_gain × stream_gain × session_gain × ducking_gain × mute_gain
```

对数音量曲线。增益变化在 10 ms 内线性渐变，防止爆音。支持淡入淡出、限幅、配置持久化。

### 5.8 DeviceManager

枚举设备、查询能力、监听热插拔、自动恢复。

**状态机：**

```
              open()
AVAILABLE ──────────────► ACTIVE
    ▲                        │ 断开 / xrun 超阈
    │ 恢复成功                ▼
RECOVERING ◄────────── DISCONNECTED
    │ 超最大重试(5次)
    ▼
  FAILED ──── 手动触发 ──► RECOVERING
```

- `ACTIVE → DISCONNECTED`：设备断开或 xrun 超阈值
- `RECOVERING → AVAILABLE`：重新枚举并验证设备可用
- `RECOVERING → FAILED`：指数退避，最大 5 次

### 5.9 Diagnostics

运行期可开关，运行在 DiagnosticsThread。

- dump 原始麦克风、预处理后、AEC reference PCM
- 记录 xrun、丢帧、重连次数
- 统计各链路延迟和处理耗时
- 提供 `HealthStatus` 快照

---

## 6. 数据流

### 6.1 采集链路

```
Microphone
  → ALSA Backend
  → CaptureThread → CaptureRingBuf (200 ms)
  → ProcessingThread → ProcessingChain
  → FrameDispatcher
  → [WakeWord Engine] [ASR Client] [Diagnostics]
```

### 6.2 播放链路

```
PlaybackRequest (PCM stream / file)
  → FocusManager (焦点仲裁)
  → Decoder / PCM Source
  → VolumeManager (per-stream gain)
  → Mixer (float32, ≤4路)
  → Limiter
  → AEC Reference Tap → ReferenceRingBuf (500 ms)
  → PlaybackThread → ALSA Backend → Speaker
```

### 6.3 AEC Reference 链路

```
ReferenceRingBuf (500 ms, 带硬件时间戳)
  → DelayCompensator (偏移 aec_reference_delay_ms)
  → AEC Processor (per-channel)
```

**延迟组成：**

```
total_delay = playback_hw_buffer_delay
            + dac_analog_delay        (5~15 ms，硬件固定)
            + speaker_to_mic_delay    (声学传播，取决于物理尺寸)
            + adc_analog_delay        (5~15 ms，硬件固定)
            + capture_hw_buffer_delay
```

`aec_reference_delay_ms` 静态配置，调试阶段实测确定。硬件批量生产前须重新标定。

---

## 7. 后端接口

```cpp
enum class BackendError {
    kOk           = 0,
    kDeviceGone   = 1,  // 触发 DeviceManager 恢复流程
    kXrun         = 2,  // 直接重试
    kIOError      = 3,  // 触发 DeviceManager 恢复流程
    kInvalidState = 4,  // 编程错误，通过 quill 记录
};

struct NegotiatedFormat {
    int  sample_rate;
    int  channels;
    int  period_frames;
    int  buffer_frames;
    bool is_float;
};

class IAudioBackend {
public:
    virtual BackendError    openInput(const DeviceConfig&, NegotiatedFormat&) = 0;
    virtual BackendError    openOutput(const DeviceConfig&, NegotiatedFormat&) = 0;
    virtual BackendError    readInput(AudioFrame&) = 0;
    virtual BackendError    writeOutput(const AudioFrame&) = 0;
    virtual AudioDeviceList listInputDevices() = 0;
    virtual AudioDeviceList listOutputDevices() = 0;
    virtual void            closeInput() = 0;
    virtual void            closeOutput() = 0;
};
```

`NegotiatedFormat` 返回实际协商结果，调用方以此重新计算 buffer 大小和延迟预算。

---

## 8. 公开 API

```cpp
class AudioManager {
public:
    bool   init(const Config&);
    void   shutdown();

    // 采集
    bool   startCapture();
    void   stopCapture();

    // FrameDispatcher：上层消费者注册 PCM 帧回调
    // 消费者（WakeupManager / ASRManager / Diagnostics）通过此接口接入 16kHz 单通道 PCM 流
    ConsumerHandle addFrameConsumer(FrameCallback cb, size_t max_queue_depth);
    void           removeFrameConsumer(ConsumerHandle);

    // 播放（TTSManager / 提示音 / 媒体音频）
    PlaybackHandle play(const PlaybackRequest&);
    void           stop(PlaybackHandle);
    void           stopAll();

    // 音量
    void  setMasterVolume(float);   // [0.0, 1.0]，内部转对数增益
    float masterVolume() const;

    // 设备
    DeviceList listInputDevices() const;
    DeviceList listOutputDevices() const;

    // 诊断
    HealthStatus health() const;

    // 事件订阅（投递到 ControlThread 队列，不在实时线程回调）
    void subscribe(AudioEventCallback);
};
```

**事件完整列表（投递到 ControlThread 队列，不在实时线程回调）：**

```
// 采集事件
CaptureStarted / CaptureStopped / CaptureError

// VAD 事件（由 AudioProcessingChain 检测，供上层 SmartAssistant 订阅）
VAD_SPEECH_START      // 检测到语音起点
VAD_SPEECH_END        // 检测到语音终点，LISTENING 状态下提示 ASRManager

// 播放事件
PlaybackStarted(handle) / PlaybackCompleted(handle)
PlaybackInterrupted(handle) / PlaybackError(handle)

// 设备事件
DeviceConnected / DeviceDisconnected / DeviceRecovered / DeviceFailed

// 其他
VolumeChanged / HealthChanged
```

---

## 9. 配置（TOML / toml++）

AudioManager 不依赖独立配置管理组件。初始化时在源文件中直接使用 toml++ 读取 `config/assistant.toml` 的音频子表。

```toml
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

[audio.processing]
pipeline = "far_field"         # far_field | near_field | debug_raw
aec = true
ns = true
agc = true
vad = true
beamforming = true
output_sample_rate = 16000
output_channels = 1
aec_reference_delay_ms = 120

[audio.volume]
master = 0.75
tts = 0.90
prompt = 0.80
media = 0.70
alarm = 1.00
ducking_gain = 0.35            # TTS 播放时 MEDIA 降至此增益

[audio.device]
max_recover_retries = 5
recover_backoff_base_ms = 500

[audio.diagnostics]
enable_metrics = true
enable_audio_dump = false
dump_dir = "/var/log/audio/dump"
```

```cpp
#include <toml++/toml.hpp>

toml::table cfg = toml::parse_file(config_path);
auto audio = cfg["audio"];

auto backend = audio["backend"].value_or("alsa");
auto capture_device = audio["capture"]["device"].value_or("hw:0,0");
auto sample_rate = audio["capture"]["sample_rate"].value_or(48000);
auto aec_delay_ms =
    audio["processing"]["aec_reference_delay_ms"].value_or(120);
```

---

## 10. 可观测性

```
capture_frames_total
capture_overrun_total
consumer_dropped_frames{consumer=wakeword|asr|diag}
playback_underrun_total
device_recover_total
device_recover_failed_total
capture_latency_ms
playback_latency_ms
processing_cost_ms
aec_reference_delay_ms
aec_clock_drift_us              (超过 ±500 us 需关注)
audio_queue_depth{queue=capture|reference}
```

---

## 11. 错误恢复

```
检测异常
  → 停止相关链路
  → 释放设备
  → 重新枚举设备
  → AudioRouter 选择目标设备
  → 重建 backend（NegotiatedFormat 重新协商）
  → 恢复采集 / 播放
  → 上报 DeviceRecovered 事件
```

覆盖场景：设备断开、ALSA xrun、backend 卡死、采集长时间全零、播放写入失败、格式协商失败。

---

## 12. 部署

```ini
[Service]
User=voice
Group=audio
Restart=always
RestartSec=3
```

运行用户加入 `audio` 组，用 udev 规则绑定设备节点名称，防止设备路径在重启后变化。

---
