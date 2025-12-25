#!/bin/bash

# 检查是否提供了 IP 参数
if [ -z "$1" ]; then
    echo "Usage: ./run_client.sh <SERVER_IP:PORT>"
    echo "Example: ./run_client.sh 57.183.34.18:50051"
    echo "Defaulting to localhost:50051"
    TARGET="localhost:50051"
else
    TARGET="$1"
fi

# 检查 node_modules 是否存在
if [ ! -d "node_modules" ]; then
    echo "Installing dependencies..."
    npm install
fi

# 检查端口 3000 是否被占用，如果是则杀掉
PORT=3000
PID=$(lsof -t -i:$PORT)
if [ ! -z "$PID" ]; then
    echo "⚠️  Port $PORT is in use by PID $PID. Killing it..."
    kill -9 $PID
    sleep 1 # 等待进程释放端口
fi

echo "Starting Web Client connecting to $TARGET..."
SERVER_ADDRESS=$TARGET npm start

