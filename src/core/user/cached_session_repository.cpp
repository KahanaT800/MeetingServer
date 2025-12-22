#include "core/user/cached_session_repository.hpp"

#include "common/logger.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

namespace meeting{
namespace core {

namespace {
// 定义会话缓存键的前缀
constexpr std::string_view kPrefix = "meeting:session:";

// 获取当前时间的秒数，用于设置会话过期时间
std::int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

// 构造函数
CachedSessionRepository::CachedSessionRepository(std::shared_ptr<SessionRepository> primary,
                                                 std::shared_ptr<meeting::cache::RedisClient> redis)
    : primary_(std::move(primary)), redis_(std::move(redis)) {}

// 创建会话 (写逻辑: 先写主存储库, 再删除缓存)
meeting::common::Status CachedSessionRepository::CreateSession(const SessionRecord& record) {
    // 在主存储库中创建会话
    auto status = primary_->CreateSession(record);
    if (!status.IsOk() || !HasCache()) {
        return status;
    }

    // 将会话数据缓存到Redis
    auto cache_status = CachePut(record);
    if (!cache_status.IsOk()) {
        MEETING_LOG_WARN("[SessionCache] put failed: {}", cache_status.Message());
    }
    return status;
}

// 验证会话 (读逻辑: 先读缓存, 缓存未命中再读主存储库并回填缓存)
meeting::common::StatusOr<SessionRecord> CachedSessionRepository::ValidateSession(const std::string& token) {
    // 首先尝试从缓存中获取会话数据
    if (HasCache()) {
        auto cached = CacheGet(token);
        if (cached.IsOk()) {
            return cached;
        }
        // 仅在非未找到错误时记录警告日志
        if (cached.GetStatus().Code() != meeting::common::StatusCode::kNotFound) {
            MEETING_LOG_WARN("[SessionCache] get failed: {}", cached.GetStatus().Message());
        }
    }

    // 从主存储库中获取会话数据
    auto db_result = primary_->ValidateSession(token);
    if (!db_result.IsOk() || !HasCache()) {
        return db_result;
    }

    // 将会话数据缓存到Redis
    auto cache_status = CachePut(db_result.Value());
    if (!cache_status.IsOk()) {
        MEETING_LOG_WARN("[SessionCache] backfill failed: {}", cache_status.Message());
    }
    return db_result;
}
// 删除会话 (写逻辑: 先写主存储库, 再删除缓存)
meeting::common::Status CachedSessionRepository::DeleteSession(const std::string& token) {
    // 在主存储库中删除会话
    auto status = primary_->DeleteSession(token);
    if (!HasCache()) {
        return status;
    }

    // 从缓存中删除会话数据
    auto del_status = CacheDelete(token);
    if (!del_status.IsOk()) {
        MEETING_LOG_WARN("[SessionCache] delete failed: {}", del_status.Message());
    }
    return status;
}

// 生成基于会话令牌的缓存键
std::string CachedSessionRepository::KeyForToken(const std::string& token) const {
    return std::string(kPrefix).append(token);
}

// 缓存会话数据
meeting::common::Status CachedSessionRepository::CachePut(const SessionRecord& record) {
    // 计算会话的剩余存活时间 (TTL)
    std::int64_t now = NowSeconds();
    std::int64_t ttl = record.expires_at > 0 ? record.expires_at - now : 0;
    // 如果TTL小于等于0，表示会话已过期，无需缓存
    if (ttl <= 0) {
        return meeting::common::Status::OK();
    }

    // 将会话数据序列化为JSON格式并存储到Redis，设置过期时间
    nlohmann::json j{
        {"token", record.token},
        {"user_id", record.user_id},
        {"user_uuid", record.user_uuid},
        {"expires_at", record.expires_at},
    };
    auto status = redis_->SetEx(KeyForToken(record.token), j.dump(), static_cast<int>(ttl));
    if (!status.IsOk()) {
        return status;
    }
    return meeting::common::Status::OK();
}

// 从缓存中获取会话数据
meeting::common::StatusOr<SessionRecord> CachedSessionRepository::CacheGet(const std::string& token) {
    // 从Redis获取会话数据
    auto resp = redis_->Get(KeyForToken(token));
    if (!resp.IsOk()) {
        return resp.GetStatus();
    }
    // 解析JSON数据
    auto val = resp.Value();
    auto json = nlohmann::json::parse(val, nullptr, false);
    if (json.is_discarded()) {
        return meeting::common::Status::Unavailable("invalid cache payload");
    }

    // 构造SessionRecord对象
    SessionRecord rec;
    rec.token = json.value("token", token);
    rec.user_id = json.value("user_id", 0ULL);
    rec.user_uuid = json.value("user_uuid", "");
    rec.expires_at = json.value("expires_at", 0LL);

    if (rec.expires_at != 0 && rec.expires_at < NowSeconds()) {
        // 过期则删缓存
        CacheDelete(token);
        return meeting::common::Status::Unauthenticated("Session expired");
    }
    return meeting::common::StatusOr<SessionRecord>(rec);
}

// 删除缓存中的会话数据
meeting::common::Status CachedSessionRepository::CacheDelete(const std::string& token) {
    auto status = redis_->Del(KeyForToken(token));
    if (!status.IsOk() && status.Code() != meeting::common::StatusCode::kNotFound) {
        return status;
    }
    return meeting::common::Status::OK();
}

} // namespace core
} // namespace meeting