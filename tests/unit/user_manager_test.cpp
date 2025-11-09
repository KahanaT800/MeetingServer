#include "core/user/user_manager.hpp"
#include "common/status.hpp"

#include <gtest/gtest.h>

using namespace meeting::core;
using namespace meeting::common;

class UserManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        user_manager_ = std::make_unique<UserManager>();
    }

    void TearDown() override {
        user_manager_.reset();
    }

    std::unique_ptr<UserManager> user_manager_;
};

// 测试用户注册成功的情况
TEST_F(UserManagerTest, RegisterUserSuccess) {
    RegisterCommand command{
        "testuser", 
        "password123", 
        "test@example.com", 
        "Test User"
    };
    
    auto status = user_manager_->RegisterUser(command);
    EXPECT_TRUE(status.IsOk()) << "注册用户失败: " << status.Message();
    
    // 验证用户是否可以通过用户名找到
    auto user_result = user_manager_->GetUserByUserName("testuser");
    EXPECT_TRUE(user_result.IsOk()) << "获取用户失败: " << user_result.GetStatus().Message();
    
    const auto& user = user_result.Value();
    EXPECT_EQ(user.user_name, "testuser");
    EXPECT_EQ(user.email, "test@example.com");
    EXPECT_EQ(user.display_name, "Test User");
    EXPECT_FALSE(user.user_id.empty());
    EXPECT_FALSE(user.password_hash.empty());
    EXPECT_FALSE(user.salt.empty());
}

// 测试重复用户名注册失败
TEST_F(UserManagerTest, RegisterUserDuplicateUserName) {
    RegisterCommand command{
        "testuser", 
        "password123", 
        "test@example.com", 
        "Test User"
    };
    
    // 第一次注册应该成功
    auto status1 = user_manager_->RegisterUser(command);
    EXPECT_TRUE(status1.IsOk());
    
    // 第二次注册相同用户名应该失败
    auto status2 = user_manager_->RegisterUser(command);
    EXPECT_FALSE(status2.IsOk());
    EXPECT_EQ(status2.Code(), StatusCode::kAlreadyExists);
}

// 测试空用户名注册失败
TEST_F(UserManagerTest, RegisterUserEmptyUserName) {
    RegisterCommand command{
        "", 
        "password123", 
        "test@example.com", 
        "Test User"
    };
    
    auto status = user_manager_->RegisterUser(command);
    EXPECT_FALSE(status.IsOk());
    EXPECT_EQ(status.Code(), StatusCode::kInvalidArgument);
}

// 测试空密码注册失败
TEST_F(UserManagerTest, RegisterUserEmptyPassword) {
    RegisterCommand command{
        "testuser", 
        "", 
        "test@example.com", 
        "Test User"
    };
    
    auto status = user_manager_->RegisterUser(command);
    EXPECT_FALSE(status.IsOk());
    EXPECT_EQ(status.Code(), StatusCode::kInvalidArgument);
}

// 测试用户登录成功
TEST_F(UserManagerTest, LoginUserSuccess) {
    // 先注册一个用户
    RegisterCommand reg_command{
        "testuser", 
        "password123", 
        "test@example.com", 
        "Test User"
    };
    auto reg_status = user_manager_->RegisterUser(reg_command);
    ASSERT_TRUE(reg_status.IsOk());
    
    // 然后测试登录
    LoginCommand login_command{
        "testuser", 
        "password123", 
        "127.0.0.1", 
        "TestAgent"
    };
    
    auto login_result = user_manager_->LoginUser(login_command);
    EXPECT_TRUE(login_result.IsOk()) << "登录失败: " << login_result.GetStatus().Message();
    
    const auto& user = login_result.Value();
    EXPECT_EQ(user.user_name, "testuser");
    EXPECT_EQ(user.email, "test@example.com");
}

// 测试错误密码登录失败
TEST_F(UserManagerTest, LoginUserWrongPassword) {
    // 先注册一个用户
    RegisterCommand reg_command{
        "testuser", 
        "password123", 
        "test@example.com", 
        "Test User"
    };
    auto reg_status = user_manager_->RegisterUser(reg_command);
    ASSERT_TRUE(reg_status.IsOk());
    
    // 用错误密码登录
    LoginCommand login_command{
        "testuser", 
        "wrongpassword", 
        "127.0.0.1", 
        "TestAgent"
    };
    
    auto login_result = user_manager_->LoginUser(login_command);
    EXPECT_FALSE(login_result.IsOk());
    EXPECT_EQ(login_result.GetStatus().Code(), StatusCode::kUnauthenticated);
}

// 测试不存在的用户登录失败
TEST_F(UserManagerTest, LoginUserNotFound) {
    LoginCommand login_command{
        "nonexistentuser", 
        "password123", 
        "127.0.0.1", 
        "TestAgent"
    };
    
    auto login_result = user_manager_->LoginUser(login_command);
    EXPECT_FALSE(login_result.IsOk());
    EXPECT_EQ(login_result.GetStatus().Code(), StatusCode::kNotFound);
}

// 测试通过用户ID获取用户
TEST_F(UserManagerTest, GetUserById) {
    // 先注册一个用户
    RegisterCommand reg_command{
        "testuser", 
        "password123", 
        "test@example.com", 
        "Test User"
    };
    auto reg_status = user_manager_->RegisterUser(reg_command);
    ASSERT_TRUE(reg_status.IsOk());
    
    // 获取用户信息
    auto user_by_name = user_manager_->GetUserByUserName("testuser");
    ASSERT_TRUE(user_by_name.IsOk());
    
    const std::string& user_id = user_by_name.Value().user_id;
    
    // 通过ID获取用户
    auto user_by_id = user_manager_->GetUserById(user_id);
    EXPECT_TRUE(user_by_id.IsOk());
    
    const auto& user = user_by_id.Value();
    EXPECT_EQ(user.user_id, user_id);
    EXPECT_EQ(user.user_name, "testuser");
}

// 测试获取不存在的用户
TEST_F(UserManagerTest, GetUserByIdNotFound) {
    auto user_result = user_manager_->GetUserById("nonexistent_id");
    EXPECT_FALSE(user_result.IsOk());
    EXPECT_EQ(user_result.GetStatus().Code(), StatusCode::kNotFound);
}

// 测试获取不存在的用户名
TEST_F(UserManagerTest, GetUserByUserNameNotFound) {
    auto user_result = user_manager_->GetUserByUserName("nonexistent_user");
    EXPECT_FALSE(user_result.IsOk());
    EXPECT_EQ(user_result.GetStatus().Code(), StatusCode::kNotFound);
}