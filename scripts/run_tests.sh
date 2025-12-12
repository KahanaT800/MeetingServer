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

echo "所有测试完成！"