#include "storage/mysql/user_repository.hpp"

#include <fmt/format.h>

namespace meeting {
namespace storage {

namespace {
// 将 MySQL 错误码映射到 Status
meeting::common::Status MapMySqlError(MYSQL* conn) {
    unsigned int err = mysql_errno(conn);
    if (err == 1062) { // Duplicate entry
        return meeting::common::Status::AlreadyExists("Duplicate entry.");
    }
    return meeting::common::Status::Internal(mysql_error(conn));
}

// 解析字符串字段为 int64_t
std::int64_t ParseInt64(const char* field) {
    if (!field) {
        return 0;
    }
    return std::strtoll(field, nullptr, 10);
}

// 转义并加引号字符串值
std::string Escape(MYSQL* conn, const std::string& value) {
    if (!conn) {
        return value;
    }

    std::string buf;
    buf.resize(value.size() * 2 + 1);
    unsigned long escaped_len = mysql_real_escape_string(conn, buf.data(), value.data(), value.size());
    buf.resize(escaped_len);
    return buf;
}
} // namespace

MySQLUserRepository::MySQLUserRepository(std::shared_ptr<ConnectionPool> pool)
    : pool_(std::move(pool)) {}

std::string MySQLUserRepository::EscapeAndQuote(MYSQL* conn, const std::string& value) {
    return fmt::format("'{}'", Escape(conn, value));
}

// 创建新用户
meeting::common::Status MySQLUserRepository::CreateUser(const meeting::core::UserData& data) {
    // 获取连接租赁
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }

    // 获取连接对象
    auto lease = std::move(lease_or.Value());
    // 获取原始连接指针
    MYSQL* conn = lease.Raw();
    // 构造插入 SQL 语句
    auto sql = fmt::format(
        "INSERT INTO users (user_uuid, username, display_name, email, password_hash, salt, status) "
        "VALUES ({}, {}, {}, {}, {}, {}, 1)",
        EscapeAndQuote(conn, data.user_id),
        EscapeAndQuote(conn, data.user_name),
        EscapeAndQuote(conn, data.display_name.empty() ? data.user_name : data.display_name),
        EscapeAndQuote(conn, data.email),
        EscapeAndQuote(conn, data.password_hash),
        EscapeAndQuote(conn, data.salt)
    );
    // 执行插入操作
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    return meeting::common::Status::OK();
}

meeting::common::StatusOr<meeting::core::UserData> MySQLUserRepository::QuerySingle(ConnectionPool::Lease& lease, const std::string& sql) const {
    MYSQL* conn = lease.Raw();
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    // 获取查询结果
    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) {
        return MapMySqlError(conn);  // 获取结果集失败
    }
    // 使用智能指针自动释放结果集
    auto cleanup = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(result, &mysql_free_result);
    // 获取单行结果
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) {
        return meeting::common::Status::NotFound("User not found.");
    }
    // 构造用户数据对象
    meeting::core::UserData data;
    data.user_id = row[0] ? row[0] : "";
    data.user_name = row[1] ? row[1] : "";
    data.display_name = row[2] ? row[2] : data.user_name;
    data.email = row[3] ? row[3] : "";
    data.password_hash = row[4] ? row[4] : "";
    data.salt = row[5] ? row[5] : "";
    data.created_at = ParseInt64(row[6]);
    data.last_login = ParseInt64(row[7]);
    return meeting::common::StatusOr<meeting::core::UserData>(data);
}

// 根据用户名查找用户
meeting::common::StatusOr<meeting::core::UserData> MySQLUserRepository::FindByUserName(const std::string& user_name) const {
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    auto sql = fmt::format(
        "SELECT user_uuid, username, display_name, email, password_hash, salt, "
        "UNIX_TIMESTAMP(created_at), IFNULL(UNIX_TIMESTAMP(last_login_at), 0) "
        "FROM users WHERE username = {} LIMIT 1",
        EscapeAndQuote(conn, user_name)
    );
    return QuerySingle(lease, sql);
}

// 根据用户ID查找用户
meeting::common::StatusOr<meeting::core::UserData> MySQLUserRepository::FindById(const std::string& id) const {
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    auto sql = fmt::format(
        "SELECT user_uuid, username, display_name, email, password_hash, salt, "
        "UNIX_TIMESTAMP(created_at), IFNULL(UNIX_TIMESTAMP(last_login_at), 0) "
        "FROM users WHERE user_uuid = {} LIMIT 1",
        EscapeAndQuote(conn, id)
    );
    return QuerySingle(lease, sql);
}

// 更新用户最后登录时间
meeting::common::Status MySQLUserRepository::UpdateLastLogin(const std::string& user_id, std::int64_t last_login) {
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }

    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    auto sql = fmt::format(
        "UPDATE users SET last_login_at = FROM_UNIXTIME({}) WHERE user_uuid = {}",
        last_login,
        EscapeAndQuote(conn, user_id)
    );
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    if (mysql_affected_rows(conn) == 0) {
        return meeting::common::Status::NotFound("User not found");
    }
    return meeting::common::Status::OK();
}

} // namespace storage
} // namespace meeting