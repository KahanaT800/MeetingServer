# MeetingServer 架构设计文档

## 1. 项目概述

MeetingServer 是一个基于 gRPC 的视频会议服务器后端系统，采用现代 C++17 开发，提供用户管理和会议管理的核心功能。项目采用分层架构设计，支持多种存储后端，具备高性能和良好的可扩展性。

### 1.1 核心特性

- **gRPC 通信**: 高性能的 RPC 通信框架
- **分层架构**: 清晰的职责划分，低耦合高内聚
- **灵活存储**: 支持内存存储和 MySQL 存储，易于扩展
- **线程池**: 自定义高性能异步任务处理
- **安全机制**: 密码加密、会话管理、权限控制
- **可配置**: JSON 配置文件驱动，支持多环境部署

### 1.2 技术栈

| 技术组件 | 说明 |
|---------|------|
| **通信协议** | gRPC + Protocol Buffers 3 |
| **数据存储** | MySQL 8.0+ (连接池) + 内存存储 |
| **日志系统** | spdlog |
| **配置管理** | nlohmann/json |
| **构建系统** | CMake 3.20+ |
| **并发处理** | 自定义线程池 |
| **密码加密** | OpenSSL PBKDF2-HMAC-SHA256 (100k iterations) |
| **测试框架** | Google Test |
| **编程语言** | C++17 |

## 2. 系统架构

### 2.1 分层架构图

```
┌──────────────────────────────────────────────────────────┐
│                   gRPC Service Layer                     │
│         UserServiceImpl | MeetingServiceImpl             │
│                    (server/)                             │
├──────────────────────────────────────────────────────────┤
│               Business Logic Layer                       │
│           UserManager | MeetingManager                   │
│         SessionManager | (core/)                         │
├──────────────────────────────────────────────────────────┤
│                  Repository Layer                        │
│   UserRepository | MeetingRepository | SessionRepository │
│              (core/*/repository)                         │
├──────────────────────────────────────────────────────────┤
│                   Storage Layer                          │
│     MySQL Implementation | In-Memory Implementation      │
│                  (storage/)                              │
├──────────────────────────────────────────────────────────┤
│               Infrastructure Layer                       │
│   ConfigLoader | Logger | ThreadPool | Status            │
│            (common/, thread_pool/)                       │
└──────────────────────────────────────────────────────────┘
```

### 2.2 模块说明

#### 2.2.1 服务层 (Service Layer)

位于 `src/server/`，负责实现 gRPC 服务接口，处理网络请求。

**UserServiceImpl** - 用户服务实现
- `Register`: 用户注册
- `Login`: 用户登录，生成会话令牌
- `Logout`: 用户登出，销毁会话
- `GetProfile`: 获取用户资料

**MeetingServiceImpl** - 会议服务实现
- `CreateMeeting`: 创建会议
- `JoinMeeting`: 加入会议
- `LeaveMeeting`: 离开会议
- `EndMeeting`: 结束会议
- `GetMeeting`: 获取会议信息

#### 2.2.2 业务逻辑层 (Business Logic Layer)

位于 `src/core/`，包含领域模型和业务规则。

**用户管理 (`core/user/`)**
- `UserManager`: 用户业务逻辑
  - 用户注册验证（用户名、邮箱格式验证）
  - 密码哈希与验证（PBKDF2-HMAC-SHA256 + 随机 Salt）
  - 用户 ID 生成（UUID）
  - 登录凭证验证
  
- `SessionManager`: 会话管理
  - Session Token 生成
  - Token 验证与过期检查
  - 会话生命周期管理

**会议管理 (`core/meeting/`)**
- `MeetingManager`: 会议业务逻辑
  - 会议创建（生成会议 ID 和会议码）
  - 参与者管理（加入/离开）
  - 会议状态机：SCHEDULED → RUNNING → ENDED
  - 权限控制（组织者 vs 普通参与者）
  - 会议配置策略应用
  
- `MeetingConfig`: 会议配置
  ```cpp
  - max_participants: 最大参与者数量（默认 100）
  - end_when_empty: 空会议自动结束（默认 true）
  - end_when_organizer_leaves: 组织者离开时结束（默认 true）
  - meeting_code_length: 会议码长度（默认 8）
  ```

#### 2.2.3 数据访问层 (Repository Layer)

