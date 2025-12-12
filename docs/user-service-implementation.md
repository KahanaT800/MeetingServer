# User Service 实现详解

本文档详细说明 MeetingServer 中 **用户服务 (UserService)** 的完整实现流程和技术细节。

## 目录

1. [服务概述](#服务概述)
2. [架构分层](#架构分层)
3. [Register - 用户注册](#register---用户注册)
4. [Login - 用户登录](#login---用户登录)
5. [Logout - 用户登出](#logout---用户登出)
6. [GetProfile - 获取用户资料](#getprofile---获取用户资料)
7. [安全机制详解](#安全机制详解)
8. [存储实现对比](#存储实现对比)
9. [错误处理机制](#错误处理机制)
10. [性能优化策略](#性能优化策略)

---

## 服务概述

UserService 提供完整的用户账号管理功能，包括：
- 用户注册与身份验证
- 基于 Token 的会话管理
- 用户资料查询
- 多存储后端支持（内存/MySQL）

**技术特点**:
- 异步处理：线程池异步执行业务逻辑
- 安全加密：PBKDF2-HMAC-SHA256 密码哈希
- 灵活存储：Repository 模式支持多种后端
- 完善错误处理：统一的 Status 错误传递机制

---

## 架构分层

```
┌─────────────────────────────────────────┐
│   gRPC Service Layer                    │
│   UserServiceImpl                       │  ← 处理 gRPC 请求/响应
├─────────────────────────────────────────┤
│   Business Logic Layer                  │
│   UserManager + SessionManager          │  ← 业务规则验证
├─────────────────────────────────────────┤
│   Repository Interface Layer            │
│   UserRepository + SessionRepository    │  ← 抽象接口
├─────────────────────────────────────────┤
│   Storage Implementation Layer          │
│   InMemory / MySQL                      │  ← 具体实现
└─────────────────────────────────────────┘
```

### 关键组件

| 组件 | 职责 | 位置 |
|-----|------|------|
| `UserServiceImpl` | gRPC 服务端点实现 | `src/server/user_service_impl.cpp` |
| `UserManager` | 用户业务逻辑管理 | `src/core/user/user_manager.cpp` |
| `SessionManager` | 会话生命周期管理 | `src/core/user/session_manager.cpp` |
| `UserRepository` | 用户数据访问抽象 | `src/core/user/user_repository.hpp` |
| `SessionRepository` | 会话数据访问抽象 | `src/core/user/session_repository.hpp` |
| `MySQLUserRepository` | MySQL 用户存储实现 | `src/storage/mysql/user_repository.cpp` |
| `MySqlSessionRepository` | MySQL 会话存储实现 | `src/storage/mysql/session_repository.cpp` |

---

## Register - 用户注册

### 1. 接口定义

**Proto 定义**:
```protobuf
message RegisterRequest {
    string user_name    = 1;  // 登录用户名
    string password     = 2;  // 密码
    string email        = 3;  // 电子邮件
    string display_name = 4;  // 显示名称
}

message RegisterResponse {
    .proto.common.Error    error = 1;  // 错误信息
    .proto.common.UserInfo user  = 2;  // 用户信息
}
```

### 2. 处理流程

```
Client Request
    ↓
UserServiceImpl::Register
    ↓
[线程池异步提交]
    ↓
UserManager::RegisterUser
    ↓
┌─ 参数验证 ─────────────────────┐
│ • 用户名不为空                  │
│ • 密码长度 >= 8                 │
│ • 邮箱不为空                    │
└────────────────────────────────┘
    ↓
┌─ 生成用户数据 ─────────────────┐
│ • user_id: "user_" + 随机16字符 │
│ • salt: 64字符随机盐值          │
│ • password_hash: PBKDF2哈希     │
│ • created_at: 当前Unix时间戳    │
└────────────────────────────────┘
    ↓
UserRepository::CreateUser
    ↓
Storage Layer (MySQL/Memory)
    ↓
[MySQL] INSERT INTO users (...)
    ↓
Response with UserInfo
```

### 3. 核心代码实现

#### 3.1 服务层 (UserServiceImpl)

```cpp
grpc::Status UserServiceImpl::Register(
    grpc::ServerContext*,
    const proto::user::RegisterRequest* request,
    proto::user::RegisterResponse* response) {
    
    // 构造注册命令
    meeting::core::RegisterCommand command{
        request->user_name(),
        request->password(),
        request->email(),
        request->display_name()
    };
    
    MEETING_LOG_INFO("[UserService] Register user={}", command.user_name);
    
    // 提交到线程池异步执行
    auto future = thread_pool_.Submit([this, command]() {
        return user_manager_->RegisterUser(command);
    });
    
    // 等待执行结果
    meeting::common::Status status = future.get();
    
    // 错误处理
    if (!status.IsOk()) {
        meeting::core::UserErrorCode error_code = /* 映射错误码 */;
        meeting::core::ErrorToProto(error_code, status, response->mutable_error());
        return ToGrpcStatus(status);
    }
    
    // 获取用户数据并填充响应
    auto user_data = user_manager_->GetUserByUserName(command.user_name);
    if (user_data.IsOk()) {
        FillUserInfo(user_data.Value(), response->mutable_user());
    }
    
    return grpc::Status::OK;
}
```

#### 3.2 业务逻辑层 (UserManager)

```cpp
UserManager::Status UserManager::RegisterUser(const RegisterCommand& command) {
    // 1. 参数验证
    if (command.user_name.empty() || 
        command.password.empty() || 
        command.email.empty()) {
        return Status::InvalidArgument(
            "User name, password, and email cannot be empty.");
    }
    
    if (command.password.length() < 8) {
        return Status::InvalidArgument(
            "Password must be at least 8 characters long.");
    }
    
    // 2. 生成用户数据
    UserData user_data;
    user_data.user_id = GenerateUserId();        // "user_" + 16位随机
    user_data.user_name = command.user_name;
    user_data.display_name = command.display_name.empty() 
        ? command.user_name 
        : command.display_name;
    user_data.email = command.email;
    user_data.salt = GenerateSalt();             // 64位随机盐
    user_data.password_hash = HashPassword(command.password, user_data.salt);
    user_data.created_at = CurrentUnixSeconds();
    user_data.last_login = 0;
    
    // 3. 存储到数据库
    return repository_->CreateUser(user_data);
}
```

#### 3.3 密码加密 (PBKDF2-HMAC-SHA256)

```cpp
std::string UserManager::HashPassword(
    const std::string& password, 
    const std::string& salt) const {
    
    constexpr int kIterations = 100000;  // PBKDF2 迭代次数
    constexpr int kHashLength = 32;      // 输出哈希长度
    
    unsigned char hash[kHashLength];
    
    // 使用 PBKDF2 with SHA-256
    int result = PKCS5_PBKDF2_HMAC(
        password.c_str(), password.length(),
        reinterpret_cast<const unsigned char*>(salt.c_str()), 
        salt.length(),
        kIterations,
        EVP_sha256(),
        kHashLength,
        hash
    );
    
    if (result != 1) {
        return "";  // 生产环境应抛出异常
    }
    
    // 转换为十六进制字符串
    std::stringstream ss;
    for (int i = 0; i < kHashLength; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') 
           << static_cast<int>(hash[i]);
    }
    return ss.str();
}
```

#### 3.4 盐值生成 (OpenSSL 加密随机数)

```cpp
std::string UserManager::GenerateSalt() const {
    constexpr int kSaltLength = 32;
    unsigned char salt_bytes[kSaltLength];
    
    // 使用 OpenSSL 的加密安全随机数生成器
    if (RAND_bytes(salt_bytes, kSaltLength) != 1) {
        // 回退到标准方法
        return RandomHexString(kSaltLength);
    }
    
    // 转换为十六进制字符串
    std::stringstream ss;
    for (int i = 0; i < kSaltLength; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') 
           << static_cast<int>(salt_bytes[i]);
    }
    return ss.str();
}
```

#### 3.5 MySQL 存储实现

```cpp
meeting::common::Status MySQLUserRepository::CreateUser(
    const meeting::core::UserData& data) {
    
    // 获取连接
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    
    // 构造 SQL (使用预编译语句防止 SQL 注入)
    auto sql = fmt::format(
        "INSERT INTO users "
        "(user_uuid, username, display_name, email, password_hash, salt, status) "
        "VALUES ({}, {}, {}, {}, {}, {}, 1)",
        EscapeAndQuote(conn, data.user_id),
        EscapeAndQuote(conn, data.user_name),
        EscapeAndQuote(conn, data.display_name),
        EscapeAndQuote(conn, data.email),
        EscapeAndQuote(conn, data.password_hash),
        EscapeAndQuote(conn, data.salt)
    );
    
    // 执行插入
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);  // 处理重复用户名等错误
    }
    
    return meeting::common::Status::OK();
}
```

### 4. 错误场景处理

| 场景 | 错误码 | HTTP状态码 | 处理方式 |
|-----|--------|-----------|---------|
| 用户名已存在 | `kUserNameExists` | `ALREADY_EXISTS` | 返回错误信息 |
| 密码长度不足 | `kInvalidPassword` | `INVALID_ARGUMENT` | 返回错误信息 |
| 参数为空 | `kInvalidPassword` | `INVALID_ARGUMENT` | 返回错误信息 |
| 数据库错误 | `kInternal` | `INTERNAL` | 记录日志，返回通用错误 |

---

## Login - 用户登录

### 1. 接口定义

**Proto 定义**:
```protobuf
message LoginRequest {
    string user_name = 1;
    string password  = 2;
}

message LoginResponse {
    .proto.common.Error    error         = 1;
    string                 session_token = 2;
    .proto.common.UserInfo user          = 3;
}
```

### 1.5 Session 模块设计背景

#### **为什么需要 Session 模块？**

在早期版本中，系统存在一个关键的架构缺陷：**UserService 和 MeetingService 之间无法共享用户身份验证状态**。

**问题场景**：

```
旧架构（无 SessionRepository）：

UserService                    MeetingService
    │                               │
    ├─ Login → 返回内存 token       │
    │   (仅存在于 UserService)      │
    │                               │
    │                               ├─ CreateMeeting
    │                               │   收到 token="1234"
    │                               │
    │                               ❌ 无法验证 token 是否合法！
    │                               ❌ 只能强制解析为数字 user_id
    │                               ⚠️  客户端可以伪造任意身份
```

**安全隐患**：
1. **身份伪造**：客户端可以传入任意数字（如 "1001"）冒充其他用户
2. **无法验证**：MeetingService 无法验证 token 的有效性和过期时间
3. **不可持久化**：内存中的 SessionManager 重启后数据丢失
4. **不可共享**：多个服务实例无法共享会话状态

**Session 模块的解决方案**：

通过引入 `SessionRepository` 和 `user_sessions` 表，实现：

1. **持久化存储**：Token 与 user_id 的映射关系存入数据库
   ```sql
   CREATE TABLE user_sessions (
       access_token CHAR(64) UNIQUE,
       user_id BIGINT UNSIGNED,
       expires_at DATETIME
   );
   ```

2. **跨服务验证**：所有服务通过 SessionRepository 验证 token
   ```cpp
   // MeetingService 可以验证 UserService 生成的 token
   auto session = session_repository_->ValidateSession(token);
   if (session.IsOk()) {
       uint64_t user_id = session.Value().user_id;  // 安全的 user_id
   }
   ```

3. **统一会话管理**：
   - 支持过期检查（默认 1 小时）
   - 支持主动登出（删除 session）
   - 为分布式部署和 Redis 缓存层打下基础

**新架构流程**：

```
客户端          UserService                     MeetingService
  │                 │                               │
  ├─ Login ────────>│                               │
  │                 ├─ CreateSession               │
  │                 │  (token → user_id)           │
  │                 │  存入 user_sessions 表       │
  │                 │                               │
  │<── token ───────┤                               │
  │  "xK9pQ2..."    │                               │
  │                 │                               │
  ├─ CreateMeeting ─────────────────────────────────>│
  │  (token="xK9pQ2...")                            │
  │                 │                               │
  │                 │<──── ValidateSession ─────────┤
  │                 │      (查询 user_sessions)     │
  │                 │─────── user_id=1001 ─────────>│
  │                 │                               │
  │                 │     ✅ 身份验证通过，创建会议  │
```

这个设计确保了：
- ✅ 无法伪造用户身份（必须持有有效 token）
- ✅ 跨服务统一认证（UserService 和 MeetingService 共享验证逻辑）
- ✅ 会话可管理（过期、登出、续期等）
- ✅ 支持分布式部署（数据库/Redis 存储）

### 2. 处理流程

```
Client Request (username, password)
    ↓
UserServiceImpl::Login
    ↓
[线程池异步] UserManager::LoginUser
    ↓
┌─ 查找用户 ─────────────────┐
│ UserRepository::FindByUserName │
│ ↓                              │
│ [MySQL] SELECT * FROM users    │
│ WHERE username = ?             │
└────────────────────────────────┘
    ↓
┌─ 验证密码 ─────────────────┐
│ HashPassword(输入密码, 盐值) │
│ ↓                          │
│ 比对哈希值                  │
└────────────────────────────┘
    ↓
┌─ 更新登录时间 ─────────────┐
│ UpdateLastLogin(user_id)   │
└────────────────────────────┘
    ↓
[线程池异步] 生成 Session Token
    ↓
┌─ 创建会话记录 ─────────────┐
│ SessionRepository::CreateSession │
│ ↓                               │
│ [MySQL] INSERT INTO user_sessions │
│ (user_id, access_token, ...)     │
└─────────────────────────────────┘
    ↓
Response (session_token, user_info)
```

### 3. 核心代码实现

#### 3.1 服务层

```cpp
grpc::Status UserServiceImpl::Login(
    grpc::ServerContext*,
    const proto::user::LoginRequest* request,
    proto::user::LoginResponse* response) {
    
    meeting::core::LoginCommand command{
        request->user_name(),
        request->password(),
        "",  // client_ip (可从 context 获取)
        ""   // user_agent
    };
    
    MEETING_LOG_INFO("[UserService] Login user={}", command.user_name);
    
    // 异步执行登录逻辑
    auto login_future = thread_pool_.Submit([this, command]() {
        return user_manager_->LoginUser(command);
    });
    
    auto status_or_user = login_future.get();
    
    // 错误处理
    if (!status_or_user.IsOk()) {
        const auto& status = status_or_user.GetStatus();
        meeting::core::UserErrorCode error_code = /* 映射错误 */;
        meeting::core::ErrorToProto(error_code, status, 
            response->mutable_error());
        return ToGrpcStatus(status);
    }
    
    // 获取用户数据
    auto user_data = status_or_user.Value();
    FillUserInfo(user_data, response->mutable_user());
    
    // 异步生成会话
    auto session_future = thread_pool_.Submit([this, user_data]() {
        meeting::core::SessionRecord rec;
        rec.token = GenerateToken();            // 32字符随机
        rec.user_id = user_data.numeric_id;     // 数值型ID
        rec.user_uuid = user_data.user_id;      // UUID
        rec.expires_at = NowSeconds() + 3600;   // 1小时过期
        return rec;
    });
    
    auto rec = session_future.get();
    auto session_status = session_repository_->CreateSession(rec);
    
    if (!session_status.IsOk()) {
        // 会话创建失败
        return ToGrpcStatus(session_status);
    }
    
    response->set_session_token(rec.token);
    return grpc::Status::OK;
}
```

#### 3.2 业务逻辑层

```cpp
UserManager::StatusOrUser UserManager::LoginUser(
    const LoginCommand& command) {
    
    // 1. 查找用户
    auto user_result = repository_->FindByUserName(command.user_name);
    if (!user_result.IsOk()) {
        return user_result.GetStatus();
    }
    
    // 2. 验证密码
    auto user_data = std::move(user_result.Value());
    const std::string hashed_password = 
        HashPassword(command.password, user_data.salt);
    
    if (hashed_password != user_data.password_hash) {
        return Status::Unauthenticated("Invalid user name or password.");
    }
    
    // 3. 更新最后登录时间
    const std::int64_t now = CurrentUnixSeconds();
    auto update_status = repository_->UpdateLastLogin(user_data.user_id, now);
    if (update_status.IsOk()) {
        user_data.last_login = now;
    }
    
    return StatusOrUser(std::move(user_data));
}
```

#### 3.3 Token 生成

```cpp
std::string GenerateToken() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char kChars[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    std::uniform_int_distribution<int> dist(0, sizeof(kChars) - 2);
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) {
        token.push_back(kChars[dist(rng)]);
    }
    return token;
}
```

#### 3.4 MySQL 会话存储

```cpp
meeting::common::Status MySqlSessionRepository::CreateSession(
    const meeting::core::SessionRecord& record) {
    
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    
    auto sql = fmt::format(
        "INSERT INTO user_sessions "
        "(user_id, access_token, refresh_token, expires_at) "
        "VALUES ({}, '{}', '{}', FROM_UNIXTIME({}))",
        record.user_id,
        record.token,
        record.token,  // 简化处理，access 和 refresh 相同
        record.expires_at
    );
    
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    
    return meeting::common::Status::OK();
}
```

### 4. 错误场景处理

| 场景 | 错误码 | 返回信息 |
|-----|--------|---------|
| 用户不存在 | `kUserNotFound` | "User not found" |
| 密码错误 | `kInvalidCredentials` | "Invalid user name or password" |
| 会话创建失败 | `kSessionExpired` | "Failed to create session" |

---

## Logout - 用户登出

### 1. 接口定义

```protobuf
message LogoutRequest {
    string session_token = 1;
}

message LogoutResponse {
    .proto.common.Error error = 1;
}
```

### 2. 处理流程

```
Client Request (session_token)
    ↓
UserServiceImpl::Logout
    ↓
[线程池异步] SessionRepository::DeleteSession
    ↓
[MySQL] DELETE FROM user_sessions 
        WHERE access_token = ?
    ↓
Response (error code)
```

### 3. 核心代码实现

```cpp
grpc::Status UserServiceImpl::Logout(
    grpc::ServerContext*,
    const proto::user::LogoutRequest* request,
    proto::user::LogoutResponse* response) {
    
    MEETING_LOG_INFO("[UserService] Logout session_token={}...", 
        request->session_token().substr(0, 6));
    
    // 异步删除会话
    auto logout_future = thread_pool_.Submit([this, token = request->session_token()]() {
        return session_repository_->DeleteSession(token);
    });
    
    auto logout_status = logout_future.get();
    
    meeting::core::UserErrorCode error_code = 
        logout_status.IsOk() ? meeting::core::UserErrorCode::kOk
                             : meeting::core::UserErrorCode::kSessionExpired;
    
    meeting::core::ErrorToProto(error_code, logout_status, 
        response->mutable_error());
    
    return ToGrpcStatus(logout_status);
}
```

#### MySQL 实现

```cpp
meeting::common::Status MySqlSessionRepository::DeleteSession(
    const std::string& token) {
    
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    
    auto sql = fmt::format(
        "DELETE FROM user_sessions WHERE access_token = '{}'",
        token
    );
    
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    
    // 检查影响行数
    if (mysql_affected_rows(conn) == 0) {
        return meeting::common::Status::NotFound("Session not found");
    }
    
    return meeting::common::Status::OK();
}
```

---

## GetProfile - 获取用户资料

### 1. 接口定义

```protobuf
message GetProfileRequest {
    string session_token = 1;
}

message GetProfileResponse {
    .proto.common.Error    error = 1;
    .proto.common.UserInfo user  = 2;
}
```

### 2. 处理流程

```
Client Request (session_token)
    ↓
UserServiceImpl::GetProfile
    ↓
[线程池异步] SessionRepository::ValidateSession
    ↓
┌─ 验证会话 ─────────────────┐
│ [MySQL] SELECT FROM user_sessions │
│         JOIN users                │
│ ↓                                 │
│ 检查 Token 存在性               │
│ 检查是否过期                    │
└───────────────────────────────────┘
    ↓
[线程池异步] UserManager::GetUserById
    ↓
┌─ 查询用户信息 ─────────────┐
│ [MySQL] SELECT FROM users  │
│ WHERE user_uuid = ?        │
└────────────────────────────┘
    ↓
Response (user_info)
```

### 3. 核心代码实现

```cpp
grpc::Status UserServiceImpl::GetProfile(
    grpc::ServerContext*,
    const proto::user::GetProfileRequest* request,
    proto::user::GetProfileResponse* response) {
    
    // 1. 验证会话
    auto session_future = thread_pool_.Submit([this, token = request->session_token()]() {
        return session_repository_->ValidateSession(token);
    });
    
    auto session_status = session_future.get();
    if (!session_status.IsOk()) {
        meeting::core::UserErrorCode error_code = 
            meeting::core::UserErrorCode::kSessionExpired;
        meeting::core::ErrorToProto(error_code, session_status.GetStatus(), 
            response->mutable_error());
        return ToGrpcStatus(session_status.GetStatus());
    }
    
    // 2. 获取用户信息
    auto user_future = thread_pool_.Submit([this, user_id = session_status.Value().user_uuid]() {
        return user_manager_->GetUserById(user_id);
    });
    
    auto user_status_or = user_future.get();
    if (!user_status_or.IsOk()) {
        meeting::core::UserErrorCode error_code = 
            meeting::core::UserErrorCode::kUserNotFound;
        meeting::core::ErrorToProto(error_code, user_status_or.GetStatus(), 
            response->mutable_error());
        return ToGrpcStatus(user_status_or.GetStatus());
    }
    
    FillUserInfo(user_status_or.Value(), response->mutable_user());
    return grpc::Status::OK;
}
```

#### 会话验证 (MySQL)

```cpp
meeting::common::StatusOr<meeting::core::SessionRecord> 
MySqlSessionRepository::ValidateSession(const std::string& token) {
    
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    
    // JOIN 查询获取用户信息和会话信息
    auto sql = fmt::format(
        "SELECT s.user_id, u.user_uuid, UNIX_TIMESTAMP(s.expires_at) "
        "FROM user_sessions s "
        "JOIN users u ON u.id = s.user_id "
        "WHERE s.access_token = '{}' LIMIT 1",
        token
    );
    
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        return meeting::common::Status::Unauthenticated("Session not found");
    }
    
    auto cleanup = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(
        res, mysql_free_result);
    
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        return meeting::common::Status::Unauthenticated("Session not found");
    }
    
    // 构造会话记录
    meeting::core::SessionRecord rec;
    rec.token = token;
    rec.user_id = std::strtoull(row[0], nullptr, 10);
    rec.user_uuid = row[1] ? row[1] : "";
    rec.expires_at = row[2] ? std::strtoll(row[2], nullptr, 10) : 0;
    
    // 检查过期时间
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    if (rec.expires_at != 0 && rec.expires_at < now) {
        return meeting::common::Status::Unauthenticated("Session expired");
    }
    
    return meeting::common::StatusOr<meeting::core::SessionRecord>(rec);
}
```

---

## 安全机制详解

### 1. 密码加密

**算法**: PBKDF2-HMAC-SHA256

**参数**:
- **迭代次数**: 100,000 次（防止暴力破解）
- **哈希长度**: 32 字节（256 位）
- **盐值长度**: 32 字节（使用 OpenSSL RAND_bytes）

**优势**:
- 计算密集型，抵抗暴力破解
- 每个用户独立盐值，防止彩虹表攻击
- OpenSSL 加密安全随机数生成器

**存储格式**:
```
数据库存储:
- password_hash: 64字符十六进制字符串 (32字节哈希)
- salt: 64字符十六进制字符串 (32字节盐值)
```

### 2. 会话管理

**Token 生成**:
- 长度: 32 字符
- 字符集: [0-9A-Za-z]
- 生成器: `std::mt19937_64` + `std::random_device`
- 线程安全: `thread_local` 随机数生成器

**会话过期**:
- 默认 TTL: 3600 秒（1 小时）
- 每次请求验证过期时间
- 过期自动删除

**安全特性**:
- Token 随机生成，难以预测
- 数据库唯一索引防止冲突
- 支持主动登出（立即失效）

### 3. SQL 注入防护

**方法**:
1. 参数转义: `mysql_real_escape_string`
2. 字符串引号包裹
3. 使用 fmt::format 构造 SQL

**示例**:
```cpp
std::string Escape(MYSQL* conn, const std::string& value) {
    std::string buf;
    buf.resize(value.size() * 2 + 1);
    unsigned long escaped_len = mysql_real_escape_string(
        conn, buf.data(), value.data(), value.size());
    buf.resize(escaped_len);
    return buf;
}

std::string EscapeAndQuote(MYSQL* conn, const std::string& value) {
    return fmt::format("'{}'", Escape(conn, value));
}
```

---

## 存储实现对比

### 内存存储 (InMemoryRepository)

**特点**:
- 基于 `std::unordered_map`
- 使用 `std::shared_mutex` 读写锁
- 适用于开发测试

**优势**:
- 无需外部依赖
- 性能极高
- 快速启动

**劣势**:
- 数据不持久化
- 重启数据丢失
- 不支持分布式

**实现**:
```cpp
class InMemoryUserRepository : public UserRepository {
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, UserData> users_by_user_name_;
    std::unordered_map<std::string, UserData> users_by_id_;
    std::uint64_t next_numeric_id_ = 1;
};
```

### MySQL 存储 (MySQLRepository)

**特点**:
- 基于连接池
- 事务支持
- 持久化存储

**优势**:
- 数据持久化
- 支持分布式部署
- ACID 保证

**劣势**:
- 需要 MySQL 服务
- 网络延迟
- 维护成本

**连接池配置**:
```json
{
  "pool_size": 10,
  "connection_timeout_ms": 500,
  "read_timeout_ms": 2000,
  "write_timeout_ms": 2000
}
```

### 存储切换机制

**自动降级**:
```cpp
std::shared_ptr<UserRepository> CreateUserRepository() {
    const auto& config = GlobalConfig();
    
    // 检查 MySQL 是否启用
    if (!config.storage.mysql.enabled) {
        MEETING_LOG_WARN("MySQL disabled; using in-memory repository");
        return std::make_shared<InMemoryUserRepository>();
    }
    
    // 尝试连接 MySQL
    auto pool = std::make_shared<ConnectionPool>(options);
    auto test_conn = pool->Acquire();
    
    if (!test_conn.IsOk()) {
        MEETING_LOG_ERROR("MySQL connection failed: {}", 
            test_conn.GetStatus().Message());
        // 降级到内存存储
        return std::make_shared<InMemoryUserRepository>();
    }
    
    MEETING_LOG_INFO("MySQL connection pool initialized");
    return std::make_shared<MySQLUserRepository>(std::move(pool));
}
```

---

## 错误处理机制

### 1. 错误码体系

**领域错误码** (`UserErrorCode`):
```cpp
enum class UserErrorCode {
    kOk = 0,
    kUserNameExists = 1,
    kInvalidPassword = 2,
    kInvalidCredentials = 3,
    kSessionExpired = 4,
    kUserNotFound = 5,
};
```

**通用错误码** (`StatusCode`):
```cpp
enum class StatusCode {
    kOk = 0,
    kInvalidArgument = 3,
    kNotFound = 5,
    kAlreadyExists = 6,
    kUnauthenticated = 16,
    kInternal = 13,
    kUnavailable = 14,
};
```

### 2. 错误转换

**领域错误 → 通用 Status**:
```cpp
inline ::meeting::common::Status FromUserError(
    UserErrorCode error, 
    std::string message = "") {
    
    switch (error) {
        case UserErrorCode::kUserNameExists:
            return Status::AlreadyExists(message);
        case UserErrorCode::kInvalidCredentials:
            return Status::Unauthenticated(message);
        // ...
    }
}
```

**Status → gRPC Status**:
```cpp
grpc::Status ToGrpcStatus(const meeting::common::Status& status) {
    switch (status.Code()) {
        case StatusCode::kOk:
            return grpc::Status::OK;
        case StatusCode::kInvalidArgument:
            return {grpc::StatusCode::INVALID_ARGUMENT, status.Message()};
        case StatusCode::kAlreadyExists:
            return {grpc::StatusCode::ALREADY_EXISTS, status.Message()};
        // ...
    }
}
```

**Status → Proto Error**:
```cpp
inline void ErrorToProto(
    UserErrorCode error,
    const ::meeting::common::Status& status,
    proto::common::Error* error_proto) {
    
    if (error_proto == nullptr) return;
    error_proto->set_code(static_cast<int32_t>(error));
    error_proto->set_message(status.Message());
}
```

### 3. 错误传递链

```
MySQL Error
    ↓
MapMySqlError() → Status
    ↓
Repository → Status
    ↓
Manager → Status / StatusOr<T>
    ↓
Service → FromUserError() → Status
    ↓
ToGrpcStatus() → grpc::Status
    ↓
ErrorToProto() → proto::Error
    ↓
Client
```

---

## 性能优化策略

### 1. 异步处理

**线程池配置**:
```json
{
  "num_workers": 4,
  "queue_capacity": 5000,
  "queue_full_policy": "block"
}
```

**提交任务**:
```cpp
auto future = thread_pool_.Submit([this, command]() {
    return user_manager_->RegisterUser(command);
});
auto result = future.get();  // 阻塞等待结果
```

**优势**:
- 避免阻塞主线程
- 提高并发处理能力
- 任务队列削峰填谷

### 2. 连接池

**RAII 连接租赁**:
```cpp
auto lease_or = pool_->Acquire();  // 获取连接
if (!lease_or.IsOk()) {
    return lease_or.GetStatus();
}
auto lease = std::move(lease_or.Value());
// 使用连接
MYSQL* conn = lease.Raw();
// 自动归还连接（析构时）
```

**优势**:
- 连接复用，减少连接开销
- 自动管理连接生命周期
- 限流保护数据库

### 3. 读写锁

**内存存储并发**:
```cpp
// 读操作（允许多个并发）
std::shared_lock<std::shared_mutex> lock(mutex_);
auto it = users_by_user_name_.find(user_name);

// 写操作（独占锁）
std::unique_lock<std::shared_mutex> lock(mutex_);
users_by_user_name_[user.user_name] = user;
```

**优势**:
- 多读单写，提高并发读性能
- 避免读操作互相阻塞

### 4. 查询优化

**数据库索引**:
```sql
-- 唯一索引（快速查找）
UNIQUE KEY uk_users_username (username)
UNIQUE KEY uk_users_email (email)
UNIQUE KEY uk_users_uuid (user_uuid)

-- 会话查询索引
KEY idx_user_sessions_user (user_id, expires_at)
```

**JOIN 优化**:
```sql
-- GetProfile 一次查询获取所有数据
SELECT s.user_id, u.user_uuid, UNIX_TIMESTAMP(s.expires_at)
FROM user_sessions s 
JOIN users u ON u.id = s.user_id
WHERE s.access_token = ? LIMIT 1
```

---

## 总结

### 技术亮点

1. **分层架构**: 职责清晰，易于测试和维护
2. **Repository 模式**: 存储无关的业务逻辑，灵活切换后端
3. **PBKDF2 加密**: 工业级密码安全标准
4. **异步处理**: 线程池提高并发性能
5. **连接池**: 高效的数据库连接管理
6. **统一错误处理**: Status 机制清晰传递错误信息
7. **自动降级**: MySQL 不可用时自动切换到内存存储

### 最佳实践

- ✅ 密码永不明文存储
- ✅ 使用加密安全的随机数生成器
- ✅ SQL 注入防护
- ✅ RAII 资源管理
- ✅ 读写锁优化并发
- ✅ 完善的日志记录
- ✅ 详细的错误信息

---

**文档版本**: v1.0  
**最后更新**: 2025-12-12  
**相关文档**: [Meeting Service 实现详解](./meeting-service-implementation.md)
