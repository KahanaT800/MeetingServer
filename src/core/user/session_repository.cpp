#include "core/user/session_repository.hpp"

#include <chrono>
#include <mutex>

namespace meeting {
namespace core {

meeting::common::Status InMemorySessionRepository::CreateSession(const SessionRecord& record) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    sessions_[record.token] = record;
    return meeting::common::Status::OK();
}

meeting::common::StatusOr<SessionRecord> InMemorySessionRepository::ValidateSession(const std::string& token) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return meeting::common::Status::Unauthenticated("Session not found");
    }
    const auto& rec = it->second;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    if (rec.expires_at != 0 && rec.expires_at < now) {
        return meeting::common::Status::Unauthenticated("Session expired");
    }
    return meeting::common::StatusOr<SessionRecord>(rec);
}

meeting::common::Status InMemorySessionRepository::DeleteSession(const std::string& token) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return meeting::common::Status::NotFound("Session not found");
    }
    sessions_.erase(it);
    return meeting::common::Status::OK();
}

} // namespace core
} // namespace meeting
