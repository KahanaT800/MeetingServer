#pragma once

#include "core/user/session_manager.hpp"
#include "core/user/user_manager.hpp"
#include "common/logger.hpp"
#include "config_path.hpp"
#include "thread_pool/config.hpp"
#include "thread_pool/thread_pool.hpp"
#include "user_service.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
namespace meeting {
namespace server {

class UserServiceImpl final : public proto::user::UserService::Service {
public:
    UserServiceImpl();
    explicit UserServiceImpl(const std::string& thread_pool_config_path);
    ~UserServiceImpl();
    
    grpc::Status Register(grpc::ServerContext* context
                         , const proto::user::RegisterRequest* request
                         , proto::user::RegisterResponse* response) override;

    grpc::Status Login(grpc::ServerContext* context
                      , const proto::user::LoginRequest* request
                      , proto::user::LoginResponse* response) override;

    grpc::Status Logout(grpc::ServerContext* context
                       , const proto::user::LogoutRequest* request
                       , proto::user::LogoutResponse* response) override;

    grpc::Status GetProfile(grpc::ServerContext* context
                           , const proto::user::GetProfileRequest* request
                           , proto::user::GetProfileResponse* response) override;

private:
    static grpc::Status ToGrpcStatus(const meeting::common::Status& status);
    void FillUserInfo(const meeting::core::UserData& user_data, proto::common::UserInfo* user_info);

    std::unique_ptr<meeting::core::UserManager> user_manager_;
    std::unique_ptr<meeting::core::SessionConfig> session_config_;
    std::unique_ptr<meeting::core::SessionManager> session_manager_;
    thread_pool::ThreadPool thread_pool_;
};

} // namespace server
} // namespace meeting