#include "core/meeting/cached_meeting_repository.hpp"
#include "common/logger.hpp"

#include <nlohmann/json.hpp>

namespace meeting {
namespace core {

namespace {
// 定义会议ID缓存键的前缀
constexpr std::string_view kIdPrefix = "meeting:info:";

}

// 构造函数
CachedMeetingRepository::CachedMeetingRepository(std::shared_ptr<MeetingRepository> primary,
                                                 std::shared_ptr<meeting::cache::RedisClient> redis,
                                                 int ttl_seconds)
    : primary_(std::move(primary)), redis_(std::move(redis)), ttl_seconds_(ttl_seconds) {}


// 创建新会议 (写逻辑: 先写主存储库, 再写缓存)
meeting::common::StatusOr<MeetingData> CachedMeetingRepository::CreateMeeting(const MeetingData& data) {
    // 先写主存储库
    auto status = primary_->CreateMeeting(data);
    if (!status.IsOk() || !HasCache()) {
        return status;
    }
    // 再写缓存
    auto cache_status = CachePut(status.Value());
    if (!cache_status.IsOk()) {
        MEETING_LOG_WARN("[MeetingCache] put after create failed: {}", cache_status.Message());
    }
    return status;
}

// 根据会议ID查找会议 (读逻辑: 先读缓存, 未命中再读主存储库)
meeting::common::StatusOr<MeetingData> CachedMeetingRepository::GetMeeting(const std::string& meeting_id) const {
    if (HasCache()) {
        auto cached = CacheGet(meeting_id);
        if (cached.IsOk()) {
            return cached;
        }
        // 仅在非未找到错误时记录日志
        if (cached.GetStatus().Code() != meeting::common::StatusCode::kNotFound) {
            MEETING_LOG_WARN("[MeetingCache] get failed: {}", cached.GetStatus().Message());
        }
    }

    // 读主存储库
    auto db_status = primary_->GetMeeting(meeting_id);
    if (!db_status.IsOk() || !HasCache()) {
        return db_status;
    }
    // 更新缓存
    auto cache_status = CachePut(db_status.Value());
    if (!cache_status.IsOk()) {
        MEETING_LOG_WARN("[MeetingCache] put after get failed: {}", cache_status.Message());
    }
    return db_status;
}

// 更新会议信息 (写逻辑: 先写主存储库, 再更新缓存)
meeting::common::Status CachedMeetingRepository::UpdateMeetingState(const std::string& meeting_id,
                                                                    MeetingState state,
                                                                    std::int64_t updated_at) {
    // 先写主存储库
    auto status = primary_->UpdateMeetingState(meeting_id, state, updated_at);
    if (!HasCache()) {
        return status;
    }
    // 删除缓存
    auto del = CacheDelete(meeting_id);
    if (!del.IsOk()) {
        MEETING_LOG_WARN("[MeetingCache] invalidate on update failed: {}", del.Message());
    }
    return status;
}

// 添加会议参与者 (写逻辑: 先写主存储库, 再更新缓存)
meeting::common::Status CachedMeetingRepository::AddParticipant(const std::string& meeting_id,
                                                                std::uint64_t participant_id,
                                                                bool is_organizer) {
    auto status = primary_->AddParticipant(meeting_id, participant_id, is_organizer);
    if (!HasCache()) {
        return status;
    }
    auto del = CacheDelete(meeting_id);
    if (!del.IsOk()) {
        MEETING_LOG_WARN("[MeetingCache] invalidate on add failed: {}", del.Message());
    }
    return status;
}

// 移除会议参与者 (写逻辑: 先写主存储库, 再更新缓存)
meeting::common::Status CachedMeetingRepository::RemoveParticipant(const std::string& meeting_id,
                                                                   std::uint64_t participant_id) {
    auto status = primary_->RemoveParticipant(meeting_id, participant_id);
    if (!HasCache()) {
        return status;
    }
    auto del = CacheDelete(meeting_id);
    if (!del.IsOk()) {
        MEETING_LOG_WARN("[MeetingCache] invalidate on remove failed: {}", del.Message());
    }
    return status;
}

// 列出会议参与者 (读逻辑: 直接读主存储库)
meeting::common::StatusOr<std::vector<std::uint64_t>> CachedMeetingRepository::ListParticipants(const std::string& meeting_id) const {
    // 暂不缓存列表，直接走主存储
    return primary_->ListParticipants(meeting_id);
}

// 生成 Redis 键
std::string CachedMeetingRepository::KeyForId(const std::string& meeting_id) const {
    return std::string(kIdPrefix).append(meeting_id);
}

// 缓存会议数据
meeting::common::Status CachedMeetingRepository::CachePut(const MeetingData& data) const {
    // 使用 nlohmann::json 序列化会议数据
    nlohmann::json j{
        {"meeting_id", data.meeting_id},
        {"meeting_code", data.meeting_code},
        {"organizer_id", data.organizer_id},
        {"topic", data.topic},
        {"state", static_cast<int>(data.state)},
        {"created_at", data.created_at},
        {"updated_at", data.updated_at},
        {"participants", data.participants},
    };
    auto payload = j.dump();

    // 存入 Redis
    auto status = redis_->SetEx(KeyForId(data.meeting_id), payload, ttl_seconds_);
    if (!status.IsOk()) {
        return status;
    }
    return meeting::common::Status::OK();
}

// 删除缓存中的会议数据
meeting::common::Status CachedMeetingRepository::CacheDelete(const std::string& meeting_id) const {
    auto status = redis_->Del(KeyForId(meeting_id));
    if (!status.IsOk() && status.Code() != meeting::common::StatusCode::kNotFound) {
        return status;
    }
    return meeting::common::Status::OK();
}

// 从缓存中获取会议数据
meeting::common::StatusOr<MeetingData> CachedMeetingRepository::CacheGet(const std::string& meeting_id) const {
    // 从 Redis 获取数据
    auto get_status = redis_->Get(KeyForId(meeting_id));
    if (!get_status.IsOk()) {
        return get_status.GetStatus();
    }

    // 解析 JSON 数据
    auto json = nlohmann::json::parse(get_status.Value(), nullptr, false);
    if (json.is_discarded()) {
        return meeting::common::Status::Unavailable("invalid cache payload");
    }

    // 构造 MeetingData 对象
    MeetingData data;
    data.meeting_id = json.value("meeting_id", "");
    data.meeting_code = json.value("meeting_code", "");
    data.organizer_id = json.value("organizer_id", 0ULL);
    data.topic = json.value("topic", "");
    data.state = static_cast<MeetingState>(json.value("state", 0));
    data.created_at = json.value("created_at", 0LL);
    data.updated_at = json.value("updated_at", 0LL);

    if (json.contains("participants") && json["participants"].is_array()) {
        data.participants = json["participants"].get<std::vector<std::uint64_t>>();
    }
    return meeting::common::StatusOr<MeetingData>(data);
}

} // namespace core
} // namespace meeting