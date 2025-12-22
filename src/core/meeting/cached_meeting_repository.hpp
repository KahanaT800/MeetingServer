#pragma once

#include "cache/redis_client.hpp"
#include "core/meeting/meeting_repository.hpp"

#include <memory>
#include <string>

namespace meeting {
namespace core {

// 带 Redis 缓存的会议仓库包装器
class CachedMeetingRepository : public MeetingRepository {
public:
    // 构造函数, 传入主会议仓库和 Redis 客户端
    CachedMeetingRepository(std::shared_ptr<MeetingRepository> primary,
                            std::shared_ptr<meeting::cache::RedisClient> redis,
                            int ttl_seconds = 300);
    
    // 重载基类方法
    // 创建新会议
    meeting::common::StatusOr<MeetingData> CreateMeeting(const MeetingData& data) override;
    // 根据会议ID查找会议
    meeting::common::StatusOr<MeetingData> GetMeeting(const std::string& meeting_id) const override;
    // 更新会议信息
    meeting::common::Status UpdateMeetingState(const std::string& meeting_id, MeetingState state, std::int64_t updated_at) override;
    // 添加会议参与者
    meeting::common::Status AddParticipant(const std::string& meeting_id, std::uint64_t participant_id, bool is_organizer) override;
    // 移除会议参与者
    meeting::common::Status RemoveParticipant(const std::string& meeting_id, std::uint64_t participant_id) override;
    // 列出会议参与者
    meeting::common::StatusOr<std::vector<std::uint64_t>> ListParticipants(const std::string& meeting_id) const override;

private:
    // 辅助函数: 确认缓存是否可用
    bool HasCache() const { return static_cast<bool>(redis_); }
    // 辅助函数: 生成 Redis 键
    std::string KeyForId(const std::string& meeting_id) const;

    // 辅助函数: 缓存会议数据
    meeting::common::Status CachePut(const MeetingData& data) const;
    // 辅助函数: 删除缓存中的会议数据
    meeting::common::Status CacheDelete(const std::string& meeting_id) const;
    // 辅助函数: 从缓存中获取会议数据
    meeting::common::StatusOr<MeetingData> CacheGet(const std::string& meeting_id) const;
private:
    std::shared_ptr<MeetingRepository> primary_; // 主会议仓库
    std::shared_ptr<meeting::cache::RedisClient> redis_; // Redis 客户端
    int ttl_seconds_; // 缓存过期时间（秒）
};

} // namespace core
} // namespace meeting