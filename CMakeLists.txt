# 顶层 CMake 配置
cmake_minimum_required(VERSION 3.20)
project(meeting_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# 编译警告配置
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

# 查找依赖包
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(redis++ CONFIG REQUIRED)

# 统一生成目录
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

set(PROTOBUF_GENERATE_CPP_APPEND_PATH ON)
set(GRPC_GENERATE_CPP_APPEND_PATH ON)

# 配置项目路径头文件
configure_file(
    "${PROJECT_SOURCE_DIR}/src/common/config_path.hpp.in"
    "${CMAKE_BINARY_DIR}/src/config_path.hpp"
    @ONLY
)

# 生成 gRPC 代码
function(generate_grpc OUT_SRCS OUT_HDRS PROTO_FILE)
    get_filename_component(PROTO_ABS "${PROTO_FILE}" ABSOLUTE)
    get_filename_component(PROTO_NAME "${PROTO_FILE}" NAME_WE)
    get_filename_component(PROTO_DIR "${PROTO_ABS}" DIRECTORY)
    message(STATUS "Generating sources for ${PROTO_NAME}.proto")

    # 使用protobuf提供的函数生成C++代码
    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS "${PROTO_ABS}")
    
    # 手动生成gRPC代码
    set(GRPC_CPP_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_H_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h")
    
    add_custom_command(
        OUTPUT "${GRPC_CPP_FILE}" "${GRPC_H_FILE}"
        COMMAND $<TARGET_FILE:protobuf::protoc>
        ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}" 
             --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             -I "${PROTO_DIR}"
             "${PROTO_ABS}"
        DEPENDS "${PROTO_ABS}" gRPC::grpc_cpp_plugin protobuf::protoc
        COMMENT "Generating gRPC C++ files for ${PROTO_NAME}.proto"
    )

    list(APPEND ${OUT_SRCS} ${PROTO_SRCS} ${GRPC_CPP_FILE})
    list(APPEND ${OUT_HDRS} ${PROTO_HDRS} ${GRPC_H_FILE})

    set(${OUT_SRCS} "${${OUT_SRCS}}" PARENT_SCOPE)
    set(${OUT_HDRS} "${${OUT_HDRS}}" PARENT_SCOPE)
endfunction()

# 添加src子目录
add_subdirectory(src)

# 添加测试子目录 (可选)
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
