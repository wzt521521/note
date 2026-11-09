Read View 是 InnoDB 存储引擎实现 **MVCC（多版本并发控制）** 的核心数据结构，本质是事务执行**快照读**（普通 `SELECT` 语句）时，生成的一份「当前数据库活跃事务状态快照」，用于判断行数据的历史版本对当前事务是否可见。

### 核心组成字段
- `creator_trx_id`：生成这个 Read View 的事务自身的事务 ID。
- `m_ids`：生成 Read View 时，系统中所有**未提交的活跃事务**的 ID 列表。
- `min_trx_id`：`m_ids` 中的最小事务 ID（可见性低水位）。
- `max_trx_id`：生成 Read View 时，系统下一个待分配的事务 ID（可见性高水位，等于当前最大已分配事务 ID + 1）。

### 可见性判断规则
行记录有一个隐藏列 `trx_id`，记录最后修改该行的事务 ID。针对行的某一个历史版本，判断规则如下：
1. 若版本 `trx_id < min_trx_id`：该版本在快照生成前就已提交，**可见**。
2. 若版本 `trx_id >= max_trx_id`：该版本是快照生成后才开启的事务修改的，**不可见**。
3. 若 `min_trx_id <= trx_id < max_trx_id`：
   - `trx_id` 在 `m_ids` 中：修改该版本的事务在快照生成时还未提交，**不可见**；
   - `trx_id` 不在 `m_ids` 中：修改该版本的事务在快照生成时已提交，**可见**。

额外规则：版本的 `trx_id` 等于 `creator_trx_id`（事务自己修改的版本），对当前事务可见。

### 不同隔离级别的生成时机
这是「读已提交（RC）」和「可重复读（RR）」隔离级别的核心差异之一：
- **可重复读（RR，InnoDB 默认）**：事务内**第一次快照读时生成一次**，后续所有快照读都复用同一个 Read View，保证事务内多次读取结果一致。
- **读已提交（RC）**：事务内**每次快照读都会重新生成**新的 Read View，因此每次都能读到其他事务刚提交的最新数据。

> 补充：Read View 仅作用于**快照读**（普通 `SELECT`）；`SELECT ... FOR UPDATE`、`INSERT`、`UPDATE`、`DELETE` 这类当前读通过加锁实现，不依赖 Read View。

### 前提准备
#### 测试表与初始数据
```sql
CREATE TABLE account (
  id INT PRIMARY KEY,
  balance INT NOT NULL COMMENT '账户余额'
) ENGINE=InnoDB;

INSERT INTO account VALUES (1, 1000);
```

初始行记录的完整结构（含隐藏字段）：

| id | balance | DB_TRX_ID（隐藏） | DB_ROLL_PTR（隐藏） |
|----|---------|-------------------|---------------------|
| 1  | 1000    | 0（系统初始事务） | 空                  |

此时没有 undo log，只有这一条最新数据。

#### 先明确两条核心规则
1. **事务 ID 分配规则**：`BEGIN` 开启事务时不会分配 ID；只有事务执行第一条**写操作**（INSERT/UPDATE/DELETE）或**加锁读**时，才会分配全局唯一自增的 `trx_id`；纯只读事务全程不分配真实事务 ID。
2. **版本链规则**：每次修改数据，旧版本会被拷贝到 undo log 中，通过 `DB_ROLL_PTR` 回滚指针串成一条单向链表，链表头是内存里的最新数据。

---

### 并发事务完整时序
我们开两个会话：事务A（写事务）、事务B（只读事务），按时间顺序依次执行。

| 时间点 | 事务A（写事务） | 事务B（只读事务） |
|--------|-----------------|-------------------|
| t1     | BEGIN;          | —                 |
| t2     | —               | BEGIN;            |
| t3     | UPDATE account SET balance=800 WHERE id=1; | — |
| t4     | —               | SELECT * FROM account WHERE id=1;  -- 第1次查询 |
| t5     | COMMIT;         | —                 |
| t6     | —               | SELECT * FROM account WHERE id=1;  -- 第2次查询 |

---

### 逐步骤底层拆解
#### t1：事务A开启事务
```sql
BEGIN;
```
- 仅标记事务开启，**不分配事务 ID**，事务A此时没有 trx_id。
- 不修改任何数据，行记录、undo log 无变化。

#### t2：事务B开启事务
```sql
BEGIN;
```
- 同样仅标记开启，**不分配事务 ID**，事务B是只读事务，全程都不会拿到真实 trx_id。
- 行记录、undo log 无变化。

#### t3：事务A执行 UPDATE（核心：生成版本链）
```sql
UPDATE account SET balance=800 WHERE id=1;
```
这是事务A的第一条写操作，触发事务 ID 分配，底层一共做 5 件事：
1. **分配全局事务 ID**：事务A拿到全局自增的 `trx_id = 10`。
2. **加行锁**：给 id=1 这行加排他行锁，阻塞其他写事务，但不阻塞普通读。
3. **写 undo log**：把修改前的旧数据（id=1, balance=1000, DB_TRX_ID=0）完整拷贝到 undo log 区域，生成一条 undo 旧版本记录。
4. **修改内存中的行数据**：
   - balance 改为 800
   - `DB_TRX_ID` 改为当前事务 ID：10
   - `DB_ROLL_PTR` 指向刚生成的 undo 旧版本记录