采用 **Repository 模式**，通过接口抽象实现存储无关的业务逻辑。

**接口定义**:
- `UserRepository`: 用户数据访问接口
  - `CreateUser`: 创建用户
  - `FindByUserName`: 根据用户名查找
  - `FindById`: 根据 ID 查找
  - `UpdateLastLogin`: 更新最后登录时间

- `MeetingRepository`: 会议数据访问接口
  - `CreateMeeting`: 创建会议
  - `GetMeeting`: 获取会议信息
  - `UpdateMeetingState`: 更新会议状态
  - `AddParticipant`: 添加参与者
  - `RemoveParticipant`: 移除参与者
  - `ListParticipants`: 列出参与者

- `SessionRepository`: 会话数据访问接口
  - `CreateSession`: 创建会话
  - `ValidateSession`: 验证会话
  - `DeleteSession`: 删除会话

**实现类型**:
- 内存实现：`InMemory*Repository` - 适用于开发测试
- MySQL 实现：`Mysql*Repository` - 适用于生产环境

#### 2.2.4 存储层 (Storage Layer)

位于 `src/storage/`，提供具体的数据存储实现。

**MySQL 连接池** (`storage/mysql/`):
- `ConnectionPool`: RAII 风格的连接池管理
  - 连接复用，减少连接开销
  - 连接租赁机制（Lease 模式）
  - 自动归还连接
  - 可配置池大小和超时参数
  
- `Connection`: MySQL 连接封装
  - 预编译语句支持
  - 错误处理
  - 字符集设置（UTF-8MB4）
  
- `Transaction`: 事务管理
  - RAII 自动提交/回滚
  - 嵌套事务支持

**配置参数**:
```json
{
  "mysql": {
    "host": "127.0.0.1",
    "port": 3306,
    "user": "dev",
    "password": "",
    "database": "meeting",
    "pool_size": 10,
    "connection_timeout_ms": 500,
    "read_timeout_ms": 2000,
    "write_timeout_ms": 2000,
    "enabled": false
  }
}
```

#### 2.2.5 基础设施层 (Infrastructure Layer)

**通用组件** (`src/common/`):
- `Status` / `StatusOr<T>`: 错误处理机制
  - 类似于 `absl::Status` 设计
  - 支持链式错误传递
  - 与 gRPC Status 映射
  
- `ConfigLoader`: JSON 配置加载
  - 支持环境变量覆盖
  - 配置验证
  - 默认值处理
  
- `Logger`: 日志系统封装
  - 基于 spdlog
  - 支持控制台和文件输出
  - 可配置日志级别和格式
  - 线程池日志集成

**线程池** (`src/thread_pool/`):
- 高性能任务调度器
- 主要特性：
  - 可配置线程数量和队列容量
  - 任务提交：`Post`（无返回值）、`Submit`（有返回值）
  - 批量任务提交
  - 动态负载均衡
  - 支持暂停/恢复
  - 队列满策略：阻塞/丢弃/覆盖
  - 优雅关闭
  - 统计信息查询

## 3. 架构流程说明

本节详细说明关键业务流程在各架构层之间的数据流转和交互。

### 3.1 用户注册流程

```
客户端                         服务层                    业务逻辑层               数据访问层              存储层
  │                             │                           │                        │                      │
  ├─ RegisterRequest ──────────>│                           │                        │                      │
  │  {username, password,        │                           │                        │                      │
  │   display_name, email}       │                           │                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ ValidateInput ──────────>│                        │                      │
  │                              │  (格式检查)                │                        │                      │
  │                              │                           │                        │                      │
  │                              │<───── OK/Error ───────────┤                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ RegisterUser ───────────>│                        │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ CheckUserExists ─────>│                      │
  │                              │                           │  (username/email)      │                      │
  │                              │                           │                        ├─ SELECT Query ─────>│
  │                              │                           │                        │                      │
  │                              │                           │                        │<─ Result ────────────┤
  │                              │                           │<──── Exists/NotFound ──┤                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ GenerateSalt ─────────┤                      │
  │                              │                           ├─ HashPassword ─────────┤                      │
  │                              │                           │  (PBKDF2, 100k iter)   │                      │
  │                              │                           ├─ GenerateUUID ─────────┤                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ CreateUser ──────────>│                      │
  │                              │                           │  {user_data}           │                      │
  │                              │                           │                        ├─ INSERT Query ─────>│
  │                              │                           │                        │                      │
  │                              │                           │                        │<─ Success/Error ─────┤
  │                              │                           │<──── UserData/Error ────┤                      │
  │                              │<───── UserInfo/Error ─────┤                        │                      │
  │<── RegisterResponse ─────────┤                           │                        │                      │
  │    {error, user_info}        │                           │                        │                      │
```

