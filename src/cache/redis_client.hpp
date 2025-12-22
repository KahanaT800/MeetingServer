#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"
#include "common/config.hpp"

// Redis++库头文件
#include <sw/redis++/redis++.h>

#include <string>
#include <memory>

namespace meeting {
namespace cache {

class RedisClient {
public:
    explicit RedisClient(const meeting::common::RedisConfig& config);
    ~RedisClient();

    // 连接到Redis服务器
    meeting::common::Status Connect();

    // 设置键值对
    meeting::common::Status Set(const std::string& key, const std::string& value);
    // 设置键值对并设置过期时间
    meeting::common::Status SetEx(const std::string& key, const std::string& value, int ttl_seconds);

    // 获取键对应的值
    meeting::common::StatusOr<std::string> Get(const std::string& key);

    // 删除键
    meeting::common::Status Del(const std::string& key);

    // 检查键是否存在
    meeting::common::StatusOr<bool> Exists(const std::string& key);
    
private:
    meeting::common::RedisConfig config_; // Redis配置
    std::shared_ptr<sw::redis::Redis> redis_; // Redis连接对象
};

} // namespace cache
} // namespace meeting