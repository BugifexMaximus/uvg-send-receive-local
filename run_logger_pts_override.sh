#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build_logger"
KEEP_LOG=0

FPS=30
# 300 ms on a 90 kHz clock = 0.3 * 90000 = 27000
PTS_OFFSET=27000
TARGET_FRAME=10
TARGET_FRAME_TS=$(( (TARGET_FRAME - 1) * (90000 / FPS) ))
NEW_FRAME_TS=$(( TARGET_FRAME_TS + PTS_OFFSET ))

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --target simple_receiver opencv_send_logger

cleanup() {
    if [[ -n "${RECEIVER_PID:-}" ]]; then
        if kill -0 "${RECEIVER_PID}" 2>/dev/null; then
            kill "${RECEIVER_PID}" >/dev/null 2>&1 || true
            wait "${RECEIVER_PID}" 2>/dev/null || true
        fi
    fi
    if [[ -n "${SENDER_LOG:-}" && -f "${SENDER_LOG}" ]]; then
        if [[ "${KEEP_LOG}" -eq 0 ]]; then
            rm -f "${SENDER_LOG}"
        else
            echo "Sender log retained at ${SENDER_LOG}"
        fi
    fi
}
trap cleanup EXIT INT TERM

RECEIVER_CMD=(
    "${BUILD_DIR}/simple_receiver"
    --remote 127.0.0.1
    --bind 127.0.0.1
    --port 2304
    --remote-port 2304
    --total-timeout-ms 10000
    --output-dir "${ROOT_DIR}/decoded_frames_logger"
)

SENDER_CMD=(
    "${BUILD_DIR}/opencv_send_logger"
    --ip 127.0.0.1
    --port 2304
    --input "${ROOT_DIR}/examples/toy_frames_720p"
    --max-frames 60
    --buffered
    --set-pts "${TARGET_FRAME}" "${NEW_FRAME_TS}"
    -v
)

SENDER_LOG="$(mktemp)"

"${RECEIVER_CMD[@]}" > /dev/null 2>&1 &
RECEIVER_PID=$!

sleep 1

if ! "${SENDER_CMD[@]}" 2>&1 | tee "${SENDER_LOG}"; then
    echo "Sender failed" >&2
    KEEP_LOG=1
    exit 1
fi

wait "${RECEIVER_PID}"

EXPECTED_FRAGMENT="(rtp_ts=${NEW_FRAME_TS}, original=${TARGET_FRAME_TS}"

if grep -qF "${EXPECTED_FRAGMENT}" "${SENDER_LOG}"; then
    echo "Frame ${TARGET_FRAME} RTP timestamp successfully updated by ${PTS_OFFSET} ticks (300 ms)."
else
    echo "Failed to verify RTP timestamp override for frame ${TARGET_FRAME}." >&2
    KEEP_LOG=1
    cat "${SENDER_LOG}"
    exit 1
fi