**关键步骤**：
1. **服务层**：接收 gRPC 请求，调用 `UserManager::RegisterUser`
2. **业务逻辑层**：
   - 验证用户名和邮箱格式
   - 检查用户名/邮箱是否已存在
   - 生成随机 Salt（32 字节加密随机数）和用户 UUID
   - 使用 PBKDF2-HMAC-SHA256 (100k iterations) + Salt 哈希密码
   - 构造 `UserData` 对象
3. **数据访问层**：调用 `UserRepository::CreateUser` 持久化
4. **存储层**：执行 INSERT 语句，返回结果

### 3.2 用户登录与 Session 创建流程

```
客户端                         服务层                    业务逻辑层               数据访问层              存储层
  │                             │                           │                        │                      │
  ├─ LoginRequest ─────────────>│                           │                        │                      │
  │  {username, password}        │                           │                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ Login ──────────────────>│                        │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ FindByUserName ──────>│                      │
  │                              │                           │                        ├─ SELECT Query ─────>│
  │                              │                           │                        │<─ UserData ──────────┤
  │                              │                           │<──── UserData ──────────┤                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ VerifyPassword ───────┤                      │
  │                              │                           │  (hash input + salt,   │                      │
  │                              │                           │   compare with stored) │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ GenerateToken ────────┤                      │
  │                              │                           │  (64 chars random)     │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ CreateSession ────────>│                      │
  │                              │                           │  {token, user_id,      │                      │
  │                              │                           │   expires_at}          │                      │
  │                              │                           │                        ├─ INSERT INTO ──────>│
  │                              │                           │                        │   user_sessions      │
  │                              │                           │                        │<─ Success ───────────┤
  │                              │                           │<──── SessionRecord ─────┤                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ UpdateLastLogin ──────>│                      │
  │                              │                           │                        ├─ UPDATE Query ─────>│
  │                              │                           │                        │<─ Success ───────────┤
  │                              │<───── Session + UserInfo ─┤                        │                      │
  │<── LoginResponse ────────────┤                           │                        │                      │
  │    {session_token, user}     │                           │                        │                      │
```

**关键步骤**：
1. **身份验证**：查找用户 → 验证密码哈希
2. **Session 创建**：
   - 生成随机 64 字符 Token
   - 计算过期时间（默认 1 小时）
   - 插入 `user_sessions` 表（Token → user_id 映射）
3. **更新登录时间**：记录 `last_login_at`
4. **返回 Token**：客户端后续请求携带此 Token

### 3.3 会议创建流程

```
客户端                         服务层                    业务逻辑层               数据访问层              存储层
  │                             │                           │                        │                      │
  ├─ CreateMeetingRequest ─────>│                           │                        │                      │
  │  {token, topic}              │                           │                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ ResolveUserId ──────────>│                        │                      │
  │                              │  (token)                  │                        │                      │
  │                              │                           ├─ ValidateSession ─────>│                      │
  │                              │                           │  (token)               │                      │
  │                              │                           │                        ├─ SELECT FROM ──────>│
  │                              │                           │                        │   user_sessions      │
  │                              │                           │                        │   WHERE token=?      │
  │                              │                           │                        │   AND expires_at>NOW │
  │                              │                           │                        │<─ SessionRecord ─────┤
  │                              │                           │<──── user_id ───────────┤                      │
  │                              │<───── user_id ────────────┤                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ CreateMeeting ──────────>│                        │                      │
  │                              │  (user_id, topic)         │                        │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ GenerateMeetingId ────┤                      │
  │                              │                           │  (KSUID/Snowflake)     │                      │
  │                              │                           ├─ GenerateMeetingCode ──┤                      │
  │                              │                           │  (8-digit random)      │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ CreateMeeting ────────>│                      │
  │                              │                           │  {meeting_data}        │                      │
  │                              │                           │                        ├─ BEGIN TRANSACTION ─>│
  │                              │                           │                        ├─ INSERT INTO ──────>│
  │                              │                           │                        │   meetings           │
  │                              │                           │                        ├─ INSERT INTO ──────>│
  │                              │                           │                        │   meeting_participants│
  │                              │                           │                        │   (organizer)        │
  │                              │                           │                        ├─ COMMIT ───────────>│
  │                              │                           │                        │<─ Success ───────────┤
  │                              │                           │<──── MeetingInfo ───────┤                      │
  │                              │<───── MeetingInfo ────────┤                        │                      │
  │<── CreateMeetingResponse ────┤                           │                        │                      │
  │    {meeting_info}            │                           │                        │                      │
```

