#include "server/meeting_service_impl.hpp"
#include "common/config_loader.hpp"
#include "config_path.hpp"
#include "core/meeting/meeting_repository.hpp"
#include "core/user/session_repository.hpp"
#include "common/status_or.hpp"
#include "storage/mysql/connection_pool.hpp"
#include "storage/mysql/meeting_repository.hpp"
#include "storage/mysql/session_repository.hpp"
// redis相关
#include "cache/redis_client.hpp"
#include "core/meeting/cached_meeting_repository.hpp"
#include "core/user/cached_session_repository.hpp"
// Zookeeper相关
#include "registry/server_registry.hpp"
#include "scheduler/load_balancer.hpp"
#include "geo/geo_location_service.hpp"

#include <charconv>
#include <chrono>
#include <system_error>
#include <optional>
#include <string_view>
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

// 根据配置创建Redis客户端
std::shared_ptr<meeting::cache::RedisClient> CreateRedisClient() {
    const auto& config = meeting::common::GlobalConfig();
    if (!config.cache.redis.enabled) {
        return nullptr;
    }
    auto client = std::make_shared<meeting::cache::RedisClient>(config.cache.redis);
    auto status = client->Connect();
    if (!status.IsOk()) {
        MEETING_LOG_WARN("[MeetingService] Redis init failed, fallback to no cache: {}", status.Message());
        return nullptr;
    }
    return client;
}

// 根据配置创建会议存储库
std::shared_ptr<meeting::core::MeetingRepository> CreateMeetingRepository(
    const std::shared_ptr<meeting::cache::RedisClient>& redis) {
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

    // 创建基础仓库
    auto base_repo = std::make_shared<meeting::storage::MySqlMeetingRepository>(std::move(pool));
    if (redis) {
        // 使用缓存包装基础仓库
        return std::make_shared<meeting::core::CachedMeetingRepository>(base_repo, redis);
    }
    return base_repo;
}

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
        MEETING_LOG_ERROR("[MeetingService] Failed to initialize MySQL session connection: {}", test_conn.GetStatus().Message());
        return std::make_shared<meeting::core::InMemorySessionRepository>();
    }

    // 创建基础仓库
    auto base_repo = std::make_shared<meeting::storage::MySqlSessionRepository>(std::move(pool));
    if (redis) {
        // 使用缓存包装基础仓库
        return std::make_shared<meeting::core::CachedSessionRepository>(base_repo, redis);
    }
    return base_repo;
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

