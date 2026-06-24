# SmartAssistant 部署 README

本文档用于把 `SmartAssistant` 部署到 Ubuntu/Jetson Linux 设备，并以 `systemd` 长期运行。内容覆盖依赖安装、模型与云端密钥、ROS2/Finav 联动、音频设备、编译、配置、启动、验证、升级和排障。

> 当前仓库的完整语音助手入口是 `build/bin/test_voice_assistant`，由 `run_voice_assistant.sh` 和 `deploy/voice-assistant.service` 启动。它不是单元测试意义上的临时程序，而是当前端到端语音助手主进程。

## 1. 部署目标

标准生产部署形态：

- 操作系统：Ubuntu 22.04 / Jetson Linux，推荐 ROS 2 Humble。
- 运行用户：`embotic`。
- 语音助手路径：`/home/embotic/sa_workspace/src/SmartAssistant`。
- 导航工作区：`/home/embotic/nav_workspace`。
- Finav 包路径：`/home/embotic/nav_workspace/src/finav`。
- sherpa-onnx 安装路径：`/usr/local/sherpa-onnx`。
- 主配置：`Config/config.toml`。
- 主启动脚本：`run_voice_assistant.sh`。
- systemd 服务：`voice-assistant.service`。

如果现场路径不同，必须同步修改：

- `deploy/voice-assistant.service` 里的 `WorkingDirectory`、`Environment=...`、`ExecStart`、`ExecStartPre`。
- `run_voice_assistant.sh` 依赖的 `VOICE_ASSISTANT_CONFIG`、`VOICE_ASSISTANT_BIN`、`SHERPA_ONNX_ROOT`、`VOICE_ASSISTANT_ROS_SETUP`。
- `Config/config.toml` 中所有本地模型、音乐、ROS2 话题和日志路径。

## 2. 系统组成

主进程启动后会初始化以下模块：

- `AudioManager`：ALSA/PulseAudio 采集与播放，AEC/NS/AGC/VAD/Beamforming，向唤醒和 ASR 分发 16 kHz 单声道 PCM。
- `WakeupManager`：sherpa-onnx KWS 唤醒词检测，可选 SV 声纹验证。
- `TTSManager`：默认走豆包在线 TTS；离线 sherpa-onnx TTS 默认关闭。
- `E2EChat`：豆包实时对话 WebSocket。
- `IntentManager`：默认走 DeepSeek / 火山方舟等 OpenAI 兼容 Chat Completions 做意图分类；离线规则默认关闭。
- `Orchestrator`：顶层流程编排，负责唤醒、招呼语、E2E 会话、意图分发、技能回复。
- `WheelchairController`：ROS2 发布 `geometry_msgs/Twist` 到 `/web_cmd_vel`。
- `ArmController`：ROS2 发布 `geometry_msgs/Twist` 到 `/arm_end_effector_controller/cmd_vel`。
- `GripperController`：ROS2 发布 `std_msgs/String` 到 `/gripper_controller/command`。
- `NavigationController`：ROS2 发布地点名到 `/nav_bridge/voice_command`，发布急停到 `/nav_clear`。
- `MusicPlayer`：通过系统 `mpg123` 播放本地 MP3。
- `SystemVolumeController`：通过 PulseAudio 控制系统默认 sink 音量/静音。

## 3. 仓库目录

```text
SmartAssistant/
  CMakeLists.txt                 # x86/通用构建配置
  CMakeLists.txt.jetson          # Jetson 构建参考配置
  Config/config.toml             # 主配置
  SmartAssistant/                # C++ 模块源码
  Test/                          # 当前可执行入口与模块验证程序
  models/
    kws/                         # 唤醒模型
    sv/                          # 声纹模型
    asr/                         # 离线 ASR 模型
    tts/                         # 离线 TTS 模型
  external/nav-2026/             # Finav ROS2 导航项目副本
  deploy/voice-assistant.service # systemd 服务模板
  scripts/wait_online_tls.sh     # systemd 启动前 TLS/时间/DNS 检查
  run_voice_assistant.sh         # 生产启动脚本
  logs/                          # 运行日志，启动后自动创建
```

## 4. 机器准备

### 4.1 创建运行用户

如果目标机器没有 `embotic` 用户：

```bash
sudo useradd -m -s /bin/bash embotic
sudo usermod -aG audio,dialout,video,plugdev embotic
```

改完用户组后重新登录，确保组权限生效：

```bash
id embotic
```

### 4.2 基础系统包

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake git pkg-config ca-certificates openssl \
  libssl-dev zlib1g-dev libasound2-dev libpulse-dev pulseaudio-utils \
  libudev-dev libsoxr-dev libwebrtc-audio-processing-dev \
  mpg123 curl wget unzip tar
