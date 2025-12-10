#!/bin/bash

# 测试构建和运行脚本
set -e

# 进入构建目录
cd build/debug

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
# 设置 MySQL 测试环境变量
export MEETING_DB_USER="${MEETING_DB_USER:-dev}"
export MEETING_DB_PASSWORD="${MEETING_DB_PASSWORD:-devpassed}"
./tests/storage_user_test

echo "运行会议存储测试..."
export MEETING_DB_USER="${MEETING_DB_USER:-dev}"
export MEETING_DB_PASSWORD="${MEETING_DB_PASSWORD:-devpassed}"
./tests/storage_meeting_test

echo "所有测试完成！"