#include "server/health_service.h"

#include "thread_pool/thread_pool.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <fmt/core.h>
#include <string>
#include <utility>

namespace meeting::server {

HealthServiceImpl::HealthServiceImpl(thread_pool::ThreadPool& pool)
    : pool_(pool) {}

grpc::Status HealthServiceImpl::Check(grpc::ServerContext* /*context*/,
                                        const meeting::health::HealthCheckRequest* request,
                                        meeting::health::HealthCheckResponse* response) {
    using Result = std::int64_t;

    auto timestamp_future = pool_.Submit(
        []() -> Result {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch());
            return now.count();
        });

    std::int64_t timestamp = 0;
    try {
        timestamp = timestamp_future.get();
    } catch (const std::exception& ex) {
        fmt::print(stderr, "[health] async timestamp task failed: {}\n", ex.what());
        return {grpc::StatusCode::INTERNAL, "timestamp task failed"};
    } catch (...) {
        fmt::print(stderr, "[health] async timestamp task failed: unknown error\n");
        return {grpc::StatusCode::INTERNAL, "timestamp task failed"};
    }

    response->set_status("SERVING");
    response->set_timestamp(timestamp);

    pool_.Post([source = std::string(request->source())] {
        fmt::print("[health] Check handled for '{}'\n", source);
    });

    return grpc::Status::OK;
}

}
