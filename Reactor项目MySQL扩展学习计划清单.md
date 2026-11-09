# 自定义 Reactor + MySQL 学习计划（V2.1 精简版）

> 最近更新：2026-10-30
> 当前项目：`module5/自定义reactor实现`
> 定位：个人 C++、MySQL、并发与系统原理学习 Demo
> 当前进度：第 0～8 轮主线完成，下一步进入第 9 轮

## 一、项目定位与边界

目标是基于现有自定义 Reactor，学习 C++ 数据库访问、并发控制、SQL 性能和
MySQL/InnoDB 原理。聊天消息只作为一种实验负载，不建设完整聊天产品。

固定技术方向：

- [x] C++17、CMake、`libmysqlclient` 原生 C API。
- [x] 保留 `[total_len | cmd | body]` 二进制协议。
- [x] 同步 MySQL API 只在 DB Worker 中执行。
- [x] 不引入 JSON、HTTP、ORM、Web Controller 或 Java 路线。
- [x] 个人学习 Demo 只做支撑当前知识点的必要验收。

不作为主线目标：

- 完整注册、权限、房间、未读和管理后台。
- 生产级连接池、自动扩缩容和无限自动重试。
- Redis、Kafka、微服务、分库分表或修改 MySQL 内核。
- 没有数据依据的复杂优化和大规模压力测试。

## 二、安全与操作规则

- 修改源码、安装软件、创建或修改数据库、提交 Git 前先说明动作并获得允许。
- 密码不写入源码、SQL、脚本、计划、命令行参数或日志；需要时通过终端隐藏输入。
- 不丢弃、覆盖或重置用户已有改动，不擅自提交。
- 数据库实验优先使用事务回滚或专用实验数据，不影响重要数据。
- 一次只推进一个小检查点，先解释字段、约束、调用链和资源所有权。

## 三、当前架构基线

```text
TCP Client
    │  [total_len | cmd | body]
    ▼
I/O EventLoop
    ├── socket 读写、Packet 编解码
    ├── 请求字段校验
    └── 构造 DbTask
            │
            ▼
DbExecutor：4 个 Worker + 共享有界队列
            │
            ▼
ConnectionPool：2 条连接
    ├── acquireFor(250ms)
    ├── ConnectionLease 独占连接
    └── 归还前 mysql_reset_connection()
            │
            ▼
MySQL / 值类型 DbResult
            │ queueInLoop
            ▼
原连接所属 I/O EventLoop
```

核心边界：

- I/O EventLoop 不执行阻塞 SQL。
- 一个连接同一时间只允许一个 Worker 持有。
- Worker 不操作 socket，不向结果中传递 `MYSQL*`、结果集或网络对象。
- 结果通过 `queueInLoop` 返回原线程，连接以 `weak_ptr` 检查是否仍存在。
- 队列满立即拒绝；`stop()` 停止接收并排空已接受任务。
- 连接重置失败后直接丢弃；当前不会自动补建。

当前数据库状态：

- 数据库：`reactor_lab`，MySQL 8.0.46。
- 五张负载表当前为空：`users`、`messages`、`accounts`、
  `transfer_requests`、`ledger_entries`。
- `schema_migrations` 保留 6 条版本记录。
- 表均使用 InnoDB、`utf8mb4`、`utf8mb4_0900_ai_ci`。

## 四、统一协议与状态码

外层帧保持：

```text
[total_len:uint32][cmd:uint16][body:bytes]
```

异步数据库请求统一携带：

```text
请求：[request_id:uint32][command-specific fields...]
响应：[request_id:uint32][status_code:uint16][result fields...]
```

当前状态码：

| 状态 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | DB 等待队列已满 |
| 2 | 数据库连接不可用 |
| 3 | 请求格式错误 |
| 4 | 等待连接租约超时 |
| 5 | MySQL 查询失败 |
| 6 | 唯一约束冲突 |
| 7 | 数据不存在 |

当前数据库命令：

- `cmd=5`：数据库健康检查。
- `cmd=6`：创建实验用户。
- `cmd=7`：按 `user_id` 查询实验用户。

