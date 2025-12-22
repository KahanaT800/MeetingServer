#include "core/user/cached_user_repository.hpp"

#include "common/logger.hpp"

#include <nlohmann/json.hpp>

namespace meeting {
namespace core {

namespace {

// 定义ID缓存键的前缀
constexpr std::string_view kIdPrefix = "meeting:user:id:";
// 定义名称缓存键的前缀
constexpr std::string_view kNamePrefix = "meeting:user:name:";

}

CachedUserRepository::CachedUserRepository(std::shared_ptr<UserRepository> primary,
                                           std::shared_ptr<meeting::cache::RedisClient> redis,
                                           int ttl_seconds)
    : primary_(std::move(primary)),
      redis_(std::move(redis)),
      ttl_seconds_(ttl_seconds) {}

// 创建新用户 (写逻辑: 先写主存储库, 再写缓存)
meeting::common::Status CachedUserRepository::CreateUser(const UserData& data) {
    // 在主存储库中创建用户
    auto status = primary_->CreateUser(data);
    if (!status.IsOk() || !HasCache()) {
        return status;
    }
    // 创建成功后从主存储库读取最新数据（含自增ID），再写入缓存
    auto refreshed = primary_->FindByUserName(data.user_name);
    if (!refreshed.IsOk()) {
        return status;
    }

    // 将用户数据缓存到Redis
    auto cache_status = CachePut(refreshed.Value());
    if (!cache_status.IsOk()) {
        MEETING_LOG_WARN("[UserCache] put failed: {}", cache_status.Message());
    }
    return status;
}

// 根据用户名查找用户 (读逻辑: 先读缓存, 缓存未命中再读主存储库并回填缓存)
meeting::common::StatusOr<UserData> CachedUserRepository::FindByUserName(const std::string& user_name) const {
    // 首先尝试从缓存中获取用户数据
    if (HasCache()) {
        // 尝试从缓存中获取用户数据
        auto cache_result = CacheGet(KeyByName(user_name));
        if (cache_result.IsOk()) {
            return cache_result;
        }
        // 仅在非未找到错误时记录警告日志
        if (cache_result.GetStatus().Code() != meeting::common::StatusCode::kNotFound) {
            MEETING_LOG_WARN("[UserCache] get by name failed: {}", cache_result.GetStatus().Message());
        }
    }

    // 从主存储库中获取用户数据
    auto db_result = primary_->FindByUserName(user_name);
    if (!db_result.IsOk() || !HasCache()) {
        return db_result;
    }

    // 将用户数据缓存到Redis
    auto cache_status = CachePut(db_result.Value());
    if (!cache_status.IsOk()) {
        MEETING_LOG_WARN("[UserCache] put failed: {}", cache_status.Message());
    }
    return db_result;
}

// 根据用户ID查找用户 (读逻辑: 先读缓存, 缓存未命中再读主存储库并回填缓存)
meeting::common::StatusOr<UserData> CachedUserRepository::FindById(const std::string& user_id) const {
    if (HasCache()) {
        // 尝试从缓存中获取用户数据
        auto cache_result = CacheGet(KeyById(user_id));
        if (cache_result.IsOk()) {
            return cache_result;
        }
        // 仅在非未找到错误时记录警告日志
        if (cache_result.GetStatus().Code() != meeting::common::StatusCode::kNotFound) {
            MEETING_LOG_WARN("[UserCache] get by id failed: {}", cache_result.GetStatus().Message());
        }   
    }

    // 从主存储库中获取用户数据
    auto db_result = primary_->FindById(user_id);
    if (!db_result.IsOk() || !HasCache()) {
        return db_result;
    }
    // 将用户数据缓存到Redis
    auto cache_status = CachePut(db_result.Value());
    if (!cache_status.IsOk()) {
        MEETING_LOG_WARN("[UserCache] put failed: {}", cache_status.Message());
    }
    return db_result;
}

// 更新用户的最后登录时间 (写逻辑: 先写主存储库, 再删除缓存)
meeting::common::Status CachedUserRepository::UpdateLastLogin(const std::string& user_id, std::int64_t last_login) {
    // 首先在主存储库中更新最后登录时间
    auto status = primary_->UpdateLastLogin(user_id, last_login);
    if (!status.IsOk() || !HasCache()) {
        return status;
    }

    // 获取最新的用户数据以更新缓存
    // 如果无法获取最新数据，则删除缓存
    auto latest = primary_->FindById(user_id);
    if (!latest.IsOk()) {
        // 如果无法获取最新数据，则删除缓存
        UserData dummy;
        dummy.user_id = user_id;
        dummy.user_name = "";
        CacheDelete(dummy);
        return status;
    }

    // 更新缓存中的用户数据
    auto cache_status = CachePut(latest.Value());
    if (!cache_status.IsOk()) {
        MEETING_LOG_WARN("[UserCache] put failed: {}", cache_status.Message());
    }
    return status;
}

// 生成基于用户ID的缓存键
std::string CachedUserRepository::KeyById(const std::string& user_id) const {
    return std::string(kIdPrefix).append(user_id);
}
// 生成基于用户名的缓存键
std::string CachedUserRepository::KeyByName(const std::string& user_name) const {
    return std::string(kNamePrefix).append(user_name);
}

// 将用户数据缓存到Redis
meeting::common::Status CachedUserRepository::CachePut(const UserData& data) const {
    // 使用nlohmann::json构建JSON对象
    nlohmann::json j {
        {"user_id", data.user_id},
        {"numeric_id", data.numeric_id},
        {"user_name", data.user_name},
        {"display_name", data.display_name},
        {"email", data.email},
        {"password_hash", data.password_hash},
        {"salt", data.salt},
        {"created_at", data.created_at},
        {"last_login", data.last_login}
    };
    // 将用户数据序列化为JSON字符串
    auto payload = j.dump();
    // 存储到Redis，设置过期时间
    auto status1 = redis_->SetEx(KeyById(data.user_id), payload, ttl_seconds_);
    if (!status1.IsOk()) {
        return status1;
    }
    auto status2 = redis_->SetEx(KeyByName(data.user_name), payload, ttl_seconds_);
    if (!status2.IsOk()) {
        return status2;
    }
    return meeting::common::Status::OK();
}

// 从缓存中获取用户数据
// key已经是完整的缓存键
meeting::common::StatusOr<UserData> CachedUserRepository::CacheGet(const std::string& key) const {
    // 从Redis获取数据
    auto get_result = redis_->Get(key);
    if (!get_result.IsOk()) {
        return get_result.GetStatus();
    }

    // 解析JSON字符串
    auto json = nlohmann::json::parse(get_result.Value(), nullptr, false);
    if (json.is_discarded()) {
        return meeting::common::Status::Unavailable("invalid cache payload");
    }

    UserData data;
    data.user_id = json.value("user_id", "");
    data.numeric_id = json.value("numeric_id", 0ULL);
    data.user_name = json.value("user_name", "");
    data.display_name = json.value("display_name", "");
    data.email = json.value("email", "");
    data.password_hash = json.value("password_hash", "");
    data.salt = json.value("salt", "");
    data.created_at = json.value("created_at", 0LL);
    data.last_login = json.value("last_login", 0LL);

    return meeting::common::StatusOr<UserData>(std::move(data));
}

// 从缓存中删除用户数据
meeting::common::Status CachedUserRepository::CacheDelete(const UserData& data) const {
    // 从Redis中删除基于ID和名称的缓存键
    auto status1 = redis_->Del(KeyById(data.user_id));
    if (!status1.IsOk() && status1.Code() != meeting::common::StatusCode::kNotFound) {
        return status1;
    }
    auto status2 = redis_->Del(KeyByName(data.user_name));
    if (!status2.IsOk() && status2.Code() != meeting::common::StatusCode::kNotFound) {
        return status2;
    }
    return meeting::common::Status::OK();
}

} // namespace core
} // namespace meeting
