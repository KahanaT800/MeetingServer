#include "server/user_service_impl.hpp"
#include "user_service.grpc.pb.h"
#include "test_mysql_utils.hpp"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <memory>

using namespace meeting::server;
class UserServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        testutils::ClearMysqlTestData();
        user_service_ = std::make_unique<UserServiceImpl>();
    }

    void TearDown() override {
        user_service_.reset();
    }

    std::unique_ptr<UserServiceImpl> user_service_;
    grpc::ServerContext context_;
};

// 测试用户注册成功
TEST_F(UserServiceTest, RegisterSuccess) {
    proto::user::RegisterRequest request;
    request.set_user_name("testuser");
    request.set_password("password123");
    request.set_email("test@example.com");
    request.set_display_name("Test User");
    
    proto::user::RegisterResponse response;
    
    auto status = user_service_->Register(&context_, &request, &response);
    
    EXPECT_TRUE(status.ok()) << "注册RPC调用失败: " << status.error_message();
    EXPECT_EQ(response.error().code(), 0) << "注册业务逻辑失败: " << response.error().message();
    
    // 验证返回的用户信息
    EXPECT_FALSE(response.user().user_id().empty());
    EXPECT_EQ(response.user().user_name(), "testuser");
    EXPECT_EQ(response.user().email(), "test@example.com");
    EXPECT_EQ(response.user().display_name(), "Test User");
}

