#include "server/meeting_service_impl.hpp"
#include "config_path.hpp"
namespace meeting {
namespace server {

namespace {

thread_pool::ThreadPool CreateThreadPool() {
    // 初始化线程池日志器
    thread_pool::log::InitFromFile(meeting::common::GetLoggerConfigPath());
    // 加载线程池配置
    auto loader = thread_pool::ThreadPoolConfigLoader::FromFile(
        meeting::common::GetThreadPoolConfigPath());
    if (loader.has_value()) {
        return thread_pool::ThreadPool(loader->GetConfig());
    }
    // 配置文件加载失败，使用默认参数
    return thread_pool::ThreadPool(4, 1024);
}

// 状态码映射
meeting::core::MeetingErrorCode MapStatus(const meeting::common::Status& status) {
    using meeting::common::StatusCode;
    switch (status.Code()) {
        case StatusCode::kNotFound:
            return meeting::core::MeetingErrorCode::kMeetingNotFound;
        case StatusCode::kAlreadyExists:
            return meeting::core::MeetingErrorCode::kParticipantExists;
        case StatusCode::kInvalidArgument:
            return meeting::core::MeetingErrorCode::kInvalidState;
        case StatusCode::kUnauthenticated:
            return meeting::core::MeetingErrorCode::kPermissionDenied;
        case StatusCode::kUnavailable:
            return meeting::core::MeetingErrorCode::kMeetingFull;
        default:
            return meeting::core::MeetingErrorCode::kInvalidState;
    }
}

} // namespace

MeetingServiceImpl::MeetingServiceImpl()
    : meeting_manager_(std::make_unique<meeting::core::MeetingManager>())
    , thread_pool_(CreateThreadPool()) {
    thread_pool_.Start();
}

MeetingServiceImpl::~MeetingServiceImpl() {
    thread_pool_.Stop();
}

grpc::Status MeetingServiceImpl::CreateMeeting(grpc::ServerContext*
                                                , const proto::meeting::CreateMeetingRequest* request
                                                , proto::meeting::CreateMeetingResponse* response) {
    meeting::core::CreateMeetingCommand command{request->session_token(), request->topic()};
    auto create_future = thread_pool_.Submit([this, command]() {
        return meeting_manager_->CreateMeeting(command);
    });
    auto status_or_meeting = create_future.get();
    if (!status_or_meeting.IsOk()) {
        auto code = MapStatus(status_or_meeting.GetStatus());
        meeting::core::ErrorToProto(code, status_or_meeting.GetStatus(), response->mutable_error());
        return ToGrpcStatus(status_or_meeting.GetStatus());
    }

    FillMeetingInfo(status_or_meeting.Value(), response->mutable_meeting());
    meeting::core::ErrorToProto(meeting::core::MeetingErrorCode::kOk
                                , meeting::common::Status::OK()
                                , response->mutable_error());
    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::JoinMeeting(grpc::ServerContext*
                                              , const proto::meeting::JoinMeetingRequest* request
                                              , proto::meeting::JoinMeetingResponse* response) {
    meeting::core::JoinMeetingCommand command{request->meeting_id(), request->session_token()};
    auto join_future = thread_pool_.Submit([this, command]() {
        return meeting_manager_->JoinMeeting(command);
    });
    auto status_or_meeting = join_future.get();
    if (!status_or_meeting.IsOk()) {
        auto code = MapStatus(status_or_meeting.GetStatus());
        meeting::core::ErrorToProto(code, status_or_meeting.GetStatus(), response->mutable_error());
        return ToGrpcStatus(status_or_meeting.GetStatus());
    }

    FillMeetingInfo(status_or_meeting.Value(), response->mutable_meeting());
    meeting::core::ErrorToProto(meeting::core::MeetingErrorCode::kOk
                                , meeting::common::Status::OK()
                                , response->mutable_error());
    auto* endpoint = response->mutable_endpoint();
    endpoint->set_ip("0.0.0.0");
    endpoint->set_port(0);
    endpoint->set_region("default");
    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::LeaveMeeting(grpc::ServerContext*
                                              , const proto::meeting::LeaveMeetingRequest* request
                                              , proto::meeting::LeaveMeetingResponse* response) {
    meeting::core::LeaveMeetingCommand command{request->meeting_id(), request->session_token()};
    auto leave_future = thread_pool_.Submit([this, command]() {
        return meeting_manager_->LeaveMeeting(command);
    });
    auto status = leave_future.get();
    if (!status.IsOk()) {
        auto code = MapStatus(status);
        meeting::core::ErrorToProto(code, status, response->mutable_error());
        return ToGrpcStatus(status);
    }
    meeting::core::ErrorToProto(meeting::core::MeetingErrorCode::kOk
                                , meeting::common::Status::OK()
                                , response->mutable_error());
    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::EndMeeting(grpc::ServerContext*
                                             , const proto::meeting::EndMeetingRequest* request
                                             , proto::meeting::EndMeetingResponse* response) {
    meeting::core::EndMeetingCommand command{request->meeting_id(), request->session_token()};
    auto end_future = thread_pool_.Submit([this, command]() {
        return meeting_manager_->EndMeeting(command);
    });
    auto status = end_future.get();
    if (!status.IsOk()) {
        auto code = MapStatus(status);
        meeting::core::ErrorToProto(code, status, response->mutable_error());
        return ToGrpcStatus(status);
    }
    meeting::core::ErrorToProto(meeting::core::MeetingErrorCode::kOk
                                , meeting::common::Status::OK()
                                , response->mutable_error());
    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::GetMeeting(grpc::ServerContext*
                                             , const proto::meeting::GetMeetingRequest* request
                                             , proto::meeting::GetMeetingResponse* response) {
    auto get_future = thread_pool_.Submit([this, id = request->meeting_id()]() {
        return meeting_manager_->GetMeeting(id);
    });
    auto status_or_meeting = get_future.get();
    if (!status_or_meeting.IsOk()) {
        auto code = MapStatus(status_or_meeting.GetStatus());
        meeting::core::ErrorToProto(code, status_or_meeting.GetStatus(), response->mutable_error());
        return ToGrpcStatus(status_or_meeting.GetStatus());
    }
    FillMeetingInfo(status_or_meeting.Value(), response->mutable_meeting());
    meeting::core::ErrorToProto(meeting::core::MeetingErrorCode::kOk
                                , meeting::common::Status::OK()
                                , response->mutable_error());
    return grpc::Status::OK;
}

// 转换为 gRPC 状态码
grpc::Status MeetingServiceImpl::ToGrpcStatus(const meeting::common::Status& status) {
    using meeting::common::StatusCode;
    switch (status.Code()) {
        case StatusCode::kOk:
            return grpc::Status::OK;
        case StatusCode::kInvalidArgument:
            return {grpc::StatusCode::INVALID_ARGUMENT, status.Message()};
        case StatusCode::kNotFound:
            return {grpc::StatusCode::NOT_FOUND, status.Message()};
        case StatusCode::kAlreadyExists:
            return {grpc::StatusCode::ALREADY_EXISTS, status.Message()};
        case StatusCode::kUnauthenticated:
            return {grpc::StatusCode::PERMISSION_DENIED, status.Message()};
        case StatusCode::kUnavailable:
            return {grpc::StatusCode::UNAVAILABLE, status.Message()};
        default:
            return {grpc::StatusCode::UNKNOWN, status.Message()};
    }
}

// 会议状态枚举转换为字符串
std::string MeetingServiceImpl::StateToString(meeting::core::MeetingState state) {
    switch (state) {
        case meeting::core::MeetingState::kScheduled:
            return "SCHEDULED";
        case meeting::core::MeetingState::kRunning:
            return "RUNNING";
        case meeting::core::MeetingState::kEnded:
            return "ENDED";
    }
    return "UNKNOWN";
}

// 填写会议信息
void MeetingServiceImpl::FillMeetingInfo(const meeting::core::MeetingData& data
                                         , proto::common::MeetingInfo* info) {
    if (info  == nullptr) {
        return;
    }
    info->set_meeting_id(data.meeting_id);
    info->set_organizer_id(data.organizer_id);
    info->set_topic(data.topic);
    info->set_state(StateToString(data.state));
    info->mutable_start_time()->set_seconds(data.created_at);
    info->mutable_start_time()->set_nanos(0);
    info->mutable_end_time()->set_seconds(data.updated_at);
    info->mutable_end_time()->set_nanos(0);
    info->clear_participant_ids();
    for (const auto& participant : data.participants) {
        info->add_participant_ids(participant);
    }
}

} // namespace server
} // namespace meeting