## 五、已完成轮次

## 第 0 轮：Reactor 与协议基线（完成）

完成内容：

- [x] CMake/CTest 构建入口。
- [x] Buffer、Packet 和协议集成测试。
- [x] 半包、粘包、非法帧隔离和多客户端 Echo。
- [x] Debug 与代表性 ASan/UBSan 验证。

保留边界：未把信号驱动的整进程优雅停机作为个人 Demo 前置门槛。

## 第 1 轮：MySQL CLI 与 SQL 基础（完成）

完成内容：

- [x] 核对 MySQL 服务、配置、日志、端口、Socket 和账号权限。
- [x] 建立 `sql_labs/01_basics` 五个基础实验脚本。
- [x] 覆盖 DDL/DML/DQL、类型、NULL、约束、关系、JOIN、聚合、
  子查询、CTE、窗口函数和基础事务。
- [x] 管理账号负责 DDL，应用账号负责数据读写。

收尾基线曾验证：`departments=3`、`employees=4`、`employee_profiles=2`、
`projects=2`、`employee_projects=4`，无非法数据和实验字段残留。

## 第 2 轮：独立 db_cli 与 MySQL C API（完成）

完成内容：

- [x] 独立单线程 `db_cli`，不依赖 Reactor。
- [x] `MySqlLibrary`、`MySqlConnection`、`MySqlStatement`、
  `MySqlTransaction` 和 `DbError`。
- [x] 普通查询、预处理 SELECT/INSERT/UPDATE、参数和结果绑定。
- [x] 缓冲/流式结果、NULL、DECIMAL、BLOB、错误码和 SQLSTATE。
- [x] move-only RAII、回滚保护和代表性集成测试。

必要验收曾通过 Debug CTest 和代表性 ASan/UBSan。没有混入 Reactor、
多线程或连接池。

## 第 3 轮：DB Worker 与连接池（完成）

完成内容：

- [x] 4 个固定 Worker、共享有界等待队列、队列满立即拒绝。
- [x] 2 条固定连接、move-only `ConnectionLease`、250ms 获取超时。
- [x] 每个 Worker 执行 `mysql_thread_init/end`。
- [x] `cmd=5` 健康查询经 Worker 执行并返回原 I/O EventLoop。
- [x] 客户端提前断开后，弱引用丢弃迟到结果。
- [x] 连接归还前重置会话；重置失败的连接不再入池。

必要实测：

- 4 Worker 争抢 2 条连接表现为两批完成。
- 两条连接被占用时，其余 Worker 约 250ms 后返回状态 4。
- 队列满返回状态 1，Echo 不被 DB Worker 阻塞。
- 会话变量和 `autocommit=0` 在重置后恢复为干净状态。

保留边界：没有自动重连、连接补建、动态扩缩容、健康淘汰或压力测试；
当前实现不能描述为生产连接池。

## 第 4 轮：数据模型、约束与典型负载（完成）

完成模型：

| 表 | 核心目的 |
|---|---|
| `users` | 代理键、自然唯一键、NULL、排序规则、逻辑删除 |
| `messages` | 追加写、外键、范围查询、Offset/游标分页 |
| `accounts` | `BIGINT balance_cents` 精确金额 |
| `transfer_requests` | 唯一 `request_key` 和幂等身份 |
| `ledger_entries` | 追加式审计流水和联合唯一约束 |
| `schema_migrations` | 结构版本与真实 ALTER 实验 |

完成内容：

- [x] 主键、唯一键、CHECK、NULL、字符排序和外键行为。
- [x] 规范化、受控冗余、逻辑/物理删除和整数最小金额单位。
- [x] 代表性 INSERT、SELECT、UPDATE、DELETE。
- [x] 批量生成 100 users、100 accounts、300 messages 后回滚。
- [x] `cmd=6/7` 完成用户创建与查询真实 Reactor 调用链。

精简验收结果：重复键、非法金额、同账户转账、重复流水和外键操作均被
数据库拒绝；负载脚本回滚后五张负载表为 0 行，迁移表保留 6 个版本。

