# src/CMakeLists.txt
set(PROTO_FILES
    ${CMAKE_SOURCE_DIR}/proto/health.proto
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
        ${CMAKE_CURRENT_BINARY_DIR}  # 生成的头文件在这里
)
target_link_libraries(meeting_proto
    PUBLIC
        gRPC::grpc++
        protobuf::libprotobuf
)

add_library(thread_pool STATIC
    thread_pool/src/thread_pool.cpp
    thread_pool/src/config.cpp
    thread_pool/src/logger.cpp
)
target_include_directories(thread_pool
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/thread_pool/include
)
target_link_libraries(thread_pool
    PUBLIC
        fmt::fmt
        spdlog::spdlog
        nlohmann_json::nlohmann_json
)

add_executable(meeting_server
    server/main.cpp
    server/health_service.cpp
)

add_executable(health_client
    client/health_client.cpp
)

target_include_directories(meeting_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(health_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(meeting_server
    PRIVATE
        meeting_proto
        thread_pool
        fmt::fmt
)

target_link_libraries(health_client
    PRIVATE
        meeting_proto
        fmt::fmt
)
