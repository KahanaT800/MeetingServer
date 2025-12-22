#pragma once

#include "cache/redis_client.hpp"
#include "core/user/user_repository.hpp"

#include <string>
#include <memory>

namespace meeting {
namespace core {

class CachedUserRepository : public UserRepository {
public:
    // 构造函数，接受一个主用户存储库(primary)和一个Redis客户端作为缓存层
    // ttl_seconds指定缓存的过期时间，默认为600秒(10分钟)
    CachedUserRepository(std::shared_ptr<UserRepository> primary,
                         std::shared_ptr<meeting::cache::RedisClient> redis,
                         int ttl_seconds = 600);
    
    // 重写基类方法
    // 创建新用户
    meeting::common::Status CreateUser(const UserData& data) override;
    // 根据用户名查找用户
    meeting::common::StatusOr<UserData> FindByUserName(const std::string& user_name) const override;
    // 根据用户ID查找用户
    meeting::common::StatusOr<UserData> FindById(const std::string& id) const override;
    // 更新用户信息
    meeting::common::Status UpdateLastLogin(const std::string& user_id, std::int64_t last_login) override;

private:
    // 辅助函数：检查是否启用了缓存
    bool HasCache() const {return static_cast<bool>(redis_);}
    // 辅助函数：生成基于用户ID的缓存键
    std::string KeyById(const std::string& user_id) const;
    // 辅助函数：生成基于用户名的缓存键
    std::string KeyByName(const std::string& user_name) const;

    // 辅助函数：缓存用户数据
    meeting::common::Status CachePut(const UserData& data) const;
    // 辅助函数：删除缓存中的用户数据
    meeting::common::Status CacheDelete(const UserData& data) const;
    // 辅助函数：从缓存中获取用户数据
    meeting::common::StatusOr<UserData> CacheGet(const std::string& key) const;

private:
    std::shared_ptr<UserRepository> primary_; // 主用户存储库
    std::shared_ptr<meeting::cache::RedisClient> redis_; // Redis客户端
    int ttl_seconds_; // 缓存过期时间（秒）

};

} // namespace core
} // namespace meeting