第四轮只建立模型和负载。真正的原子转账、并发余额更新、幂等重试、
MVCC、锁和死锁属于第五轮。

## 六、后续主线

## 第 5 轮：事务、MVCC、锁、死锁与幂等

核心检查点：

1. 在同一个 `ConnectionLease` 和事务内完成扣款、入账、请求状态和两条流水。
2. 使用唯一 `request_key` 保证重复请求不产生第二次资金变化。
3. 用条件 UPDATE 或锁定读处理余额不足和并发更新。
4. 用两个连接观察隔离级别、快照读、当前读和锁等待。
5. 构造一次反向转账死锁，解释原因并实现有限重试。

最小验收：

- [x] 成功转账后总余额不变，流水一正一负。
- [x] 任一中途失败全部回滚。
- [x] 相同请求重复执行不重复扣款。
- [x] 两连接复现一次锁等待和一次死锁；不做压力测试。

完成结果（2026-10-29）：

- `cmd=8` 已接入 Reactor → DB Worker → `ConnectionLease` 调用链；同一租约和
  `MySqlTransaction` 内完成账户锁、扣款、入账、请求状态和两条流水。
- 相同 `request_key`、相同参数返回原转账 ID；参数冲突返回状态 9，余额不足
  返回状态 8。
- `REPEATABLE-READ` 下实测普通快照读保持旧值，`SELECT ... FOR UPDATE` 当前读
  看到已提交新值；条件 UPDATE 在锁等待后根据最新余额影响 0 行，未发生超扣。
- 双连接反向加锁实测 `ERROR 1213`；C++ 转账按账户 ID 升序加锁，并对 1213、
  1205 最多整体重试 3 次。
- 两个并发相同幂等请求返回同一转账 ID，数据库只产生一次余额变化和两条流水；
  实验结束后五张负载表恢复为空，迁移表仍为 6 行。

## 第 6 轮：索引、优化器与分页

核心检查点：

- [x] 用可配置生成器建立有限规模的 messages/ledger 数据。
- [x] 使用 `EXPLAIN ANALYZE` 观察聚簇索引、二级索引、回表和范围扫描。
- [x] 验证复合索引、最左前缀和覆盖索引。
- [x] 对比深 Offset 与基于 `(id)` 或 `(created_at,id)` 的游标分页。

最小验收：选择两条代表性查询，记录数据量、SQL、索引、执行计划和耗时；
不要求百万级数据或高并发压测。

完成结果（2026-10-30）：生成 30000 messages 和 20000 ledger 后，深 Offset
实际扫描 30000 行并排序、约 19.1 ms；展开条件的 `(created_at,id)` 游标分页
使用联合范围扫描，实际读取 20 行、约 0.098 ms。覆盖索引、最左前缀和回表
均已实测；实验结束后五张负载表恢复为空，迁移表仍为 6 行。

## 第 7 轮：InnoDB 内部机制（完成）

核心检查点：

- [x] 解释页、聚簇索引页、二级索引页和 Buffer Pool。
- [x] 观察脏页、Redo、Undo、Binlog 和 Checkpoint。
- [x] 梳理事务从 COMMIT 到持久化的大致路径。
- [x] 仅在专用实验环境进行一次受控异常退出和恢复观察。

最小验收：保留一份提交路径说明和一组状态/日志证据，不在重要实例上做
破坏性实验。

完成结果（2026-10-30）：专用 3307 实例提交 10000 行后实测 748 个脏页、
LSN 前进而 Checkpoint 未立即前进；有限刷页后脏页归零且两者追平。一次受控
`SIGKILL` 后，已提交行恢复、未提交行撤销，表检查为 `OK`；错误日志和
ROW Binlog 均保留了恢复与已提交事务证据。临时实例和数据目录已清理。

## 第 8 轮：C++ 数据库访问性能（完成）

核心检查点：

- [x] 对比预处理语句复用、单行/批量写入、缓冲/流式结果。
- [x] 比较少量连接池大小，分解队列等待、连接等待、SQL 和结果读取耗时。
- [x] 限制任务队列、结果行数和结果字节数，验证背压边界。
- [x] 只对实际热点使用 ASan/UBSan、必要时 Valgrind/perf。

