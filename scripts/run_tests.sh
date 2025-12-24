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
  "geoip": { "db_path": "${REPO_ROOT}/ip_data/GeoLite2-City.mmdb" },
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

# 使用 Docker 启动一个临时 Zookeeper（默认开启，若无 docker 则跳过）
start_docker_zk() {
    if [ "${START_ZK_DOCKER:-1}" != "1" ]; then
        echo "跳过自动启动 Zookeeper（START_ZK_DOCKER=0）"
        return
    fi
    if ! command -v docker >/dev/null 2>&1; then
        echo "未找到 docker，跳过自动启动 Zookeeper"
        return
    fi
    local name="meeting_zk_test"
    local hosts="127.0.0.1:2181"

    # 1. 尝试清理旧的测试容器（如果存在），确保环境干净
    if docker ps -a --format '{{.Names}}' | grep -q "^${name}\$"; then
        echo "清理旧的测试容器 ${name}..."
        docker rm -f "${name}" >/dev/null 2>&1 || true
    fi

    # 2. 端口已占用则跳过启动，直接用现有服务（可能是 docker-compose 启动的 meeting_zookeeper）
    if (echo > /dev/tcp/127.0.0.1/2181) >/dev/null 2>&1; then
        echo "检测到 127.0.0.1:2181 已被占用，假设已有 Zookeeper 运行，直接使用"
        export ZK_HOSTS="${ZK_HOSTS:-$hosts}"
        return
    fi

    echo "启动临时 Zookeeper 容器(${name})..."
    set +e
    docker run -d --rm --name "${name}" -p 2181:2181 zookeeper:3.8 >/dev/null
    local rc=$?
    set -e
    if [ $rc -ne 0 ]; then
        echo "警告：启动 Zookeeper 容器失败，跳过自动启动（rc=${rc}）。若需测试请手动启动 ZK 或释放 2181 端口。"
        return
    fi
    export ZK_HOSTS="${ZK_HOSTS:-$hosts}"
    # 等待端口就绪（最多 30 秒）
    for i in $(seq 1 30); do
        if (echo > /dev/tcp/127.0.0.1/2181) >/dev/null 2>&1; then
            echo "Zookeeper 已就绪: ${ZK_HOSTS}"
            # 仅在本函数启动时添加清理
            trap 'docker stop ${name} >/dev/null 2>&1 || true' EXIT
            return
        fi
        sleep 1
    done
    echo "警告：等待 Zookeeper 就绪超时，后续测试可能跳过或降级"
}

start_docker_zk

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

echo "运行 Zookeeper 注册中心集成测试..."
./tests/server_registry_test

echo "所有测试完成！"
