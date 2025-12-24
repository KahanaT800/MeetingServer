#include "registry/server_registry.hpp"
#include "common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <sys/select.h>

namespace meeting {
namespace registry {

namespace {
// Zookeeper 操作的回调函数，用于设置 promise 的值
void VoidCompletion(int rc, const void* data) {
    auto* promise = static_cast<std::promise<int>*>(const_cast<void*>(data));
    if (promise) {
        promise->set_value(rc);
    }
}

// 用于获取 Stat 结构体的回调函数
void StatCompletion(int rc, const struct Stat*, const void* data) {
    VoidCompletion(rc, data);
}

// 用于创建节点的回调函数
void CreateCompletion(int rc, const char*, const void* data) {
    VoidCompletion(rc, data);
}

// 用于获取字符串列表的回调函数
void StringsCompletion(int rc, const struct String_vector* strings, const void* data) {
    auto* promise = static_cast<std::promise<std::pair<int, String_vector>>*>(const_cast<void*>(data));
    if (!promise) return;
    String_vector copy{};
    if (rc == ZOK && strings) {
        copy.count = strings->count;
        copy.data = (char**)calloc(strings->count, sizeof(char*));
        for (int i = 0; i < strings->count; ++i) {
            copy.data[i] = strdup(strings->data[i]);
        }
    }
    promise->set_value({rc, copy});
}

template<typename T>
T Wait(std::future<T>& f, zhandle_t* zk) {
    while (f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        int fd = -1;
        int interest = 0;
        struct timeval tv = {0, 10000}; // 10ms
        if (zookeeper_interest(zk, &fd, &interest, &tv) != ZOK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        if (interest & ZOOKEEPER_READ) FD_SET(fd, &rfds);
        if (interest & ZOOKEEPER_WRITE) FD_SET(fd, &wfds);

        struct timeval select_tv = {0, 10000}; // 10ms
        select(fd + 1, &rfds, &wfds, nullptr, &select_tv);
        
        int events = 0;
        if (FD_ISSET(fd, &rfds)) events |= ZOOKEEPER_READ;
        if (FD_ISSET(fd, &wfds)) events |= ZOOKEEPER_WRITE;
        zookeeper_process(zk, events);
    }
    return f.get();
}

} // namespace

ServerRegistry::ServerRegistry(std::string zk_hosts) : zk_hosts_(std::move(zk_hosts)) {
    enabled_ = !zk_hosts_.empty(); // 是否启用注册功能, 取决于是否配置了zk地址
    if (!enabled_) {
        MEETING_LOG_WARN("[ServerRegistry] zk hosts empty, registry disabled");
    }
}

ServerRegistry::~ServerRegistry() {
    // 关闭zookeeper连接
    if (zk_) {
        zookeeper_close(zk_);
        zk_ = nullptr;
    }
}

// 注册本节点
void ServerRegistry::Register(const NodeInfo& node) {
    if (!enabled_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_); // 保护 nodes_
    if (!EnsureConnected()) {
        MEETING_LOG_ERROR("[ServerRegistry] failed to connect to zookeeper");
        return;
    }

    // 确保父路径存在
    EnsurePath("/meeting", false, "");
    EnsurePath("/meeting/servers", false, "");

    std::string base = "/meeting/servers/" + node.region; // 节点基础路径
    // 确保基础路径存在
    EnsurePath(base, false, "");

    std::string path = base + "/" + node.host + ":" + std::to_string(node.port); // 节点完整路径
    std::string data = node.meta_json; // 节点数据
    // 创建临时节点
    int rc = EnsurePath(path, true, data);
    if (rc != ZOK && rc != ZNODEEXISTS) {
        MEETING_LOG_ERROR("[ServerRegistry] register failed rc={} path={}", rc, path);
    } else {
        nodes_.push_back(node);
        MEETING_LOG_INFO("[ServerRegistry] register node {}:{} region={}", node.host, node.port, node.region);
    }
}

// 注销本节点
void ServerRegistry::Unregister(const NodeInfo& node) {
    if (!enabled_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (zk_) {
        // 删除节点
        std::promise<int> p;
        auto f = p.get_future();
        std::string path = "/meeting/servers/" + node.region + "/" + node.host + ":" + std::to_string(node.port);
        int rc = zoo_adelete(zk_, path.c_str(), -1, VoidCompletion, &p);
        if (rc != ZOK) {
            p.set_value(rc);
        }
        Wait(f, zk_);
    }
    // 从缓存中移除节点
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const NodeInfo& n) {
        return n.host == node.host && n.port == node.port && n.region == node.region;
    }), nodes_.end());
    MEETING_LOG_INFO("[ServerRegistry] unregister node {}:{} region={}", node.host, node.port, node.region);
}

// 列出指定 region 的节点，region 为空则返回全部
std::vector<NodeInfo> ServerRegistry::List(const std::string& region) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !zk_) {
        // 如果未启用或未连接，直接返回缓存的节点列表
        if (region.empty()) {
            return nodes_;
        }
        std::vector<NodeInfo> filtered; // 过滤指定 region 的节点
        for (const auto& n : nodes_) {
            if (n.region == region) {
                filtered.push_back(n);
            }
        }
        if (filtered.empty()) {
            return nodes_; // 如果没有匹配的，返回全部
        }
        return filtered;
    }

    // 获取指定 region 的节点列表
    std::string base = "/meeting/servers/" + (region.empty() ? std::string("default") : region);
    // 异步获取子节点列表
    std::promise<std::pair<int, String_vector>> p; // 用于接收回调结果
    auto f = p.get_future();
    int rc = zoo_aget_children(zk_, base.c_str(), 0, StringsCompletion, &p); // 异步获取子节点
    if (rc != ZOK) {
        p.set_value({rc, {}});
    }
    auto res = Wait(f, zk_); // 等待结果

    // 解析结果
    std::vector<NodeInfo> result;
    if (res.first == ZOK) {
        for (int i = 0; i < res.second.count; ++i) {
            std::string name(res.second.data[i]);
            auto pos = name.find(':');
            if (pos == std::string::npos) {
                continue;
            }
            NodeInfo n;
            n.host = name.substr(0, pos);
            n.port = std::atoi(name.substr(pos + 1).c_str());
            n.region = region.empty() ? "default" : region;
            result.push_back(n);
        }
    }
    deallocate_String_vector(&res.second);
    if (result.empty()) {
        return nodes_;
    }
    return result;
}

