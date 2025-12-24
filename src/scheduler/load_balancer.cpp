#include "scheduler/load_balancer.hpp"

namespace meeting {
namespace scheduler {

LoadBalancer::LoadBalancer(std::shared_ptr<meeting::registry::ServerRegistry> registry)
    : registry_(std::move(registry)) {}

std::optional<meeting::registry::NodeInfo> LoadBalancer::Select(const meeting::geo::GeoInfo& geo) const {
    if (!registry_) {
        return std::nullopt;
    }
    auto nodes = registry_->List(geo.region.empty() ? "default" : geo.region); // 先尝试同 region
    if (nodes.empty()) {
        return std::nullopt;
    }
    // 简单策略：取首个
    return nodes.front();
}

} // namespace scheduler
} // namespace meeting