```

说明：

- `libasound2-dev`：ALSA 编译与运行。
- `libpulse-dev`、`pulseaudio-utils`：PulseAudio 编译、`pactl`、echo-cancel 虚拟源/输出。
- `libwebrtc-audio-processing-dev`：AEC/NS/AGC/VAD。
- `libsoxr-dev`：高质量重采样。
- `libudev-dev`：音频设备热插拔。
- `libssl-dev`、`zlib1g-dev`：WebSocket TLS。
- `mpg123`：本地音乐播放。

### 4.3 ROS2

完整语音助手需要 ROS2 C++ 依赖，否则 `test_voice_assistant` 不会生成。标准部署使用 ROS 2 Humble：

```bash
source /opt/ros/humble/setup.bash
ros2 --version
```

需要能找到这些包：

```bash
ros2 pkg list | grep -E '^(rclcpp|geometry_msgs|std_msgs)$'
```

如果只想编译非硬件模块，可在 CMake 配置时关闭 ROS2：

```bash
cmake -S . -B build -DSMARTASSISTANT_ENABLE_ROS2=OFF
```

但关闭 ROS2 后不会生成完整主进程 `build/bin/test_voice_assistant`，只能运行部分模块验证程序。

安装 ROS2/Finav 常用依赖示例：

```bash
sudo apt install -y \
  ros-humble-rclcpp ros-humble-rclpy \
  ros-humble-std-msgs ros-humble-geometry-msgs ros-humble-sensor-msgs \
  ros-humble-nav-msgs ros-humble-std-srvs \
  ros-humble-tf2 ros-humble-tf2-ros ros-humble-tf2-sensor-msgs \
  ros-humble-message-filters ros-humble-laser-geometry \
  ros-humble-visualization-msgs \
  ros-humble-nav2-bringup ros-humble-nav2-map-server \
  ros-humble-nav2-lifecycle-manager \
  ros-humble-slam-toolbox ros-humble-robot-localization \
  python3-colcon-common-extensions python3-numpy python3-opencv
```

如果使用 Gazebo/仿真，再安装：

```bash
sudo apt install -y ros-humble-gazebo-ros ros-humble-ros-gz-bridge ros-humble-ros-gz-sim
```

### 4.4 systemd 与 PulseAudio

`voice-assistant.service` 是系统级 systemd 服务，但音频使用运行用户的 PulseAudio socket。开机无人登录也要有 `/run/user/<uid>/pulse/native`，建议启用 linger：

```bash
sudo loginctl enable-linger embotic
loginctl show-user embotic | grep Linger
id -u embotic
```

如果 `embotic` 的 UID 不是 `1000`，必须把 `deploy/voice-assistant.service` 中的这些值改成实际 UID：

```ini
Environment=XDG_RUNTIME_DIR=/run/user/1000
Environment=PULSE_SERVER=unix:/run/user/1000/pulse/native
```

## 5. 获取代码

标准路径：

```bash
sudo -iu embotic
mkdir -p /home/embotic/sa_workspace/src
cd /home/embotic/sa_workspace/src
git clone <你的仓库地址> SmartAssistant
cd SmartAssistant
```

如果仓库已经复制到设备，确认目录结构：

```bash
pwd
ls Config SmartAssistant Test models deploy scripts run_voice_assistant.sh
```

## 6. 安装 sherpa-onnx

`WakeupManager` 和离线 ASR 依赖 sherpa-onnx C API。CMake 默认开启 `SMARTASSISTANT_ENABLE_SHERPA_ONNX=ON`，会查找：

- `include/sherpa-onnx/c-api/c-api.h`
- `lib/libsherpa-onnx-c-api.so`
- `lib/libonnxruntime.so`

推荐安装到：

```text
/usr/local/sherpa-onnx
```

解压官方 shared tarball 后应类似：

```text
/usr/local/sherpa-onnx/
  include/sherpa-onnx/c-api/c-api.h
  lib/libsherpa-onnx-c-api.so
  lib/libonnxruntime.so
```

设置环境变量：

```bash
export SHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$SHERPA_ONNX_ROOT/lib:$SHERPA_ONNX_ROOT/lib/aarch64-linux-gnu:$SHERPA_ONNX_ROOT/lib/x86_64-linux-gnu"
```

验证：

```bash
test -f "$SHERPA_ONNX_ROOT/include/sherpa-onnx/c-api/c-api.h"
find "$SHERPA_ONNX_ROOT/lib" -name 'libsherpa-onnx-c-api.so' -o -name 'libonnxruntime.so'
```

如果 sherpa-onnx 装在其它路径，编译时加：

```bash
cmake -S . -B build -DSHERPA_ONNX_ROOT=/path/to/sherpa-onnx
```

或者临时关闭唤醒/离线 ASR 相关目标：

```bash
cmake -S . -B build -DSMARTASSISTANT_ENABLE_SHERPA_ONNX=OFF
```

关闭后不能运行完整唤醒语音助手。

## 7. 模型文件

仓库当前引用的模型路径都是相对工作目录的路径，生产运行时必须从仓库根目录启动。

必需模型：

```text
models/kws/encoder.int8.onnx
models/kws/decoder.onnx
models/kws/joiner.int8.onnx
models/kws/tokens.txt
models/kws/keywords.txt
```

可选模型：

```text
models/sv/sv.onnx
models/asr/encoder.fp16.onnx
models/asr/decoder.fp16.onnx
models/asr/joiner.fp16.onnx
models/asr/tokens.txt
models/tts/kokoro-multi-lang-v1_1/model.onnx
models/tts/kokoro-multi-lang-v1_1/voices.bin
models/tts/kokoro-multi-lang-v1_1/tokens.txt
```

验证模型：

```bash
cd /home/embotic/sa_workspace/src/SmartAssistant
for f in \
  models/kws/encoder.int8.onnx \
  models/kws/decoder.onnx \
  models/kws/joiner.int8.onnx \
  models/kws/tokens.txt \
  models/kws/keywords.txt
