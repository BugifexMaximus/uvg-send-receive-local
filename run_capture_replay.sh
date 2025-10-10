#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build_logger"
CAPTURE_DIR="$(mktemp -d)"
KEEP_LOG=0

cleanup() {
    if [[ -n "${RECEIVER_PID:-}" ]]; then
        if kill -0 "${RECEIVER_PID}" 2>/dev/null; then
            kill "${RECEIVER_PID}" >/dev/null 2>&1 || true
            wait "${RECEIVER_PID}" 2>/dev/null || true
        fi
    fi
    rm -rf "${CAPTURE_DIR}"
    if [[ -n "${CAPTURE_LOG:-}" && -f "${CAPTURE_LOG}" ]]; then
        if [[ "${KEEP_LOG}" -eq 0 ]]; then
            rm -f "${CAPTURE_LOG}"
        else
            echo "Capture/replay log retained at ${CAPTURE_LOG}"
        fi
    fi
}
trap cleanup EXIT INT TERM

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --target simple_receiver opencv_send_logger capture_replay

RECEIVER_CMD=(
    "${BUILD_DIR}/simple_receiver"
    --remote 127.0.0.1
    --bind 127.0.0.1
    --port 2304
    --remote-port 2304
    --total-timeout-ms 10000
)

SENDER_CMD=(
    "${BUILD_DIR}/opencv_send_logger"
    --ip 127.0.0.1
    --port 2304
    --input "${ROOT_DIR}/examples/toy_frames_720p"
    --max-frames 10
    --buffered
    --capture-dir "${CAPTURE_DIR}"
    -v
)

REPLAY_CMD=(
    "${BUILD_DIR}/capture_replay"
    --capture-dir "${CAPTURE_DIR}"
    --ip 127.0.0.1
    --port 2304
    -v
)

CAPTURE_LOG="$(mktemp)"

"${RECEIVER_CMD[@]}" > /dev/null 2>&1 &
RECEIVER_PID=$!

sleep 1

if ! "${SENDER_CMD[@]}" 2>&1 | tee "${CAPTURE_LOG}"; then
    echo "Capture phase failed" >&2
    KEEP_LOG=1
    exit 1
fi

wait "${RECEIVER_PID}"

if [[ ! -f "${CAPTURE_DIR}/capture.csv" ]]; then
    echo "capture.csv was not created in ${CAPTURE_DIR}" >&2
    KEEP_LOG=1
    exit 1
fi

RELAY_LOG="$(mktemp)"
if ! "${REPLAY_CMD[@]}" 2>&1 | tee "${RELAY_LOG}"; then
    echo "Replay phase failed" >&2
    KEEP_LOG=1
    exit 1
fi

EXPECTED="$(grep -c '^Replayed frame' "${RELAY_LOG}" || true)"
if [[ "${EXPECTED}" -lt 1 ]]; then
    echo "Replay log did not contain frame entries" >&2
    KEEP_LOG=1
    cat "${RELAY_LOG}"
    exit 1
fi

echo "Capture/replay test succeeded (${EXPECTED} frames)."

rm -f "${RELAY_LOG}"
