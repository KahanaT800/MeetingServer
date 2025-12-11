#include "core/user/user_repository.hpp"

#include <mutex>

namespace meeting {
namespace core {

// 创建新用户
meeting::common::Status InMemoryUserRepository::CreateUser(const UserData& data) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (users_by_user_name_.count(data.user_name) > 0) {
        return meeting::common::Status::AlreadyExists("User name already exists.");
    }
    UserData stored = data;
    if (stored.numeric_id == 0) {
        stored.numeric_id = next_numeric_id_++;
    }
    users_by_user_name_[stored.user_name] = stored;
    users_by_id_[stored.user_id] = stored;
    return meeting::common::Status::OK();
}

// 根据用户名查找用户
meeting::common::StatusOr<UserData> InMemoryUserRepository::FindByUserName(const std::string& user_name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = users_by_user_name_.find(user_name);
    if (it == users_by_user_name_.end()) {
        return meeting::common::Status::NotFound("User not found.");
    }
    return meeting::common::StatusOr<UserData>(it->second);
}

// 根据用户ID查找用户
meeting::common::StatusOr<UserData> InMemoryUserRepository::FindById(const std::string& user_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = users_by_id_.find(user_id);
    if (it == users_by_id_.end()) {
        return meeting::common::Status::NotFound("User not found.");
    }
    return meeting::common::StatusOr<UserData>(it->second);
}

// 更新用户最后登录时间
meeting::common::Status InMemoryUserRepository::UpdateLastLogin(const std::string& user_id, std::int64_t last_login) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = users_by_id_.find(user_id);
    if (it == users_by_id_.end()) {
        return meeting::common::Status::NotFound("User not found.");
    }
    it->second.last_login = last_login;
    // 同步更新按用户名存储的用户数据
    auto name_it = users_by_user_name_.find(it->second.user_name);
    if (name_it != users_by_user_name_.end()) {
        name_it->second.last_login = last_login;
    }
    return meeting::common::Status::OK();
}

} // namespace core
} // namespace meeting
