#include "server/user_service_impl.hpp"
#include "config_path.hpp"

namespace meeting {
namespace server {

namespace {
thread_pool::ThreadPool CreateUserThreadPool(const std::string& path) {
    auto config_loader = thread_pool::ThreadPoolConfigLoader::FromFile(path);
    if (config_loader.has_value()) {
        return thread_pool::ThreadPool(config_loader->GetConfig());
    }
    return thread_pool::ThreadPool(4, 5000);
}

} // namespace

UserServiceImpl::UserServiceImpl(): UserServiceImpl(meeting::common::GetThreadPoolConfigPath()) {}

UserServiceImpl::UserServiceImpl(const std::string& thread_pool_config_path)
    : user_manager_(std::make_unique<meeting::core::UserManager>())
    , session_manager_(std::make_unique<meeting::core::SessionManager>())
    , thread_pool_(CreateUserThreadPool(thread_pool_config_path)) {
    // 启动线程池
    thread_pool_.Start();
}

UserServiceImpl::~UserServiceImpl() {
    // 停止线程池
    thread_pool_.Stop();
}

grpc::Status UserServiceImpl::Register(grpc::ServerContext*
                                        , const proto::user::RegisterRequest* request           
                                        , proto::user::RegisterResponse* response) {
    meeting::core::RegisterCommand command{request->user_name()
                                         , request->password()
                                         , request->email()
                                         , request->display_name()};
    MEETING_LOG_INFO("[UserService] Register user={}", command.user_name);                                     
    auto future = thread_pool_.Submit([this, command]() {
        return user_manager_->RegisterUser(command);
    });
    meeting::common::Status status = future.get();
    
    if (!status.IsOk()) {
        meeting::core::UserErrorCode error_code = meeting::core::UserErrorCode::kOk;
        switch (status.Code()) {
            case meeting::common::StatusCode::kAlreadyExists:
                error_code = meeting::core::UserErrorCode::kUserNameExists;
                break;
            case meeting::common::StatusCode::kInvalidArgument:
                error_code = meeting::core::UserErrorCode::kInvalidPassword;
                break;
            default:
                error_code = meeting::core::UserErrorCode::kInvalidPassword;
                break;
        }
        meeting::core::ErrorToProto(error_code, status, response->mutable_error());
        return ToGrpcStatus(status);
    }
    
    auto user_data = user_manager_->GetUserByUserName(command.user_name);
    if (user_data.IsOk()) {
        FillUserInfo(user_data.Value(), response->mutable_user());
    }
    
    return grpc::Status::OK;
}

grpc::Status UserServiceImpl::Login(grpc::ServerContext*
                                    , const proto::user::LoginRequest* request
                                    , proto::user::LoginResponse* response) {
    meeting::core::LoginCommand command{request->user_name()
                                         , request->password()
                                         , ""
                                         , ""};
    MEETING_LOG_INFO("[UserService] Login user={}", command.user_name);                                     
    auto login_future = thread_pool_.Submit([this, command]() {
        return user_manager_->LoginUser(command);
    });
    meeting::common::StatusOr status_or_user = login_future.get();

    meeting::core::UserErrorCode error_code = meeting::core::UserErrorCode::kOk;
    if (!status_or_user.IsOk()) {
        const auto& status = status_or_user.GetStatus();
        meeting::common::Status err_status = status;
        if (status.Code() == meeting::common::StatusCode::kUnauthenticated) {
            error_code = meeting::core::UserErrorCode::kInvalidCredentials;
        } else if (status.Code() == meeting::common::StatusCode::kNotFound) {
            error_code = meeting::core::UserErrorCode::kUserNotFound;
        } else {
            error_code = meeting::core::UserErrorCode::kInvalidPassword;
        }
        meeting::core::ErrorToProto(error_code, err_status, response->mutable_error());
        return ToGrpcStatus(status);
    }

    auto user_data = status_or_user.Value();
    FillUserInfo(user_data, response->mutable_user());

    auto session_future = thread_pool_.Submit([this, &user_data]() {
        return session_manager_->CreateSession(user_data.user_id, /*client_ip=*/"", /*user_agent=*/"");
    }); 
    auto session_result = session_future.get();
    if (!session_result.IsOk()) {
        meeting::core::UserErrorCode session_error = meeting::core::UserErrorCode::kSessionExpired;
        meeting::core::ErrorToProto(session_error, session_result.GetStatus(), response->mutable_error());
        return ToGrpcStatus(session_result.GetStatus());
    }

    response->set_session_token(session_result.Value().token);
    meeting::core::ErrorToProto(meeting::core::UserErrorCode::kOk, meeting::common::Status::OK(),
                                    response->mutable_error());

    return grpc::Status::OK;
}

grpc::Status UserServiceImpl::Logout(grpc::ServerContext* /*context*/,
                                      const proto::user::LogoutRequest* request,
                                      proto::user::LogoutResponse* response) {
    MEETING_LOG_INFO("[UserService] Logout session_token={}...", request->session_token().substr(0, 6));                                    
    auto logout_future = thread_pool_.Submit([this, token = request->session_token()]() {
        return session_manager_->DeleteSession(token);
    });

    auto logout_status = logout_future.get();
    meeting::core::UserErrorCode error_code = logout_status.IsOk() ? meeting::core::UserErrorCode::kOk
                                                          : meeting::core::UserErrorCode::kSessionExpired;

    meeting::core::ErrorToProto(error_code, logout_status, response->mutable_error());

    return ToGrpcStatus(logout_status);
}

grpc::Status UserServiceImpl::GetProfile(grpc::ServerContext* /*context*/,
                                          const proto::user::GetProfileRequest* request,
                                          proto::user::GetProfileResponse* response) {
    auto session_future = thread_pool_.Submit([this, token = request->session_token()]() {
        return session_manager_->ValidateSession(token, /*client_ip=*/"", /*user_agent=*/"");
    });
    auto session_status = session_future.get();
    if (!session_status.IsOk()) {
        meeting::core::UserErrorCode error_code = meeting::core::UserErrorCode::kSessionExpired;
        meeting::core::ErrorToProto(error_code, session_status.GetStatus(), response->mutable_error());
        return ToGrpcStatus(session_status.GetStatus());
    }

    auto user_future = thread_pool_.Submit([this, user_id = session_status.Value().user_id]() {
        return user_manager_->GetUserById(user_id);
    });
    auto user_status_or = user_future.get();
    if (!user_status_or.IsOk()) {
        meeting::core::UserErrorCode error_code = meeting::core::UserErrorCode::kUserNotFound;
        meeting::core::ErrorToProto(error_code, user_status_or.GetStatus(), response->mutable_error());
        return ToGrpcStatus(user_status_or.GetStatus());
    }

    FillUserInfo(user_status_or.Value(), response->mutable_user());
    meeting::core::ErrorToProto(meeting::core::UserErrorCode::kOk, meeting::common::Status::OK(),
                                    response->mutable_error());
    return grpc::Status::OK;
}

grpc::Status UserServiceImpl::ToGrpcStatus(const meeting::common::Status& status) {
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
        return {grpc::StatusCode::UNAUTHENTICATED, status.Message()};
        case StatusCode::kInternal:
        return {grpc::StatusCode::INTERNAL, status.Message()};
        case StatusCode::kUnavailable:
        return {grpc::StatusCode::UNAVAILABLE, status.Message()};
    }
    return {grpc::StatusCode::UNKNOWN, status.Message()};
}

void UserServiceImpl::FillUserInfo(const meeting::core::UserData& user_data
                                  , proto::common::UserInfo* user_info) {
  if (user_info == nullptr) {
    return;
  }
  user_info->set_user_id(user_data.user_id);
  user_info->set_user_name(user_data.user_name);
  user_info->set_display_name(user_data.display_name);
  user_info->set_email(user_data.email);
  user_info->mutable_created_at()->set_seconds(user_data.created_at);
  user_info->mutable_created_at()->set_nanos(0);
  user_info->mutable_last_login()->set_seconds(user_data.last_login);
  user_info->mutable_last_login()->set_nanos(0);
}

} // namespace server
} // namespace meeting