#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"
#include "storage/mysql/connection.hpp"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <memory>

namespace meeting {
namespace storage {

class ConnectionPool {
public:
    explicit ConnectionPool(Options options);

    // 连接租赁类, RAII管理连接的获取和归还
    class Lease {
    public:
        Lease() = default;
        // 构造函数，初始化连接租赁对象
        Lease(ConnectionPool* pool, std::unique_ptr<Connection> connection);
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        Connection* operator->() noexcept { return connection_.get(); }
        Connection& operator*() noexcept { return *connection_; }
        MYSQL* Raw() const noexcept { return connection_ ? connection_->Raw() : nullptr; }
        explicit operator bool() const noexcept { return connection_ != nullptr; }
    private:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ConnectionPool* pool_ = nullptr;
        std::unique_ptr<Connection> connection_;
    };
    // 获取连接租赁对象
    meeting::common::StatusOr<Lease> Acquire();

private:
    // 创建新的连接
    meeting::common::StatusOr<std::unique_ptr<Connection>> CreateConnection();
    // 归还连接到连接池
    void Return(std::unique_ptr<Connection> connection);

    Options options_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::unique_ptr<Connection>> connections_; // 空闲连接队列
    std::size_t total_connections_ = 0; // 总连接数
};

}// namespace storage
} // namespace meeting