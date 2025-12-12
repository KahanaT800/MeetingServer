#pragma once

#include "core/meeting/meeting_repository.hpp"
#include "storage/mysql/connection_pool.hpp"
#include "storage/mysql/transaction.hpp"

#include <cstdint>
#include <memory>

namespace meeting {
namespace storage {

class MySqlMeetingRepository : public meeting::core::MeetingRepository {
public:
    explicit MySqlMeetingRepository(std::shared_ptr<ConnectionPool> pool);

    // 创建新会议
    meeting::common::StatusOr<meeting::core::MeetingData> CreateMeeting(const meeting::core::MeetingData& data) override;

    // 根据会议ID查找会议
    meeting::common::StatusOr<meeting::core::MeetingData> GetMeeting(const std::string& meeting_id) const override;

    // 更新会议信息
    meeting::common::Status UpdateMeetingState(const std::string& meeting_id, meeting::core::MeetingState state, std::int64_t updated_at) override;

    // 添加会议参与者
    meeting::common::Status AddParticipant(const std::string& meeting_id, std::uint64_t participant_id, bool is_organizer) override;

    // 移除会议参与者
    meeting::common::Status RemoveParticipant(const std::string& meeting_id, std::uint64_t participant_id) override;

    // 列出会议参与者
    meeting::common::StatusOr<std::vector<std::uint64_t>> ListParticipants(const std::string& meeting_id) const override;

private:
    // 转义并加引号字符串值
    static std::string EscapeAndQuote(MYSQL* conn, const std::string& value);
    // 加载会议数据
    meeting::common::StatusOr<meeting::core::MeetingData> LoadMeeting(MYSQL* conn, const std::string& meeting_id) const;
    
private:
    std::shared_ptr<ConnectionPool> pool_;
};    

} // namespace storage
} // namespace meeting
