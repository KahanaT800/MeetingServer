#!/bin/bash

# 测试构建和运行脚本
set -e

# 进入构建目录
cd build/debug

# 运行测试
echo "运行用户管理器测试..."
./tests/user_manager_test

echo "运行用户服务测试..."
./tests/user_service_test

echo "运行会议管理器测试..."
./tests/meeting_manager_test

echo "运行会议服务测试..."
./tests/meeting_service_test

echo "所有测试完成！"