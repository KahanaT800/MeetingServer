#include <gtest/gtest.h>

#include "cache/redis_client.hpp"
#include "common/config.hpp"

#include <cstdlib>
#include <string_view>

TEST(RedisClientTest, BasicOps) {
    meeting::common::RedisConfig cfg;
    cfg.enabled = true;
    if (const char* host = std::getenv("REDIS_HOST")) cfg.host = host;
    if (const char* port = std::getenv("REDIS_PORT")) cfg.port = std::atoi(port);
    if (const char* pass = std::getenv("REDIS_PASSWORD")) cfg.password = pass;
    meeting::cache::RedisClient client(cfg);

    auto st = client.Set("test:key", "value");
    ASSERT_TRUE(st.IsOk()) << st.Message();

    auto get = client.Get("test:key");
    ASSERT_TRUE(get.IsOk()) << get.GetStatus().Message();
    EXPECT_EQ(get.Value(), "value");

    auto ex = client.Exists("test:key");
    ASSERT_TRUE(ex.IsOk()) << ex.GetStatus().Message();
    EXPECT_TRUE(ex.Value());

    auto del = client.Del("test:key");
    ASSERT_TRUE(del.IsOk()) << del.Message();
}