**关键步骤**：
1. **身份验证**：
   - 从 Token 解析出 `user_id`
   - 调用 `SessionRepository::ValidateSession` 验证 Token 有效性
   - **这是 Session 模块的核心价值：跨服务统一认证**
2. **会议创建**：
   - 生成全局唯一 `meeting_id`（KSUID/Snowflake）
   - 生成 8 位数字 `meeting_code`
   - 设置初始状态为 `SCHEDULED`
3. **参与者关联**：
   - 将组织者自动添加为参与者
   - 设置 `is_organizer=true`
4. **事务保证**：使用数据库事务确保会议和参与者记录的原子性

### 3.4 加入会议流程

```
客户端                         服务层                    业务逻辑层               数据访问层              存储层
  │                             │                           │                        │                      │
  ├─ JoinMeetingRequest ───────>│                           │                        │                      │
  │  {token, meeting_code}       │                           │                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ ResolveUserId ──────────>│ (Token → user_id)      │                      │
  │                              │<───── user_id ────────────┤                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ JoinMeeting ────────────>│                        │                      │
  │                              │  (user_id, meeting_code)  │                        │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ GetMeetingByCode ────>│                      │
  │                              │                           │                        ├─ SELECT Query ─────>│
  │                              │                           │                        │   WHERE code=?       │
  │                              │                           │                        │<─ MeetingData ───────┤
  │                              │                           │<──── MeetingData ───────┤                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ ValidateMeetingState ─┤                      │
  │                              │                           │  (state == RUNNING?)   │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ CheckParticipantLimit ┤                      │
  │                              │                           │  (count < max_limit?)  │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ CheckDuplicateUser ───┤                      │
  │                              │                           │  (user already joined?)│                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ AddParticipant ───────>│                      │
  │                              │                           │  (meeting_id, user_id) │                      │
  │                              │                           │                        ├─ INSERT INTO ──────>│
  │                              │                           │                        │   meeting_participants│
  │                              │                           │                        │<─ Success ───────────┤
  │                              │                           │<──── Success ───────────┤                      │
  │                              │<───── MeetingInfo ────────┤                        │                      │
  │<── JoinMeetingResponse ──────┤                           │                        │                      │
  │    {meeting_info}            │                           │                        │                      │
```

**关键步骤**：
1. **Token 验证**：解析用户身份（同创建会议流程）
2. **会议查找**：根据 `meeting_code` 查找会议
3. **状态检查**：
   - 会议必须处于 `RUNNING` 状态
   - 参与者数量未达到上限（`max_participants`）
   - 用户未重复加入
4. **添加参与者**：插入 `meeting_participants` 表

### 3.5 结束会议流程

```
客户端                         服务层                    业务逻辑层               数据访问层              存储层
  │                             │                           │                        │                      │
  ├─ EndMeetingRequest ────────>│                           │                        │                      │
  │  {token, meeting_id}         │                           │                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ ResolveUserId ──────────>│ (Token → user_id)      │                      │
  │                              │<───── user_id ────────────┤                        │                      │
  │                              │                           │                        │                      │
  │                              ├─ EndMeeting ─────────────>│                        │                      │
  │                              │  (user_id, meeting_id)    │                        │                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ GetMeeting ──────────>│                      │
  │                              │                           │                        ├─ SELECT Query ─────>│
  │                              │                           │                        │<─ MeetingData ───────┤
  │                              │                           │<──── MeetingData ───────┤                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ CheckPermission ──────┤                      │
  │                              │                           │  (user_id == organizer?)│                      │
  │                              │                           │                        │                      │
  │                              │                           ├─ UpdateMeetingState ───>│                      │
  │                              │                           │  (state = ENDED)       │                      │
  │                              │                           │                        ├─ UPDATE meetings ──>│
  │                              │                           │                        │   SET state=2        │
  │                              │                           │                        │<─ Success ───────────┤
  │                              │                           │<──── Success ───────────┤                      │
  │                              │<───── Success ────────────┤                        │                      │
  │<── EndMeetingResponse ───────┤                           │                        │                      │
  │    {success}                 │                           │                        │                      │
```

