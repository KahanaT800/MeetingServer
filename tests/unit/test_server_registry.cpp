#include "registry/server_registry.hpp"

#include <gtest/gtest.h>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <sys/select.h>
#include <zookeeper/zookeeper.h>

namespace {

std::string ZkHosts() {
    const char* env = std::getenv("ZK_HOSTS");
    return env && *env ? std::string(env) : std::string("127.0.0.1:2181");
}

meeting::registry::NodeInfo MakeNode() {
    meeting::registry::NodeInfo node;
    node.host = "127.0.0.1";
    // 简单生成一个随机端口，避免与真实服务冲突
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    node.port = static_cast<int>(20000 + (now % 10000));
    node.region = "itest";
    node.meta_json = R"({"from":"registry_integration_test"})";
    return node;
}

bool ContainsNode(const std::vector<meeting::registry::NodeInfo>& nodes,
                  const meeting::registry::NodeInfo& target) {
    for (const auto& n : nodes) {
        if (n.host == target.host && n.port == target.port && n.region == target.region) {
            return true;
        }
    }
    return false;
}

bool CanConnectZk(const std::string& hosts) {
    // Zookeeper 要求 session timeout 至少 2 * tickTime，容器默认 tickTime=2000ms。
    // vcpkg 的 C client 默认不启用内部线程，需要显式驱动 interest/process 完成握手。
    zhandle_t* zk = zookeeper_init(hosts.c_str(), nullptr, 5000, 0, nullptr, 0);
    if (!zk) return false;
    for (int i = 0; i < 50; ++i) { // 最多等待 ~5s
        int fd = -1;
        int interest = 0;
        struct timeval tv {};
        if (zookeeper_interest(zk, &fd, &interest, &tv) != ZOK) {
            break;
        }
        if (zoo_state(zk) == ZOO_CONNECTED_STATE) {
            zookeeper_close(zk);
            return true;
        }
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        if (interest & ZOOKEEPER_READ) FD_SET(fd, &rfds);
        if (interest & ZOOKEEPER_WRITE) FD_SET(fd, &wfds);
        int rc = select(fd + 1, &rfds, &wfds, nullptr, &tv);
        if (rc < 0) break;
        int events = 0;
        if (FD_ISSET(fd, &rfds)) events |= ZOOKEEPER_READ;
        if (FD_ISSET(fd, &wfds)) events |= ZOOKEEPER_WRITE;
        zookeeper_process(zk, events);
    }
    zookeeper_close(zk);
    return false;
}

} // namespace

TEST(ServerRegistryIntegration, RegisterListUnregister) {
    const auto hosts = ZkHosts();
    if (!CanConnectZk(hosts)) {
        GTEST_SKIP() << "Zookeeper 不可用，跳过集成测试，hosts=" << hosts;
    }

    meeting::registry::ServerRegistry registry(hosts);
    auto node = MakeNode();

    registry.Register(node);

    auto listed = registry.List(node.region);
    ASSERT_FALSE(listed.empty()) << "Zookeeper 不可用或注册失败";
    EXPECT_TRUE(ContainsNode(listed, node));

    registry.Unregister(node);
    auto after = registry.List(node.region);
    EXPECT_FALSE(ContainsNode(after, node));
}