do
  test -f "$f" || echo "missing: $f"
done
```

注意：

- `[wakeup.kws]` 当前使用 CPU provider。
- `[wakeup.sv].enabled=false`，声纹模型不是默认必需。
- `[asr.offline].enabled=false`，离线 ASR 模型不是默认运行路径，但开启离线 ASR 前必须准备完整模型。
- `[tts.offline].enabled=false`，离线 TTS 默认不参与主流程。

## 8. 云端密钥

`Config/config.toml` 当前包含在线 ASR/TTS/E2E/Intent 的示例密钥字段。生产部署不要把真实密钥提交到 Git，也不要在多人机器上长期明文保存。

推荐做法：

1. 把 `Config/config.toml` 中这些明文 key 改为空字符串：

```toml
[asr.online]
api_key = ""
app_key = ""
access_key = ""

[tts.online]
api_key = ""

[e2echat]
app_id = ""
access_key = ""

[intent.online.providers.doubao]
api_key = ""

[intent.online.providers.deepseek]
api_key = ""
```

2. 用环境变量注入：

```bash
export ASR_API_KEY='...'
export ASR_APP_KEY='...'
export ASR_ACCESS_KEY='...'
export TTS_API_KEY='...'
export E2ECHAT_APP_ID='...'
export E2ECHAT_ACCESS_KEY='...'
export ARK_API_KEY='...'
export DEEPSEEK_API_KEY='...'
```

3. systemd 部署时建议创建 `/etc/smart-assistant/secrets.env`：

```bash
sudo mkdir -p /etc/smart-assistant
sudo install -m 600 -o root -g root /dev/null /etc/smart-assistant/secrets.env
sudoedit /etc/smart-assistant/secrets.env
```

文件内容示例：

```ini
ASR_API_KEY=xxx
ASR_APP_KEY=xxx
ASR_ACCESS_KEY=xxx
TTS_API_KEY=xxx
E2ECHAT_APP_ID=xxx
E2ECHAT_ACCESS_KEY=xxx
ARK_API_KEY=xxx
DEEPSEEK_API_KEY=xxx
```

然后在 `deploy/voice-assistant.service` 的 `[Service]` 增加：

```ini
EnvironmentFile=/etc/smart-assistant/secrets.env
```

## 9. 编译

首次 CMake 配置会通过 `FetchContent` 拉取第三方源码：

- `tomlplusplus` v3.4.0
- `quill` v11.1.0
- `IXWebSocket` v11.4.5

目标机器需要能访问 `gitcode.com`。拉取成功后源码会缓存到 `Middlewares/`，后续构建通常不再重新下载。离线部署时，需要提前在同版本环境中准备好 `Middlewares/tomlplusplus`、`Middlewares/quill`、`Middlewares/ixwebsocket`，或配置企业内网镜像。

### 9.1 标准完整编译

```bash
cd /home/embotic/sa_workspace/src/SmartAssistant
source /opt/ros/humble/setup.bash

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSHERPA_ONNX_ROOT=/usr/local/sherpa-onnx \
  -DSMARTASSISTANT_ENABLE_SHERPA_ONNX=ON \
  -DSMARTASSISTANT_ENABLE_ROS2=ON \
  -DBUILD_TESTING=ON

cmake --build build -j"$(nproc)"
```

编译成功后确认：

```bash
test -x build/bin/test_voice_assistant
ls build/bin
```

如果 `build/bin/test_voice_assistant` 不存在，通常是 ROS2 没 source，或 `rclcpp / geometry_msgs / std_msgs` 没找到。重新配置前建议清理 CMake cache：

```bash
rm -rf build
source /opt/ros/humble/setup.bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
cmake --build build -j"$(nproc)"
```

### 9.2 Jetson 构建

仓库提供 `CMakeLists.txt.jetson` 作为 Jetson 参考。两种用法任选一种。

临时替换：

```bash
cp CMakeLists.txt CMakeLists.txt.host.backup
cp CMakeLists.txt.jetson CMakeLists.txt
rm -rf build
source /opt/ros/humble/setup.bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
cmake --build build -j"$(nproc)"
mv CMakeLists.txt.host.backup CMakeLists.txt
```

或者把 Jetson 配置内容合并到主 CMake 后使用普通构建流程。Jetson 重点差异是 sherpa-onnx 库搜索路径包含 `lib/aarch64-linux-gnu`。

### 9.3 常用构建选项

```text
SMARTASSISTANT_ENABLE_SHERPA_ONNX=ON/OFF
  ON：构建 WakeupManager、ASRManager 离线能力，需要 sherpa-onnx。

SMARTASSISTANT_ENABLE_ROS2=ON/OFF
  ON：构建 Wheelchair/Arm/Gripper ROS2 控制器，完整主程序需要它们。

SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS=OFF
  主 CMake 强制关闭，当前默认使用在线 TTS；Jetson 参考 CMake 可手动开启。

SHERPA_ONNX_ROOT=/path
  sherpa-onnx 安装前缀。
