#include "core/meeting/meeting_repository.hpp"

#include <algorithm>
#include <mutex>

namespace meeting {
namespace core {

// 创建新会议
meeting::common::StatusOr<MeetingData> InMemoryMeetingRepository::CreateMeeting(const MeetingData& data) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(data.meeting_id);
    if (it != meetings_.end()) {
        return meeting::common::Status::AlreadyExists("meeting already exists");
    }
    meetings_.emplace(data.meeting_id, data);
    return meeting::common::StatusOr<MeetingData>(data);
}

// 根据会议ID查找会议
meeting::common::StatusOr<MeetingData> InMemoryMeetingRepository::GetMeeting(const std::string& meeting_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(meeting_id);
    if (it == meetings_.end()) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    return meeting::common::StatusOr<MeetingData>(it->second);
}

// 更新会议信息
meeting::common::Status InMemoryMeetingRepository::UpdateMeetingState(const std::string& meeting_id, MeetingState state, std::int64_t updated_at) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(meeting_id);
    if (it == meetings_.end()) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    it->second.state = state;
    it->second.updated_at = updated_at;
    return meeting::common::Status::OK();
}

// 添加会议参与者
meeting::common::Status InMemoryMeetingRepository::AddParticipant(const std::string& meeting_id, std::uint64_t participant_id, bool /*is_organizer*/) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(meeting_id);
    if (it == meetings_.end()) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    auto& participants = it->second.participants;
    if (std::find(participants.begin(), participants.end(), participant_id) != participants.end()) {
        return meeting::common::Status::AlreadyExists("participant already in meeting");
    }
    participants.push_back(participant_id);
    return meeting::common::Status::OK();
}

// 移除会议参与者
meeting::common::Status InMemoryMeetingRepository::RemoveParticipant(const std::string& meeting_id, std::uint64_t participant_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(meeting_id);
    if (it == meetings_.end()) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    auto& participants = it->second.participants;
    auto pos = std::find(participants.begin(), participants.end(), participant_id);
    if (pos == participants.end()) {
        return meeting::common::Status::NotFound("participant not in meeting");
    }
    participants.erase(pos);
    return meeting::common::Status::OK();
}

// 列出会议参与者
meeting::common::StatusOr<std::vector<std::uint64_t>> InMemoryMeetingRepository::ListParticipants(const std::string& meeting_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(meeting_id);
    if (it == meetings_.end()) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    return meeting::common::StatusOr<std::vector<std::uint64_t>>(it->second.participants);
}


} // namespace core
} // namespace meeting