**关键步骤**：
1. **权限验证**：
   - 验证用户身份
   - 检查用户是否为会议组织者（`organizer_id == user_id`）
2. **状态更新**：将会议状态从 `RUNNING` 更新为 `ENDED`
3. **权限控制**：只有组织者可以结束会议

### 3.6 数据流总结

#### 3.6.1 跨层数据传递原则

```
Proto 消息 → C++ 结构体 → 数据库记录

LoginRequest (Proto)
    ↓ (服务层转换)
UserCredentials (C++ struct)
    ↓ (业务逻辑层处理)
UserData (C++ struct)
    ↓ (数据访问层转换)
SQL INSERT Statement
    ↓ (存储层执行)
MySQL Row
```

#### 3.6.2 错误传播机制

```
Storage Exception
    ↓ (捕获并转换)
Status::Internal
    ↓ (Repository 返回)
StatusOr<T>
    ↓ (Manager 处理)
DomainError (UserErrorCode/MeetingErrorCode)
    ↓ (Service 转换)
proto::common::Error
    ↓ (gRPC 响应)
Client
```

#### 3.6.3 Session 跨服务流转

```
UserService::Login
    ↓ (创建 Session)
SessionRepository::CreateSession
    ↓ (持久化)
user_sessions 表
    ↓ (客户端携带 Token)
MeetingService::CreateMeeting
    ↓ (验证 Token)
SessionRepository::ValidateSession
    ↓ (解析 user_id)
MeetingManager::CreateMeeting
```

**核心价值**：Session 模块使得不同服务（UserService、MeetingService）可以共享用户身份验证状态，避免身份伪造风险。

## 4. 数据模型

### 3.1 用户模型

```cpp
struct UserData {
    std::string user_id;           // UUID 格式的用户唯一标识
    std::uint64_t numeric_id;      // 数据库自增 ID
    std::string user_name;         // 登录用户名（唯一）
    std::string display_name;      // 显示名称
    std::string email;             // 电子邮件（唯一）
    std::string password_hash;     // PBKDF2-HMAC-SHA256 密码哈希
    std::string salt;              // 随机盐值（32 字节十六进制）
    std::int64_t created_at;       // 创建时间戳（Unix 时间）
    std::int64_t last_login;       // 最后登录时间戳
};
```

### 3.2 会议模型

```cpp
struct MeetingData {
    std::string meeting_id;                  // 会议唯一 ID（KSUID/Snowflake）
    std::string meeting_code;                // 会议码（8 位）
    std::uint64_t organizer_id;              // 组织者用户 ID
    std::string topic;                       // 会议主题
    MeetingState state;                      // 会议状态
    std::vector<std::uint64_t> participants; // 参与者 ID 列表
    std::int64_t created_at;                 // 创建时间戳
    std::int64_t updated_at;                 // 更新时间戳
};

enum class MeetingState {
    kScheduled = 0,  // 已安排（未开始）
    kRunning,        // 进行中
    kEnded,          // 已结束
};
```

### 3.3 会话模型

```cpp
struct SessionRecord {
    std::string token;        // Session Token（64 字符）
    std::uint64_t user_id;    // 数值型用户 ID
    std::string user_uuid;    // 字符串用户 UUID
    std::int64_t expires_at;  // 过期时间戳
};
```

## 4. 数据库设计

### 4.1 表结构

**users 表** - 用户基本信息
```sql
- id: BIGINT UNSIGNED (主键, 自增)
- user_uuid: CHAR(36) (唯一索引)
- username: VARCHAR(64) (唯一索引)
- display_name: VARCHAR(128)
- email: VARCHAR(128) (唯一索引)
- password_hash: VARCHAR(255)
- salt: VARCHAR(64)
- status: TINYINT (1: active, 2: locked, 0: deleted)
- last_login_at: DATETIME
- created_at: TIMESTAMP
- updated_at: TIMESTAMP
```

