#include <gtest/gtest.h>

#include "storage/mysql/connection_pool.hpp"
#include "storage/mysql/user_repository.hpp"

#include <cstdlib>
#include <string_view>

namespace {

std::string GetEnvOr(const char* name, const std::string& fallback) {
    if (const char* value = std::getenv(name)) {
        return value;
    }
    return fallback;
}

void ExecuteSQL(meeting::storage::ConnectionPool& pool, const std::string& sql) {
    auto lease_or = pool.Acquire();
    ASSERT_TRUE(lease_or.IsOk()) << lease_or.GetStatus().Message();
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    ASSERT_EQ(0, mysql_real_query(conn, sql.c_str(), sql.size())) << mysql_error(conn);
}

class MysqlUserRepositoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        meeting::storage::Options options;
        options.host = GetEnvOr("MEETING_DB_HOST", "127.0.0.1");
        options.port = static_cast<std::uint16_t>(std::stoi(GetEnvOr("MEETING_DB_PORT", "3306")));
        options.user = GetEnvOr("MEETING_DB_USER", "root");
        options.password = GetEnvOr("MEETING_DB_PASSWORD", "");
        options.database = GetEnvOr("MEETING_DB_NAME", "meeting");
        pool_ = std::make_shared<meeting::storage::ConnectionPool>(options);
        auto test_conn = pool_->Acquire();
        ASSERT_TRUE(test_conn.IsOk()) << test_conn.GetStatus().Message();
        repo_ = std::make_unique<meeting::storage::MySQLUserRepository>(pool_);
        ExecuteSQL(*pool_, "DELETE FROM user_sessions");
        ExecuteSQL(*pool_, "DELETE FROM users");
    }

    void TearDown() override {
        if (pool_) {
            ExecuteSQL(*pool_, "DELETE FROM user_sessions");
            ExecuteSQL(*pool_, "DELETE FROM users");
        }
    }

    std::shared_ptr<meeting::storage::ConnectionPool> pool_;
    std::unique_ptr<meeting::storage::MySQLUserRepository> repo_;
};

meeting::core::UserData MakeUser(std::string name) {
    meeting::core::UserData data;
    data.user_id = "user_" + name;
    data.user_name = name;
    data.display_name = name;
    data.email = name + "@example.com";
    data.password_hash = "hash" + name;
    data.salt = "salt" + name;
    data.created_at = 0;
    data.last_login = 0;
    return data;
}

TEST_F(MysqlUserRepositoryTest, CreateAndFetch) {
    if (!repo_) {
        GTEST_SKIP();
    }
    auto data = MakeUser("alice");
    auto status = repo_->CreateUser(data);
    ASSERT_TRUE(status.IsOk()) << status.Message();

    /*
    // 暂停以便查看数据库
    std::cout << "\n=== 数据已插入，按 Enter 继续测试... ===" << std::endl;
    std::cin.get();
    */

    auto fetched = repo_->FindByUserName("alice");
    ASSERT_TRUE(fetched.IsOk()) << fetched.GetStatus().Message();
    EXPECT_EQ(fetched.Value().email, data.email);

    auto by_id = repo_->FindById(data.user_id);
    ASSERT_TRUE(by_id.IsOk());
    EXPECT_EQ(by_id.Value().user_name, data.user_name);

    auto update_status = repo_->UpdateLastLogin(data.user_id, 1234);
    ASSERT_TRUE(update_status.IsOk());
    auto after = repo_->FindById(data.user_id);
    ASSERT_TRUE(after.IsOk());
    EXPECT_EQ(after.Value().last_login, 1234);
}

TEST_F(MysqlUserRepositoryTest, DuplicateUser) {
    if (!repo_) {
        GTEST_SKIP();
    }
    auto data = MakeUser("bob");
    auto status = repo_->CreateUser(data);
    ASSERT_TRUE(status.IsOk());
    auto dup = repo_->CreateUser(data);
    ASSERT_FALSE(dup.IsOk());
    EXPECT_EQ(dup.Code(), meeting::common::StatusCode::kAlreadyExists);
}

}