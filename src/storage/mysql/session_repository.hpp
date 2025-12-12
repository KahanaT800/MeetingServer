#pragma once

#include "core/user/session_repository.hpp"
#include "storage/mysql/connection_pool.hpp"

#include <memory>

namespace meeting {
namespace storage {

class MySqlSessionRepository : public meeting::core::SessionRepository {
public:
    explicit MySqlSessionRepository(std::shared_ptr<ConnectionPool> pool);

    meeting::common::Status CreateSession(const meeting::core::SessionRecord& record) override;
    meeting::common::StatusOr<meeting::core::SessionRecord> ValidateSession(const std::string& token) override;
    meeting::common::Status DeleteSession(const std::string& token) override;

private:
    std::shared_ptr<ConnectionPool> pool_;
};

} // namespace storage
} // namespace meeting
