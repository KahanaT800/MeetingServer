#pragma once

// 项目头文件
#include "cache/redis_client.hpp"
#include "common/logger.hpp"
#include "config_path.hpp"
#include "core/user/session_repository.hpp"
#include "core/user/user_manager.hpp"
#include "thread_pool/config.hpp"
#include "thread_pool/thread_pool.hpp"

// gRPC 生成的头文件
#include "user_service.grpc.pb.h"

// 第三方库
#include <grpcpp/grpcpp.h>

// C++ 标准库
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
    // 辅助函数: 转换状态码 
    static grpc::Status ToGrpcStatus(const meeting::common::Status& status);
    // 辅助函数: 填充用户信息
    void FillUserInfo(const meeting::core::UserData& user_data, proto::common::UserInfo* user_info);

private:
    // redis 客户端
    std::shared_ptr<meeting::cache::RedisClient> redis_client_;
    // 用户管理类
    std::unique_ptr<meeting::core::UserManager> user_manager_;
    // 会话存储库
    std::shared_ptr<meeting::core::SessionRepository> session_repository_;
    thread_pool::ThreadPool thread_pool_;
};

} // namespace server
} // namespace meeting
