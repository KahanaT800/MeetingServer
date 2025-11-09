#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"

#include <chrono>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace meeting {
namespace core {

// 会话数据结构
struct SessionData {
    std::string token;
    std::string user_id;
    std::string client_ip;
    std::string user_agent;
    std::int64_t issued_at = 0;
    std::int64_t expires_at = 0;
};

struct SessionConfig {
    std::chrono::seconds ttl = std::chrono::seconds(3600);
};

class SessionManager {
public:
    using Status = meeting::common::Status;
    using StatusOrSession = meeting::common::StatusOr<SessionData>;

    explicit SessionManager(const SessionConfig& config = SessionConfig());

    // 创建新会话
    StatusOrSession CreateSession(const std::string& user_id
                                  , const std::string& client_ip
                                  , const std::string& user_agent);

    // 验证会话令牌
    StatusOrSession ValidateSession(const std::string& token
                                    , const std::string& client_ip
                                    , const std::string& user_agent);

    // 终止会话
    Status DeleteSession(const std::string& token);
private:
    std::string GenerateToken() const;

    SessionConfig config_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SessionData> sessions_;
};

}
}