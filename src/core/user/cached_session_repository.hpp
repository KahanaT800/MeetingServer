#pragma once

#include "cache/redis_client.hpp"
#include "common/status.hpp"
#include "common/status_or.hpp"
#include "core/user/session_repository.hpp"

#include <memory>
#include <string>

namespace meeting{
namespace core {

// 带 Redis 读写的 Session 仓库包装器
class CachedSessionRepository : public SessionRepository {
public:
    // 构造函数
    // primary: 主存储库实例
    // redis: Redis 客户端实例
    CachedSessionRepository(std::shared_ptr<SessionRepository> primary,
                            std::shared_ptr<meeting::cache::RedisClient> redis);
    // 重写基类方法
    // 创建会话
    meeting::common::Status CreateSession(const SessionRecord& record) override;
    // 验证会话
    meeting::common::StatusOr<SessionRecord> ValidateSession(const std::string& token) override;
    // 删除会话
    meeting::common::Status DeleteSession(const std::string& token) override;

private:
    // 辅助函数：检查是否启用了缓存
    bool HasCache() const { return static_cast<bool>(redis_); }
    // 辅助函数：生成基于会话令牌的缓存键
    std::string KeyForToken(const std::string& token) const;

    // 辅助函数：缓存会话数据
    meeting::common::Status CachePut(const SessionRecord& record);
    // 辅助函数：删除缓存中的会话数据
    meeting::common::Status CacheDelete(const std::string& token);
    // 辅助函数：从缓存中获取会话数据
    meeting::common::StatusOr<SessionRecord> CacheGet(const std::string& token);

private:
    std::shared_ptr<SessionRepository> primary_; // 主存储库
    std::shared_ptr<meeting::cache::RedisClient> redis_; // Redis 客户端
};

} // namespace core
} // namespace meeting