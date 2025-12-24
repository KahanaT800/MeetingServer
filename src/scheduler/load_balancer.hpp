#pragma once

#include "registry/server_registry.hpp"
#include "geo/geo_location_service.hpp"

#include <memory>
#include <optional>
#include <vector>
#include <string>

namespace meeting{
namespace scheduler {

// 负载均衡器，根据地理位置选择合适的服务器节点
class LoadBalancer {
public:
    // 构造函数，传入服务器注册中心的共享指针
    explicit LoadBalancer(std::shared_ptr<meeting::registry::ServerRegistry> registry);

    // 根据地理位置选择合适的服务器节点
    std::optional<meeting::registry::NodeInfo> Select(const meeting::geo::GeoInfo& geo) const;

private:
    // 服务器注册中心
    std::shared_ptr<meeting::registry::ServerRegistry> registry_;
};

} // namespace scheduler
} // namespace meeting