# C++ 标准里的几种锁包装器（RAII）

## 1. std::lock_guard (C++11)

```
std::lock_guard<std::mutex> lg(m);
```

- 只能**单个锁**
- 构造 lock，析构 unlock
- 最简单，不能手动解锁，不支持多锁同时获取
- 不能转移所有权

## 2. std::unique_lock (C++11) 最灵活

```
std::unique_lock<std::mutex> ul(m);
```

特性：

1. 支持手动 `lock()` / `unlock()`，可以中途解锁再上锁
2. 支持延迟加锁、尝试加锁、超时等待
3. 支持锁所有权转移（move）
4. 开销略大于 lock_guard，内部多保存状态

```
// 延迟上锁
std::unique_lock<std::mutex> ul(m, std::defer_lock);
ul.lock();
ul.unlock();
```

常用在 `std::condition_variable`（cv.wait 要求传 unique_lock，**不能用 lock_guard/scoped_lock**）

## 3. std::scoped_lock (C++17)

上面讲的，**专门用于一次性获取多个互斥锁**。

```
std::scoped_lock lk(m1,m2,m3);
```

- 支持 1 个或多个 mutex
- 不支持手动 unlock
- 推荐：**需要同时拿多把锁优先选 scoped_lock**，防止死锁

## 4. std::shared_lock (C++14)

搭配 `std::shared_mutex`（读写锁）

- 共享模式（读锁）：多个线程可以同时持有 shared_lock
- 独占模式（写锁）：unique_lock 获取，排他

```
std::shared_mutex rw_mtx;
// 读：共享锁，允许多线程同时读
std::shared_lock<std::shared_mutex> sl(rw_mtx);

// 写：独占锁，阻塞所有读、写
std::unique_lock<std::shared_mutex> ul(rw_mtx);
```

# 底层原始锁类型（mutex 本身，不是 RAII 包装器）

1. `std::mutex`：普通互斥锁，独占，不可递归
2. `std::recursive_mutex`：递归互斥锁，同一个线程可以多次 lock，需要同样次数 unlock。慎用，容易掩盖逻辑 bug。
3. `std::timed_mutex`：带超时的普通 mutex，`try_lock_for()` / `try_lock_until()`
4. `std::recursive_timed_mutex`：递归 + 超时
5. `std::shared_mutex`：读写锁（C++17，读写分离）