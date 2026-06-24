#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${VOICE_ASSISTANT_BIN:-${APP_DIR}/build/bin/test_voice_assistant}"
CONFIG="${VOICE_ASSISTANT_CONFIG:-${APP_DIR}/Config/config.toml}"

source_ros_setup() {
  local setup_file="$1"
  set +u
  # shellcheck disable=SC1090
  source "${setup_file}"
  local rc=$?
  set -u
  return "${rc}"
}

if [[ -f /opt/ros/humble/setup.bash ]]; then
  # The ROS2 controllers are linked into the assistant on Jetson builds.
  # Source ROS before launching so shared libraries and middleware env are set.
  source_ros_setup /opt/ros/humble/setup.bash
elif [[ -f /opt/ros/foxy/setup.bash ]]; then
  source_ros_setup /opt/ros/foxy/setup.bash
fi

# When launched by systemd, the assistant also needs the workspace overlay so
# it joins the same ROS graph as the nav stack and web bridge.
ros_overlay_candidates=()
if [[ -n "${VOICE_ASSISTANT_ROS_SETUP:-}" ]]; then
  ros_overlay_candidates+=("${VOICE_ASSISTANT_ROS_SETUP}")
fi
ros_overlay_candidates+=(
  "${APP_DIR}/../../install/local_setup.bash"
  "${APP_DIR}/../../install/setup.bash"
  "${APP_DIR}/../install/local_setup.bash"
  "${APP_DIR}/../install/setup.bash"
)

for ros_setup in "${ros_overlay_candidates[@]}"; do
  if [[ -f "${ros_setup}" ]]; then
    source_ros_setup "${ros_setup}"
    echo "[voice-assistant] sourced ROS overlay: ${ros_setup}"
    break
  fi
done

echo "[voice-assistant] ROS env: RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION:-unset} ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-unset} ROS_LOCALHOST_ONLY=${ROS_LOCALHOST_ONLY:-unset} FASTRTPS_DEFAULT_PROFILES_FILE=${FASTRTPS_DEFAULT_PROFILES_FILE:-unset}"

if [[ ! -f "${CONFIG}" ]]; then
  echo "[voice-assistant] config not found: ${CONFIG}" >&2
  exit 2
fi

if [[ ! -x "${BIN}" ]]; then
  echo "[voice-assistant] executable not found or not executable: ${BIN}" >&2
  exit 3
fi

mkdir -p "${APP_DIR}/logs"

SHERPA_ROOT="${SHERPA_ONNX_ROOT:-/usr/local}"
# Kep the ROS-managed LD_LIBRARY_PATH (set by source /opt/ros/humble/setup.bash)
# at the FRONT.  Otherwise the local /usr/local/lib copies of fastrtps / rmw
# get loaded first and the binary ends up speaking a different DS dialect
# from the rest of the ROS graph — its node never appears in `ros2 node list`.
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:${APP_DIR}/build/lib:${SHERPA_ROOT}/lib:${SHERPA_ROOT}/lib/aarch64-linux-gnu:/usr/local/lib:/usr/local/lib/aarch64-linux-gnu"

export VOICE_ASSISTANT_CONFIG="${CONFIG}"

if command -v pactl >/dev/null 2>&1; then
  if [[ "${PULSE_SINK:-}" == "echo_cancel_sink" &&
        "${PULSE_SOURCE:-}" == "echo_cancel_source" ]]; then
    if ! pactl list short sinks | awk '{print $2}' | grep -Fxq "${PULSE_SINK}" ||
       ! pactl list short sources | awk '{print $2}' | grep -Fxq "${PULSE_SOURCE}"; then
      src_master="${VOICE_ASSISTANT_PULSE_SOURCE_MASTER:-}"
      sink_master="${VOICE_ASSISTANT_PULSE_SINK_MASTER:-}"
      if [[ -z "${src_master}" ]]; then
        src_master="$(pactl list short sources | awk '$2 !~ /\.monitor$/ && $2 !~ /^echo_cancel_/ {print $2; exit}')"
      fi
      if [[ -z "${sink_master}" ]]; then
        sink_master="$(pactl list short sinks | awk '$2 !~ /^echo_cancel_/ {print $2; exit}')"
      fi
      if [[ -n "${src_master}" && -n "${sink_master}" ]]; then
        echo "[audio] loading PulseAudio echo-cancel source_master=${src_master} sink_master=${sink_master}"
        pactl load-module module-echo-cancel \
          source_master="${src_master}" \
          sink_master="${sink_master}" \
          source_name="${PULSE_SOURCE}" \
          sink_name="${PULSE_SINK}" \
          aec_method=webrtc \
          use_master_format=1 >/dev/null || true
      else
        echo "[audio] cannot infer echo-cancel master source/sink" >&2
      fi
    fi
  fi

  if [[ -n "${PULSE_SINK:-}" ]]; then
    if pactl list short sinks | awk '{print $2}' | grep -Fxq "${PULSE_SINK}"; then
      pactl set-default-sink "${PULSE_SINK}" || true
      echo "[audio] default sink set to ${PULSE_SINK}"
    else
      echo "[audio] requested PULSE_SINK not found: ${PULSE_SINK}" >&2
    fi
  fi
  if [[ -n "${PULSE_SOURCE:-}" ]]; then
    if pactl list short sources | awk '{print $2}' | grep -Fxq "${PULSE_SOURCE}"; then
      pactl set-default-source "${PULSE_SOURCE}" || true
      echo "[audio] default source set to ${PULSE_SOURCE}"
    else
      echo "[audio] requested PULSE_SOURCE not found: ${PULSE_SOURCE}" >&2
    fi
  fi
fi

cd "${APP_DIR}"
exec "${BIN}" --config "${CONFIG}"
