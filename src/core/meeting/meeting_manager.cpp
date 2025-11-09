#include "core/meeting/meeting_manager.hpp"

#include <algorithm>
#include <mutex>
#include <random>
#include <sstream>

namespace meeting {
namespace core {

namespace {

std::int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string RandomAlphanumericString(std::size_t length) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char kChars[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::uniform_int_distribution<int> dist(0, sizeof(kChars) - 2);
    std::string result;
    result.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        result.push_back(kChars[dist(rng)]);
    }
    return result;
}

} // namespace

MeetingManager::MeetingManager(MeetingConfig config)
    : config_(std::move(config)) {}

MeetingManager::StatusOrMeeting MeetingManager::CreateMeeting(const CreateMeetingCommand& command) {
    if (command.organizer_id.empty()) {
        return Status::InvalidArgument("Organizer ID cannot be empty.");
    }

    if (command.topic.empty()) {
        return Status::InvalidArgument("Meeting topic cannot be empty.");
    }

    MeetingData meeting;
    meeting.meeting_id = GenerateMeetingID();
    meeting.meeting_code = GenerateMeetingCode();
    meeting.organizer_id = command.organizer_id;
    meeting.topic = command.topic;
    meeting.state = MeetingState::kScheduled;
    meeting.created_at = CurrentUnixSeconds();
    meeting.updated_at = meeting.created_at;
    meeting.participants.push_back(command.organizer_id);

    {
        // 确保 meeting_id 和 meeting_code 唯一
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (code_index_.count(meeting.meeting_code) != 0) {
            return Status::Internal("Meeting code collision");
        }
        code_index_[meeting.meeting_code] = meeting.meeting_id; // 建立索引
        meetings_.emplace(meeting.meeting_id, meeting); // 存储会议数据
    }

    return StatusOrMeeting(std::move(meeting));
}

MeetingManager::StatusOrMeeting MeetingManager::JoinMeeting(const JoinMeetingCommand& command) {
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.participant_id.empty()) {
        return Status::InvalidArgument("Participant ID cannot be empty.");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(command.meeting_id); // 查找会议
    if (it == meetings_.end()) {
        return Status::NotFound("Meeting not found.");
    }

    MeetingData& meeting = it->second;
    if (meeting.state == MeetingState::kEnded) {
        return Status::InvalidArgument("Cannot join a meeting that has ended.");
    }

    if (std::find(meeting.participants.begin(), meeting.participants.end(), command.participant_id) != meeting.participants.end()) {
        return Status::AlreadyExists("Participant already in the meeting.");
    }

    if (meeting.participants.size() >= config_.max_participants) {
        return Status::Unavailable("Meeting has reached maximum participant limit.");
    }

    // 添加参与者
    meeting.participants.push_back(command.participant_id);
    if (meeting.state == MeetingState::kScheduled && command.participant_id != meeting.organizer_id) {
        meeting.state = MeetingState::kRunning;
    }
    // 更新会议的更新时间戳
    Touch(meeting);

    return StatusOrMeeting(meeting);
}

MeetingManager::Status MeetingManager::LeaveMeeting(const LeaveMeetingCommand& command) {
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.participant_id.empty()) {
        return Status::InvalidArgument("Participant ID cannot be empty.");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(command.meeting_id);
    if (it == meetings_.end()) {
        return Status::NotFound("Meeting not found.");
    }

    MeetingData& meeting = it->second;
    auto exists = std::find(meeting.participants.begin(), meeting.participants.end()
                            , command.participant_id);
    if (exists == meeting.participants.end()) {
        return Status::AlreadyExists("Participant not found in the meeting.");
    }
    if (meeting.state == MeetingState::kEnded) {
        return Status::InvalidArgument("Cannot leave a meeting that has ended.");
    }
    if (meeting.participants.size() >= config_.max_participants) {
        return Status::Unavailable("Meeting has reached maximum participant limit.");
    }

    // 移除参与者
    meeting.participants.erase(exists);

    if (config_.end_when_organizer_leaves && command.participant_id == meeting.organizer_id) {
        meeting.state = MeetingState::kEnded;
    } else if (config_.end_when_empty && meeting.participants.empty()) {
        meeting.state = MeetingState::kEnded;
    }

    // 更新会议的更新时间戳
    Touch(meeting);

    return Status::OK();
}

MeetingManager::Status MeetingManager::EndMeeting(const EndMeetingCommand& command) {
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.requester_id.empty()) {
        return Status::InvalidArgument("Requester ID cannot be empty.");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(command.meeting_id);
    if (it == meetings_.end()) {
        return Status::NotFound("Meeting not found.");
    }

    MeetingData& meeting = it->second;
    if (meeting.state == MeetingState::kEnded) {
        return Status::InvalidArgument("Meeting has already ended.");
    }
    if (command.requester_id != meeting.organizer_id) {
        return Status::Unauthenticated("Only the organizer can end the meeting.");
    }

    meeting.state = MeetingState::kEnded;
    // 更新会议的更新时间戳
    Touch(meeting);

    return Status::OK();
}

MeetingManager::StatusOrMeeting MeetingManager::GetMeeting(const std::string& meeting_id) {
    if (meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = meetings_.find(meeting_id);
    if (it == meetings_.end()) {
        return Status::NotFound("Meeting not found.");
    }
    return StatusOrMeeting(it->second);
}

std::string MeetingManager::GenerateMeetingID() {
    return "meeting_-" + RandomAlphanumericString(16);
}

std::string MeetingManager::GenerateMeetingCode() {
    return RandomAlphanumericString(config_.meeting_code_length);
}

void MeetingManager::Touch(MeetingData& meeting) {
    meeting.updated_at = CurrentUnixSeconds();
}

} // namespace core
} // namespace meeting