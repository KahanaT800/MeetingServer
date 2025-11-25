#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"
#include "core/user/errors.hpp"

#include <memory>
#include <optional>
#include <string>

namespace meeting {
namespace core {
    
struct RegisterCommand {
  std::string user_name;
  std::string password;
  std::string email;
  std::string display_name;
};

struct LoginCommand {
  std::string user_name;
  std::string password;
  std::string client_ip;
  std::string user_agent;
};

struct UserData {
  std::string user_id;
  std::string user_name;
  std::string display_name;
  std::string email;
  std::string password_hash;
  std::string salt;
  std::int64_t created_at = 0;
  std::int64_t last_login = 0;
};

class UserManager {
public:
    using Status = meeting::common::Status;
    using StatusOrUser = meeting::common::StatusOr<UserData>;

    explicit UserManager(std::shared_ptr<class UserRepository> repository = nullptr);

    // 用户管理
    Status RegisterUser(const RegisterCommand& command);
    StatusOrUser LoginUser(const LoginCommand& command);
    Status LogoutUser(const std::string& user_name);
    StatusOrUser GetUserByUserName(const std::string& user_name) const;
    StatusOrUser GetUserById(const std::string& user_id) const;

private:
    std::string GenerateUserId() const;
    std::string GenerateSalt() const;
    std::string HashPassword(const std::string& password, const std::string& salt) const;

    std::shared_ptr<UserRepository> repository_;
};

}
}