```

## 10. Finav/ROS2 导航部署

语音助手的轮椅移动和导航能力依赖 Finav ROS2 图。

标准路径：

```bash
mkdir -p /home/embotic/nav_workspace/src
cp -a /home/embotic/sa_workspace/src/SmartAssistant/external/nav-2026 /home/embotic/nav_workspace/src/finav
cd /home/embotic/nav_workspace
source /opt/ros/humble/setup.bash
colcon build --packages-select finav
source install/local_setup.bash
```

Finav 需要的 ROS2/系统依赖包括：

- `rclcpp`、`rclpy`
- `geometry_msgs`、`std_msgs`、`sensor_msgs`、`nav_msgs`
- `tf2`、`tf2_ros`、`tf2_sensor_msgs`
- `message_filters`、`laser_geometry`
- `visualization_msgs`、`std_srvs`
- `nav2_bringup`、`nav2_map_server`、`nav2_lifecycle_manager`
- `slam_toolbox`
- `robot_localization`
- `python3-numpy`、`python3-opencv`

验证 Finav：

```bash
cd /home/embotic/nav_workspace
source /opt/ros/humble/setup.bash
source install/local_setup.bash
ros2 pkg prefix finav
```

启动底盘、摇杆、Web 后台和键盘/网页路由：

```bash
cd /home/embotic/nav_workspace/src/finav
bash start_finav.sh --joy-port /dev/ttyUSB0 --host 0.0.0.0 --port 8010
```

浏览器访问：

```text
http://<设备IP>:8010
```

语音导航需要启动 `nav_bridge`。启动导航链路时确认传入：

```bash
ros2 launch finav nav.launch.py use_nav_bridge:=true
```

话题对应关系：

```text
SmartAssistant WheelchairController -> /web_cmd_vel
Finav js_kb_router                 -> /cmd_vel
SmartAssistant NavigationController -> /nav_bridge/voice_command
Finav nav_bridge.py                -> /goal_pose
SmartAssistant NavigationController -> /nav_clear
Finav nav_control.py               -> 急停/清空导航
```

## 11. 音频设备配置

当前 `Config/config.toml` 默认：

```toml
[audio.capture]
device = "pulse"
sample_rate = 16000
channels = 2

[audio.playback]
device = "pulse"
sample_rate = 16000
channels = 2
```

生产运行时 `run_voice_assistant.sh` 会：

- source ROS2 环境。
- 设置 `LD_LIBRARY_PATH`。
- 如果配置了 `PULSE_SOURCE` 和 `PULSE_SINK`，通过 `pactl` 创建/选择 PulseAudio echo-cancel source/sink。
- 将 `PULSE_SOURCE` 设置为默认输入，将 `PULSE_SINK` 设置为默认输出。

检查音频设备：

```bash
pactl list short sinks
pactl list short sources
arecord -L | head -80
aplay -L | head -80
```

ReSpeaker Lite 示例：

```bash
export PULSE_SOURCE=aec_source
export PULSE_SINK=aec_sink
export VOICE_ASSISTANT_PULSE_SOURCE_MASTER=alsa_input.usb-Seeed_Studio_ReSpeaker_Lite_0000000001-00.analog-stereo
export VOICE_ASSISTANT_PULSE_SINK_MASTER=alsa_output.usb-Seeed_Studio_ReSpeaker_Lite_0000000001-00.analog-stereo
```

如果不使用 PulseAudio echo-cancel，可直接把 `Config/config.toml` 中的设备改为实际 ALSA 设备，例如：

```toml
[audio.capture]
device = "hw:1,0"

[audio.playback]
device = "hw:1,0"
```

也可以临时用环境变量覆盖测试入口的音频设备：

```bash
export AUDIO_TEST_DEVICE=pulse
export AUDIO_TEST_CAPTURE_DEVICE=pulse
export AUDIO_TEST_PLAYBACK_DEVICE=pulse
```

## 12. 主配置检查

部署前必须逐项检查 `Config/config.toml`。

### 12.1 音频

```toml
[audio.capture]
device = "pulse"
sample_rate = 16000
channels = 2
channel_map = [0, 1]

[audio.playback]
device = "pulse"
sample_rate = 16000
channels = 2

[audio.processing]
aec = true
ns = true
agc = true
vad = true
beamforming = true
output_sample_rate = 16000
output_channels = 1
aec_reference_delay_ms = 120
```

调试建议：

- 播放/录音异常时先把 `aec=false`、`beamforming=false` 简化链路。
- 双麦/多麦设备需要确认 `channels` 和 `channel_map`。
- `dump_dir=/var/log/audio/dump` 需要运行用户有写权限；不开 dump 时无需创建。

### 12.2 唤醒

```toml
[wakeup]
keywords = ["你好弈宝", "弈宝弈宝", "嗨弈宝"]
sample_rate = 16000
cooldown_ms = 1500

[wakeup.kws]
encoder = "models/kws/encoder.int8.onnx"
decoder = "models/kws/decoder.onnx"
joiner = "models/kws/joiner.int8.onnx"
tokens = "models/kws/tokens.txt"
keywords_file = "models/kws/keywords.txt"
provider = "cpu"
keywords_threshold = 0.25
```

如果误唤醒多，调高 `keywords_threshold`；如果唤醒困难，适当调低。

### 12.3 E2E 对话

```toml
[e2echat]
endpoint = "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"
app_id = ""
access_key = ""
app_id_env = "E2ECHAT_APP_ID"
access_key_env = "E2ECHAT_ACCESS_KEY"
verify_ssl = true

[e2echat.input]
format = "pcm"
sample_rate = 16000
channels = 1
chunk_ms = 20

