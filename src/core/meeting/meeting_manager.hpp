#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"

#include <chrono>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace meeting {
namespace core {

enum class MeetingState {
    kScheduled = 0,
    kRunning,
    kEnded,
};

struct MeetingData {
    std::string              meeting_id;    // 会议ID
    std::string              meeting_code;  // 会议码
    std::string              organizer_id;  // 组织者用户ID
    std::string              topic;         // 会议主题
    MeetingState             state;         // 会议状态
    std::vector<std::string> participants;  // 参与者用户ID列表
    std::int64_t             created_at;    // 会议创建时间
    std::int64_t             updated_at;    // 会议更新时间
};

struct MeetingConfig {
    std::size_t max_participants          = 100;   // 最大参与者数量
    bool        end_when_empty            = true;  // 当没有参与者时结束会议
    bool        end_when_organizer_leaves = true;  // 当组织者离开时结束会议
    std::size_t meeting_code_length       = 8;     // 会议码长度
};

struct CreateMeetingCommand {
    std::string organizer_id;  // 组织者用户ID
    std::string topic;         // 会议主题
};

struct JoinMeetingCommand {
    std::string meeting_id;    // 会议ID
    std::string participant_id; // 参与者用户ID
};

struct LeaveMeetingCommand {
    std::string meeting_id;     // 会议ID
    std::string participant_id; // 参与者用户ID
};

struct EndMeetingCommand {
    std::string meeting_id;    // 会议ID
    std::string requester_id;  // 请求者用户ID
};

class MeetingManager {
public:
    using Status = meeting::common::Status;
    using StatusOrMeeting = meeting::common::StatusOr<MeetingData>;

    explicit MeetingManager(MeetingConfig config = MeetingConfig{});

    StatusOrMeeting CreateMeeting(const CreateMeetingCommand& command);
    StatusOrMeeting JoinMeeting(const JoinMeetingCommand& command);
    Status LeaveMeeting(const LeaveMeetingCommand& command);
    Status EndMeeting(const EndMeetingCommand& command);

    StatusOrMeeting GetMeeting(const std::string& meeting_id);

private:
    std::string GenerateMeetingID();
    std::string GenerateMeetingCode();
    void Touch(MeetingData& meeting); // 更新会议的更新时间戳
private:
    MeetingConfig config_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, MeetingData> meetings_; // meeting_id -> MeetingData
    std::unordered_map<std::string, std::string> code_index_; // meeting_code -> meeting_id
};

}
}