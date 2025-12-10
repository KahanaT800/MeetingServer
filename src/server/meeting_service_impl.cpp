#include "server/meeting_service_impl.hpp"
#include "common/config_loader.hpp"
#include "config_path.hpp"
#include "core/meeting/meeting_repository.hpp"
#include "common/status_or.hpp"
#include "storage/mysql/connection_pool.hpp"
#include "storage/mysql/meeting_repository.hpp"

#include <charconv>
#include <chrono>
#include <system_error>
namespace meeting {
namespace server {

namespace {

thread_pool::ThreadPool CreateThreadPool(const std::string& config_path) {
    auto loader = thread_pool::ThreadPoolConfigLoader::FromFile(config_path);
    if (loader.has_value()) {
        return thread_pool::ThreadPool(loader->GetConfig());
    }
    return thread_pool::ThreadPool(4, 1024);
}

// 根据配置创建会议存储库
std::shared_ptr<meeting::core::MeetingRepository> CreateMeetingRepository() {
    const auto& config = meeting::common::GlobalConfig();
    if (!config.storage.mysql.enabled) {
        MEETING_LOG_WARN("[MeetingService] MySQL backend disabled; using in-memory repository");
        return std::make_shared<meeting::core::InMemoryMeetingRepository>();
    }

    meeting::storage::Options options;
    options.host = config.storage.mysql.host;
    options.port = static_cast<std::uint16_t>(config.storage.mysql.port);
    options.user = config.storage.mysql.user;
    options.password = config.storage.mysql.password;
    options.database = config.storage.mysql.database;
    options.pool_size = static_cast<std::size_t>(config.storage.mysql.pool_size);
    options.acquire_timeout = std::chrono::milliseconds(config.storage.mysql.connection_timeout_ms);
    options.connect_timeout = std::chrono::milliseconds(config.storage.mysql.connection_timeout_ms);
    options.read_timeout = std::chrono::milliseconds(config.storage.mysql.read_timeout_ms);
    options.write_timeout = std::chrono::milliseconds(config.storage.mysql.write_timeout_ms);

    auto pool = std::make_shared<meeting::storage::ConnectionPool>(options);
    auto test_conn = pool->Acquire();
    if (!test_conn.IsOk()) {
        MEETING_LOG_ERROR("[MeetingService] Failed to initialize MySQL connection: {}", test_conn.GetStatus().Message());
        return std::make_shared<meeting::core::InMemoryMeetingRepository>();
    }
    return std::make_shared<meeting::storage::MySqlMeetingRepository>(std::move(pool));
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

meeting::common::StatusOr<std::uint64_t> ParseUserId(const std::string& token) {
    if (token.empty()) {
        return meeting::common::Status::InvalidArgument("session token is empty");
    }
    std::uint64_t value = 0;
    auto first = token.data();
    auto last = token.data() + token.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last) {
        return meeting::common::Status::InvalidArgument("session token must be numeric user id");
    }
    return meeting::common::StatusOr<std::uint64_t>(value);
}

} // namespace

MeetingServiceImpl::MeetingServiceImpl(): MeetingServiceImpl(meeting::common::GetThreadPoolConfigPath()) {}

MeetingServiceImpl::MeetingServiceImpl(const std::string& thread_pool_config_path)
    : meeting_manager_(std::make_unique<meeting::core::MeetingManager>(meeting::core::MeetingConfig{}, CreateMeetingRepository()))
    , thread_pool_(CreateThreadPool(thread_pool_config_path)) {
    thread_pool_.Start();
}


MeetingServiceImpl::~MeetingServiceImpl() {
    thread_pool_.Stop();
}

grpc::Status MeetingServiceImpl::CreateMeeting(grpc::ServerContext*
                                                , const proto::meeting::CreateMeetingRequest* request
                                                , proto::meeting::CreateMeetingResponse* response) {
    auto organizer_id_or = ParseUserId(request->session_token());
    if (!organizer_id_or.IsOk()) {
        auto code = MapStatus(organizer_id_or.GetStatus());
        meeting::core::ErrorToProto(code, organizer_id_or.GetStatus(), response->mutable_error());
        return ToGrpcStatus(organizer_id_or.GetStatus());
    }
    meeting::core::CreateMeetingCommand command{organizer_id_or.Value(), request->topic()};
    MEETING_LOG_INFO("[MeetingService] CreateMeeting topic={} organizer={}",
                     command.topic, command.organizer_id);
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
    auto participant_or = ParseUserId(request->session_token());
    if (!participant_or.IsOk()) {
        auto code = MapStatus(participant_or.GetStatus());
        meeting::core::ErrorToProto(code, participant_or.GetStatus(), response->mutable_error());
        return ToGrpcStatus(participant_or.GetStatus());
    }
    meeting::core::JoinMeetingCommand command{request->meeting_id(), participant_or.Value()};
    MEETING_LOG_INFO("[MeetingService] JoinMeeting meeting={} participant={}",
                     command.meeting_id, command.participant_id);
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
    auto participant_or = ParseUserId(request->session_token());
    if (!participant_or.IsOk()) {
        auto code = MapStatus(participant_or.GetStatus());
        meeting::core::ErrorToProto(code, participant_or.GetStatus(), response->mutable_error());
        return ToGrpcStatus(participant_or.GetStatus());
    }
    meeting::core::LeaveMeetingCommand command{request->meeting_id(), participant_or.Value()};
    MEETING_LOG_INFO("[MeetingService] LeaveMeeting meeting={} participant={}",
                     command.meeting_id, command.participant_id);
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
    auto requester_or = ParseUserId(request->session_token());
    if (!requester_or.IsOk()) {
        auto code = MapStatus(requester_or.GetStatus());
        meeting::core::ErrorToProto(code, requester_or.GetStatus(), response->mutable_error());
        return ToGrpcStatus(requester_or.GetStatus());
    }
    meeting::core::EndMeetingCommand command{request->meeting_id(), requester_or.Value()};
    MEETING_LOG_INFO("[MeetingService] EndMeeting meeting={} requester={}",
                     command.meeting_id, command.requester_id);
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
    MEETING_LOG_INFO("[MeetingService] GetMeeting meeting={}", request->meeting_id());
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
    info->set_organizer_id(std::to_string(data.organizer_id));
    info->set_topic(data.topic);
    info->set_state(StateToString(data.state));
    info->mutable_start_time()->set_seconds(data.created_at);
    info->mutable_start_time()->set_nanos(0);
    info->mutable_end_time()->set_seconds(data.updated_at);
    info->mutable_end_time()->set_nanos(0);
    info->clear_participant_ids();
    for (const auto participant : data.participants) {
        info->add_participant_ids(std::to_string(participant));
    }
}

} // namespace server
} // namespace meeting