[e2echat.output]
format = "pcm_s16le"
sample_rate = 16000
channels = 1
```

### 12.4 TTS

```toml
[tts]
preferred_engine = "online"
fallback_to_offline = false

[tts.online]
enabled = true
endpoint = "wss://openspeech.bytedance.com/api/v3/tts/bidirection"
api_key = ""
api_key_env = "TTS_API_KEY"
resource_id = "seed-tts-2.0"
default_voice = "zh_female_vv_uranus_bigtts"
sample_rate = 16000
verify_ssl = true
```

### 12.5 意图识别

```toml
[intent]
preferred_engine = "online"
fallback_to_offline = false

[intent.online]
enabled = true
provider = "deepseek"

[intent.online.providers.deepseek]
api_key = ""
api_key_env = "DEEPSEEK_API_KEY"
```

如改用火山方舟：

```toml
[intent.online]
provider = "doubao"

[intent.online.providers.doubao]
api_key = ""
api_key_env = "ARK_API_KEY"
```

### 12.6 轮椅/机械臂/夹爪/导航

确认以下话题与底层 ROS2 包一致：

```toml
[wheelchair.ros2]
cmd_vel_topic = "/web_cmd_vel"

[arm.ros2]
cmd_vel_topic = "/arm_end_effector_controller/cmd_vel"

[gripper.ros2]
command_topic = "/gripper_controller/command"

[navigation.ros2]
voice_command_topic = "/nav_bridge/voice_command"
nav_clear_topic = "/nav_clear"
```

首次实车部署建议保守调小速度限制：

```toml
[wheelchair.limits]
max_linear_velocity = 0.20
max_angular_velocity = 0.30

[wheelchair.defaults]
linear_velocity = 0.10
angular_velocity = 0.20
forward_distance = 0.30
```

确认摇杆或急停机制工作后再恢复目标速度。

### 12.7 音乐

```toml
[music]
enabled = true
library_dir = "/home/embotic/Music/"

[[music.tracks]]
file = "稻香—周杰伦.mp3"
```

检查：

```bash
test -f "/home/embotic/Music/稻香—周杰伦.mp3"
command -v mpg123
```

没有本地音乐时设置：

```toml
[music]
enabled = false
```

## 13. 手动启动

先在前台跑，确认没有路径、库、音频和密钥问题：

```bash
cd /home/embotic/sa_workspace/src/SmartAssistant
source /opt/ros/humble/setup.bash
source /home/embotic/nav_workspace/install/local_setup.bash

export SHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
export VOICE_ASSISTANT_CONFIG=/home/embotic/sa_workspace/src/SmartAssistant/Config/config.toml
export VOICE_ASSISTANT_ROS_SETUP=/home/embotic/nav_workspace/install/local_setup.bash
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export ROS_DOMAIN_ID=99
export ROS_LOCALHOST_ONLY=0
export FASTRTPS_DEFAULT_PROFILES_FILE=/home/embotic/nav_workspace/src/finav/config/fastdds_profiles.xml
export FINAV_REPO_DIR=/home/embotic/nav_workspace/src/finav
export FINAV_MAPS_DIR=/home/embotic/nav_workspace/src/finav/maps
export PULSE_SERVER=unix:/run/user/$(id -u)/pulse/native
export PULSE_SOURCE=aec_source
export PULSE_SINK=aec_sink

bash run_voice_assistant.sh
```

限时 smoke run：

```bash
VOICE_ASSISTANT_BIN=/home/embotic/sa_workspace/src/SmartAssistant/build/bin/test_voice_assistant \
VOICE_ASSISTANT_CONFIG=/home/embotic/sa_workspace/src/SmartAssistant/Config/config.toml \
/home/embotic/sa_workspace/src/SmartAssistant/build/bin/test_voice_assistant --config Config/config.toml --seconds 30
```

成功启动会看到类似：

```text
E2E voice assistant started.
Flow: wake -> Doubao TTS greeting -> E2E ASR session -> DeepSeek intent -> skill dispatch + TTS reply.
Press Ctrl+C to stop.
```

## 14. systemd 部署

### 14.1 安装服务

检查 `deploy/voice-assistant.service` 的路径和用户，确认符合目标机器：

```ini
User=embotic
WorkingDirectory=/home/embotic/sa_workspace/src/SmartAssistant
Environment=VOICE_ASSISTANT_CONFIG=/home/embotic/sa_workspace/src/SmartAssistant/Config/config.toml
Environment=SHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
Environment=VOICE_ASSISTANT_ROS_SETUP=/home/embotic/nav_workspace/install/local_setup.bash
Environment=ROS_DOMAIN_ID=99
Environment=FASTRTPS_DEFAULT_PROFILES_FILE=/home/embotic/nav_workspace/src/finav/config/fastdds_profiles.xml
Environment=FINAV_REPO_DIR=/home/embotic/nav_workspace/src/finav
Environment=FINAV_MAPS_DIR=/home/embotic/nav_workspace/src/finav/maps
ExecStartPre=/usr/bin/env bash /home/embotic/sa_workspace/src/SmartAssistant/scripts/wait_online_tls.sh
ExecStart=/home/embotic/sa_workspace/src/SmartAssistant/run_voice_assistant.sh
```

安装：

```bash
sudo cp /home/embotic/sa_workspace/src/SmartAssistant/deploy/voice-assistant.service /etc/systemd/system/voice-assistant.service
sudo systemctl daemon-reload
sudo systemctl enable voice-assistant.service
```

如果使用 `/etc/smart-assistant/secrets.env`，先把 `EnvironmentFile` 加进服务文件再 `daemon-reload`。

### 14.2 启动/停止/查看

```bash
sudo systemctl start voice-assistant.service
systemctl status voice-assistant.service --no-pager
journalctl -u voice-assistant.service -f
```

停止：

```bash
sudo systemctl stop voice-assistant.service
```

重启：

```bash
sudo systemctl restart voice-assistant.service
```

查看最近日志：

```bash
journalctl -u voice-assistant.service -n 200 --no-pager
```

### 14.3 systemd 启动前检查

`scripts/wait_online_tls.sh` 会检查：

- DNS 能解析 `openspeech.bytedance.com`。
- 系统时间已经同步。
- TLS 握手可用。

可配置环境变量：

```ini
Environment=VOICE_ASSISTANT_TLS_HOST=openspeech.bytedance.com
Environment=VOICE_ASSISTANT_TLS_PORT=443
Environment=VOICE_ASSISTANT_BOOT_WAIT_SEC=120
Environment=VOICE_ASSISTANT_BOOT_WAIT_INTERVAL_SEC=2
```

如果部署在纯离线模式，且不需要云端服务，可移除 `ExecStartPre` 或把等待目标改成内网服务。

## 15. 启动顺序

推荐实车启动顺序：

1. 系统启动，PulseAudio 可用。
2. ROS2/Finav 底盘、摇杆、Web 后台启动。
3. 导航链路按需启动，并开启 `use_nav_bridge:=true`。
4. 启动 `voice-assistant.service`。
5. 用 `ros2 node list` 和 `journalctl` 验证语音助手已加入同一个 ROS graph。

手动联调顺序：

```bash
# 终端 1：Finav
cd /home/embotic/nav_workspace/src/finav
source /opt/ros/humble/setup.bash
source /home/embotic/nav_workspace/install/local_setup.bash
bash start_finav.sh

