#include "storage/mysql/transaction.hpp"

namespace meeting {
namespace storage {

Transaction::Transaction(std::shared_ptr<ConnectionPool> pool)
    : pool_(std::move(pool)) {}

Transaction::~Transaction() {
    if (active_) {
        Rollback();
    }
}

// 开始事务
meeting::common::Status Transaction::Begin() {
    // 获取连接租赁
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    lease_ = std::move(lease_or.Value());
    conn_ = lease_.Raw();
    // 设置连接为非自动提交模式
    if (mysql_autocommit(conn_, 0) != 0) {
        return meeting::common::Status::Internal(mysql_error(conn_));
    }
    // 标记事务为活跃状态
    active_ = true;
    return meeting::common::Status::OK();
}

meeting::common::Status Transaction::Commit() {
    if (!active_) {
        // 如果事务不活跃, 直接返回 OK
        return meeting::common::Status::OK();
    }
    // 提交事务
    if (mysql_commit(conn_) != 0) {
        return meeting::common::Status::Internal(mysql_error(conn_));
    }
    // 恢复自动提交模式
    mysql_autocommit(conn_, 1);
    // 成功提交, 标记事务为非活跃状态
    active_ = false;
    return meeting::common::Status::OK();
}

meeting::common::Status Transaction::Rollback() {
    if (!active_) {
        // 如果事务不活跃, 直接返回 OK
        return meeting::common::Status::OK();
    }
    // 回滚事务
    mysql_rollback(conn_);
    // 恢复自动提交模式
    mysql_autocommit(conn_, 1);
    // 标记事务为非活跃状态
    active_ = false;
    return meeting::common::Status::OK();
}

}
}