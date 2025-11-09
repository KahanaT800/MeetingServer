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
        meeting_service
        meeting_core
        meeting_proto
        thread_pool
)
add_test(NAME MeetingServiceTest COMMAND meeting_service_test)

# 设置测试输出目录
set_target_properties(meeting_manager_test meeting_service_test
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)