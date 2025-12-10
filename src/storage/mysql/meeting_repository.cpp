#include "storage/mysql/meeting_repository.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>

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

// 将 MySQL 错误码映射到 Status
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

std::uint64_t ParseUInt64(const char* field) {
    if (!field) return 0;
    return static_cast<std::uint64_t>(std::strtoull(field, nullptr, 10));
}

std::int64_t ParseInt64(const char* field) {
    if (!field) return 0;
    return std::strtoll(field, nullptr, 10);
}
} // namespace

// 构造函数
MySqlMeetingRepository::MySqlMeetingRepository(std::shared_ptr<ConnectionPool> pool)
    : pool_(std::move(pool)) {}

// 转义并加引号字符串值
std::string MySqlMeetingRepository::EscapeAndQuote(MYSQL* conn, const std::string& value) {
    return fmt::format("'{}'", Escape(conn, value));
}

// 创建新会议
meeting::common::StatusOr<meeting::core::MeetingData> MySqlMeetingRepository::CreateMeeting(const meeting::core::MeetingData& data) {
    // 开始事务
    Transaction transaction(pool_); // 创建事务对象
    auto status = transaction.Begin();
    if (!status.IsOk()) {
        return status;
    }

    MYSQL* conn = transaction.Raw(); // 获取原始连接指针

    // 插入会议数据
    auto created_at = std::max<std::int64_t>(data.created_at, 1);
    auto updated_at = std::max<std::int64_t>(data.updated_at, created_at);
    auto sql_meeting = fmt::format(
        "INSERT INTO meetings (meeting_id, meeting_code, organizer_id, topic, state, created_at, updated_at) "
        "VALUES ({}, {}, {}, {}, {}, FROM_UNIXTIME({}), FROM_UNIXTIME({}))",
        EscapeAndQuote(conn, data.meeting_id),
        EscapeAndQuote(conn, data.meeting_code),
        data.organizer_id,
        EscapeAndQuote(conn, data.topic),
        static_cast<int>(data.state),
        created_at,
        updated_at);
    // 执行SQL语句
    if (mysql_real_query(conn, sql_meeting.c_str(), sql_meeting.size()) != 0) {
        // 执行失败，回滚事务并返回错误
        transaction.Rollback();
        return MapMySqlError(conn);
    }

    // 插入组织者作为参与者
    auto sql_participant = fmt::format(
        "INSERT INTO meeting_participants (meeting_id, user_id, role, joined_at) "
        "VALUES ((SELECT id FROM meetings WHERE meeting_id = {}), {}, 1, NOW())",
        EscapeAndQuote(conn, data.meeting_id),
        data.organizer_id);
    if (mysql_real_query(conn, sql_participant.c_str(), sql_participant.size()) != 0) {
        // 执行失败，回滚事务并返回错误
        transaction.Rollback();
        return MapMySqlError(conn);
    }

    // 提交事务
    status = transaction.Commit();
    if (!status.IsOk()) {
        return status;
    }

    return meeting::common::StatusOr<meeting::core::MeetingData>(data);
}

