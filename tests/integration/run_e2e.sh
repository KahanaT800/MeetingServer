#!/bin/bash
set -e

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GENERATED_DIR="${SCRIPT_DIR}/generated"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo "--- Meeting Server E2E Test ---"

# 1. Check Python dependencies
if ! python3 -c "import grpc" 2>/dev/null; then
    echo -e "${RED}Error: python3-grpcio is not installed.${NC}"
    echo "Please install it using: pip3 install grpcio grpcio-tools protobuf"
    exit 1
fi

if ! python3 -c "import grpc_tools.protoc" 2>/dev/null; then
    echo -e "${RED}Error: grpcio-tools is not installed.${NC}"
    echo "Please install it using: pip3 install grpcio-tools"
    exit 1
fi

# 2. Generate Python Protos
echo "Generating Python protos..."
mkdir -p "${GENERATED_DIR}"
# Create __init__.py to make it a package
touch "${GENERATED_DIR}/__init__.py"

python3 -m grpc_tools.protoc \
    -I"${PROJECT_ROOT}/proto" \
    --python_out="${GENERATED_DIR}" \
    --grpc_python_out="${GENERATED_DIR}" \
    "${PROJECT_ROOT}/proto/common.proto" \
    "${PROJECT_ROOT}/proto/user_service.proto" \
    "${PROJECT_ROOT}/proto/meeting_service.proto"

# Fix imports in generated files (common issue with protobuf python generation)
# Protoc generates "import common_pb2" which fails if not in path. 
# We need to ensure the generated directory is in sys.path (handled in python script)
# But sometimes relative imports are tricky. 
# For this simple structure, adding generated dir to sys.path in the python script is usually enough.

# 3. Run Test
echo "Running E2E Test..."
export PYTHONPATH="${PYTHONPATH}:${GENERATED_DIR}"

# Check if server is running (simple check on port 50051)
if ! nc -z localhost 50051 2>/dev/null; then
    echo -e "${RED}Warning: Port 50051 is not open. Is the server running?${NC}"
    echo "You can run the server with: docker-compose up -d server"
    read -p "Continue anyway? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

python3 "${SCRIPT_DIR}/test_e2e.py"
