#include "storage/mysql/session_repository.hpp"

#include <fmt/format.h>

#include <chrono>

namespace meeting {
namespace storage {

namespace {

meeting::common::Status MapMySqlError(MYSQL* conn) {
    unsigned int err = mysql_errno(conn);
    if (err == 1062) { // duplicate
        return meeting::common::Status::AlreadyExists("Session already exists");
    }
    return meeting::common::Status::Internal(mysql_error(conn));
}

} // namespace

MySqlSessionRepository::MySqlSessionRepository(std::shared_ptr<ConnectionPool> pool)
    : pool_(std::move(pool)) {}

meeting::common::Status MySqlSessionRepository::CreateSession(const meeting::core::SessionRecord& record) {
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    auto sql = fmt::format(
        "INSERT INTO user_sessions (user_id, access_token, refresh_token, expires_at) "
        "VALUES ({}, '{}', '{}', FROM_UNIXTIME({}))",
        record.user_id,
        record.token,
        record.token,
        record.expires_at);
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    return meeting::common::Status::OK();
}

meeting::common::StatusOr<meeting::core::SessionRecord> MySqlSessionRepository::ValidateSession(const std::string& token) {
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    auto sql = fmt::format(
        "SELECT s.user_id, u.user_uuid, UNIX_TIMESTAMP(s.expires_at) "
        "FROM user_sessions s JOIN users u ON u.id = s.user_id "
        "WHERE s.access_token = '{}' LIMIT 1",
        token);
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        return meeting::common::Status::Unauthenticated("Session not found");
    }
    auto cleanup = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(res, mysql_free_result);
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        return meeting::common::Status::Unauthenticated("Session not found");
    }
    meeting::core::SessionRecord rec;
    rec.token = token;
    rec.user_id = static_cast<std::uint64_t>(std::strtoull(row[0], nullptr, 10));
    rec.user_uuid = row[1] ? row[1] : "";
    rec.expires_at = row[2] ? std::strtoll(row[2], nullptr, 10) : 0;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    if (rec.expires_at != 0 && rec.expires_at < now) {
        return meeting::common::Status::Unauthenticated("Session expired");
    }
    return meeting::common::StatusOr<meeting::core::SessionRecord>(rec);
}

meeting::common::Status MySqlSessionRepository::DeleteSession(const std::string& token) {
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    auto sql = fmt::format(
        "DELETE FROM user_sessions WHERE access_token = '{}'",
        token);
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    if (mysql_affected_rows(conn) == 0) {
        return meeting::common::Status::NotFound("Session not found");
    }
    return meeting::common::Status::OK();
}

} // namespace storage
} // namespace meeting
