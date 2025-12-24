#!/bin/bash

# 测试构建和运行脚本
set -e

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 设置 MySQL 测试环境变量
export MEETING_DB_USER="${MEETING_DB_USER:-dev}"
export MEETING_DB_PASSWORD="${MEETING_DB_PASSWORD:-devpassed}"
export MEETING_DB_NAME="${MEETING_DB_NAME:-meeting}"
export MEETING_DB_HOST="${MEETING_DB_HOST:-127.0.0.1}"
export MEETING_DB_PORT="${MEETING_DB_PORT:-3306}"

# 设置 Redis 测试环境变量
export REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
export REDIS_PORT="${REDIS_PORT:-6379}"
export REDIS_PASSWORD="${REDIS_PASSWORD:-}"

# 生成临时配置文件，强制开启 MySQL/Redis 并写入凭据
TEMP_CONFIG_FILE="$(mktemp /tmp/meeting_app_test_XXXX.json)"
cat > "${TEMP_CONFIG_FILE}" <<EOF
{
  "server": { "host": "0.0.0.0", "port": 50051 },
  "logging": { "level": "error", "console": true },
  "thread_pool": { "config_path": "config/thread_pool.json" },
  "storage": {
    "mysql": {
      "host": "${MEETING_DB_HOST}",
      "port": ${MEETING_DB_PORT},
      "user": "${MEETING_DB_USER}",
      "password": "${MEETING_DB_PASSWORD}",
      "database": "${MEETING_DB_NAME}",
      "pool_size": 10,
      "connection_timeout_ms": 500,
      "read_timeout_ms": 2000,
      "write_timeout_ms": 2000,
      "enabled": true
    }
  },
  "cache": {
    "redis": {
      "host": "${REDIS_HOST}",
      "port": ${REDIS_PORT},
      "password": "${REDIS_PASSWORD}",
      "db": 0,
      "pool_size": 4,
      "connection_timeout_ms": 500,
      "socket_timeout_ms": 2000,
      "enabled": true
    }
  }
}
EOF
export MEETING_SERVER_CONFIG="${TEMP_CONFIG_FILE}"
cleanup() { rm -f "${TEMP_CONFIG_FILE}"; }
trap cleanup EXIT

# 运行数据库迁移以确保数据库和表结构存在
echo "初始化测试数据库..."
bash "${SCRIPT_DIR}/migrate.sh" up

# 进入构建目录
cd "${REPO_ROOT}/build/debug"

# 运行测试

echo "运行配置与日志测试..."
./tests/config_logger_test

echo "运行用户管理器测试..."
./tests/user_manager_test

echo "运行用户服务测试..."
./tests/user_service_test

echo "运行会议管理器测试..."
./tests/meeting_manager_test

echo "运行会议服务测试..."
./tests/meeting_service_test

echo "运行用户存储测试..."
./tests/storage_user_test

echo "运行会议存储测试..."
./tests/storage_meeting_test

echo "运行 MySQL 流测试..."
./tests/storage_flow_test

echo "运行 Redis 客户端测试..."
./tests/redis_client_test

echo "运行 Redis 会话缓存全流程测试..."
./tests/redis_flow_test

echo "运行 GeoLocationService 测试..."
./tests/geo_location_test

echo "所有测试完成！"
