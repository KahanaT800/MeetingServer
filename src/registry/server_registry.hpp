#pragma once

// 接入zookeeper头文件
#include <zookeeper/zookeeper.h>

#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <future>

namespace meeting{
namespace registry {

struct NodeInfo {
    std::string host;
    int port = 0;
    std::string region = "default";
    int weight = 1;
    std::string meta_json;
};

// 服务器注册中心
class ServerRegistry {
public:
    // 构造函数，传入zookeeper的连接地址
    explicit ServerRegistry(std::string zk_hosts);
    ~ServerRegistry();

    // 注册本节点
    void Register(const NodeInfo& node);
    // 注销本节点
    void Unregister(const NodeInfo& node);

    bool Enabled() const {return enabled_;}
    // 列出指定 region 的节点，region 为空则返回全部
    std::vector<NodeInfo> List(const std::string& region) const;
private:
    // 确保与zookeeper的连接
    bool EnsureConnected();
    // 确保指定路径存在
    int EnsurePath(const std::string& path, bool ephemeral, const std::string& data);
private:
    std::string zk_hosts_; // zookeeper 连接地址
    bool enabled_ = false; // 是否启用注册功能
    mutable std::mutex mutex_; // 保护 nodes_
    std::vector<NodeInfo> nodes_; // 缓存的节点列表
    zhandle_t* zk_ = nullptr; // zookeeper 句柄
};

} // namespace registry
} // namespace meeting