5. **写 redo log**：把「修改数据页」和「生成 undo 页」的操作都记入 redo log 缓冲区，用于崩溃恢复。

此时版本链形成（从新到旧）：
`最新行(balance=800, trx_id=10) → undo旧版本(balance=1000, trx_id=0)`

> 注意：此时事务A还未提交，`trx_id=10` 属于「未提交的活跃事务」。

#### t4：事务B第1次查询（核心：MVCC 快照读）
```sql
SELECT * FROM account WHERE id=1;
```
普通 SELECT 是**快照读**，走 MVCC 逻辑，不加锁，不会被事务A的行锁阻塞。

##### 第一步：生成 Read View（读视图）
事务B是只读事务，没有真实 trx_id，不影响可见性判断。此时系统中只有事务A（trx_id=10）是未提交的活跃事务，因此 Read View 内容为：
- `m_ids = [10]`：当前所有未提交的活跃事务 ID 列表
- `min_trx_id = 10`：活跃事务中的最小 ID
- `max_trx_id = 11`：系统下一个要分配的事务 ID（边界值）
- `creator_trx_id`：只读事务用特殊大值标记，无实际判断作用

##### 第二步：遍历版本链做可见性判断
从最新版本开始，按规则逐条核对：
1. 第一个版本：balance=800，trx_id=10
   - trx_id=10 落在 `[min_trx_id, max_trx_id)` 区间内，且在 `m_ids` 活跃列表中 → 未提交事务的修改，**不可见**。
2. 顺着 `DB_ROLL_PTR` 找到 undo 里的旧版本：balance=1000，trx_id=0
   - trx_id=0 < min_trx_id=10 → 在视图生成前就已提交，**可见**。

✅ 最终查询结果：**balance = 1000**
事务B完全感知不到事务A正在修改数据，读写互不阻塞。

#### t5：事务A提交
```sql
COMMIT;
```
提交时发生 3 件核心事：
1. 释放 id=1 这行的排他行锁。
2. 将 redo log 刷入磁盘，保证本次修改永久持久化（崩溃后可恢复）。
3. **undo log 保留不删除**：因为事务B还处于活跃状态，可能还需要读取旧版本，必须等所有依赖该版本的事务都结束后，才由后台 purge 线程清理。

提交后，`trx_id=10` 从系统的活跃事务列表中移除。

#### t6：事务B第2次查询（核心：RR 隔离级别的体现）
```sql
SELECT * FROM account WHERE id=1;
```
这一步是 RR 和 RC 隔离级别的分水岭，完全由 Read View 的生成时机决定。

##### 情况1：RR 可重复读（MySQL 默认）
- 规则：**事务内只在第一次 SELECT 时生成一次 Read View，后续所有查询复用同一个视图**。
- 事务B继续复用 t4 时刻生成的旧 Read View（`m_ids=[10]`），判断逻辑和第一次完全一致。
- 即使事务A已经提交，旧视图里仍然认为 trx_id=10 是活跃未提交的，因此最新版本依然不可见。

✅ 最终查询结果：**balance = 1000**
整个事务内两次查询结果完全一致，实现了“可重复读”。

##### 情况2：RC 读已提交
- 规则：**每次执行 SELECT 都会重新生成最新的 Read View**。
- 此时事务A已提交，新的 Read View 里 `m_ids=[]`（没有活跃事务），`min_trx_id=11`，`max_trx_id=11`。
- 重新判断：最新版本 trx_id=10 < min_trx_id=11 → 可见。

✅ 最终查询结果：**balance = 800**
能读到其他事务刚提交的最新数据，存在不可重复读。

---

### 对应 undo log、redo log 的关系
1. **与 undo log：强绑定，MVCC 的物理基础**
   例子里 balance=1000 的旧版本就存在 undo log 里。没有 undo log 就没有版本链，MVCC 找不到历史数据，读写不阻塞就无法实现。事务提交后 undo 不能立刻删除，就是因为还有活跃事务的 Read View 依赖这些旧版本。

2. **与 redo log：弱关联，底层可靠性兜底**
   redo log 不参与任何可见性判断，MVCC 的核心逻辑完全用不到它。它的作用是：如果 t3 执行完、事务提交前数据库崩溃，内存里的数据和 undo 都会丢失，重启后靠 redo log 可以把数据页和 undo 页完整恢复回来，保证 MVCC 能正常工作。

---

### 补充：什么情况不走 MVCC
如果是**当前读**（加锁读），会直接读取最新提交的数据并加锁，不经过 MVCC 版本链判断：
```sql
-- 共享锁当前读
SELECT * FROM account WHERE id=1 LOCK IN SHARE MODE;
-- 排他锁当前读
SELECT * FROM account WHERE id=1 FOR UPDATE;
```
在 t5 事务A提交后，事务B执行 `FOR UPDATE`，会直接读到 balance=800，因为它读取的是最新提交的版本，不看历史快照。