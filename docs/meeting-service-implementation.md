# Meeting Service 实现详解

本文档详细说明 MeetingServer 中 **会议服务 (MeetingService)** 的完整实现流程和技术细节。

## 目录

1. [服务概述](#服务概述)
2. [架构分层](#架构分层)
3. [CreateMeeting - 创建会议](#createmeeting---创建会议)
4. [JoinMeeting - 加入会议](#joinmeeting---加入会议)
5. [LeaveMeeting - 离开会议](#leavemeeting---离开会议)
6. [EndMeeting - 结束会议](#endmeeting---结束会议)
7. [GetMeeting - 获取会议信息](#getmeeting---获取会议信息)
8. [会议状态机](#会议状态机)
9. [权限控制机制](#权限控制机制)
10. [会议配置策略](#会议配置策略)
11. [存储实现对比](#存储实现对比)
12. [错误处理机制](#错误处理机制)
13. [性能优化策略](#性能优化策略)

---

## 服务概述

MeetingService 提供完整的会议管理功能，包括：
- 会议创建与配置
- 参与者管理（加入/离开）
- 会议生命周期控制
- 会议状态查询
- 组织者权限管理

**技术特点**:
- 异步处理：线程池执行业务逻辑
- 状态机管理：SCHEDULED → RUNNING → ENDED
- 权限控制：组织者 vs 普通参与者
- 灵活配置：支持多种会议策略
- 事务处理：MySQL 事务保证数据一致性

---

## 架构分层

```
┌─────────────────────────────────────────┐
│   gRPC Service Layer                    │
│   MeetingServiceImpl                    │  ← 处理 gRPC 请求/响应
├─────────────────────────────────────────┤
│   Business Logic Layer                  │
│   MeetingManager                        │  ← 会议状态机和业务规则
├─────────────────────────────────────────┤
│   Repository Interface Layer            │
│   MeetingRepository                     │  ← 抽象接口
├─────────────────────────────────────────┤
│   Storage Implementation Layer          │
│   InMemory / MySQL                      │  ← 具体实现
└─────────────────────────────────────────┘
```

### 关键组件

| 组件 | 职责 | 位置 |
|-----|------|------|
| `MeetingServiceImpl` | gRPC 服务端点实现 | `src/server/meeting_service_impl.cpp` |
| `MeetingManager` | 会议业务逻辑和状态机 | `src/core/meeting/meeting_manager.cpp` |
| `MeetingRepository` | 会议数据访问抽象 | `src/core/meeting/meeting_repository.hpp` |
| `SessionRepository` | 会话验证（用户鉴权） | `src/core/user/session_repository.hpp` |
| `MySqlMeetingRepository` | MySQL 会议存储实现 | `src/storage/mysql/meeting_repository.cpp` |

---

## CreateMeeting - 创建会议

### 1. 接口定义

**Proto 定义**:
```protobuf
message CreateMeetingRequest {
    string session_token   = 1;  // 会话令牌（组织者身份）
    string topic           = 2;  // 会议主题
    Timestamp scheduled_start = 3;  // 计划开始时间
}

message CreateMeetingResponse {
    .proto.common.Error       error   = 1;  // 错误信息
    .proto.common.MeetingInfo meeting = 2;  // 会议信息
}
```

### 2. 处理流程

```
Client Request (session_token, topic)
    ↓
MeetingServiceImpl::CreateMeeting
    ↓
┌─ 用户身份解析 ───────────────┐
│ ResolveUserId(session_token) │
│ ↓                            │
│ SessionRepository::ValidateSession │
│ ↓                            │
│ 获取 organizer_id            │
└──────────────────────────────┘
    ↓
[线程池异步] MeetingManager::CreateMeeting
    ↓
┌─ 参数验证 ───────────────────┐
│ • organizer_id != 0          │
│ • topic 不为空               │
└──────────────────────────────┘
    ↓
┌─ 生成会议数据 ───────────────┐
│ • meeting_id: "meeting_-" + 16字符 │
│ • meeting_code: 8位随机码    │
│ • state: SCHEDULED           │
│ • participants: [organizer]  │
│ • created_at/updated_at: 时间戳 │
└──────────────────────────────┘
    ↓
MeetingRepository::CreateMeeting
    ↓
[MySQL 事务处理]
    ↓
┌─ 插入会议记录 ───────────────┐
│ INSERT INTO meetings (...)   │
└──────────────────────────────┘
    ↓
┌─ 插入组织者参与记录 ─────────┐
│ INSERT INTO meeting_participants │
│ (meeting_id, user_id, role=1) │
└──────────────────────────────┘
    ↓
[提交事务]
    ↓
Response (meeting_info)
```

### 3. 核心代码实现

#### 3.1 服务层

```cpp
grpc::Status MeetingServiceImpl::CreateMeeting(
    grpc::ServerContext*,
    const proto::meeting::CreateMeetingRequest* request,
    proto::meeting::CreateMeetingResponse* response) {
    
    // 1. 解析用户身份（组织者）
    auto organizer_id_or = ResolveUserId(
        request->session_token(), 
        session_repository_.get(),
        !GlobalConfig().storage.mysql.enabled  // 是否允许数字降级
    );
    
    if (!organizer_id_or.IsOk()) {
        auto code = MapStatus(organizer_id_or.GetStatus());
        meeting::core::ErrorToProto(code, organizer_id_or.GetStatus(), 
            response->mutable_error());
        return ToGrpcStatus(organizer_id_or.GetStatus());
    }
    
    // 2. 构造创建命令
    meeting::core::CreateMeetingCommand command{
        organizer_id_or.Value(),  // organizer_id
        request->topic()          // topic
    };
    
    MEETING_LOG_INFO("[MeetingService] CreateMeeting topic={} organizer={}",
                     command.topic, command.organizer_id);
    
    // 3. 提交到线程池异步执行
    auto create_future = thread_pool_.Submit([this, command]() {
        return meeting_manager_->CreateMeeting(command);
    });
    
    auto status_or_meeting = create_future.get();
    
    // 4. 错误处理
    if (!status_or_meeting.IsOk()) {
        auto code = MapStatus(status_or_meeting.GetStatus());
        meeting::core::ErrorToProto(code, status_or_meeting.GetStatus(), 
            response->mutable_error());
        return ToGrpcStatus(status_or_meeting.GetStatus());
    }
    
    // 5. 填充响应
    FillMeetingInfo(status_or_meeting.Value(), response->mutable_meeting());
    meeting::core::ErrorToProto(meeting::core::MeetingErrorCode::kOk,
        meeting::common::Status::OK(), response->mutable_error());
    
    return grpc::Status::OK;
}
```

#### 3.2 用户身份解析

**关键功能**：MeetingService 通过 SessionRepository 验证 UserService 生成的 token，获取真实的 user_id。

**为什么需要这个函数？**

在引入 Session 模块之前，MeetingService 只能直接将 token 解析为数字 ID，这存在严重的安全隐患：
- ❌ 客户端可以伪造任意数字冒充其他用户
- ❌ 无法验证 token 的有效性
- ❌ 无法检查 token 是否过期

通过 `ResolveUserId`，实现了**跨服务的统一身份验证**：
- ✅ 验证 token 是否由 UserService 合法签发
- ✅ 检查 token 是否过期
- ✅ 安全地获取真实 user_id

```cpp
meeting::common::StatusOr<std::uint64_t> ResolveUserId(
    const std::string& token,
    meeting::core::SessionRepository* repo,
    bool allow_numeric_fallback) {
    
    // 1. 优先通过 SessionRepository 验证（生产环境）
    if (repo) {
        auto session = repo->ValidateSession(token);
        if (session.IsOk()) {
            // ✅ Token 有效，返回数据库中映射的 user_id
            return meeting::common::StatusOr<std::uint64_t>(
                session.Value().user_id);
        }
        if (!allow_numeric_fallback) {
            // 严格模式：验证失败直接返回错误
            return session.GetStatus();
        }
    }
    
    if (!allow_numeric_fallback) {
        return meeting::common::Status::Unauthenticated("Session not found");
    }
    
    // 2. 降级策略：将 token 解析为数字 ID（仅测试环境）
    // 用于在 MySQL 未启用时快速测试
    if (token.empty()) {
        return meeting::common::Status::InvalidArgument(
            "session token is empty");
    }
    
    std::uint64_t value = 0;
    auto first = token.data();
    auto last = token.data() + token.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    
    if (ec != std::errc() || ptr != last) {
        return meeting::common::Status::InvalidArgument(
            "session token must be numeric user id");
    }
    
    return meeting::common::StatusOr<std::uint64_t>(value);
}
```

**使用场景**：

```cpp
// 在 CreateMeeting 中调用
auto organizer_id_or = ResolveUserId(
    request->session_token(),           // 客户端传来的 token
    session_repository_.get(),          // SessionRepository 实例
    !GlobalConfig().storage.mysql.enabled  // MySQL 未启用时允许降级
);

if (!organizer_id_or.IsOk()) {
    // Token 无效或已过期
    return ToGrpcStatus(organizer_id_or.GetStatus());
}

uint64_t organizer_id = organizer_id_or.Value();  // 安全的 user_id
```

**安全机制**：
- 生产环境（MySQL 启用）：必须通过 SessionRepository 验证，杜绝身份伪造
- 测试环境（MySQL 未启用）：允许数字降级，方便快速测试

#### 3.3 业务逻辑层

```cpp
MeetingManager::StatusOrMeeting MeetingManager::CreateMeeting(
    const CreateMeetingCommand& command) {
    
    // 1. 参数验证
    if (command.organizer_id == 0) {
        return Status::InvalidArgument("Organizer ID cannot be empty.");
    }
    
    if (command.topic.empty()) {
        return Status::InvalidArgument("Meeting topic cannot be empty.");
    }
    
    // 2. 构造会议数据
    MeetingData meeting;
    meeting.meeting_id = GenerateMeetingID();       // "meeting_-" + 16字符
    meeting.meeting_code = GenerateMeetingCode();   // 8位随机码
    meeting.organizer_id = command.organizer_id;
    meeting.topic = command.topic;
    meeting.state = MeetingState::kScheduled;       // 初始状态
    meeting.created_at = CurrentUnixSeconds();
    meeting.updated_at = meeting.created_at;
    meeting.participants.push_back(command.organizer_id);
    
    // 3. 存储会议数据
    auto status = repository_->CreateMeeting(meeting);
    if (!status.IsOk()) {
        return status.GetStatus();
    }
    
    // 4. 添加组织者为参与者
    auto add_status = repository_->AddParticipant(
        meeting.meeting_id, 
        meeting.organizer_id, 
        true  // is_organizer
    );
    
    if (!add_status.IsOk() && 
        add_status.Code() != meeting::common::StatusCode::kAlreadyExists) {
        return add_status;
    }
    
    return StatusOrMeeting(std::move(meeting));
}
```

#### 3.4 会议 ID 和会议码生成

```cpp
std::string MeetingManager::GenerateMeetingID() {
    return "meeting_-" + RandomAlphanumericString(16);
}

std::string MeetingManager::GenerateMeetingCode() {
    return RandomAlphanumericString(config_.meeting_code_length);
}

std::string RandomAlphanumericString(std::size_t length) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char kChars[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    std::uniform_int_distribution<int> dist(0, sizeof(kChars) - 2);
    std::string result;
    result.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        result.push_back(kChars[dist(rng)]);
    }
    return result;
}
```

#### 3.5 MySQL 存储实现（事务处理）

```cpp
meeting::common::StatusOr<meeting::core::MeetingData> 
MySqlMeetingRepository::CreateMeeting(
    const meeting::core::MeetingData& data) {
    
    // 1. 开始事务
    Transaction transaction(pool_);
    auto status = transaction.Begin();
    if (!status.IsOk()) {
        return status;
    }
    
    MYSQL* conn = transaction.Raw();
    
    // 2. 插入会议记录
    auto created_at = std::max<std::int64_t>(data.created_at, 1);
    auto updated_at = std::max<std::int64_t>(data.updated_at, created_at);
    
    auto sql_meeting = fmt::format(
        "INSERT INTO meetings "
        "(meeting_id, meeting_code, organizer_id, topic, state, "
        "created_at, updated_at) "
        "VALUES ({}, {}, {}, {}, {}, FROM_UNIXTIME({}), FROM_UNIXTIME({}))",
        EscapeAndQuote(conn, data.meeting_id),
        EscapeAndQuote(conn, data.meeting_code),
        data.organizer_id,
        EscapeAndQuote(conn, data.topic),
        static_cast<int>(data.state),
        created_at,
        updated_at
    );
    
    if (mysql_real_query(conn, sql_meeting.c_str(), sql_meeting.size()) != 0) {
        transaction.Rollback();
        return MapMySqlError(conn);
    }
    
    // 3. 插入组织者作为参与者
    auto sql_participant = fmt::format(
        "INSERT INTO meeting_participants "
        "(meeting_id, user_id, role, joined_at) "
        "VALUES ("
        "  (SELECT id FROM meetings WHERE meeting_id = {}), "
        "  {}, 1, NOW()"
        ")",
        EscapeAndQuote(conn, data.meeting_id),
        data.organizer_id
    );
    
    if (mysql_real_query(conn, sql_participant.c_str(), 
        sql_participant.size()) != 0) {
        transaction.Rollback();
        return MapMySqlError(conn);
    }
    
    // 4. 提交事务
    status = transaction.Commit();
    if (!status.IsOk()) {
        return status;
    }
    
    return meeting::common::StatusOr<meeting::core::MeetingData>(data);
}
```

### 4. 错误场景处理

| 场景 | 错误码 | 返回信息 |
|-----|--------|---------|
| Session Token 无效 | `kPermissionDenied` | "Session not found" |
| 组织者 ID 为空 | `kInvalidState` | "Organizer ID cannot be empty" |
| 会议主题为空 | `kInvalidState` | "Meeting topic cannot be empty" |
| 数据库错误 | `kInvalidState` | MySQL error message |

---

## JoinMeeting - 加入会议

### 1. 接口定义

```protobuf
message JoinMeetingRequest {
    string session_token = 1;  // 参与者会话令牌
    string meeting_id    = 2;  // 会议 ID
    string client_info   = 3;  // 客户端信息
}

message JoinMeetingResponse {
    .proto.common.Error          error    = 1;  // 错误信息
    .proto.common.ServerEndpoint endpoint = 2;  // 媒体服务器端点
    .proto.common.MeetingInfo    meeting  = 3;  // 会议信息
}
```

### 2. 处理流程

```
Client Request (session_token, meeting_id)
    ↓
MeetingServiceImpl::JoinMeeting
    ↓
┌─ 解析参与者 ID ──────────────┐
│ ResolveUserId(session_token) │
└──────────────────────────────┘
    ↓
[线程池异步] MeetingManager::JoinMeeting
    ↓
┌─ 参数验证 ───────────────────┐
│ • meeting_id 不为空          │
│ • participant_id != 0        │
└──────────────────────────────┘
    ↓
┌─ 获取会议信息 ───────────────┐
│ MeetingRepository::GetMeeting │
└──────────────────────────────┘
    ↓
┌─ 业务规则检查 ───────────────┐
│ • 会议是否已结束             │
│ • 参与者是否已在会议中       │
│ • 是否达到最大参与者限制     │
└──────────────────────────────┘
    ↓
┌─ 添加参与者 ─────────────────┐
│ MeetingRepository::AddParticipant │
└──────────────────────────────┘
    ↓
┌─ 状态转换（如需要）─────────┐
│ SCHEDULED → RUNNING          │
│ (首个非组织者加入时)         │
└──────────────────────────────┘
    ↓
Response (endpoint, meeting_info)
```

### 3. 核心代码实现

#### 3.1 服务层

```cpp
grpc::Status MeetingServiceImpl::JoinMeeting(
    grpc::ServerContext*,
    const proto::meeting::JoinMeetingRequest* request,
    proto::meeting::JoinMeetingResponse* response) {
    
    // 1. 解析参与者身份
    auto participant_or = ResolveUserId(
        request->session_token(),
        session_repository_.get(),
        !GlobalConfig().storage.mysql.enabled
    );
    
    if (!participant_or.IsOk()) {
        auto code = MapStatus(participant_or.GetStatus());
        meeting::core::ErrorToProto(code, participant_or.GetStatus(), 
            response->mutable_error());
        return ToGrpcStatus(participant_or.GetStatus());
    }
    
    // 2. 构造加入命令
    meeting::core::JoinMeetingCommand command{
        request->meeting_id(),
        participant_or.Value()
    };
    
    MEETING_LOG_INFO("[MeetingService] JoinMeeting meeting={} participant={}",
                     command.meeting_id, command.participant_id);
    
    // 3. 异步执行加入逻辑
    auto join_future = thread_pool_.Submit([this, command]() {
        return meeting_manager_->JoinMeeting(command);
    });
    
    auto status_or_meeting = join_future.get();
    
    if (!status_or_meeting.IsOk()) {
        auto code = MapStatus(status_or_meeting.GetStatus());
        meeting::core::ErrorToProto(code, status_or_meeting.GetStatus(), 
            response->mutable_error());
        return ToGrpcStatus(status_or_meeting.GetStatus());
    }
    
    // 4. 填充响应
    FillMeetingInfo(status_or_meeting.Value(), response->mutable_meeting());
    
    // 5. 返回媒体服务器端点（待实现）
    auto* endpoint = response->mutable_endpoint();
    endpoint->set_ip("0.0.0.0");
    endpoint->set_port(0);
    endpoint->set_region("default");
    
    meeting::core::ErrorToProto(meeting::core::MeetingErrorCode::kOk,
        meeting::common::Status::OK(), response->mutable_error());
    
    return grpc::Status::OK;
}
```

#### 3.2 业务逻辑层

```cpp
MeetingManager::StatusOrMeeting MeetingManager::JoinMeeting(
    const JoinMeetingCommand& command) {
    
    // 1. 参数验证
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.participant_id == 0) {
        return Status::InvalidArgument("Participant ID cannot be empty.");
    }
    
    // 2. 获取会议信息
    auto meeting_or = repository_->GetMeeting(command.meeting_id);
    if (!meeting_or.IsOk()) {
        return meeting_or.GetStatus();
    }
    auto meeting = meeting_or.Value();
    
    // 3. 检查会议状态
    if (meeting.state == MeetingState::kEnded) {
        return Status::InvalidArgument(
            "Cannot join a meeting that has ended.");
    }
    
    // 4. 检查是否已在会议中
    if (std::find(meeting.participants.begin(), 
                  meeting.participants.end(), 
                  command.participant_id) != meeting.participants.end()) {
        return Status::AlreadyExists("Participant already in the meeting.");
    }
    
    // 5. 检查参与者数量限制
    if (meeting.participants.size() >= config_.max_participants) {
        return Status::Unavailable(
            "Meeting has reached maximum participant limit.");
    }
    
    // 6. 添加参与者
    auto status = repository_->AddParticipant(
        command.meeting_id, 
        command.participant_id, 
        false  // 非组织者
    );
    if (!status.IsOk()) {
        return status;
    }
    
    // 7. 状态转换：首个非组织者加入时，SCHEDULED → RUNNING
    if (meeting.state == MeetingState::kScheduled && 
        command.participant_id != meeting.organizer_id) {
        meeting.state = MeetingState::kRunning;
        repository_->UpdateMeetingState(
            meeting.meeting_id, 
            meeting.state, 
            CurrentUnixSeconds()
        );
    }
    
    meeting.participants.push_back(command.participant_id);
    Touch(meeting);  // 更新 updated_at
    
    return StatusOrMeeting(std::move(meeting));
}
```

#### 3.3 MySQL 添加参与者

```cpp
meeting::common::Status MySqlMeetingRepository::AddParticipant(
    const std::string& meeting_id, 
    std::uint64_t participant_id, 
    bool is_organizer) {
    
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    
    auto sql = fmt::format(
        "INSERT INTO meeting_participants "
        "(meeting_id, user_id, role, joined_at) "
        "VALUES ("
        "  (SELECT id FROM meetings WHERE meeting_id = {}), "
        "  {}, {}, NOW()"
        ")",
        EscapeAndQuote(conn, meeting_id),
        participant_id,
        is_organizer ? 1 : 0
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
| 会议不存在 | `kMeetingNotFound` | "meeting not found" |
| 会议已结束 | `kInvalidState` | "Cannot join a meeting that has ended" |
| 已在会议中 | `kParticipantExists` | "Participant already in the meeting" |
| 达到人数上限 | `kMeetingFull` | "Meeting has reached maximum participant limit" |

---

## LeaveMeeting - 离开会议

### 1. 接口定义

```protobuf
message LeaveMeetingRequest {
    string session_token = 1;
    string meeting_id    = 2;
}

message LeaveMeetingResponse {
    .proto.common.Error error = 1;
}
```

### 2. 处理流程

```
Client Request (session_token, meeting_id)
    ↓
MeetingServiceImpl::LeaveMeeting
    ↓
[解析参与者 ID]
    ↓
[线程池异步] MeetingManager::LeaveMeeting
    ↓
┌─ 获取会议信息 ───────────────┐
│ MeetingRepository::GetMeeting │
└──────────────────────────────┘
    ↓
┌─ 检查参与者是否在会议中 ─────┐
│ std::find(participants, id)  │
└──────────────────────────────┘
    ↓
┌─ 移除参与者 ─────────────────┐
│ MeetingRepository::RemoveParticipant │
└──────────────────────────────┘
    ↓
┌─ 策略判断 ───────────────────┐
│ • 组织者离开 && end_when_organizer_leaves │
│   → 结束会议                 │
│ • 最后一人离开 && end_when_empty │
│   → 结束会议                 │
└──────────────────────────────┘
    ↓
Response
```

### 3. 核心代码实现

```cpp
MeetingManager::Status MeetingManager::LeaveMeeting(
    const LeaveMeetingCommand& command) {
    
    // 1. 参数验证
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.participant_id == 0) {
        return Status::InvalidArgument("Participant ID cannot be empty.");
    }
    
    // 2. 获取会议信息
    auto meeting_or = repository_->GetMeeting(command.meeting_id);
    if (!meeting_or.IsOk()) {
        return meeting_or.GetStatus();
    }
    auto meeting = meeting_or.Value();
    
    // 3. 检查参与者是否在会议中
    auto iter = std::find(
        meeting.participants.begin(), 
        meeting.participants.end(), 
        command.participant_id
    );
    if (iter == meeting.participants.end()) {
        return Status::AlreadyExists("Participant not found in the meeting.");
    }
    
    // 4. 移除参与者
    auto rm_status = repository_->RemoveParticipant(
        command.meeting_id, 
        command.participant_id
    );
    if (!rm_status.IsOk()) {
        return rm_status;
    }
    
    // 5. 组织者离开策略
    if (command.participant_id == meeting.organizer_id && 
        config_.end_when_organizer_leaves) {
        meeting.state = MeetingState::kEnded;
        repository_->UpdateMeetingState(
            meeting.meeting_id, 
            meeting.state, 
            CurrentUnixSeconds()
        );
    } else {
        // 6. 检查是否还有参与者
        auto list = repository_->ListParticipants(meeting.meeting_id);
        if (list.IsOk()) {
            meeting.participants = list.Value();
        }
        
        // 空会议策略
        if (meeting.participants.empty() && config_.end_when_empty) {
            meeting.state = MeetingState::kEnded;
            repository_->UpdateMeetingState(
                meeting.meeting_id, 
                meeting.state, 
                CurrentUnixSeconds()
            );
        }
    }
    
    return Status::OK();
}
```

---

## EndMeeting - 结束会议

### 1. 接口定义

```protobuf
message EndMeetingRequest {
    string session_token = 1;
    string meeting_id    = 2;
}

message EndMeetingResponse {
    .proto.common.Error error = 1;
}
```

### 2. 处理流程

```
Client Request (session_token, meeting_id)
    ↓
MeetingServiceImpl::EndMeeting
    ↓
[解析请求者 ID]
    ↓
[线程池异步] MeetingManager::EndMeeting
    ↓
┌─ 获取会议信息 ───────────────┐
│ MeetingRepository::GetMeeting │
└──────────────────────────────┘
    ↓
┌─ 权限验证 ───────────────────┐
│ requester_id == organizer_id │
└──────────────────────────────┘
    ↓
┌─ 状态检查 ───────────────────┐
│ state != ENDED               │
└──────────────────────────────┘
    ↓
┌─ 更新会议状态 ───────────────┐
│ UpdateMeetingState(ENDED)    │
└──────────────────────────────┘
    ↓
Response
```

### 3. 核心代码实现

```cpp
MeetingManager::Status MeetingManager::EndMeeting(
    const EndMeetingCommand& command) {
    
    // 1. 参数验证
    if (command.meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    if (command.requester_id == 0) {
        return Status::InvalidArgument("Requester ID cannot be empty.");
    }
    
    // 2. 获取会议信息
    auto meeting_or = repository_->GetMeeting(command.meeting_id);
    if (!meeting_or.IsOk()) {
        return meeting_or.GetStatus();
    }
    auto meeting = meeting_or.Value();
    
    // 3. 检查会议状态
    if (meeting.state == MeetingState::kEnded) {
        return Status::InvalidArgument("Meeting has already ended.");
    }
    
    // 4. 权限验证：只有组织者可以结束会议
    if (command.requester_id != meeting.organizer_id) {
        return Status::Unauthenticated(
            "Only the organizer can end the meeting.");
    }
    
    // 5. 更新会议状态为已结束
    return repository_->UpdateMeetingState(
        command.meeting_id, 
        MeetingState::kEnded, 
        CurrentUnixSeconds()
    );
}
```

#### MySQL 更新状态

```cpp
meeting::common::Status MySqlMeetingRepository::UpdateMeetingState(
    const std::string& meeting_id, 
    meeting::core::MeetingState state, 
    std::int64_t updated_at) {
    
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    
    auto sql = fmt::format(
        "UPDATE meetings "
        "SET state = {}, updated_at = FROM_UNIXTIME({}) "
        "WHERE meeting_id = {}",
        static_cast<int>(state),
        std::max<std::int64_t>(updated_at, 1),
        EscapeAndQuote(conn, meeting_id)
    );
    
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    
    if (mysql_affected_rows(conn) == 0) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    
    return meeting::common::Status::OK();
}
```

---

## GetMeeting - 获取会议信息

### 1. 接口定义

```protobuf
message GetMeetingRequest {
    string session_token = 1;
    string meeting_id    = 2;
}

message GetMeetingResponse {
    .proto.common.Error       error   = 1;
    .proto.common.MeetingInfo meeting = 2;
}
```

### 2. 处理流程

```
Client Request (meeting_id)
    ↓
MeetingServiceImpl::GetMeeting
    ↓
[线程池异步] MeetingManager::GetMeeting
    ↓
┌─ 参数验证 ───────────────────┐
│ meeting_id 不为空            │
└──────────────────────────────┘
    ↓
┌─ 查询会议 ───────────────────┐
│ MeetingRepository::GetMeeting │
│ ↓                            │
│ [MySQL] SELECT FROM meetings │
│         JOIN meeting_participants │
└──────────────────────────────┘
    ↓
Response (meeting_info with participants)
```

### 3. 核心代码实现

#### 3.1 业务逻辑层

```cpp
MeetingManager::StatusOrMeeting MeetingManager::GetMeeting(
    const std::string& meeting_id) {
    
    if (meeting_id.empty()) {
        return Status::InvalidArgument("Meeting ID cannot be empty.");
    }
    
    auto meeting = repository_->GetMeeting(meeting_id);
    if (!meeting.IsOk()) {
        return meeting.GetStatus();
    }
    
    return meeting;
}
```

#### 3.2 MySQL 查询实现

```cpp
meeting::common::StatusOr<meeting::core::MeetingData> 
MySqlMeetingRepository::GetMeeting(const std::string& meeting_id) const {
    
    auto lease_or = pool_->Acquire();
    if (!lease_or.IsOk()) {
        return lease_or.GetStatus();
    }
    
    auto lease = std::move(lease_or.Value());
    return LoadMeeting(lease.Raw(), meeting_id);
}

// 加载完整的会议数据（包括参与者列表）
meeting::common::StatusOr<meeting::core::MeetingData> 
MySqlMeetingRepository::LoadMeeting(
    MYSQL* conn, 
    const std::string& meeting_id) const {
    
    // 1. 查询会议基本信息
    auto sql = fmt::format(
        "SELECT meeting_id, meeting_code, organizer_id, topic, state, "
        "UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(updated_at) "
        "FROM meetings WHERE meeting_id = {} LIMIT 1",
        EscapeAndQuote(conn, meeting_id)
    );
    
    if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    
    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    
    auto cleanup = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(
        result, mysql_free_result);
    
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) {
        return meeting::common::Status::NotFound("meeting not found");
    }
    
    // 2. 解析会议数据
    meeting::core::MeetingData data;
    data.meeting_id = row[0] ? row[0] : "";
    data.meeting_code = row[1] ? row[1] : "";
    data.organizer_id = ParseUInt64(row[2]);
    data.topic = row[3] ? row[3] : "";
    data.state = static_cast<meeting::core::MeetingState>(
        row[4] ? std::atoi(row[4]) : 0);
    data.created_at = ParseInt64(row[5]);
    data.updated_at = ParseInt64(row[6]);
    
    // 3. 查询参与者列表
    auto participants_sql = fmt::format(
        "SELECT user_id FROM meeting_participants "
        "WHERE meeting_id = (SELECT id FROM meetings WHERE meeting_id = {})",
        EscapeAndQuote(conn, meeting_id)
    );
    
    if (mysql_real_query(conn, participants_sql.c_str(), 
        participants_sql.size()) != 0) {
        return MapMySqlError(conn);
    }
    
    MYSQL_RES* pres = mysql_store_result(conn);
    if (pres) {
        auto cleanup_p = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(
            pres, mysql_free_result);
        
        MYSQL_ROW prow;
        while ((prow = mysql_fetch_row(pres)) != nullptr) {
            data.participants.push_back(ParseUInt64(prow[0]));
        }
    }
    
    return meeting::common::StatusOr<meeting::core::MeetingData>(data);
}
```

---

## 会议状态机

### 状态定义

```cpp
enum class MeetingState {
    kScheduled = 0,  // 已安排（创建后初始状态）
    kRunning,        // 进行中
    kEnded,          // 已结束
};
```

### 状态转换图

```
         CreateMeeting
              ↓
       ┌─────────────┐
       │  SCHEDULED  │  ← 会议创建后的初始状态
       └─────────────┘
              ↓
         JoinMeeting (首个非组织者加入)
              ↓
       ┌─────────────┐
       │   RUNNING   │  ← 会议进行中
       └─────────────┘
              ↓
         EndMeeting / LeaveMeeting (策略触发)
              ↓
       ┌─────────────┐
       │    ENDED    │  ← 会议已结束（终态）
       └─────────────┘
```

### 状态转换规则

| 当前状态 | 触发事件 | 新状态 | 条件 |
|---------|---------|--------|------|
| SCHEDULED | JoinMeeting | RUNNING | 首个非组织者加入 |
| RUNNING | EndMeeting | ENDED | 组织者主动结束 |
| RUNNING | LeaveMeeting | ENDED | 组织者离开 && `end_when_organizer_leaves` |
| RUNNING | LeaveMeeting | ENDED | 最后一人离开 && `end_when_empty` |
| ENDED | 任何操作 | ENDED | 终态，不可再转换 |

### 状态检查

```cpp
// 加入会议前检查
if (meeting.state == MeetingState::kEnded) {
    return Status::InvalidArgument("Cannot join a meeting that has ended.");
}

// 结束会议前检查
if (meeting.state == MeetingState::kEnded) {
    return Status::InvalidArgument("Meeting has already ended.");
}
```

---

## 权限控制机制

### 角色定义

```cpp
// 数据库中的角色标识
enum Role {
    kParticipant = 0,  // 普通参与者
    kOrganizer = 1,    // 组织者
};
```

### 权限矩阵

| 操作 | 组织者 | 普通参与者 |
|-----|--------|-----------|
| CreateMeeting | ✅ | ❌ |
| JoinMeeting | ✅ | ✅ |
| LeaveMeeting | ✅ | ✅ |
| EndMeeting | ✅ | ❌ |
| GetMeeting | ✅ | ✅ |

### 权限验证实现

```cpp
// EndMeeting 权限检查
if (command.requester_id != meeting.organizer_id) {
    return Status::Unauthenticated(
        "Only the organizer can end the meeting.");
}
```

### 特殊权限

**组织者特权**:
1. 自动成为首个参与者
2. 可以主动结束会议
3. 离开会议可触发会议结束（配置决定）

---

## 会议配置策略

### 配置结构

```cpp
struct MeetingConfig {
    std::size_t max_participants = 100;          // 最大参与者数量
    bool end_when_empty = true;                  // 空会议自动结束
    bool end_when_organizer_leaves = true;       // 组织者离开时结束
    std::size_t meeting_code_length = 8;         // 会议码长度
};
```

### 策略应用

#### 1. 最大参与者限制

```cpp
if (meeting.participants.size() >= config_.max_participants) {
    return Status::Unavailable(
        "Meeting has reached maximum participant limit.");
}
```

#### 2. 空会议自动结束

```cpp
if (meeting.participants.empty() && config_.end_when_empty) {
    meeting.state = MeetingState::kEnded;
    repository_->UpdateMeetingState(
        meeting.meeting_id, 
        meeting.state, 
        CurrentUnixSeconds()
    );
}
```

#### 3. 组织者离开结束会议

```cpp
if (command.participant_id == meeting.organizer_id && 
    config_.end_when_organizer_leaves) {
    meeting.state = MeetingState::kEnded;
    repository_->UpdateMeetingState(
        meeting.meeting_id, 
        meeting.state, 
        CurrentUnixSeconds()
    );
}
```

---

## 存储实现对比

### 内存存储 (InMemoryMeetingRepository)

**特点**:
- 基于 `std::unordered_map<std::string, MeetingData>`
- 使用 `std::shared_mutex` 保护并发访问
- 参与者列表存储在 `MeetingData.participants` 向量中

**优势**:
- 无需外部依赖
- 查询速度极快 (O(1))
- 适合开发测试

**劣势**:
- 数据不持久化
- 无事务保证
- 单机限制

**实现示例**:
```cpp
meeting::common::StatusOr<MeetingData> 
InMemoryMeetingRepository::CreateMeeting(const MeetingData& data) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = meetings_.find(data.meeting_id);
    if (it != meetings_.end()) {
        return meeting::common::Status::AlreadyExists(
            "meeting already exists");
    }
    
    meetings_.emplace(data.meeting_id, data);
    return meeting::common::StatusOr<MeetingData>(data);
}
```

### MySQL 存储 (MySqlMeetingRepository)

**特点**:
- 关系型数据库存储
- 支持事务（CreateMeeting 使用事务）
- 分表设计：`meetings` + `meeting_participants`

**优势**:
- 数据持久化
- ACID 保证
- 支持复杂查询
- 易于扩展（如查询历史会议）

**劣势**:
- 网络延迟
- 需要连接池管理
- 维护成本

**事务处理示例**:
```cpp
Transaction transaction(pool_);
transaction.Begin();

// 插入会议记录
mysql_real_query(conn, sql_meeting.c_str(), sql_meeting.size());

// 插入参与者记录
mysql_real_query(conn, sql_participant.c_str(), sql_participant.size());

// 提交事务
transaction.Commit();
```

---

## 错误处理机制

### 错误码体系

**会议领域错误** (`MeetingErrorCode`):
```cpp
enum class MeetingErrorCode {
    kOk = 0,
    kMeetingNotFound = 1,       // 会议不存在
    kMeetingEnded = 2,          // 会议已结束
    kParticipantExists = 3,     // 参与者已存在
    kMeetingFull = 4,           // 会议已满
    kPermissionDenied = 5,      // 权限被拒绝
    kInvalidState = 6,          // 无效状态
};
```

### 错误映射

**Status → MeetingErrorCode**:
```cpp
meeting::core::MeetingErrorCode MapStatus(
    const meeting::common::Status& status) {
    
    using meeting::common::StatusCode;
    switch (status.Code()) {
        case StatusCode::kNotFound:
            return MeetingErrorCode::kMeetingNotFound;
        case StatusCode::kAlreadyExists:
            return MeetingErrorCode::kParticipantExists;
        case StatusCode::kInvalidArgument:
            return MeetingErrorCode::kInvalidState;
        case StatusCode::kUnauthenticated:
            return MeetingErrorCode::kPermissionDenied;
        case StatusCode::kUnavailable:
            return MeetingErrorCode::kMeetingFull;
        default:
            return MeetingErrorCode::kInvalidState;
    }
}
```

### 错误传递链

```
MySQL Error
    ↓
MapMySqlError() → Status
    ↓
Repository → Status / StatusOr<T>
    ↓
Manager → Status / StatusOr<T>
    ↓
Service → MapStatus() → MeetingErrorCode
    ↓
ErrorToProto() → proto::Error
    ↓
ToGrpcStatus() → grpc::Status
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
  "queue_capacity": 1024
}
```

**异步提交**:
```cpp
auto create_future = thread_pool_.Submit([this, command]() {
    return meeting_manager_->CreateMeeting(command);
});
auto result = create_future.get();
```

### 2. 连接池优化

**RAII 连接管理**:
```cpp
auto lease_or = pool_->Acquire();
auto lease = std::move(lease_or.Value());
MYSQL* conn = lease.Raw();
// 自动归还连接
```

### 3. 事务优化

**仅在必要时使用事务**:
- CreateMeeting: 需要事务（插入会议 + 参与者）
- UpdateMeetingState: 单条 UPDATE，无需事务
- AddParticipant: 单条 INSERT，无需事务

### 4. 查询优化

**索引设计**:
```sql
-- 主键索引
PRIMARY KEY (id)

-- 会议 ID 唯一索引（快速查找）
UNIQUE KEY uk_meeting_id (meeting_id)

-- 参与者查询索引
KEY idx_meeting_participants (meeting_id, user_id)

-- 组织者查询索引
KEY idx_meetings_organizer (organizer_id)
```

**JOIN 优化**:
```sql
-- GetMeeting 一次查询获取所有参与者
SELECT user_id FROM meeting_participants 
WHERE meeting_id = (SELECT id FROM meetings WHERE meeting_id = ?)
```

### 5. 内存优化

**参与者列表管理**:
```cpp
// 预分配空间
meeting.participants.reserve(config_.max_participants);

// 移除参与者时使用 erase-remove
participants.erase(
    std::remove(participants.begin(), participants.end(), participant_id),
    participants.end()
);
```

---

## 总结

### 技术亮点

1. **状态机管理**: 清晰的会议生命周期控制
2. **权限控制**: 组织者与参与者角色分离
3. **灵活配置**: 多种会议策略支持
4. **事务处理**: 保证数据一致性
5. **异步处理**: 提高并发性能
6. **自动降级**: Session 验证失败时支持数字 ID 降级

### 业务规则

- ✅ 组织者自动成为首个参与者
- ✅ 首个非组织者加入时会议开始
- ✅ 组织者可主动结束会议
- ✅ 组织者离开可触发会议结束
- ✅ 最后一人离开自动结束会议
- ✅ 已结束的会议无法再加入

### 最佳实践

- ✅ 使用事务保证原子性
- ✅ 参与者列表与会议数据分表存储
- ✅ 读写锁优化并发访问
- ✅ 连接池复用数据库连接
- ✅ 完善的日志记录
- ✅ 详细的错误信息

---

**文档版本**: v1.0  
**最后更新**: 2025-12-12  
**相关文档**: [User Service 实现详解](./user-service-implementation.md) | [架构设计文档](./architecture.md)
