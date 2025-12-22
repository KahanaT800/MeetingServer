#include <gtest/gtest.h>

#include "cache/redis_client.hpp"
#include "core/user/cached_session_repository.hpp"
#include "core/user/session_repository.hpp"
#include "common/config.hpp"

#include <chrono>
#include <cstdlib>
#include <random>
#include <string>

using meeting::cache::RedisClient;
using meeting::common::RedisConfig;
using meeting::core::CachedSessionRepository;
using meeting::core::InMemorySessionRepository;
using meeting::core::SessionRecord;
using meeting::core::SessionRepository;

namespace {

std::string RandomToken() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char kChars[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<int> dist(0, sizeof(kChars) - 2);
    std::string out;
    out.reserve(24);
    for (int i = 0; i < 24; ++i) out.push_back(kChars[dist(rng)]);
    return out;
}

RedisConfig LoadRedisConfig() {
    RedisConfig cfg;
    cfg.enabled = true;
    if (const char* host = std::getenv("REDIS_HOST")) cfg.host = host;
    if (const char* port = std::getenv("REDIS_PORT")) cfg.port = std::atoi(port);
    if (const char* pass = std::getenv("REDIS_PASSWORD")) cfg.password = pass;
    return cfg;
}

std::shared_ptr<RedisClient> RequireRedis() {
    auto cfg = LoadRedisConfig();
    auto client = std::make_shared<RedisClient>(cfg);
    auto st = client->Connect();
    if (!st.IsOk()) {
        ADD_FAILURE() << "Redis unavailable: " << st.Message();
        return {};
    }
    return client;
}

} // namespace

TEST(RedisFlowTest, SessionCacheEndToEnd) {
    auto redis = RequireRedis();
    auto primary = std::make_shared<InMemorySessionRepository>();
    CachedSessionRepository repo(primary, redis);

    SessionRecord rec;
    rec.token = "test:" + RandomToken();
    rec.user_id = 123;
    rec.user_uuid = "user_redis_flow";
    rec.expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count() +
                     300;

    // Create -> should populate cache
    auto st = repo.CreateSession(rec);
    ASSERT_TRUE(st.IsOk()) << st.Message();
    auto exists = redis->Exists("meeting:session:" + rec.token);
    ASSERT_TRUE(exists.IsOk()) << exists.GetStatus().Message();
    EXPECT_TRUE(exists.Value());

    // Delete cache to force miss, then Validate should hit primary and backfill
    redis->Del("meeting:session:" + rec.token);
    auto validated = repo.ValidateSession(rec.token);
    ASSERT_TRUE(validated.IsOk()) << validated.GetStatus().Message();
    EXPECT_EQ(validated.Value().user_id, rec.user_id);
    auto exists_after = redis->Exists("meeting:session:" + rec.token);
    ASSERT_TRUE(exists_after.IsOk()) << exists_after.GetStatus().Message();
    EXPECT_TRUE(exists_after.Value());

    // Delete session -> cache should be removed
    auto del = repo.DeleteSession(rec.token);
    ASSERT_TRUE(del.IsOk()) << del.Message();
    auto exists_final = redis->Exists("meeting:session:" + rec.token);
    ASSERT_TRUE(exists_final.IsOk()) << exists_final.GetStatus().Message();
    EXPECT_FALSE(exists_final.Value());
}