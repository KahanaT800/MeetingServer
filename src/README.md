# Meeting Server Source Layout
- `server/`: gRPC 服务端入口与模块集成。
- `core/`: 业务核心逻辑（聚合用户、会议、通知等领域服务）。
- `thread_pool/`: 线程池与异步任务调度组件。
- `storage/`: 数据访问封装（数据库、持久化相关）。
- `cache/`: 缓存层组件（Redis/LRU 等策略）。
- `auth/`: 认证鉴权、令牌验证。
- `utils/`: 通用工具与基础设施代码。