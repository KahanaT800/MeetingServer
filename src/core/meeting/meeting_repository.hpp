#pragma once    

#include "common/status.hpp"
#include "common/status_or.hpp"
#include "core/meeting/meeting_manager.hpp"

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace meeting {
namespace core {

// 会议存储库接口
class MeetingRepository {
public:
    virtual ~MeetingRepository() = default;

    // 创建新会议
    virtual meeting::common::StatusOr<MeetingData> CreateMeeting(const MeetingData& data) = 0;

    // 根据会议ID查找会议
    virtual meeting::common::StatusOr<MeetingData> GetMeeting(const std::string& meeting_id) const = 0;

    // 更新会议信息
    virtual meeting::common::Status UpdateMeetingState(const std::string& meeting_id, MeetingState state, std::int64_t updated_at) = 0;

    // 添加会议参与者
    virtual meeting::common::Status AddParticipant(const std::string& meeting_id, std::uint64_t participant_id, bool is_organizer) = 0;

    // 移除会议参与者
    virtual meeting::common::Status RemoveParticipant(const std::string& meeting_id, std::uint64_t participant_id) = 0;

    // 列出会议参与者
    virtual meeting::common::StatusOr<std::vector<std::uint64_t>> ListParticipants(const std::string& meeting_id) const = 0;
};

class InMemoryMeetingRepository : public MeetingRepository {
public:
    // 创建新会议
    meeting::common::StatusOr<MeetingData> CreateMeeting(const MeetingData& data) override;

    // 根据会议ID查找会议
    meeting::common::StatusOr<MeetingData> GetMeeting(const std::string& meeting_id) const override;

    // 更新会议信息
    meeting::common::Status UpdateMeetingState(const std::string& meeting_id, MeetingState state, std::int64_t updated_at) override;

    // 添加会议参与者
    meeting::common::Status AddParticipant(const std::string& meeting_id, std::uint64_t participant_id, bool is_organizer) override;

    // 移除会议参与者
    meeting::common::Status RemoveParticipant(const std::string& meeting_id, std::uint64_t participant_id) override;

    // 列出会议参与者
    meeting::common::StatusOr<std::vector<std::uint64_t>> ListParticipants(const std::string& meeting_id) const override;

private:
    mutable std::shared_mutex mutex_; // 保护 meetings_ 的读写锁
    std::unordered_map<std::string, MeetingData> meetings_; // 会议ID 到 会议数据的映射
};




} // namespace core
} // namespace meeting