// 加载会议数据
meeting::common::StatusOr<meeting::core::MeetingData> MySqlMeetingRepository::LoadMeeting(MYSQL* conn, const std::string& meeting_id) const {
    // 查询会议数据
    auto sql = fmt::format(
        "SELECT meeting_id, meeting_code, organizer_id, topic, state, "
        "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
        "FROM meetings WHERE meeting_id = {} LIMIT 1",
        EscapeAndQuote(conn, meeting_id));
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }

    // 获取查询结果
    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) {
        return meeting::common::Status::NotFound("meeting not found");
    }

    // 确保结果集释放
    auto cleanup = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(result, mysql_free_result);
    MYSQL_ROW row = mysql_fetch_row(result); // 获取第一行数据
    if (!row) {
        return meeting::common::Status::NotFound("meeting not found");
    }

    // 解析会议数据
    meeting::core::MeetingData data;
    data.meeting_id = row[0] ? row[0] : "";
    data.meeting_code = row[1] ? row[1] : "";
    data.organizer_id = ParseUInt64(row[2]);
    data.topic = row[3] ? row[3] : "";
    data.state = static_cast<meeting::core::MeetingState>(row[4] ? std::atoi(row[4]) : 0);
    data.created_at = ParseInt64(row[5]);
    data.updated_at = ParseInt64(row[6]);

    // 查询参与者列表
    auto participants_sql = fmt::format(
        "SELECT user_id FROM meeting_participants WHERE meeting_id = (SELECT id FROM meetings WHERE meeting_id = {})",
        EscapeAndQuote(conn, meeting_id));
    if (mysql_real_query(conn, participants_sql.c_str(), participants_sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    MYSQL_RES* pres = mysql_store_result(conn); // 获取查询结果
    if (pres) {
        // 确保结果集释放
        auto cleanup_p = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(pres, mysql_free_result);
        MYSQL_ROW prow;
        while ((prow = mysql_fetch_row(pres)) != nullptr) {
            data.participants.push_back(ParseUInt64(prow[0]));
        }
    }
    return meeting::common::StatusOr<meeting::core::MeetingData>(data);
}

// 根据会议ID查找会议
meeting::common::StatusOr<meeting::core::MeetingData> MySqlMeetingRepository::GetMeeting(const std::string& meeting_id) const {
    // 获取连接租赁对象
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());

    return LoadMeeting(lease.Raw(), meeting_id);
}

// 更新会议信息
meeting::common::Status MySqlMeetingRepository::UpdateMeetingState(const std::string& meeting_id, meeting::core::MeetingState state, std::int64_t updated_at) {
    // 获取连接租赁对象
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();

    // 更新会议状态
    auto sql = fmt::format(
        "UPDATE meetings SET state = {}, updated_at = FROM_UNIXTIME({}) WHERE meeting_id = {}",
        static_cast<int>(state),
        std::max<std::int64_t>(updated_at, 1),
        EscapeAndQuote(conn, meeting_id));
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    if (mysql_affected_rows(conn) == 0) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    return meeting::common::Status::OK();
}

// 添加会议参与者
meeting::common::Status MySqlMeetingRepository::AddParticipant(const std::string& meeting_id, std::uint64_t participant_id, bool is_organizer) {
    // 获取连接租赁对象
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    
    auto sql = fmt::format(
        "INSERT INTO meeting_participants (meeting_id, user_id, role, joined_at) "
        "VALUES ((SELECT id FROM meetings WHERE meeting_id = {}), {}, {}, NOW())",
        EscapeAndQuote(conn, meeting_id),
        participant_id,
        is_organizer ? 1 : 0);
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    return meeting::common::Status::OK();
}

// 移除会议参与者
meeting::common::Status MySqlMeetingRepository::RemoveParticipant(const std::string& meeting_id, std::uint64_t participant_id) {
    // 获取连接租赁对象
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();

    auto sql = fmt::format(
        "DELETE FROM meeting_participants WHERE meeting_id = (SELECT id FROM meetings WHERE meeting_id = {}) AND user_id = {}",
        EscapeAndQuote(conn, meeting_id),
        participant_id);
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return meeting::common::Status::Internal(mysql_error(conn));
    }
    if (mysql_affected_rows(conn) == 0) {
        return meeting::common::Status::NotFound("participant not found");
    }
    return meeting::common::Status::OK();
}

// 列出会议参与者
meeting::common::StatusOr<std::vector<std::uint64_t>> MySqlMeetingRepository::ListParticipants(const std::string& meeting_id) const {
    // 获取连接租赁对象
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    
    auto sql = fmt::format(
        "SELECT user_id FROM meeting_participants WHERE meeting_id = (SELECT id FROM meetings WHERE meeting_id = {})",
        EscapeAndQuote(conn, meeting_id));
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return meeting::common::Status::Internal(mysql_error(conn));
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        return meeting::common::Status::OK();
    }
    auto cleanup = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(res, mysql_free_result);
    std::vector<std::uint64_t> users;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        users.push_back(ParseUInt64(row[0]));
    }
    return meeting::common::StatusOr<std::vector<std::uint64_t>>(users);
}

} // namespace storage
} // namespace meeting
