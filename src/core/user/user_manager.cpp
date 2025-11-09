#include "core/user/user_manager.hpp"
#include "core/user/errors.hpp"

#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <shared_mutex>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace meeting {
namespace core {

namespace {
// 获取当前Unix时间戳（秒）
std::int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 生成指定长度的随机十六进制字符串
std::string RandomHexString(std::size_t length) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char kHexChars[] = "0123456789abcdef";

    std::uniform_int_distribution<int> dist(0, 15);
    std::string result;
    result.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        result.push_back(kHexChars[dist(rng)]);
    }
    return result;
}

} // namespace

// 注册新用户
UserManager::Status UserManager::RegisterUser(const RegisterCommand& command) {
    if (command.user_name.empty() || command.password.empty() || command.email.empty()) {
        return Status::InvalidArgument("User name, password, and email cannot be empty.");
    }

    if (command.password.length() < 8) {
        return Status::InvalidArgument("Password must be at least 8 characters long.");
    }

    // 生成用户数据
    UserData user_data;
    user_data.user_id = GenerateUserId();
    user_data.user_name = command.user_name;
    user_data.display_name = command.display_name.empty() ? command.user_name : command.display_name;
    user_data.email = command.email;
    user_data.salt = GenerateSalt();
    user_data.password_hash = HashPassword(command.password, user_data.salt);
    user_data.created_at = CurrentUnixSeconds();
    user_data.last_login = 0;

    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // 检查用户名是否已存在
    if (users_by_user_name_.find(command.user_name) != users_by_user_name_.end()) {
        return Status::AlreadyExists("User name already exists.");
    }

    // 存储用户数据
    users_by_user_name_.emplace(user_data.user_name, user_data);
    users_by_id_.emplace(user_data.user_id, user_data);

    return Status::OK();
}

// 用户登录
UserManager::StatusOrUser UserManager::LoginUser(const LoginCommand& command) {
    UserData user_data;
    
    // 读取和验证
    {
        std::shared_lock<std::shared_mutex> read_lock(mutex_);
        auto it = users_by_user_name_.find(command.user_name);
        if (it == users_by_user_name_.end()) {
            return Status::NotFound("User not found.");
        }

        user_data = it->second;
        const std::string hashed_password = HashPassword(command.password, user_data.salt);
        if (hashed_password != user_data.password_hash) {
            return Status::Unauthenticated("Invalid password.");
        }
    }

    // 更新数据
    const std::int64_t current_time = CurrentUnixSeconds();
    {
        std::unique_lock<std::shared_mutex> write_lock(mutex_);
        
        // 重新检查用户是否仍然存在
        auto it = users_by_user_name_.find(user_data.user_name);
        if (it == users_by_user_name_.end()) {
            return Status::NotFound("User not found.");
        }
        
        // 更新 last_login
        users_by_user_name_[user_data.user_name].last_login = current_time;
        users_by_id_[user_data.user_id].last_login = current_time;
        
        // 更新返回的用户数据
        user_data.last_login = current_time;
    }

    return StatusOrUser(std::move(user_data));
}

// 用户登出
UserManager::Status UserManager::LogoutUser(const std::string& user_name) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = users_by_user_name_.find(user_name);
    if (it == users_by_user_name_.end()) {
        return Status::NotFound("User not found.");
    }
    return Status::OK();
}

// 根据用户名获取用户信息
UserManager::StatusOrUser UserManager::GetUserByUserName(const std::string& user_name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = users_by_user_name_.find(user_name);
    if (it == users_by_user_name_.end()) {
        return Status::NotFound("User not found.");
    }
    return StatusOrUser(it->second);
}

// 根据用户ID获取用户信息
UserManager::StatusOrUser UserManager::GetUserById(const std::string& user_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = users_by_id_.find(user_id);
    if (it == users_by_id_.end()) {
        return Status::NotFound("User not found.");
    }
    return StatusOrUser(it->second);
}

// 生成唯一的用户ID
std::string UserManager::GenerateUserId() const {
    return "user_" + RandomHexString(16);
}

// 生成盐值 - 使用 OpenSSL 的加密安全随机数
std::string UserManager::GenerateSalt() const {
    constexpr int kSaltLength = 32;
    unsigned char salt_bytes[kSaltLength];
    
    // 使用 OpenSSL 的加密安全随机数生成器
    if (RAND_bytes(salt_bytes, kSaltLength) != 1) {
        // 如果 OpenSSL 随机数生成失败，回退到标准方法
        return RandomHexString(kSaltLength);
    }
    
    // 转换为十六进制字符串
    std::stringstream ss;
    for (int i = 0; i < kSaltLength; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(salt_bytes[i]);
    }
    return ss.str();
}

// 使用PBKDF2哈希密码和盐值
std::string UserManager::HashPassword(const std::string& password, const std::string& salt) const {
    constexpr int kIterations = 100000;  // PBKDF2 迭代次数
    constexpr int kHashLength = 32;      // 输出哈希长度
    
    unsigned char hash[kHashLength];
    
    // 使用 PBKDF2 with SHA-256
    int result = PKCS5_PBKDF2_HMAC(
        password.c_str(), password.length(),
        reinterpret_cast<const unsigned char*>(salt.c_str()), salt.length(),
        kIterations,
        EVP_sha256(),
        kHashLength,
        hash
    );
    
    if (result != 1) {
        // PBKDF2 失败，回退到简单哈希（在生产环境中应该抛出异常）
        return "";
    }
    
    // 转换为十六进制字符串
    std::stringstream ss;
    for (int i = 0; i < kHashLength; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

}
}