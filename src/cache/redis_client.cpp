#include "cache/redis_client.hpp"

// 网络相关头文件
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// 标准库头文件
#include <chrono>
#include <cerrno>
#include <cstring>
#include <sstream>

namespace meeting {
namespace cache {

// 构造函数
RedisClient::RedisClient(const meeting::common::RedisConfig& config)
    : config_(config) {}

// 析构函数
RedisClient::~RedisClient() = default;

// 连接到Redis服务器
meeting::common::Status RedisClient::Connect() {
    if (!config_.enabled) {
        return meeting::common::Status::Unavailable("Redis is disabled in the configuration.");
    }
    if (redis_) {
        return meeting::common::Status::OK(); // 已经连接
    }

    try {
        sw::redis::ConnectionOptions opts; // Redis连接选项
        // 设置连接选项
        opts.host = config_.host;
        opts.port = config_.port;
        if (!config_.password.empty()) {
            opts.password = config_.password;
        }
        opts.db = config_.db;
        opts.connect_timeout = std::chrono::milliseconds(config_.connection_timeout_ms);
        opts.socket_timeout = std::chrono::milliseconds(config_.socket_timeout_ms);

        // 创建连接池选项
        sw::redis::ConnectionPoolOptions pool_opts;
        pool_opts.size = static_cast<size_t>(config_.pool_size);
        pool_opts.wait_timeout = std::chrono::milliseconds(config_.connection_timeout_ms);

        // 创建Redis连接对象
        redis_ = std::make_shared<sw::redis::Redis>(opts, pool_opts);
        return meeting::common::Status::OK();
    } catch (const sw::redis::Error& err) {
        return meeting::common::Status::Unavailable("Failed to connect to Redis: " + std::string(err.what()));
    }
}

// 设置键值对
meeting::common::Status RedisClient::Set(const std::string& key, const std::string& value) {
    auto status = Connect();
    if (!status.IsOk()) {
        return status;
    }

    try {
        redis_->set(key, value);
        return meeting::common::Status::OK();
    } catch (const sw::redis::Error& err) {
        return meeting::common::Status::Unavailable("Failed to set key in Redis: " + std::string(err.what()));
    }
}

// 设置键值对并设置过期时间
meeting::common::Status RedisClient::SetEx(const std::string& key, const std::string& value, int ttl_seconds) {
    auto status = Connect();
    if (!status.IsOk()) {
        return status;
    }

    try {
        redis_->set(key, value, std::chrono::seconds(ttl_seconds));
        return meeting::common::Status::OK();
    } catch (const sw::redis::Error& err) {
        return meeting::common::Status::Unavailable("Failed to set key with expiration in Redis: " + std::string(err.what()));
    }
}

// 获取键对应的值
meeting::common::StatusOr<std::string> RedisClient::Get(const std::string& key) {
    auto status = Connect();
    if (!status.IsOk()) {
        return status;
    }

    try {
        auto val = redis_->get(key);
        if (!val) {
            return meeting::common::Status::NotFound("Key not found in Redis: " + key);
        }
        return meeting::common::StatusOr<std::string>(*val);
    } catch (const sw::redis::Error& err) {
        return meeting::common::Status::Unavailable("Failed to get key from Redis: " + std::string(err.what()));
    }
}

// 删除键
meeting::common::Status RedisClient::Del(const std::string& key) {
    auto status = Connect();
    if (!status.IsOk()) {
        return status;
    }

    try {
        redis_->del(key);
        return meeting::common::Status::OK();
    } catch (const sw::redis::Error& err) {
        return meeting::common::Status::Unavailable("Failed to delete key from Redis: " + std::string(err.what()));
    }
}

// 检查键是否存在
meeting::common::StatusOr<bool> RedisClient::Exists(const std::string& key) {
    auto status = Connect();
    if (!status.IsOk()) {        
        return status;
    }

    try {
        auto count = redis_->exists(key);
        return meeting::common::StatusOr<bool>(count > 0);
    } catch (const sw::redis::Error& err) {
        return meeting::common::Status::Unavailable("Failed to check key existence in Redis: " + std::string(err.what()));
    }
}

} // namespace cache
} // namespace meeting