#pragma once

#include "core/user/user_repository.hpp"
#include "storage/mysql/connection_pool.hpp"

#include <memory>

namespace meeting {
namespace storage {

// 基于 MySQL 的用户存储实现
class MySQLUserRepository : public meeting::core::UserRepository {
public:
    explicit MySQLUserRepository(std::shared_ptr<ConnectionPool> pool);

    // 重写基类方法
    meeting::common::Status CreateUser(const meeting::core::UserData& data) override;
    meeting::common::StatusOr<meeting::core::UserData> FindByUserName(const std::string& user_name) const override;
    meeting::common::StatusOr<meeting::core::UserData> FindById(const std::string& id) const override;
    meeting::common::Status UpdateLastLogin(const std::string& user_id, std::int64_t last_login) override;

private:
    // 辅助函数, 执行查询并返回单个用户数据
    meeting::common::StatusOr<meeting::core::UserData> QuerySingle(ConnectionPool::Lease& lease, const std::string& sql) const;

    // 辅助函数, 转义并加引号字符串值
    static std::string EscapeAndQuote(MYSQL* conn, const std::string& value);
private:
    std::shared_ptr<ConnectionPool> pool_;
};

}  // namespace storage
} // namespace meeting