// 测试重复用户名注册失败
TEST_F(UserServiceTest, RegisterDuplicateUserName) {
    proto::user::RegisterRequest request;
    request.set_user_name("testuser");
    request.set_password("password123");
    request.set_email("test@example.com");
    request.set_display_name("Test User");
    
    proto::user::RegisterResponse response1;
    proto::user::RegisterResponse response2;
    
    // 第一次注册应该成功
    auto status1 = user_service_->Register(&context_, &request, &response1);
    EXPECT_TRUE(status1.ok());
    EXPECT_EQ(response1.error().code(), 0);
    
    // 第二次注册相同用户名应该失败
    auto status2 = user_service_->Register(&context_, &request, &response2);
    EXPECT_FALSE(status2.ok());
    EXPECT_EQ(status2.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// 测试空用户名注册失败
TEST_F(UserServiceTest, RegisterEmptyUserName) {
    proto::user::RegisterRequest request;
    request.set_user_name("");
    request.set_password("password123");
    request.set_email("test@example.com");
    request.set_display_name("Test User");
    
    proto::user::RegisterResponse response;
    
    auto status = user_service_->Register(&context_, &request, &response);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// 测试用户登录成功
TEST_F(UserServiceTest, LoginSuccess) {
    // 先注册一个用户
    proto::user::RegisterRequest reg_request;
    reg_request.set_user_name("testuser");
    reg_request.set_password("password123");
    reg_request.set_email("test@example.com");
    reg_request.set_display_name("Test User");
    
    proto::user::RegisterResponse reg_response;
    auto reg_status = user_service_->Register(&context_, &reg_request, &reg_response);
    ASSERT_TRUE(reg_status.ok());
    ASSERT_EQ(reg_response.error().code(), 0);
    
    // 然后测试登录
    proto::user::LoginRequest login_request;
    login_request.set_user_name("testuser");
    login_request.set_password("password123");
    
    proto::user::LoginResponse login_response;
    auto login_status = user_service_->Login(&context_, &login_request, &login_response);
    
    EXPECT_TRUE(login_status.ok()) << "登录RPC调用失败: " << login_status.error_message();
    EXPECT_EQ(login_response.error().code(), 0) << "登录业务逻辑失败: " << login_response.error().message();
    
    // 验证登录响应
    EXPECT_FALSE(login_response.session_token().empty());
    EXPECT_EQ(login_response.user().user_name(), "testuser");
    EXPECT_EQ(login_response.user().email(), "test@example.com");
}

// 测试错误密码登录失败
TEST_F(UserServiceTest, LoginWrongPassword) {
    // 先注册一个用户
    proto::user::RegisterRequest reg_request;
    reg_request.set_user_name("testuser");
    reg_request.set_password("password123");
    reg_request.set_email("test@example.com");
    reg_request.set_display_name("Test User");
    
    proto::user::RegisterResponse reg_response;
    auto reg_status = user_service_->Register(&context_, &reg_request, &reg_response);
    ASSERT_TRUE(reg_status.ok());
    ASSERT_EQ(reg_response.error().code(), 0);
    
    // 用错误密码登录
    proto::user::LoginRequest login_request;
    login_request.set_user_name("testuser");
    login_request.set_password("wrongpassword");
    
    proto::user::LoginResponse login_response;
    auto login_status = user_service_->Login(&context_, &login_request, &login_response);
    
    EXPECT_FALSE(login_status.ok());
    EXPECT_EQ(login_status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}

// 测试不存在用户登录失败
TEST_F(UserServiceTest, LoginUserNotFound) {
    proto::user::LoginRequest login_request;
    login_request.set_user_name("nonexistentuser");
    login_request.set_password("password123");
    
    proto::user::LoginResponse login_response;
    auto login_status = user_service_->Login(&context_, &login_request, &login_response);
    
    EXPECT_FALSE(login_status.ok());
    EXPECT_EQ(login_status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// 测试登出成功
TEST_F(UserServiceTest, LogoutSuccess) {
    // 先注册并登录一个用户
    proto::user::RegisterRequest reg_request;
    reg_request.set_user_name("testuser");
    reg_request.set_password("password123");
    reg_request.set_email("test@example.com");
    reg_request.set_display_name("Test User");
    
    proto::user::RegisterResponse reg_response;
    auto reg_status = user_service_->Register(&context_, &reg_request, &reg_response);
    ASSERT_TRUE(reg_status.ok());
    
    proto::user::LoginRequest login_request;
    login_request.set_user_name("testuser");
    login_request.set_password("password123");
    
    proto::user::LoginResponse login_response;
    auto login_status = user_service_->Login(&context_, &login_request, &login_response);
    ASSERT_TRUE(login_status.ok());
    ASSERT_EQ(login_response.error().code(), 0);
    
    // 登出
    proto::user::LogoutRequest logout_request;
    logout_request.set_session_token(login_response.session_token());
    
    proto::user::LogoutResponse logout_response;
    auto logout_status = user_service_->Logout(&context_, &logout_request, &logout_response);
    
    EXPECT_TRUE(logout_status.ok()) << "登出RPC调用失败: " << logout_status.error_message();
    EXPECT_EQ(logout_response.error().code(), 0) << "登出业务逻辑失败: " << logout_response.error().message();
}

// 测试获取用户资料成功
TEST_F(UserServiceTest, GetProfileSuccess) {
    // 先注册并登录一个用户
    proto::user::RegisterRequest reg_request;
    reg_request.set_user_name("testuser");
    reg_request.set_password("password123");
    reg_request.set_email("test@example.com");
    reg_request.set_display_name("Test User");
    
    proto::user::RegisterResponse reg_response;
    auto reg_status = user_service_->Register(&context_, &reg_request, &reg_response);
    ASSERT_TRUE(reg_status.ok());
    
    proto::user::LoginRequest login_request;
    login_request.set_user_name("testuser");
    login_request.set_password("password123");
    
    proto::user::LoginResponse login_response;
    auto login_status = user_service_->Login(&context_, &login_request, &login_response);
    ASSERT_TRUE(login_status.ok());
    ASSERT_EQ(login_response.error().code(), 0);
    
    // 获取用户资料
    proto::user::GetProfileRequest profile_request;
    profile_request.set_session_token(login_response.session_token());
    
    proto::user::GetProfileResponse profile_response;
    auto profile_status = user_service_->GetProfile(&context_, &profile_request, &profile_response);
    
    EXPECT_TRUE(profile_status.ok()) << "获取用户资料RPC调用失败: " << profile_status.error_message();
    EXPECT_EQ(profile_response.error().code(), 0) << "获取用户资料业务逻辑失败: " << profile_response.error().message();
    
    // 验证用户资料
    EXPECT_EQ(profile_response.user().user_name(), "testuser");
    EXPECT_EQ(profile_response.user().email(), "test@example.com");
    EXPECT_EQ(profile_response.user().display_name(), "Test User");
}

// 测试无效session token获取用户资料失败
TEST_F(UserServiceTest, GetProfileInvalidToken) {
    proto::user::GetProfileRequest profile_request;
    profile_request.set_session_token("invalid_token");
    
    proto::user::GetProfileResponse profile_response;
    auto profile_status = user_service_->GetProfile(&context_, &profile_request, &profile_response);
    
    // 应该返回session过期错误
    EXPECT_FALSE(profile_status.ok());
    // 由于session验证失败，应该返回相应的错误码
}