# 终端 2：语音助手
cd /home/embotic/sa_workspace/src/SmartAssistant
bash run_voice_assistant.sh

# 终端 3：观察 ROS2
source /opt/ros/humble/setup.bash
source /home/embotic/nav_workspace/install/local_setup.bash
ros2 node list
ros2 topic list
ros2 topic echo /web_cmd_vel
```

## 16. 验证清单

### 16.1 编译验证

```bash
test -x build/bin/test_voice_assistant
ldd build/bin/test_voice_assistant | grep -E 'not found|sherpa|onnx|rclcpp|pulse|asound'
```

`ldd` 不应出现 `not found`。

### 16.2 模块验证程序

根据编译目标不同，`build/bin` 可能包含：

```text
test_audio_manager
test_network_manager
test_tts_manager
test_chat
test_wakeup_manager
wakeup_pulse_listener
test_asr_manager
test_intent_manager
test_wheelchair_controller
test_voice_assistant
```

常用命令：

```bash
./build/bin/test_network_manager
./build/bin/test_intent_manager
./build/bin/test_tts_manager --config Config/config.toml --text "您好，我是弈宝。"
./build/bin/test_chat --config Config/config.toml --seconds 30
./build/bin/test_wakeup_manager
```

`test_wheelchair_controller` 会发布 ROS2 控制消息，实车上必须架空或确保有人接管急停后再运行。

### 16.3 音频验证

录音：

```bash
arecord -D pulse -f S16_LE -r 16000 -c 2 -d 5 /tmp/sa_mic.wav
aplay /tmp/sa_mic.wav
```

播放：

```bash
speaker-test -D pulse -c 2 -r 16000 -t sine -l 1
```

PulseAudio 默认源/输出：

```bash
pactl info | grep -E 'Default Sink|Default Source'
```

### 16.4 ROS2 验证

```bash
source /opt/ros/humble/setup.bash
source /home/embotic/nav_workspace/install/local_setup.bash
export ROS_DOMAIN_ID=99
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp

ros2 node list
ros2 topic list | grep -E '/web_cmd_vel|/cmd_vel|/nav_bridge/voice_command|/nav_clear'
ros2 topic echo /web_cmd_vel
```

如果语音助手启动了但节点不出现在 `ros2 node list`，优先检查：

- `ROS_DOMAIN_ID` 是否一致。
- `RMW_IMPLEMENTATION` 是否一致。
- `FASTRTPS_DEFAULT_PROFILES_FILE` 是否一致。
- `run_voice_assistant.sh` 是否把 ROS 的 `LD_LIBRARY_PATH` 保留在前面。

### 16.5 云端连通性

```bash
VOICE_ASSISTANT_BOOT_WAIT_SEC=15 bash scripts/wait_online_tls.sh
```

也可直接检查：

```bash
getent hosts openspeech.bytedance.com
openssl s_client -connect openspeech.bytedance.com:443 -servername openspeech.bytedance.com -verify_return_error </dev/null
```

## 17. 日志

进程日志：

```bash
journalctl -u voice-assistant.service -f
```

模块日志目录：

```text
logs/
  wakeup.log
  asr.log
  tts.log
  e2echat.log
  intent.log
  wheelchair.log
  arm_controller.log
  gripper_controller.log
  navigation.log