最小验收：同一数据集和负载下报告吞吐及 P50/P95/P99，并能指出瓶颈所在；
不把“连接更多”默认视为更快。

完成结果（2026-10-30）：同连接复用prepared吞吐约为每次prepare的1.8倍；
300次写入放入一个事务后吞吐约为autocommit的24倍。4 Worker下1/2/4连接的
代表性吞吐为3964/8663/17477 ops/s，尾延迟主要来自突发队列等待；队列容量1
时实测立即拒绝97/100任务。结果行/字节上限和ASan+UBSan均通过，夹具已清理。

## 第 9 轮：可靠性、监控、迁移与恢复

核心检查点：

1. 启动时检查 schema 版本，形成可重复 migration 流程。
2. 记录队列长度、连接使用量、错误数和关键 SQL 耗时。
3. 使用慢查询日志或 Performance Schema 定位一条代表性问题 SQL。
4. 验证 MySQL 中断时 Reactor 仍能处理非数据库请求。
5. 完成一次逻辑备份并恢复到全新实验实例。
6. 补齐整进程停机顺序：停止接入、停止新任务、排空 Worker、关闭连接、
   停止 EventLoop。

最小验收：一次中断/恢复、一次备份/恢复和一次停机流程；不建设完整生产监控平台。

## 第 10 轮：可选高级实验

主线完成后再决定是否学习：

- MySQL 非阻塞 C API 与 epoll 状态机。
- 主从复制、读写分离和读后写一致性。
- Row Binlog、CDC、断点续传和至少一次投递。

这些内容不作为第 0～9 轮的完成条件。

## 七、计划中的核心 C++ 组件

| 组件 | 状态 | 职责 |
|---|---|---|
| `MySqlLibrary` | 已完成 | 客户端库生命周期 |
| `MySqlConnection` | 已完成 | move-only 连接 RAII |
| `MySqlStatement` | 已完成 | 预处理语句和绑定 |
| `MySqlTransaction` | 已完成基础版 | 回滚与 autocommit 恢复 |
| `DbError` | 已完成 | 错误码、SQLSTATE、安全描述 |
| `DbTask/DbResult` | 已完成基础版 | 跨线程值类型任务和结果 |
| `DbExecutor` | 已完成 | 有界队列和 Worker 生命周期 |
| `ConnectionPool/Lease` | 已完成基础版 | 有界连接复用和独占租约 |
| `BinaryReader/Writer` | 待实现 | 多字段二进制协议边界检查 |
| `BenchmarkRunner` | 待第 6/8 轮 | 负载、吞吐和延迟统计 |

组件只封装明确资源边界，不建设万能数据库框架。

## 八、精简验收规则

以后默认遵守：

1. 每个检查点只回答一个核心问题。
2. 只改 SQL：运行当前脚本，检查代表性成功/失败和最终数据状态；不构建 C++。
3. 改 C++：Debug 构建受影响目标，运行一个针对性测试和一次必要真实调用。
4. 并发/锁实验：两个连接足够，不做压力测试。
5. 性能数据只在第 6、8 轮作为必要证据，并明确环境和数据规模。
6. 已验证且未被改动的旧路径不重复验收。
7. 静态代码结论与运行实测分开表述。
8. 每轮保留实际 SQL、关键输出、调用链说明和清晰的未完成边界。

## 九、主线最终完成标准

- 能从业务不变量设计类型、主键、唯一键、CHECK、外键和事务。
- 能解释并实测 MVCC、隔离级别、行锁、死锁和幂等。
- 能用 `EXPLAIN ANALYZE` 和有限基准定位 SQL/索引问题。
- 能说明 Buffer Pool、Redo、Undo、Binlog 和事务提交路径。
- 能正确管理 C API 资源、线程、连接、结果缓冲区和跨线程回调。
- 能判断瓶颈位于协议、队列、连接池、SQL、锁还是磁盘。
- 能完成 migration、最小监控、备份恢复和安全停机实验。
- 能清楚说明个人 Demo 与生产系统之间仍存在的差距。
