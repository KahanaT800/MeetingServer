#!/bin/bash
set -e

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GENERATED_DIR="${SCRIPT_DIR}/generated"
VENV_DIR="${SCRIPT_DIR}/venv"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Default Server Address (can be overridden by first argument)
SERVER_ADDRESS="${1:-localhost:50051}"

echo -e "${GREEN}--- Meeting Server E2E Test ---${NC}"
echo "Target Server: ${SERVER_ADDRESS}"

# 1. Setup/Activate Virtual Environment
if [ -d "$VENV_DIR" ]; then
    echo "Activating virtual environment..."
    source "$VENV_DIR/bin/activate"
else
    echo "Creating virtual environment..."
    python3 -m venv "$VENV_DIR"
    source "$VENV_DIR/bin/activate"
fi

# 2. Install Dependencies
echo "Installing/Checking dependencies..."
pip install -q grpcio grpcio-tools protobuf

# 3. Generate Python Protos
echo "Generating Python protos..."
mkdir -p "${GENERATED_DIR}"
touch "${GENERATED_DIR}/__init__.py"

python3 -m grpc_tools.protoc \
    -I"${PROJECT_ROOT}/proto" \
    --python_out="${GENERATED_DIR}" \
    --grpc_python_out="${GENERATED_DIR}" \
    "${PROJECT_ROOT}/proto/common.proto" \
    "${PROJECT_ROOT}/proto/user_service.proto" \
    "${PROJECT_ROOT}/proto/meeting_service.proto"

# 4. Run Test
echo "Running E2E Test..."
export SERVER_ADDRESS="${SERVER_ADDRESS}"
python3 "${SCRIPT_DIR}/test_e2e.py"

