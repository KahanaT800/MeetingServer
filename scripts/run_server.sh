#!/usr/bin/env bash
set -euo pipefail

BINARY="build/debug/meeting_server"
if [[ ! -x "${BINARY}" ]]; then
    echo "[run] binary not found: ${BINARY}, please run scripts/build.sh first." >&2
    exit 1
fi

export MEETING_SERVER_PORT="${MEETING_SERVER_PORT:-0.0.0.0:50051}"
echo "[run] starting meeting_server on ${MEETING_SERVER_PORT}"
exec "${BINARY}"