**user_sessions 表** - 用户会话
```sql
- id: BIGINT UNSIGNED (主键, 自增)
- user_id: BIGINT UNSIGNED (外键 -> users.id)
- access_token: CHAR(64) (唯一索引)
- refresh_token: CHAR(64) (唯一索引)
- client_ip: VARCHAR(64)
- user_agent: VARCHAR(255)
- expires_at: DATETIME
- created_at: TIMESTAMP
- revoked_at: TIMESTAMP
```

**meetings 表** - 会议信息
```sql
- id: BIGINT UNSIGNED (主键, 自增)
- meeting_id: CHAR(26) (唯一索引)
- meeting_code: VARCHAR(16) (索引)
- organizer_id: BIGINT UNSIGNED (外键 -> users.id)
- topic: VARCHAR(255)
- state: TINYINT (0: SCHEDULED, 1: RUNNING, 2: ENDED)
- created_at: TIMESTAMP
- updated_at: TIMESTAMP
```

**meeting_participants 表** - 会议参与者关联
```sql
- id: BIGINT UNSIGNED (主键, 自增)
- meeting_id: BIGINT UNSIGNED (外键 -> meetings.id)
- user_id: BIGINT UNSIGNED (外键 -> users.id)
- is_organizer: BOOLEAN
- joined_at: TIMESTAMP
- left_at: TIMESTAMP
- 唯一索引: (meeting_id, user_id)
```

### 4.2 索引策略

- **唯一索引**: 用户名、邮箱、UUID、Session Token
- **普通索引**: 会议码、用户状态、会议状态
- **复合索引**: (user_id, expires_at) 用于会话查询
- **外键约束**: 级联删除确保数据一致性

## 5. 错误处理机制

### 5.1 统一 Status 机制

```cpp
enum class StatusCode {
    kOk = 0,               // 成功
    kInvalidArgument = 3,  // 无效参数
    kNotFound = 5,         // 未找到
    kAlreadyExists = 6,    // 已存在
    kUnauthenticated = 16, // 未认证
    kInternal = 13,        // 内部错误
    kUnavailable = 14,     // 服务不可用
};
```

### 5.2 领域错误码

**用户错误** (`UserErrorCode`):
```cpp
- kOk: 成功
- kUserNameExists: 用户名已存在
- kInvalidPassword: 无效密码
- kInvalidCredentials: 凭证无效
- kSessionExpired: 会话过期
- kUserNotFound: 用户未找到
```

**会议错误** (`MeetingErrorCode`):
```cpp
- kOk: 成功
- kMeetingNotFound: 会议未找到
- kMeetingEnded: 会议已结束
- kParticipantExists: 参与者已存在
- kMeetingFull: 会议已满
- kPermissionDenied: 权限被拒绝
- kInvalidState: 无效状态
```

### 5.3 错误传递

```
Domain Error → Status → gRPC Status → Client
```

## 6. 安全机制

### 6.1 密码安全

- **哈希算法**: PBKDF2-HMAC-SHA256
- **迭代次数**: 100,000 次（符合 OWASP 推荐标准）
- **盐值**: 每个用户独立的 32 字节加密随机数
- **存储**: 仅存储哈希值和盐值，不存储明文密码
- **验证**: 客户端传入密码 → 服务端 PBKDF2 加盐哈希 → 比对数据库哈希值

### 6.2 会话管理

- **Token 生成**: 随机生成 64 字符 Token
- **过期机制**: 可配置的会话过期时间
- **Token 验证**: 每次请求验证 Token 有效性和过期时间
- **登出处理**: 主动删除会话记录

### 6.3 权限控制

- **会议组织者**: 可以结束会议
- **普通参与者**: 只能加入/离开会议
- **权限验证**: 在业务逻辑层进行权限检查

### 6.4 SQL 注入防护

- **预编译语句**: 所有 SQL 查询使用 Prepared Statement
- **参数绑定**: 用户输入通过参数绑定而非字符串拼接

## 7. 并发与线程安全

### 7.1 并发策略

- **服务层**: 线程池异步处理 gRPC 请求
- **Repository 层**: 使用 `std::shared_mutex`（读写锁）
  - 多读单写，提高并发读性能
