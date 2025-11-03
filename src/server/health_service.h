#pragma once

#include "health.grpc.pb.h"
#include "health.pb.h"

#include <grpcpp/grpcpp.h>

namespace thread_pool {
class ThreadPool;
}

namespace meeting::server {
class HealthServiceImpl final : public meeting::health::HealthService::Service {
public:
    explicit HealthServiceImpl(thread_pool::ThreadPool& pool);

    grpc::Status Check(grpc::ServerContext* context,
                    const meeting::health::HealthCheckRequest* request,
                    meeting::health::HealthCheckResponse* response) override;
private:
    thread_pool::ThreadPool& pool_;
};

}