```

崩溃栈：

```text
/tmp/test_voice_assistant_crash.log
```

Finav 日志：

```text
/home/embotic/nav_workspace/src/finav/server/runtime/mapping.log
/home/embotic/nav_workspace/src/finav/server/runtime/navigation.log
```

查看：

```bash
tail -f logs/e2echat.log logs/tts.log logs/wakeup.log
```

## 18. 升级

建议升级步骤：

```bash
sudo systemctl stop voice-assistant.service

cd /home/embotic/sa_workspace/src/SmartAssistant
git status --short
git pull

source /opt/ros/humble/setup.bash
source /home/embotic/nav_workspace/install/local_setup.bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
cmake --build build -j"$(nproc)"

bash run_voice_assistant.sh
```

前台验证通过后：

```bash
sudo systemctl start voice-assistant.service
systemctl status voice-assistant.service --no-pager
```

如果同步升级 Finav：

```bash
sudo systemctl stop voice-assistant.service
cd /home/embotic/nav_workspace
source /opt/ros/humble/setup.bash
colcon build --packages-select finav
source install/local_setup.bash
sudo systemctl start voice-assistant.service
```

## 19. 回滚

代码回滚：

```bash
sudo systemctl stop voice-assistant.service
cd /home/embotic/sa_workspace/src/SmartAssistant
git log --oneline -20
git checkout <上一个稳定提交>
rm -rf build
source /opt/ros/humble/setup.bash
source /home/embotic/nav_workspace/install/local_setup.bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
cmake --build build -j"$(nproc)"
sudo systemctl start voice-assistant.service
```

配置回滚：

```bash
sudo systemctl stop voice-assistant.service
cp Config/config.toml.bak Config/config.toml
sudo systemctl start voice-assistant.service
```

不要在运行中覆盖 `Config/config.toml` 后期待热加载；当前配置在进程启动时读取。

## 20. 安全注意事项

- 真实密钥不要提交到 Git。
- `Config/config.toml` 如保留明文密钥，文件权限至少设为 `600`，并限制仓库访问。
- 语音控制轮椅/机械臂/夹爪前，必须确认急停、摇杆接管、速度限制和现场人员监护。
- 首次实车部署时把速度限制调低，确认 `/web_cmd_vel -> js_kb_router -> /cmd_vel -> base_control` 路径正确后再放开。
- Finav Web 后台未设计为公网服务，不要暴露到公网。
- `ROS_DOMAIN_ID` 应与现场其它 ROS 网络隔离，避免串网。
- `verify_ssl=false` 仅用于本地临时调试，生产必须保持 `true`。

## 21. 常见问题

### 21.1 CMake 找不到 sherpa-onnx

现象：

```text
sherpa-onnx not found; missing: include/sherpa-onnx/c-api/c-api.h, lib/libsherpa-onnx-c-api.so, lib/libonnxruntime.so
```

处理：

```bash
export SHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
cmake -S . -B build -DSHERPA_ONNX_ROOT="$SHERPA_ONNX_ROOT"
```

确认路径：

```bash
find "$SHERPA_ONNX_ROOT" -name c-api.h -o -name 'libsherpa-onnx-c-api.so' -o -name 'libonnxruntime.so'
```

### 21.2 没有生成 `test_voice_assistant`

原因通常是 ROS2 控制器目标没构建出来。检查 CMake 输出中是否有：

```text
ROS2 found: building SmartAssistantWheelchairController...
```

处理：

```bash
rm -rf build
source /opt/ros/humble/setup.bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSMARTASSISTANT_ENABLE_ROS2=ON
cmake --build build -j"$(nproc)"
```

### 21.3 运行时报 `.so not found`

检查：

```bash
ldd build/bin/test_voice_assistant | grep 'not found'
echo "$LD_LIBRARY_PATH"
```

确保包含：

```text
build/lib
/usr/local/sherpa-onnx/lib
/usr/local/sherpa-onnx/lib/aarch64-linux-gnu
/usr/local/sherpa-onnx/lib/x86_64-linux-gnu
```

生产启动脚本已经自动设置这些路径。

### 21.4 systemd 下没有声音

检查：

```bash
systemctl --user status pulseaudio
pactl info
ls -ld /run/user/1000 /run/user/1000/pulse
journalctl -u voice-assistant.service -n 100 --no-pager
```

确认 service 中：

```ini
Environment=XDG_RUNTIME_DIR=/run/user/1000
Environment=PULSE_SERVER=unix:/run/user/1000/pulse/native
```

如果运行用户 UID 不是 `1000`，必须改成对应 `/run/user/<uid>`。

### 21.5 创建 echo-cancel 失败

查看真实设备名：

```bash
pactl list short sinks
pactl list short sources
```

修改：

```ini
Environment=VOICE_ASSISTANT_PULSE_SOURCE_MASTER=<真实 source 名>
Environment=VOICE_ASSISTANT_PULSE_SINK_MASTER=<真实 sink 名>
Environment=PULSE_SOURCE=aec_source
Environment=PULSE_SINK=aec_sink
```

如果不需要 echo-cancel，删掉 `PULSE_SOURCE/PULSE_SINK` 环境变量，并把配置中的 `device` 改为可用设备。

### 21.6 能启动但不能唤醒

检查：

```bash
tail -f logs/wakeup.log
test -f models/kws/keywords.txt
arecord -D pulse -f S16_LE -r 16000 -c 2 -d 5 /tmp/test.wav
```

处理方向：

- 确认麦克风采集有波形。
- 确认 `models/kws/*` 文件存在。
- 暂时关闭 `beamforming` 或调整 `channel_map`。
- 适当降低 `[wakeup.kws].keywords_threshold`。

### 21.7 云端 WebSocket 连接失败

检查：

```bash
bash scripts/wait_online_tls.sh
journalctl -u voice-assistant.service -n 200 --no-pager
tail -f logs/e2echat.log logs/tts.log
```

常见原因：

- 系统时间未同步，TLS 证书校验失败。
- DNS 不通。
- 密钥为空或过期。
- `verify_ssl=true` 但系统缺少 CA 证书。
- 网络策略阻断 `openspeech.bytedance.com:443`。

### 21.8 语音助手 ROS2 节点看不到

检查：

```bash
env | grep -E 'ROS_DOMAIN_ID|RMW_IMPLEMENTATION|FASTRTPS'
ros2 node list
```

确保 systemd 与 Finav 使用同一组：

```ini
Environment=RMW_IMPLEMENTATION=rmw_fastrtps_cpp
Environment=ROS_DOMAIN_ID=99
Environment=ROS_LOCALHOST_ONLY=0
Environment=FASTRTPS_DEFAULT_PROFILES_FILE=/home/embotic/nav_workspace/src/finav/config/fastdds_profiles.xml
```

还要确认 `run_voice_assistant.sh` 先 source `/opt/ros/humble/setup.bash`，再 source `VOICE_ASSISTANT_ROS_SETUP`。

### 21.9 轮椅不动

检查链路：

```bash
ros2 topic echo /web_cmd_vel
ros2 topic echo /cmd_vel
ros2 node list | grep -E 'js_kb_router|base_control|wheelchair'
```

可能原因：

- Finav `js_kb_router.py` 没启动。
- 摇杆处于接管状态，屏蔽网页/语音命令。
- `wheelchair.ros2.cmd_vel_topic` 与 Finav 路由话题不一致。
- `base_control.py` 未连接底盘。

### 21.10 语音导航不工作

检查：

```bash
ros2 topic echo /nav_bridge/voice_command
ros2 topic echo /goal_pose
ros2 node list | grep nav_bridge
```

处理：

- 用 `ros2 launch finav nav.launch.py use_nav_bridge:=true` 启动导航。
- 确认 `FINAV_MAPS_DIR` 指向正确 maps 目录。
- 确认 `<地图>.locations.yaml` 中存在目标地点。
- 确认 `Config/config.toml` 的 `[navigation.ros2].voice_command_topic` 与 `external/nav-2026/config/nav_bridge.yaml` 一致。

### 21.11 本地音乐播放失败

检查：

```bash
command -v mpg123
test -d /home/embotic/Music
ls -l /home/embotic/Music
tail -f logs/assistant.log logs/intent.log 2>/dev/null
```

处理：

- 安装 `mpg123`。
- 确认 `library_dir` 存在。
- 确认 MP3 文件名和 TOML 中完全一致，包括中文符号。
- 不用音乐时设置 `[music].enabled=false`。

## 22. 一键部署参考流程

以下命令假设目标机已经安装 ROS2 Humble、sherpa-onnx、系统依赖，并已准备云端密钥。

```bash
sudo -iu embotic

# 1. 代码
mkdir -p /home/embotic/sa_workspace/src
cd /home/embotic/sa_workspace/src
git clone <你的仓库地址> SmartAssistant

# 2. Finav
mkdir -p /home/embotic/nav_workspace/src
cp -a /home/embotic/sa_workspace/src/SmartAssistant/external/nav-2026 /home/embotic/nav_workspace/src/finav
cd /home/embotic/nav_workspace
source /opt/ros/humble/setup.bash
colcon build --packages-select finav

# 3. SmartAssistant
cd /home/embotic/sa_workspace/src/SmartAssistant
source /opt/ros/humble/setup.bash
source /home/embotic/nav_workspace/install/local_setup.bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHERPA_ONNX_ROOT=/usr/local/sherpa-onnx
cmake --build build -j"$(nproc)"
test -x build/bin/test_voice_assistant

# 4. 前台验证
bash run_voice_assistant.sh
```

另开 root shell 安装 systemd：

```bash
sudo cp /home/embotic/sa_workspace/src/SmartAssistant/deploy/voice-assistant.service /etc/systemd/system/voice-assistant.service
sudo systemctl daemon-reload
sudo systemctl enable --now voice-assistant.service
journalctl -u voice-assistant.service -f
```

## 23. 部署完成判定

满足以下条件才算部署完成：

- `cmake --build build` 成功。
- `build/bin/test_voice_assistant` 存在且可执行。
- `ldd build/bin/test_voice_assistant` 无 `not found`。
- `models/kws/*` 必需模型存在。
- `run_voice_assistant.sh` 前台启动成功。
- `systemctl status voice-assistant.service` 为 `active (running)`。
- `journalctl -u voice-assistant.service -f` 无反复重启。
- `pactl info` 默认 source/sink 正确。
- 唤醒词能触发 `wakeup.log`。
- 云端 TTS/E2E/Intent 日志无鉴权错误。
- `ros2 node list` 能看到语音助手相关节点。
- `/web_cmd_vel`、`/nav_bridge/voice_command` 等话题能按语音指令发布。
- 实车低速验证通过，急停/摇杆接管有效。
