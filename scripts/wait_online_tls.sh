#!/usr/bin/env bash
set -euo pipefail

HOST="${VOICE_ASSISTANT_TLS_HOST:-openspeech.bytedance.com}"
PORT="${VOICE_ASSISTANT_TLS_PORT:-443}"
TIMEOUT_SEC="${VOICE_ASSISTANT_BOOT_WAIT_SEC:-120}"
SLEEP_SEC="${VOICE_ASSISTANT_BOOT_WAIT_INTERVAL_SEC:-2}"

deadline=$((SECONDS + TIMEOUT_SEC))

while (( SECONDS < deadline )); do
  if ! getent hosts "$HOST" >/dev/null 2>&1; then
    echo "[boot-wait] DNS not ready for ${HOST}"
    sleep "$SLEEP_SEC"
    continue
  fi

  now_epoch="$(date +%s)"
  if (( now_epoch < 1700000000 )); then
    echo "[boot-wait] system clock not synced yet: ${now_epoch}"
    sleep "$SLEEP_SEC"
    continue
  fi

  if timeout 8 openssl s_client \
      -connect "${HOST}:${PORT}" \
      -servername "$HOST" \
      -verify_return_error \
      </dev/null >/dev/null 2>&1; then
    echo "[boot-wait] TLS ready for ${HOST}:${PORT}"
    exit 0
  fi

  echo "[boot-wait] TLS check failed for ${HOST}:${PORT}; retrying"
  sleep "$SLEEP_SEC"
done

echo "[boot-wait] timeout waiting for TLS readiness: ${HOST}:${PORT}" >&2
exit 1
