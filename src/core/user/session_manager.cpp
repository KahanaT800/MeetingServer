#include "core/user/session_manager.hpp"

#include <random>
#include <mutex>

namespace meeting {
namespace core {

namespace {

std::int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string RandomToken(size_t length) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char kChars[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::uniform_int_distribution<int> dist(0, sizeof(kChars) - 2);
    std::string token;
    token.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        token.push_back(kChars[dist(rng)]);
    }
    return token;
}

} // namespace

SessionManager::SessionManager(const SessionConfig& config) : config_(config) {}

SessionManager::StatusOrSession SessionManager::CreateSession(const std::string& user_id
                                                              , const std::string& client_ip
                                                              , const std::string& user_agent) {
    SessionData session;
    session.token = GenerateToken();
    session.user_id = user_id;
    session.client_ip = client_ip;
    session.user_agent = user_agent;
    session.issued_at = CurrentUnixSeconds();
    session.expires_at = session.issued_at + config_.ttl.count();

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        sessions_[session.token] = session;
    }

    return StatusOrSession(std::move(session));
}

SessionManager::StatusOrSession SessionManager::ValidateSession(const std::string& token
                                                                , const std::string& client_ip
                                                                , const std::string& user_agent) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return Status::Unauthenticated("Invalid session token.");
    }
    
    const SessionData& session = it->second;
    
    // 检查会话是否已过期
    if (session.expires_at < CurrentUnixSeconds()) {
        // 删除过期的会话
        sessions_.erase(it);
        return Status::Unauthenticated("Session expired.");
    }
    
    // 返回有效的会话数据
    return StatusOrSession(std::move(session));
}

SessionManager::Status SessionManager::DeleteSession(const std::string& token) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return Status::NotFound("Session token not found.");
    }
    sessions_.erase(it);
    return Status::OK();
}

std::string SessionManager::GenerateToken() const {
    return RandomToken(32); 
}

}
}