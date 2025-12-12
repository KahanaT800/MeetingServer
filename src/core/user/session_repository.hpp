#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace meeting {
namespace core {

struct SessionRecord {
    std::string  token;
    std::uint64_t user_id = 0;   // 数值型用户ID
    std::string  user_uuid;      // 字符串用户UUID
    std::int64_t expires_at = 0;
};

class SessionRepository {
public:
    virtual ~SessionRepository() = default;

    virtual meeting::common::Status CreateSession(const SessionRecord& record) = 0;
    virtual meeting::common::StatusOr<SessionRecord> ValidateSession(const std::string& token) = 0;
    virtual meeting::common::Status DeleteSession(const std::string& token) = 0;
};

class InMemorySessionRepository : public SessionRepository {
public:
    meeting::common::Status CreateSession(const SessionRecord& record) override;
    meeting::common::StatusOr<SessionRecord> ValidateSession(const std::string& token) override;
    meeting::common::Status DeleteSession(const std::string& token) override;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SessionRecord> sessions_;
};

} // namespace core
} // namespace meeting
