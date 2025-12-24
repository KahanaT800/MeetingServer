# tests/CMakeLists.txt

# 查找 Google Test
find_package(GTest REQUIRED)

# 启用测试
enable_testing()

# 包含源代码目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src  # 为生成的proto文件
)

# 测试单元

# 配置与日志测试
add_executable(config_logger_test
    unit/config_logger_test.cpp
)

target_link_libraries(config_logger_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        meeting_common
        thread_pool
)

add_test(NAME ConfigLoggerTest COMMAND config_logger_test)

set_target_properties(config_logger_test
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

# 用户管理器单元测试
add_executable(user_manager_test
    unit/user_manager_test.cpp
)
target_link_libraries(user_manager_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        user_core
        meeting_proto
)
add_test(NAME UserManagerTest COMMAND user_manager_test)

# 用户服务实现单元测试
add_executable(user_service_test
    unit/user_service_test.cpp
)
target_link_libraries(user_service_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        user_service
        user_core
        meeting_proto
)
add_test(NAME UserServiceTest COMMAND user_service_test)

# 设置测试输出目录
set_target_properties(user_manager_test user_service_test
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

# 会议管理器单元测试
add_executable(meeting_manager_test
    unit/meeting_manager_test.cpp
)
target_link_libraries(meeting_manager_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        meeting_core
        meeting_proto
)
add_test(NAME MeetingManagerTest COMMAND meeting_manager_test)

# 会议服务实现单元测试
add_executable(meeting_service_test
    unit/meeting_service_test.cpp
)
target_link_libraries(meeting_service_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        user_service
        meeting_service
        meeting_registry
        meeting_core
        meeting_proto
        thread_pool
)
add_test(NAME MeetingServiceTest COMMAND meeting_service_test)

# MySQL 存储单元测试
add_executable(storage_user_test
    unit/mysql_user_test.cpp
)
target_link_libraries(storage_user_test    
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        storage_mysql
        user_core
)
set_target_properties(storage_user_test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME StorageUserTest COMMAND storage_user_test)

# MySQL 会议存储单元测试
add_executable(storage_meeting_test
    unit/mysql_meeting_tests.cpp
)
target_link_libraries(storage_meeting_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        storage_mysql
        meeting_core
)
set_target_properties(storage_meeting_test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME StorageMeetingTest COMMAND storage_meeting_test)

# MySQL 流程存储单元测试
add_executable(storage_flow_test
    unit/mysql_flow_tests.cpp
)
target_link_libraries(storage_flow_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        meeting_service
        user_service
        meeting_common
        storage_mysql
)
set_target_properties(storage_flow_test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME StorageFlowTest COMMAND storage_flow_test)

# Redis 客户端测试
add_executable(redis_client_test
    unit/redis_client_test.cpp
)
target_link_libraries(redis_client_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        meeting_cache
)
set_target_properties(redis_client_test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME RedisClientTest COMMAND redis_client_test)

# Redis 流程测试（缓存 Session 端到端）
add_executable(redis_flow_test
    unit/redis_flow_test.cpp
)
target_link_libraries(redis_flow_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        user_core
        meeting_cache
        thread_pool
)
set_target_properties(redis_flow_test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME RedisFlowTest COMMAND redis_flow_test)

# GeoLocationService 测试
add_executable(geo_location_test
    unit/test_geo_location_service.cpp
)
target_link_libraries(geo_location_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        meeting_geo
)
target_compile_definitions(geo_location_test
    PRIVATE
        GEOIP_TEST_DB_PATH=\"${CMAKE_SOURCE_DIR}/ip_data/GeoLite2-City.mmdb\"
)
set_target_properties(geo_location_test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME GeoLocationServiceTest COMMAND geo_location_test)

# Zookeeper 注册中心集成测试
add_executable(server_registry_test
    unit/test_server_registry.cpp
)
target_link_libraries(server_registry_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        meeting_registry
        meeting_common
        thread_pool
)
set_target_properties(server_registry_test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME ServerRegistryTest COMMAND server_registry_test)

# 设置测试输出目录
set_target_properties(meeting_manager_test meeting_service_test
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
