#!/usr/bin/env bash
#
# Simple migration runner for Meeting Server.
# Usage:
#   ./scripts/migrate.sh up        # apply all forward migrations
#   ./scripts/migrate.sh down      # apply rollback scripts (reverse order)
# Environment variables (override defaults to adapt to different environments):
#   MEETING_DB_HOST, MEETING_DB_PORT, MEETING_DB_USER, MEETING_DB_PASSWORD, MEETING_DB_NAME
#
# The script intentionally keeps the orchestration logic minimal so it can be
# extended later (e.g., add "status" command, record applied versions, integrate
# with Flyway/Liquibase). For now it just streams SQL files to mysql in order.

set -euo pipefail

MODE="${1:-up}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MIGRATIONS_DIR="${REPO_ROOT}/db/migrations"

HOST="${MEETING_DB_HOST:-127.0.0.1}"
PORT="${MEETING_DB_PORT:-3306}"
USER="${MEETING_DB_USER:-root}"
PASSWORD="${MEETING_DB_PASSWORD:-}"
DATABASE="${MEETING_DB_NAME:-meeting}"

if [[ ! -d "${MIGRATIONS_DIR}" ]]; then
    echo "[migrate] migrations directory not found: ${MIGRATIONS_DIR}" >&2
    exit 1
fi

mysql_cmd=(mysql -h "${HOST}" -P "${PORT}" -u "${USER}")
if [[ -n "${PASSWORD}" ]]; then
    mysql_cmd+=(-p"${PASSWORD}")
fi

run_sql() {
    local file="$1"
    echo "[migrate] applying ${file}"
    # SQL 文件内部负责 CREATE DATABASE / USE meeting。
    # 若未来需要针对不同库执行，可在这里追加 --database 参数。
    "${mysql_cmd[@]}" < "${file}"
}

collect_up_files() {
    find "${MIGRATIONS_DIR}" -maxdepth 1 -type f -name '*.sql' ! -name '*_down.sql' \
        | sort
}

collect_down_files() {
    find "${MIGRATIONS_DIR}" -maxdepth 1 -type f -name '*_down.sql' \
        | sort -r
}

case "${MODE}" in
    up)
        mapfile -t files < <(collect_up_files)
        ;;
    down)
        mapfile -t files < <(collect_down_files)
        ;;
    *)
        echo "Usage: $0 {up|down}" >&2
        exit 1
        ;;
esac

if [[ ${#files[@]} -eq 0 ]]; then
    echo "[migrate] no ${MODE} migrations found in ${MIGRATIONS_DIR}"
    exit 0
fi

for file in "${files[@]}"; do
    run_sql "${file}"
done

echo "[migrate] ${MODE} completed successfully"