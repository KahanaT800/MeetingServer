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

# 用户核心库 (业务逻辑)
add_library(user_core STATIC
    core/user/user_manager.cpp
    core/user/session_manager.cpp
)
target_include_directories(user_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/src  # 为生成的配置头文件
)
target_link_libraries(user_core
    PUBLIC
        meeting_proto
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
        user_core
        thread_pool
        meeting_proto
        gRPC::grpc++
)

# 会议核心库 (业务逻辑)
add_library(meeting_core STATIC
    core/meeting/meeting_manager.cpp
)
target_include_directories(meeting_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(meeting_core
    PUBLIC
        meeting_proto
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
        meeting_proto
        thread_pool
        gRPC::grpc++
)