meeting::common::StatusOr<std::uint64_t> ResolveUserId(const std::string& token,
                                                       meeting::core::SessionRepository* repo,
                                                       bool allow_numeric_fallback) {
    if (repo) {
        auto session = repo->ValidateSession(token);
        if (session.IsOk()) {
            return meeting::common::StatusOr<std::uint64_t>(session.Value().user_id);
        }
        if (!allow_numeric_fallback) {
            return session.GetStatus();
        }
    }
    if (!allow_numeric_fallback) {
        return meeting::common::Status::Unauthenticated("Session not found");
    }
    // fallback: interpret token as numeric id
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

// 移除IPv6地址的方括号
std::string StripBrackets(std::string_view ip) {
    if (!ip.empty() && ip.front() == '[' && ip.back() == ']') {
        return std::string(ip.substr(1, ip.size() - 2));
    }
    return std::string(ip);
}

// 从gRPC peer字符串中提取IP地址
std::string ExtractIpFromPeer(const std::string& peer) {
    // gRPC peer examples: "ipv4:127.0.0.1:54321", "ipv6:[::1]:54321"
    if (peer.empty()) {
        return {};
    }
    std::string_view sv(peer);
    auto pos_prefix = sv.find(':');
    if (pos_prefix != std::string_view::npos && (sv.rfind("ipv4:", 0) == 0 || sv.rfind("ipv6:", 0) == 0)) {
        sv.remove_prefix(pos_prefix + 1);
    }
    if (!sv.empty() && sv.front() == '[') {
        auto close = sv.find(']');
        if (close != std::string_view::npos) {
            return StripBrackets(sv.substr(0, close + 1));
        }
    }
    auto pos_port = sv.rfind(':');
    if (pos_port == std::string_view::npos) {
        return StripBrackets(sv);
    }
    return StripBrackets(sv.substr(0, pos_port));
}

std::string ExtractClientIp(const grpc::ServerContext* context, const proto::meeting::JoinMeetingRequest* request) {
    auto decode_brackets = [](std::string ip) {
        auto replace_all = [](std::string& target, std::string_view from, std::string_view to) {
            std::size_t pos = 0;
            while ((pos = target.find(from, pos)) != std::string::npos) {
                target.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replace_all(ip, "%5B", "[");
        replace_all(ip, "%5D", "]");
        return ip;
    };

    if (request && !request->client_info().empty()) {
        // 传入IP，本地校验在 Geo 查询时完成
        return StripBrackets(decode_brackets(request->client_info()));
    }
    if (context) {
        return StripBrackets(decode_brackets(ExtractIpFromPeer(context->peer())));
    }
    return {};
}

// 选择合适的会议服务节点
meeting::registry::NodeInfo PickEndpoint(const meeting::scheduler::LoadBalancer* lb,
                                         const meeting::registry::NodeInfo& self_node,
                                         const meeting::geo::GeoLocationService* geo_service,
                                         const std::string& client_ip) {
    meeting::geo::GeoInfo geo; // 默认地理信息
    if (geo_service && !client_ip.empty()) {
        auto geo_res = geo_service->Lookup(client_ip);
        if (geo_res.IsOk()) {
            geo = geo_res.Value();
        } else {
            MEETING_LOG_WARN("[MeetingService] Geo lookup failed for {}: {}", client_ip, geo_res.GetStatus().Message());
        }
    }
    std::optional<meeting::registry::NodeInfo> selected; // 选择的节点
    if (lb) {
        selected = lb->Select(geo);
    }
    if (selected.has_value()) {
        return *selected;
    }
    return self_node;
}

} // namespace

MeetingServiceImpl::MeetingServiceImpl(): MeetingServiceImpl(meeting::common::GetThreadPoolConfigPath()) {}

MeetingServiceImpl::MeetingServiceImpl(const std::string& thread_pool_config_path)
    : redis_client_(CreateRedisClient())
    , meeting_manager_(std::make_unique<meeting::core::MeetingManager>(meeting::core::MeetingConfig{}, CreateMeetingRepository(redis_client_)))
    , session_repository_(CreateSessionRepository(redis_client_))
    , registry_(std::make_shared<meeting::registry::ServerRegistry>(meeting::common::GlobalConfig().zookeeper.hosts))
    , load_balancer_(std::make_shared<meeting::scheduler::LoadBalancer>(registry_))
    , geo_service_(std::make_shared<meeting::geo::GeoLocationService>(meeting::common::GlobalConfig().geoip.db_path))
    , self_node_()
    , thread_pool_(CreateThreadPool(thread_pool_config_path)) {

    thread_pool_.Start();
    self_node_.host = meeting::common::GlobalConfig().server.host;
    self_node_.port = meeting::common::GlobalConfig().server.port;
    self_node_.region = "default";
    // 自注册当前节点
    registry_->Register(self_node_);
}


MeetingServiceImpl::~MeetingServiceImpl() {
    if (registry_) {
        // 注销当前节点
        registry_->Unregister(self_node_);
    }
    thread_pool_.Stop();
}

grpc::Status MeetingServiceImpl::CreateMeeting(grpc::ServerContext* context
                                                , const proto::meeting::CreateMeetingRequest* request
                                                , proto::meeting::CreateMeetingResponse* response) {
    (void)context; // 未使用
    auto organizer_id_or = ResolveUserId(request->session_token(), session_repository_.get(),
                                         !meeting::common::GlobalConfig().storage.mysql.enabled);
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

grpc::Status MeetingServiceImpl::JoinMeeting(grpc::ServerContext* context
                                              , const proto::meeting::JoinMeetingRequest* request
                                              , proto::meeting::JoinMeetingResponse* response) {
    auto participant_or = ResolveUserId(request->session_token(), session_repository_.get(),
                                        !meeting::common::GlobalConfig().storage.mysql.enabled);
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
    // 选择合适的会议服务节点
    const auto client_ip = ExtractClientIp(context, request);
    auto endpoint_node = PickEndpoint(load_balancer_.get(), self_node_, geo_service_.get(), client_ip);
    auto* endpoint = response->mutable_endpoint();
    endpoint->set_ip(endpoint_node.host);
    endpoint->set_port(endpoint_node.port);
    endpoint->set_region(endpoint_node.region);
    return grpc::Status::OK;
}

grpc::Status MeetingServiceImpl::LeaveMeeting(grpc::ServerContext* context
                                              , const proto::meeting::LeaveMeetingRequest* request
                                              , proto::meeting::LeaveMeetingResponse* response) {
    (void)context; // 未使用
    auto participant_or = ResolveUserId(request->session_token(), session_repository_.get(),
                                        !meeting::common::GlobalConfig().storage.mysql.enabled);
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

grpc::Status MeetingServiceImpl::EndMeeting(grpc::ServerContext* context
                                             , const proto::meeting::EndMeetingRequest* request
                                             , proto::meeting::EndMeetingResponse* response) {
    (void)context; // 未使用
    auto requester_or = ResolveUserId(request->session_token(), session_repository_.get(),
                                      !meeting::common::GlobalConfig().storage.mysql.enabled);
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

grpc::Status MeetingServiceImpl::GetMeeting(grpc::ServerContext* context
                                             , const proto::meeting::GetMeetingRequest* request
                                             , proto::meeting::GetMeetingResponse* response) {
    (void)context; // 未使用
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