- **连接池**: 使用互斥锁保护连接队列
- **RAII 模式**: 自动管理锁和资源生命周期

### 7.2 线程安全保证

- 所有共享数据结构使用锁保护
- 无锁数据结构用于线程池队列
- 原子操作用于统计计数器

## 8. 配置管理

### 8.1 配置文件

位于 `config/app.example.json`，包含：

```json
{
  "server": {
    "host": "0.0.0.0",
    "port": 50051
  },
  "logging": {
    "level": "info",
    "pattern": "[%Y-%m-%d %H:%M:%S.%e][%^%l%$][%t] %v",
    "console": true,
    "file": "logs/meeting_server.log"
  },
  "thread_pool": {
    "config_path": "config/thread_pool.json"
  },
  "storage": {
    "mysql": { ... }
  }
}
```

### 8.2 配置加载顺序

1. **命令行参数**: `./meeting_server /path/to/config.json`
2. **环境变量**: `MEETING_SERVER_CONFIG=/path/to/config.json`
3. **默认路径**: `config/app.example.json`

## 9. 构建与部署

### 9.1 构建脚本

- `scripts/build.sh`: 编译项目
- `scripts/migrate.sh`: 执行数据库迁移
- `scripts/run_server.sh`: 启动服务器
- `scripts/run_tests.sh`: 运行测试套件

### 9.2 依赖管理

支持多种包管理器：
- vcpkg
- Conan
- 系统包管理器（apt, yum）

### 9.3 编译步骤

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

## 10. 测试策略

### 10.1 测试类型

- **单元测试**: 测试各层独立功能
  - `user_manager_test`
  - `meeting_manager_test`
  
- **集成测试**: 测试跨层交互
  - `mysql_user_test`
  - `mysql_meeting_test`
  - `storage_flow_test`
  
- **服务测试**: 测试 gRPC 服务端到端
  - `user_service_test`
  - `meeting_service_test`

### 10.2 测试覆盖

- 正常流程测试
- 异常场景测试
- 边界条件测试
- 并发安全测试

## 11. 可扩展性设计

### 11.1 扩展点

- **存储后端**: 通过 Repository 接口轻松切换（MySQL/Redis/MongoDB）
- **认证方式**: 预留 `auth/` 模块用于 JWT/OAuth 集成
- **缓存层**: 预留 `cache/` 模块用于 Redis 集成
- **通知系统**: 可扩展会议通知功能
- **分布式部署**: 预留 Zookeeper 配置用于服务发现

### 11.2 性能优化方向

- 连接池优化
- 缓存热点数据
- 数据库查询优化
- 负载均衡
- 水平扩展

## 12. 目录结构

```
MeetingServer/
├── src/
│   ├── server/          # gRPC 服务实现
│   ├── core/            # 业务逻辑
│   │   ├── user/        # 用户管理
│   │   └── meeting/     # 会议管理
│   ├── storage/         # 存储层
│   │   └── mysql/       # MySQL 实现
│   ├── common/          # 通用组件
│   ├── thread_pool/     # 线程池
│   ├── auth/            # 认证模块（预留）
│   ├── cache/           # 缓存模块（预留）
│   └── utils/           # 工具类（预留）
├── proto/               # Protocol Buffers 定义
├── config/              # 配置文件
├── db/migrations/       # 数据库迁移脚本
├── tests/               # 测试代码
├── scripts/             # 构建和运行脚本
├── docs/                # 文档
└── CMakeLists.txt       # CMake 配置
```

## 13. 最佳实践

### 13.1 代码规范

- C++17 标准
- RAII 资源管理
- 智能指针优先
- 常量引用传参
- 明确的错误处理
- 完善的日志记录

### 13.2 开发建议

- 先写接口，再写实现
- 单元测试驱动开发
- 代码审查
- 文档与代码同步更新
- 遵循 SOLID 原则

## 14. 未来规划

- [ ] WebRTC 媒体服务集成
- [ ] Redis 缓存层实现
- [ ] JWT 认证支持
- [ ] 分布式会议调度
- [ ] 实时消息推送
- [ ] 会议录制功能
- [ ] 性能监控和指标收集
- [ ] API 网关集成

---

**文档版本**: v1.0  
**最后更新**: 2025-12-12  
**维护者**: MeetingServer Team