bool ServerRegistry::EnsureConnected() {
    if (!enabled_) {
        return false;
    }
    if (zk_) {
        return true;
    }

    zk_ = zookeeper_init(zk_hosts_.c_str(), nullptr, 30000, 0, nullptr, 0);
    if (!zk_) {
        MEETING_LOG_ERROR("[ServerRegistry] connect zookeeper failed: {}", zk_hosts_);
        return false;
    }

    // 等待连接建立（最多等待5秒），显式驱动非线程化客户端
    int max_retries = 50;  // 50 * 100ms = 5秒
    for (int i = 0; i < max_retries; ++i) {
        int fd = -1;
        int interest = 0;
        struct timeval tv {};
        if (zookeeper_interest(zk_, &fd, &interest, &tv) != ZOK) {
            break;
        }

        int state = zoo_state(zk_);
        if (state == ZOO_CONNECTED_STATE) {
            MEETING_LOG_INFO("[ServerRegistry] connected to zookeeper: {}", zk_hosts_);
            return true;
        }

        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        if (interest & ZOOKEEPER_READ) FD_SET(fd, &rfds);
        if (interest & ZOOKEEPER_WRITE) FD_SET(fd, &wfds);

        int rc = select(fd + 1, &rfds, &wfds, nullptr, &tv);
        if (rc < 0) {
            break;
        }
        int events = 0;
        if (FD_ISSET(fd, &rfds)) events |= ZOOKEEPER_READ;
        if (FD_ISSET(fd, &wfds)) events |= ZOOKEEPER_WRITE;
        zookeeper_process(zk_, events);
    }

    int state = zoo_state(zk_);
    MEETING_LOG_WARN("[ServerRegistry] zookeeper connection timeout (state={}), disable registry", state);
    zookeeper_close(zk_);
    zk_ = nullptr;
    enabled_ = false;
    return false;
}

// 确保指定路径存在
int ServerRegistry::EnsurePath(const std::string& path, bool ephemeral, const std::string& data) {
    int flags = ephemeral ? ZOO_EPHEMERAL : 0; // 节点类型标志

    // 检查节点是否存在
    std::promise<int> p;
    auto f = p.get_future();
    int rc = zoo_aexists(zk_, path.c_str(), 0, StatCompletion, &p); // 异步检查节点是否存在
    if (rc != ZOK) {
        p.set_value(rc);
    }
    rc = Wait(f, zk_);

    // 如果节点不存在，则创建节点
    if (rc == ZNONODE) {
        std::promise<int> c;
        auto cf = c.get_future();
        rc = zoo_acreate(zk_, path.c_str(), data.data(), static_cast<int>(data.size()),
                         &ZOO_OPEN_ACL_UNSAFE, flags, CreateCompletion, &c);
        if (rc != ZOK) {
            c.set_value(rc);
        }
        rc = Wait(cf, zk_);
    }
    return rc;
}

} // namespace registry
} // namespace meeting
