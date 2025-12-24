# src/CMakeLists.txt

# Proto文件生成规则
set(PROTO_FILES
    ${CMAKE_SOURCE_DIR}/proto/common.proto
    ${CMAKE_SOURCE_DIR}/proto/user_service.proto
    ${CMAKE_SOURCE_DIR}/proto/meeting_service.proto
)

set(GENERATED_SRCS "")
set(GENERATED_HDRS "")
foreach(PROTO_FILE IN LISTS PROTO_FILES)
    generate_grpc(GENERATED_SRCS GENERATED_HDRS "${PROTO_FILE}")
endforeach()

add_library(meeting_proto STATIC
    ${GENERATED_SRCS}
)
target_include_directories(meeting_proto
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(meeting_proto
    PUBLIC
        gRPC::grpc++
        protobuf::libprotobuf
)

# 配置和日志公共库
add_library(meeting_common STATIC
    common/config_loader.cpp
    common/logger.cpp
)
target_include_directories(meeting_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/src
)
target_link_libraries(meeting_common
    PUBLIC
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        thread_pool
)

# 线程池库
add_library(thread_pool STATIC
    thread_pool/src/thread_pool.cpp
    thread_pool/src/config.cpp
    thread_pool/src/logger.cpp
)
target_include_directories(thread_pool
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/thread_pool/include
        ${CMAKE_BINARY_DIR}/src  # 为生成的配置头文件
)
target_link_libraries(thread_pool
    PUBLIC
        fmt::fmt
        spdlog::spdlog
        nlohmann_json::nlohmann_json
)

# Storage MySQL库
add_library(storage_mysql STATIC
    storage/mysql/connection.cpp
    storage/mysql/connection_pool.cpp
    storage/mysql/user_repository.cpp
    storage/mysql/transaction.cpp
    storage/mysql/meeting_repository.cpp
    storage/mysql/session_repository.cpp
)
target_include_directories(storage_mysql
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/src
)
target_link_libraries(storage_mysql
    PUBLIC
        meeting_common
        user_core
        meeting_core
        fmt::fmt
        mysqlclient
)

# Redis 库
add_library(meeting_cache STATIC
    cache/redis_client.cpp
)
target_include_directories(meeting_cache
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(meeting_cache
    PUBLIC
        meeting_common
        redis++::redis++_static
)

# 用户核心库 (业务逻辑)
add_library(user_core STATIC
    core/user/user_manager.cpp
    core/user/session_manager.cpp
    core/user/user_repository.cpp
    core/user/session_repository.cpp
    core/user/cached_session_repository.cpp
    core/user/cached_user_repository.cpp
)
target_include_directories(user_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/src  # 为生成的配置头文件
)
target_link_libraries(user_core
    PUBLIC
        meeting_proto
        meeting_cache
        OpenSSL::SSL
        OpenSSL::Crypto
)

# 用户服务库 (gRPC 服务实现)
add_library(user_service STATIC
    server/user_service_impl.cpp
)
target_include_directories(user_service
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/src  # 为生成的配置头文件
)
target_link_libraries(user_service
    PUBLIC
        meeting_common
        storage_mysql
        user_core
        thread_pool
        meeting_proto
        gRPC::grpc++
)

# 会议核心库 (业务逻辑)
add_library(meeting_core STATIC
    core/meeting/meeting_manager.cpp
    core/meeting/meeting_repository.cpp
    core/meeting/cached_meeting_repository.cpp
)
target_include_directories(meeting_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(meeting_core
    PUBLIC
        meeting_proto
        meeting_cache
)

# 会议服务库 (gRPC 服务实现)
add_library(meeting_service STATIC
    server/meeting_service_impl.cpp
)
target_include_directories(meeting_service
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(meeting_service
    PUBLIC
        meeting_core
        user_core
        meeting_common
        storage_mysql
        meeting_registry
        unofficial::zookeeper::zookeeper
        unofficial::zookeeper::hashtable
        meeting_cache
        meeting_proto
        thread_pool
        gRPC::grpc++
)

# GeoLocationService 库
add_library(meeting_geo STATIC
    geo/geo_location_service.cpp
)
target_include_directories(meeting_geo
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(meeting_geo
    PUBLIC
        meeting_common
        maxminddb::maxminddb
)

# Zookeeper 注册中心库
add_library(meeting_registry STATIC
    registry/server_registry.cpp
    scheduler/load_balancer.cpp
)
target_include_directories(meeting_registry
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(meeting_registry
    PUBLIC
        meeting_common
        meeting_geo
        unofficial::zookeeper::zookeeper
        unofficial::zookeeper::hashtable
)

# meeting_server executable
add_executable(meeting_server
    server/main.cpp
)
target_include_directories(meeting_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/src
)
target_link_libraries(meeting_server
    PRIVATE
        meeting_service
        meeting_common
        user_service
        meeting_proto
        thread_pool
        meeting_registry
        gRPC::grpc++
)

# 工具：geo_lookup - 用于测试 IP 地理位置查询
add_executable(geo_lookup
    ${CMAKE_SOURCE_DIR}/src/utils/geo_lookup.cpp
)
target_link_libraries(geo_lookup
    PRIVATE
        meeting_geo
)

