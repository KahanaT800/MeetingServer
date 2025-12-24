#pragma once

#include "cache/redis_client.hpp"
#include "core/meeting/meeting_manager.hpp"
#include "core/meeting/errors.hpp"
#include "core/user/session_repository.hpp"
#include "thread_pool/config.hpp"
#include "thread_pool/thread_pool.hpp"
#include "common/logger.hpp"
#include "config_path.hpp"
// Zookeeper相关
#include "registry/server_registry.hpp"
#include "scheduler/load_balancer.hpp"
#include "geo/geo_location_service.hpp"

#include "meeting_service.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

namespace meeting {
namespace server {

class MeetingServiceImpl final : public proto::meeting::MeetingService::Service {
public:
    MeetingServiceImpl();
    explicit MeetingServiceImpl(const std::string& thread_pool_config_path);
    ~MeetingServiceImpl();

    grpc::Status CreateMeeting(grpc::ServerContext* context
                               , const proto::meeting::CreateMeetingRequest* request
                               , proto::meeting::CreateMeetingResponse* response) override;

    grpc::Status JoinMeeting(grpc::ServerContext* context
                              , const proto::meeting::JoinMeetingRequest* request
                              , proto::meeting::JoinMeetingResponse* response) override;

    grpc::Status LeaveMeeting(grpc::ServerContext* context
                               , const proto::meeting::LeaveMeetingRequest* request
                               , proto::meeting::LeaveMeetingResponse* response) override;

    grpc::Status EndMeeting(grpc::ServerContext* context
                             , const proto::meeting::EndMeetingRequest* request
                             , proto::meeting::EndMeetingResponse* response) override;

    grpc::Status GetMeeting(grpc::ServerContext* context
                             , const proto::meeting::GetMeetingRequest* request
                             , proto::meeting::GetMeetingResponse* response) override;
private:
    static grpc::Status ToGrpcStatus(const meeting::common::Status& status);
    static std::string StateToString(meeting::core::MeetingState state);
    void FillMeetingInfo(const meeting::core::MeetingData& data
                         , proto::common::MeetingInfo* info);

private:
    std::shared_ptr<meeting::cache::RedisClient> redis_client_; // Redis客户端
    std::unique_ptr<meeting::core::MeetingManager> meeting_manager_; // 会议管理器
    std::shared_ptr<meeting::core::SessionRepository> session_repository_; // 会话存储库

    std::shared_ptr<meeting::registry::ServerRegistry> registry_; // 服务器注册中心
    std::shared_ptr<meeting::scheduler::LoadBalancer> load_balancer_; // 负载均衡器
    std::shared_ptr<meeting::geo::GeoLocationService> geo_service_; // 地理位置服务
    meeting::registry::NodeInfo self_node_; // 本节点信息

    thread_pool::ThreadPool thread_pool_;

};

} // namespace server
} // namespace meeting
