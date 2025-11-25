#!/usr/bin/env bash
set -euo pipefail

BINARY="build/debug/meeting_server"
if [[ ! -x "${BINARY}" ]]; then
    echo "[run] binary not found: ${BINARY}, please run scripts/build.sh first." >&2
    exit 1
fi

CONFIG_PATH="${1:-${MEETING_SERVER_CONFIG:-config/app.example.json}}"
echo "[run] starting meeting_server with config ${CONFIG_PATH}"
exec "${BINARY}" "${CONFIG_PATH}"