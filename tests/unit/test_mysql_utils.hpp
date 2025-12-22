#pragma once

#include "common/config_loader.hpp"
#include "storage/mysql/connection_pool.hpp"

#include <gtest/gtest.h>
#include <mysql/mysql.h>

#include <chrono>
#include <memory>
#include <string>

namespace testutils {

inline std::shared_ptr<meeting::storage::ConnectionPool> CreatePoolFromConfig() {
    const auto cfg = meeting::common::ConfigLoader::LoadFromEnvOrDefault();
    if (!cfg.storage.mysql.enabled) {
        return nullptr;
    }
    meeting::storage::Options opts;
    opts.host = cfg.storage.mysql.host;
    opts.port = static_cast<std::uint16_t>(cfg.storage.mysql.port);
    opts.user = cfg.storage.mysql.user;
    opts.password = cfg.storage.mysql.password;
    opts.database = cfg.storage.mysql.database;
    opts.pool_size = static_cast<std::size_t>(cfg.storage.mysql.pool_size);
    opts.acquire_timeout = std::chrono::milliseconds(cfg.storage.mysql.connection_timeout_ms);
    opts.connect_timeout = std::chrono::milliseconds(cfg.storage.mysql.connection_timeout_ms);
    opts.read_timeout = std::chrono::milliseconds(cfg.storage.mysql.read_timeout_ms);
    opts.write_timeout = std::chrono::milliseconds(cfg.storage.mysql.write_timeout_ms);
    return std::make_shared<meeting::storage::ConnectionPool>(opts);
}

inline void ExecuteSql(meeting::storage::ConnectionPool& pool, const std::string& sql) {
    auto lease_or = pool.Acquire();
    ASSERT_TRUE(lease_or.IsOk()) << lease_or.GetStatus().Message();
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    ASSERT_EQ(0, mysql_real_query(conn, sql.c_str(), sql.size())) << mysql_error(conn);
}

// 清理测试表，确保每次用例运行前数据库干净
inline void ClearMysqlTestData() {
    auto pool = CreatePoolFromConfig();
    if (!pool) return;
    ExecuteSql(*pool, "DELETE FROM meeting_participants");
    ExecuteSql(*pool, "DELETE FROM meetings");
    ExecuteSql(*pool, "DELETE FROM user_sessions");
    ExecuteSql(*pool, "DELETE FROM users");
}

} // namespace testutils
