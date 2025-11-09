#!/usr/bin/env bash
set -euo pipefail

# 检查VCPKG_ROOT环境变量
if [ -z "${VCPKG_ROOT:-}" ]; then
    echo "Error: VCPKG_ROOT environment variable is not set"
    exit 1
fi

echo "[build] configure Debug build with vcpkg..."
cmake -S . -B build/debug \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET=x64-linux \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "[build] compile..."
cmake --build build/debug -j "$(nproc)"