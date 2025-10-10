#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build_logger"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --target simple_receiver opencv_send_logger

cleanup() {
    if [[ -n "${RECEIVER_PID:-}" ]]; then
        if kill -0 "${RECEIVER_PID}" 2>/dev/null; then
            kill "${RECEIVER_PID}" >/dev/null 2>&1 || true
            wait "${RECEIVER_PID}" 2>/dev/null || true
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
    -v
    --output-dir "${ROOT_DIR}/decoded_frames_logger"
)

SENDER_CMD=(
    "${BUILD_DIR}/opencv_send_logger"
    --ip 127.0.0.1
    --port 2304
    --input "${ROOT_DIR}/examples/toy_frames_720p"
    --max-frames 60
    --buffered
    -v
)

"${RECEIVER_CMD[@]}" &
RECEIVER_PID=$!

sleep 1

"${SENDER_CMD[@]}"

wait "${RECEIVER_PID}"
