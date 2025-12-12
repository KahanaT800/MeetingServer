#include "storage/mysql/connection_pool.hpp"

#include <chrono>

namespace meeting {
namespace storage {

ConnectionPool::ConnectionPool(Options options): options_(std::move(options)) {}

// 连接租赁
ConnectionPool::Lease::Lease(ConnectionPool* pool, std::unique_ptr<Connection> connection)
    : pool_(pool), connection_(std::move(connection)) {}
ConnectionPool::Lease::Lease(Lease&& other) noexcept
    : pool_(other.pool_), connection_(std::move(other.connection_)) {
    other.pool_ = nullptr;
}

ConnectionPool::Lease& ConnectionPool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        // 归还当前连接
        if (pool_ && connection_) {
            pool_->Return(std::move(connection_));
        }
        pool_ = other.pool_;
        connection_ = std::move(other.connection_);
        other.pool_ = nullptr;
    }
    return *this;
}

ConnectionPool::Lease::~Lease() {
    if (pool_ && connection_) {
        pool_->Return(std::move(connection_));
    }
}

meeting::common::StatusOr<ConnectionPool::Lease> ConnectionPool::Acquire() {
    std::unique_ptr<Connection> connection;

    {
        std::unique_lock lock(mutex_);
        // 策略1: 有空闲连接 -> 直接使用
        if (!connections_.empty()) {
            // 直接获取可用连接
            connection = std::move(connections_.front());
            connections_.pop();

        // 策略2: 未达最大连接数 -> 创建新连接
        } else if (total_connections_ < options_.pool_size) {
            ++total_connections_;
            lock.unlock();
            auto new_connection = CreateConnection();
            if (!new_connection.IsOk()) {
                std::lock_guard<std::mutex> guard(mutex_);
                --total_connections_;
                cv_.notify_one();
                return new_connection.GetStatus();
            }
            connection = std::move(new_connection.Value());
        
        // 策略3: 达到最大连接数 -> 等待归还或超时
        } else {
            auto timeout = options_.acquire_timeout;
            if (!cv_.wait_for(lock, timeout, [this]() { return !connections_.empty(); })) {
                return meeting::common::Status::Unavailable("Acquire connection timeout");
            }
            connection = std::move(connections_.front());
            connections_.pop();
        }
    }
    return meeting::common::StatusOr<Lease>(Lease(this, std::move(connection)));
}

meeting::common::StatusOr<std::unique_ptr<Connection>> ConnectionPool::CreateConnection() {
    return Connection::Create(options_);
}

void ConnectionPool::Return(std::unique_ptr<Connection> connection) {
    // 验证连接是否有效
    if (mysql_ping(connection->Raw()) != 0) {
        // 连接已断开，丢弃并减少计数
        std::lock_guard<std::mutex> lock(mutex_);
        --total_connections_;
        cv_.notify_one();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.push(std::move(connection));
    }
    cv_.notify_one();
}

} // namespace storage
} // namespace meeting