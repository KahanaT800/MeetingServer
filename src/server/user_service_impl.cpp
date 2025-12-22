// 主服务头文件
#include "server/user_service_impl.hpp"
#include "common/config_loader.hpp"
#include "config_path.hpp"
// 储存库(mysql)相关头文件
#include "core/user/user_repository.hpp"
#include "core/user/session_repository.hpp"
#include "storage/mysql/connection_pool.hpp"
#include "storage/mysql/user_repository.hpp"
#include "storage/mysql/session_repository.hpp"
// 缓存(redis)相关头文件
#include "cache/redis_client.hpp"
#include "core/user/cached_user_repository.hpp"
#include "core/user/cached_session_repository.hpp"

#include <chrono>
#include <random>
namespace meeting {
namespace server {

namespace {
// 创建用户相关的线程池
thread_pool::ThreadPool CreateUserThreadPool(const std::string& path) {
    auto config_loader = thread_pool::ThreadPoolConfigLoader::FromFile(path);
    if (config_loader.has_value()) {
        return thread_pool::ThreadPool(config_loader->GetConfig());
    }
    return thread_pool::ThreadPool(4, 5000);
}
// 创建Redis客户端
std::shared_ptr<meeting::cache::RedisClient> CreateRedisClient() {
    // 读取全局配置
    const auto& config = meeting::common::GlobalConfig();
    // 如果Redis未启用，则返回空指针
    if (!config.cache.redis.enabled) {
        return nullptr;
    }

    // 创建Redis客户端实例
    auto client = std::make_shared<meeting::cache::RedisClient>(config.cache.redis);
    auto status = client->Connect();
    if (!status.IsOk()) {
        MEETING_LOG_WARN("[UserService] Redis init failed, fallback to no cache: {}", status.Message());
        return nullptr;
    }
    return client;
}

// 创建用户存储库
std::shared_ptr<meeting::core::UserRepository> CreateUserRepository(
    const std::shared_ptr<meeting::cache::RedisClient>& redis) {
    
    // 读取全局配置
    const auto& config = meeting::common::GlobalConfig();
    // 如果MySQL未启用，则使用内存中的用户仓库
    if (!config.storage.mysql.enabled) {
        MEETING_LOG_WARN("[UserService] MySQL backend disabled; using in-memory repository");
        // 使用内存中的用户仓库作为后备方案
        return std::make_shared<meeting::core::InMemoryUserRepository>();
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
        MEETING_LOG_ERROR("[UserService] Failed to initialize MySQL connection: {}", test_conn.GetStatus().Message());
        // 使用内存中的用户仓库作为后备方案
        return std::make_shared<meeting::core::InMemoryUserRepository>();
    }

    // 成功创建连接池
    MEETING_LOG_INFO("[UserService] MySQL connection pool initialized successfully");
    auto base_repo = std::make_shared<meeting::storage::MySQLUserRepository>(std::move(pool));
    // 如果Redis客户端存在，则使用缓存用户存储库包装基础存储库
    if (redis) {
        return std::make_shared<meeting::core::CachedUserRepository>(base_repo, redis);
    }
    return base_repo;
}

// 创建会话存储库
std::shared_ptr<meeting::core::SessionRepository> CreateSessionRepository(
    const std::shared_ptr<meeting::cache::RedisClient>& redis) {
    const auto& config = meeting::common::GlobalConfig();
    if (!config.storage.mysql.enabled) {
        return std::make_shared<meeting::core::InMemorySessionRepository>();
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
        MEETING_LOG_ERROR("[UserService] Failed to initialize MySQL session connection: {}", test_conn.GetStatus().Message());
        return std::make_shared<meeting::core::InMemorySessionRepository>();
    }
    
    // 成功创建连接池
    auto base_repo = std::make_shared<meeting::storage::MySqlSessionRepository>(std::move(pool));
    // 如果Redis客户端存在，则使用缓存会话存储库包装基础存储库
    if (redis) {
        return std::make_shared<meeting::core::CachedSessionRepository>(base_repo, redis);
    }
    return base_repo;
}

std::string GenerateToken() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char kChars[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<int> dist(0, sizeof(kChars) - 2);
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) {
        token.push_back(kChars[dist(rng)]);
    }
    return token;
}

std::int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

UserServiceImpl::UserServiceImpl(): UserServiceImpl(meeting::common::GetThreadPoolConfigPath()) {}

UserServiceImpl::UserServiceImpl(const std::string& thread_pool_config_path)
    : redis_client_(CreateRedisClient()) // 创建Redis客户端
    , user_manager_(std::make_unique<meeting::core::UserManager>(CreateUserRepository(redis_client_))) // 创建用户管理器
    , session_repository_(CreateSessionRepository(redis_client_)) // 创建会话存储库
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

    auto session_future = thread_pool_.Submit([this, user_data]() {
        meeting::core::SessionRecord rec;
        rec.token = GenerateToken();
        rec.user_id = user_data.numeric_id;
        rec.user_uuid = user_data.user_id;
        rec.expires_at = NowSeconds() + 3600;
        return rec;
    });
    auto rec = session_future.get();
    auto session_status = session_repository_->CreateSession(rec);
    if (!session_status.IsOk()) {
        meeting::core::UserErrorCode session_error = meeting::core::UserErrorCode::kSessionExpired;
        meeting::core::ErrorToProto(session_error, session_status, response->mutable_error());
        return ToGrpcStatus(session_status);
    }

    response->set_session_token(rec.token);
    meeting::core::ErrorToProto(meeting::core::UserErrorCode::kOk, meeting::common::Status::OK(),
                                    response->mutable_error());

    return grpc::Status::OK;
}

grpc::Status UserServiceImpl::Logout(grpc::ServerContext* /*context*/,
                                      const proto::user::LogoutRequest* request,
                                      proto::user::LogoutResponse* response) {
    MEETING_LOG_INFO("[UserService] Logout session_token={}...", request->session_token().substr(0, 6));                                    
    auto logout_future = thread_pool_.Submit([this, token = request->session_token()]() {
        return session_repository_->DeleteSession(token);
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
        return session_repository_->ValidateSession(token);
    });
    auto session_status = session_future.get();
    if (!session_status.IsOk()) {
        meeting::core::UserErrorCode error_code = meeting::core::UserErrorCode::kSessionExpired;
        meeting::core::ErrorToProto(error_code, session_status.GetStatus(), response->mutable_error());
        return ToGrpcStatus(session_status.GetStatus());
    }

    auto user_future = thread_pool_.Submit([this, user_id = session_status.Value().user_uuid]() {
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
