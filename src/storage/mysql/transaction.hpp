#pragma once

#include "common/status.hpp"
#include "storage/mysql/connection_pool.hpp"

#include <memory>

namespace meeting {
namespace storage {

// MySQL事务管理类 
class Transaction {
public:
    explicit Transaction(std::shared_ptr<ConnectionPool> pool);
    ~Transaction();

    meeting::common::Status Begin(); // 开始事务
    meeting::common::Status Commit(); // 提交事务
    meeting::common::Status Rollback(); // 回滚事务

    // 获取原始MySQL连接指针
    MYSQL* Raw() const noexcept {return  conn_;}
private:
    std::shared_ptr<ConnectionPool> pool_;
    ConnectionPool::Lease lease_;
    MYSQL* conn_ = nullptr;
    bool active_ = false;
};

}
}