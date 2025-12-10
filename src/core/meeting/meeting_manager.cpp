#include "core/meeting/meeting_manager.hpp"
#include "core/meeting/meeting_repository.hpp"

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

MeetingManager::MeetingManager(MeetingConfig config, std::shared_ptr<MeetingRepository> repository)
    : config_(std::move(config))
    , repository_(std::move(repository)) {
    if (!repository_) {
        repository_ = std::make_shared<InMemoryMeetingRepository>();
    }
}

MeetingManager::StatusOrMeeting MeetingManager::CreateMeeting(const CreateMeetingCommand& command) {
    if (command.organizer_id == 0) {
        return Status::InvalidArgument("Organizer ID cannot be empty.");
    }

    if (command.topic.empty()) {
        return Status::InvalidArgument("Meeting topic cannot be empty.");
    }

    // 构造会议数据
    MeetingData meeting;
    meeting.meeting_id = GenerateMeetingID();
    meeting.meeting_code = GenerateMeetingCode();
    meeting.organizer_id = command.organizer_id;
    meeting.topic = command.topic;
    meeting.state = MeetingState::kScheduled;
    meeting.created_at = CurrentUnixSeconds();
    meeting.updated_at = meeting.created_at;
    meeting.participants.push_back(command.organizer_id);

    // 存储会议数据
    auto status = repository_->CreateMeeting(meeting);
    if (!status.IsOk()) {
        return status.GetStatus();
    }

    // 添加组织者为参与者
    auto add_status = repository_->AddParticipant(meeting.meeting_id, meeting.organizer_id, true);
    if (!add_status.IsOk() &&  add_status.Code() != meeting::common::StatusCode::kAlreadyExists) {
        return add_status;
    }

    return StatusOrMeeting(std::move(meeting));
}

MeetingManager::StatusOrMeeting MeetingManager::JoinMeeting(const JoinMeetingCommand& command) {
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.participant_id == 0) {
        return Status::InvalidArgument("Participant ID cannot be empty.");
    }

    // 获取会议信息
    auto meeting_or = repository_->GetMeeting(command.meeting_id);
    if (!meeting_or.IsOk()) {
        return meeting_or.GetStatus();
    }
    auto meeting = meeting_or.Value();

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
    auto status = repository_->AddParticipant(command.meeting_id, command.participant_id, false);
    if (!status.IsOk()) {
        return status;
    }
    if (meeting.state == MeetingState::kScheduled && command.participant_id != meeting.organizer_id) {
        meeting.state = MeetingState::kRunning;
        repository_->UpdateMeetingState(meeting.meeting_id, meeting.state, CurrentUnixSeconds());
    }
    meeting.participants.push_back(command.participant_id);
    // 更新会议的更新时间戳
    Touch(meeting);

    return StatusOrMeeting(std::move(meeting));
}

MeetingManager::Status MeetingManager::LeaveMeeting(const LeaveMeetingCommand& command) {
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.participant_id == 0) {
        return Status::InvalidArgument("Participant ID cannot be empty.");
    }

    // 获取会议信息
    auto meeting_or = repository_->GetMeeting(command.meeting_id);
    if (!meeting_or.IsOk()) {
        return meeting_or.GetStatus();
    }
    auto meeting = meeting_or.Value();
    auto iter = std::find(meeting.participants.begin(), meeting.participants.end(), command.participant_id);
    if (iter == meeting.participants.end()) {
        return Status::AlreadyExists("Participant not found in the meeting.");
    }

    // 移除参与者
    auto rm_status = repository_->RemoveParticipant(command.meeting_id, command.participant_id);
    if (!rm_status.IsOk()) {
        return rm_status;
    }

    if (command.participant_id == meeting.organizer_id && config_.end_when_organizer_leaves) {
        // 组织者离开，结束会议
        meeting.state = MeetingState::kEnded;
        repository_->UpdateMeetingState(meeting.meeting_id, meeting.state, CurrentUnixSeconds());
    } else {
        // 非组织者离开，更新参与者列表
        auto list = repository_->ListParticipants(meeting.meeting_id);
        if (list.IsOk()) {
            meeting.participants = list.Value();
        }
        if (meeting.participants.empty() && config_.end_when_empty) {
            meeting.state = MeetingState::kEnded;
            repository_->UpdateMeetingState(meeting.meeting_id, meeting.state, CurrentUnixSeconds());
        }
    }
    return Status::OK();
}

MeetingManager::Status MeetingManager::EndMeeting(const EndMeetingCommand& command) {
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.requester_id == 0) {
        return Status::InvalidArgument("Requester ID cannot be empty.");
    }

    // 获取会议信息
    auto meeting_or = repository_->GetMeeting(command.meeting_id);
    if (!meeting_or.IsOk()) {
        return meeting_or.GetStatus();
    }
    auto meeting = meeting_or.Value();

    if (meeting.state == MeetingState::kEnded) {
        return Status::InvalidArgument("Meeting has already ended.");
    }
    if (command.requester_id != meeting.organizer_id) {
        return Status::Unauthenticated("Only the organizer can end the meeting.");
    }

    // 更新会议状态为已结束
    return repository_->UpdateMeetingState(command.meeting_id, MeetingState::kEnded, CurrentUnixSeconds());
}

MeetingManager::StatusOrMeeting MeetingManager::GetMeeting(const std::string& meeting_id) {
    if (meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }

    auto meeting = repository_->GetMeeting(meeting_id);
    if (!meeting.IsOk()) {
        return meeting.GetStatus();
    }
    return meeting;
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
