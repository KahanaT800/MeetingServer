#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"
#include "core/user/user_manager.hpp"

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace meeting {
namespace core {

// 用户存储接口
class UserRepository {
public:
    virtual ~UserRepository() = default;
    // 创建新用户   
    virtual meeting::common::Status CreateUser(const UserData& data) = 0;

    // 根据用户名查找用户
    virtual meeting::common::StatusOr<UserData> FindByUserName(const std::string& user_name) const = 0;

    // 根据用户ID查找用户
    virtual meeting::common::StatusOr<UserData> FindById(const std::string& id) const = 0;
    
    // 更新用户信息
    virtual meeting::common::Status UpdateLastLogin(const std::string& user_id, std::int64_t last_login) = 0;
};

// 基于内存的用户存储实现
class InMemoryUserRepository : public UserRepository {
public:
    // 重写基类方法
    meeting::common::Status CreateUser(const UserData& data) override;
    meeting::common::StatusOr<UserData> FindByUserName(const std::string& user_name) const override;
    meeting::common::StatusOr<UserData> FindById(const std::string& id) const override;
    meeting::common::Status UpdateLastLogin(const std::string& user_id, std::int64_t last_login) override;
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, UserData> users_by_user_name_;
    std::unordered_map<std::string, UserData> users_by_id_;
};

} // namespace core
} // namespace meeting