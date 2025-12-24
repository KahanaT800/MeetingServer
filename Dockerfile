# Build stage
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    curl \
    zip \
    unzip \
    tar \
    pkg-config \
    linux-libc-dev \
    default-jdk \
    && rm -rf /var/lib/apt/lists/*

# Setup vcpkg
ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT && \
    $VCPKG_ROOT/bootstrap-vcpkg.sh

# Install dependencies
WORKDIR /app
COPY vcpkg.json .
# Install dependencies (this may take a while)
RUN $VCPKG_ROOT/vcpkg install --triplet x64-linux

# Build application
COPY . .
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=x64-linux \
    && cmake --build build -j $(nproc)

# Runtime stage
FROM ubuntu:22.04

WORKDIR /app

# Install runtime dependencies
# We install common libraries that might be needed by dynamic linking
RUN apt-get update && apt-get install -y \
    ca-certificates \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy artifacts from builder
COPY --from=builder /app/build/meeting_server /app/meeting_server
# Copy shared libraries from vcpkg (if any are needed)
COPY --from=builder /app/vcpkg_installed/x64-linux/lib/*.so* /usr/local/lib/

# Update shared library cache
RUN ldconfig

# Copy configuration and data
COPY config /app/config
COPY ip_data /app/ip_data
# Create logs directory
RUN mkdir -p /app/logs

# Expose the gRPC port
EXPOSE 50051

# Run the server
CMD ["./meeting_server"]
