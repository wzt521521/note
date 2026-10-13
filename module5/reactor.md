```cpp
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <memory>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cassert>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

// 使用单调时钟：不受系统时间回拨影响，适合定时器、心跳计算
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;  // 允许直接写 8s、1s 这种时长字面量

// ================= Buffer：应用层环形缓冲区 =================
// 设计：三段式内存布局 [头部预留区 | 待读数据区 | 可写空间区]
// 核心优势：读取只移动指针、零内存拷贝；空间不足时优先前移腾空间，减少扩容次数
// 新增：支持前置写入（prepend），利用预留区零拷贝封装协议头部
class Buffer {
public:
    static const size_t kCheapPrepend = 8;    // 头部预留8字节，方便协议封包时直接在前面填头部
    static const size_t kInitialSize  = 1024; // 初始容量
    explicit Buffer(size_t n = kInitialSize)
        : buffer_(kCheapPrepend + n),    // 总大小=预留区+初始可写区
        readIndex_(kCheapPrepend),     // 读指针初始在预留区末尾
        writeIndex_(kCheapPrepend) {}  // 写指针初始和读指针重合，无待读数据

    // 当前可读字节数
    size_t readableBytes() const { return writeIndex_ - readIndex_; }
    // 当前可写字节数
    size_t writableBytes() const { return buffer_.size() - writeIndex_; }
    // 当前可前置写入的字节数（读指针前方的空闲空间）
    size_t prependableBytes() const { return readIndex_; }

    // 获取可读数据起始指针（不移动读指针）
    const char* peek() const { return &*buffer_.begin() + readIndex_; }

    // 读走len字节：仅移动读指针，不做内存拷贝（零拷贝读取）
    void retrieve(size_t len) {
        if (len < readableBytes()) readIndex_ += len;
        else readIndex_ = writeIndex_ = kCheapPrepend; // 全部读完，指针复位到预留区末尾
    }

    // 读取4字节网络序整数（memcpy规避内存对齐问题）
    uint32_t peekInt32() const {
        uint32_t x; std::memcpy(&x, peek(), 4);
        return ntohl(x); // 网络序转主机序
    }
    // 读取指定偏移处的2字节网络序整数
    uint16_t peekInt16(size_t o) const {
        uint16_t x; std::memcpy(&x, peek() + o, 2);
        return ntohs(x);
    }

    // 追加数据到缓冲区末尾，空间不足自动腾挪/扩容
    void append(const char* p, size_t len) {
        if (writableBytes() < len) makeSpace(len);
        std::copy(p, p + len, &*buffer_.begin() + writeIndex_);
        writeIndex_ += len;
    }
    void append(const std::string& s) { append(s.data(), s.size()); }

    // 前置写入数据到可读区域之前（零拷贝封包核心）
    // 利用读指针前方的空闲空间写入协议头部，无需移动已有数据
    void prepend(const char* data, size_t len) {
        assert(len <= prependableBytes() && "预留空间不足，无法前置写入");
        readIndex_ -= len;
        std::copy(data, data + len, &*buffer_.begin() + readIndex_);
    }

    // 从fd批量读取数据：readv分散读，先填缓冲区，溢出部分落到栈上应急区
    // 减少系统调用次数，同时避免一次读入过多直接触发缓冲区扩容
    ssize_t readFd(int fd, int* saveErrno) {
        char extrabuf[65536];   // 栈上64KB应急缓冲区
        struct iovec vec[2];    // 两块内存：第一块是Buffer可写区，第二块是栈上应急区
        size_t w = writableBytes();
        vec[0].iov_base = &*buffer_.begin() + writeIndex_;
        vec[0].iov_len  = w;
        vec[1].iov_base = extrabuf;
        vec[1].iov_len  = sizeof(extrabuf);
        // 可写区小于应急区才用两块，否则只用一块
        ssize_t n = ::readv(fd, vec, w < sizeof(extrabuf) ? 2 : 1);
        if (n < 0) {
            *saveErrno = errno;
        } else if (static_cast<size_t>(n) <= w) {
            writeIndex_ += n;  // 数据全落在Buffer内，只移动写指针
        } else {
            writeIndex_ = buffer_.size();          // 第一块写满
            append(extrabuf, static_cast<size_t>(n) - w); // 溢出部分追加，触发扩容
        }
        return n;
    }

private:
    // 获取缓冲区起始地址
    char* begin() { return &*buffer_.begin(); }
    const char* begin() const { return &*buffer_.begin(); }

    // 确保可写空间足够，不够就腾挪或扩容
    void makeSpace(size_t len) {
        // 预留区+可写区 总空间都不够 → 直接扩容
        if (writableBytes() + readIndex_ - kCheapPrepend < len) {
            buffer_.resize(writeIndex_ + len);
        }
        // 空间够但集中在头部 → 把未读数据整体前移到预留区末尾，腾出尾部空间
        else {
            size_t r = readableBytes();
            std::copy(begin() + readIndex_,  // 源：当前读指针位置
                      begin() + writeIndex_, // 源：当前写指针位置
                      begin() + kCheapPrepend); // 目标：预留区末尾
            readIndex_  = kCheapPrepend;      // 读指针复位，保证前方有固定预留空间
            writeIndex_ = readIndex_ + r;     // 写指针对应移动
        }
    }

    std::vector<char> buffer_;  // 底层内存容器
    size_t readIndex_;          // 读指针偏移量
    size_t writeIndex_;         // 写指针偏移量
};

// 前向声明，用于PacketCodec中的回调类型
class TcpConnection;

// ================= PacketCodec：协议编解码 =================
// 帧格式：[总长度4字节(网络序，含头部) | 命令字2字节(网络序) | 包体数据]
// 作用：解决TCP粘包半包，把字节流转换成结构化消息帧
// 优化：支持直接在Buffer上零拷贝编码，利用预留头部前置写入协议头
class PacketCodec {
public:
    static constexpr size_t kHeaderLen = 6;       // 头部总长度：4+2
    static constexpr uint32_t kMaxPacket = 10u * 1024 * 1024; // 最大包长10MB，防攻击

    using FrameCallback = std::function<void(const TcpConnection::Ptr&, uint16_t, const std::string&)>;

    void setFrameCallback(FrameCallback cb) { frameCallback_ = std::move(cb); }

    // 解码：从输入缓冲区循环切出完整帧，半包留下等待
    void decode(const TcpConnection::Ptr& conn, Buffer* buf) {
        while (buf->readableBytes() >= 4) { // 够读长度字段
            uint32_t total = buf->peekInt32();
            // 长度合法性校验，非法直接断开
            if (total < kHeaderLen || total > kMaxPacket) {
                conn->forceClose(); return;
            }
            if (buf->readableBytes() < total) break; // 半包，等下一批数据
            uint16_t cmd = buf->peekInt16(4); // 读取命令字
            std::string body(buf->peek() + kHeaderLen, total - kHeaderLen); // 读取包体
            buf->retrieve(total); // 整帧读走，移动指针
            if (frameCallback_) frameCallback_(conn, cmd, body); // 交付上层
        }
    }



    // 编码：直接在Buffer上零拷贝构造帧（核心优化）
    // 先追加包体，再前置写入头部，全程无额外内存拷贝
    static void encode(Buffer* buf, uint16_t cmd, const std::string& body) {
        uint32_t totalLen = kHeaderLen + body.size();
        // 1. 先写入包体到缓冲区尾部
        buf->append(body.data(), body.size());
        // 2. 前置写入2字节命令字
        uint16_t cmdNet = htons(cmd);
        buf->prepend(reinterpret_cast<const char*>(&cmdNet), 2);
        // 3. 前置写入4字节总长度
        uint32_t lenNet = htonl(totalLen);
        buf->prepend(reinterpret_cast<const char*>(&lenNet), 4);
    }

private:
    FrameCallback frameCallback_;
};

// ================= Channel：IO事件分发器 + 生命周期守卫 =================
// 作用：封装一个fd的所有事件监听与回调分发；通过tie机制保证回调期间对象存活
class Channel {
public:
    using EventCallback = std::function<void()>; // 所有事件回调统一格式：无参无返回值
    explicit Channel(int fd) : fd_(fd) {}
    int fd() const { return fd_; }
    uint32_t events() const { return events_; }

    // 事件开关：通过位运算设置要监听的事件类型
    void enableReading()  { events_ |= (EPOLLIN | EPOLLPRI | EPOLLRDHUP); }
    void enableWriting()  { events_ |= EPOLLOUT; }
    void disableWriting() { events_ &= ~EPOLLOUT; }
    void disableAll()     { events_ = 0; }
    bool isWriting() const { return events_ & EPOLLOUT; }

    // 设置四类事件回调
    void setReadCallback(EventCallback cb)  { readCallback_  = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    // 绑定持有者对象（弱引用，不增加引用计数，避免循环引用）
    void tie(const std::shared_ptr<void>& o) {
        tie_ = o;
        tied_ = true;
    }

    // 事件分发入口：先续命，再分发事件
    void handleEvent(uint32_t revents) {
        std::shared_ptr<void> guard; // 栈上局部强引用，为持有者对象续命
        if (tied_) {
            guard = tie_.lock(); // 弱引用升级为强引用
            if (!guard) return;  // 升级失败说明对象已销毁，直接丢弃事件
        }
        // 走到这里保证对象存活，按优先级分发事件
        if ((revents & EPOLLHUP) && !(revents & EPOLLIN) && closeCallback_)
            closeCallback_();
        if ((revents & EPOLLERR) && errorCallback_)
            errorCallback_();
        if ((revents & (EPOLLIN | EPOLLPRI)) && readCallback_)
            readCallback_();
        if (revents & EPOLLOUT && writeCallback_)
            writeCallback_();
    }

private:
    int fd_;               // 关联的文件描述符
    uint32_t events_ = 0;  // 当前监听的事件掩码
    EventCallback readCallback_, writeCallback_, closeCallback_, errorCallback_;
    std::weak_ptr<void> tie_; // 持有者对象弱引用，只观察不持有
    bool tied_ = false;       // 是否已绑定持有者
};

// ================= EventLoop：epoll事件循环 =================
// 作用：单线程事件驱动器，管理所有Channel，阻塞等待IO事件并分发
class EventLoop {
public:
    EventLoop() : activeEvents_(16) {
        epollfd_ = epoll_create1(EPOLL_CLOEXEC); // 创建epoll实例，exec时自动关闭
    }
    ~EventLoop() { close(epollfd_); }

    // 注册/更新Channel到epoll
    void updateChannel(Channel* ch) {
        epoll_event ev{};
        ev.events = ch->events();
        ev.data.ptr = ch; // 预埋Channel指针，事件返回时直接带回，避免查表
        if (channels_.count(ch->fd()))
            epoll_ctl(epollfd_, EPOLL_CTL_MOD, ch->fd(), &ev); // 已存在则修改
        else {
            epoll_ctl(epollfd_, EPOLL_CTL_ADD, ch->fd(), &ev); // 不存在则新增
            channels_[ch->fd()] = ch;
        }
    }

    // 从epoll移除Channel
    void removeChannel(Channel* ch) {
        epoll_ctl(epollfd_, EPOLL_CTL_DEL, ch->fd(), nullptr);
        channels_.erase(ch->fd());
    }

    // 事件循环主函数：阻塞等待事件，逐个分发
    void loop() {
        for (;;) {
            int n = epoll_wait(epollfd_, activeEvents_.data(),
                               static_cast<int>(activeEvents_.size()), -1);
            for (int i = 0; i < n; ++i)
                static_cast<Channel*>(activeEvents_[i].data.ptr)
                    ->handleEvent(activeEvents_[i].events);
        }
    }

private:
    int epollfd_;                           // epoll实例fd
    std::vector<struct epoll_event> activeEvents_; // 就绪事件数组
    std::unordered_map<int, Channel*> channels_;  // fd到Channel的映射表
};

// ================= TimerQueue 定时器队列 =================
// 设计：基于 timerfd 实现，定时器事件也变成IO事件，挂到同一个epoll上
// 优势：单线程统一事件驱动，无多线程锁开销，时序精准
// 两个容器配合：
//   schedule_：按到期时间排序，快速找到最早到期的定时器
//   timers_：按id索引，快速查找、取消定时器
using TimerId = uint64_t; // 定时器唯一ID
struct Timer {
    std::function<void()> cb;       // 定时器回调
    Clock::time_point expiration;   // 到期时间点
    Clock::duration interval{0};    // 周期间隔（0表示一次性）
    bool repeat = false;            // 是否周期执行
    TimerId id = 0;                 // 定时器ID
};

class TimerQueue {
public:
    explicit TimerQueue(EventLoop* loop)
        : loop_(loop),
          // 创建定时器fd：单调时钟 + 非阻塞 + 执行时自动关闭
          timerfd_(::timerfd_create(CLOCK_MONOTONIC,
                                   TFD_NONBLOCK | TFD_CLOEXEC)),
          channel_(timerfd_) {
        // 定时器fd也走Channel事件分发机制，和网络fd完全一致
        channel_.setReadCallback([this] { handleTimeout(); });
        channel_.enableReading();
        loop_->updateChannel(&channel_);
    }
    ~TimerQueue() { ::close(timerfd_); }

    // 添加定时器
    // delay：首次执行延迟
    // interval：周期间隔，大于0则周期执行
    // 返回：定时器ID，可用于取消
    TimerId addTimer(std::function<void()> cb,
                     Clock::duration delay,
                     Clock::duration interval = Clock::duration::zero()) {
        Timer t;
        t.id = ++nextId_;       // 分配唯一ID
        t.cb = std::move(cb);
        t.expiration = Clock::now() + delay; // 计算首次到期时间
        t.repeat = interval.count() > 0;
        t.interval = interval;

        // 判断新加入的定时器是不是最早到期的
        bool earliestChanged =
            schedule_.empty() || t.expiration < schedule_.begin()->first;
        TimerId id = t.id;
        timers_[id] = std::move(t);               // 存入id索引表
        schedule_.emplace(timers_[id].expiration, id); // 插入时间排序表
        if (earliestChanged) armTimerfd(); // 最早到期时间变了，重设timerfd
        return id;
    }

    // 取消指定ID的定时器
    void cancel(TimerId id) {
        auto it = timers_.find(id);
        if (it == timers_.end()) return;
        // 从时间排序表中精确删除（同时间可能有多个，按id匹配）
        auto range = schedule_.equal_range(it->second.expiration);
        for (auto q = range.first; q != range.second; ++q) {
            if (q->second == id) {
                schedule_.erase(q);
                break;
            }
        }
        timers_.erase(it);
        armTimerfd(); // 取消后重设最早到期时间
    }

private:
    // 重设timerfd的到期时间
    void armTimerfd() {
        struct itimerspec its{}; // 初始全0：全0表示解除定时器
        if (!schedule_.empty()) {
            auto now = Clock::now();
            auto when = schedule_.begin()->first; // 取最早到期时间
            if (when <= now) when = now + 1ns;   // 已经过期就设为1ns后立即触发
            // 计算时间差，拆成秒和纳秒
            auto d = when - now;
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(d);
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d - secs);
            its.it_value.tv_sec  = secs.count();
            its.it_value.tv_nsec = ns.count();
        }
        // 设置定时器，it_interval为0表示不重复（内部自己管理周期）
        ::timerfd_settime(timerfd_, 0, &its, nullptr);
    }

    // 定时器到期处理函数
    void handleTimeout() {
        uint64_t expireCount = 0;
        // 读取timerfd，清空事件（否则会一直触发读事件）
        ssize_t n = ::read(timerfd_, &expireCount, sizeof(expireCount));
        (void)n;

        auto now = Clock::now();
        // ---------- 阶段一：收集所有到期的定时器，先从容器里摘出来 ----------
        // 原因：回调函数里可能增删定时器，如果边遍历边执行会导致迭代器失效
        std::vector<Timer> fired;
        auto it = schedule_.begin();
        while (it != schedule_.end() && it->first <= now) {
            auto tit = timers_.find(it->second);
            if (tit != timers_.end()) {
                fired.push_back(std::move(tit->second)); // 移到临时数组
                timers_.erase(tit);
            }
            it = schedule_.erase(it); // 从时间表移除
        }

        // ---------- 阶段二：执行回调，周期任务重新入队 ----------
        for (auto& t : fired) {
            if (t.cb) t.cb(); // 执行回调
            // 周期定时器：计算下一次到期时间，重新插入队列
            if (t.repeat) {
                auto next = t.expiration + t.interval;
                // 落后太多（比如系统阻塞），从当前时间重新排，避免雪崩式集中触发
                if (next <= now) next = now + t.interval;
                t.expiration = next;
                TimerId id = t.id;
                timers_[id] = std::move(t);
                schedule_.emplace(next, id);
            }
        }
        armTimerfd(); // 重新对准下一个最早到期点
    }

    EventLoop* loop_;
    int timerfd_;                      // 定时器文件描述符
    Channel channel_;                  // 定时器fd对应的Channel，走统一事件分发
    TimerId nextId_ = 0;               // 自增ID生成器
    std::multimap<Clock::time_point, TimerId> schedule_; // 按到期时间排序：时间→ID
    std::unordered_map<TimerId, Timer> timers_;          // 按ID索引：ID→Timer对象
};

// ================= Acceptor：TCP连接接收器 =================
// 作用：监听端口，接受新连接，通过回调交付给上层业务
class Acceptor {
public:
    using NewConnectionCallback = std::function<void(int, const std::string&, uint16_t)>;
    Acceptor(EventLoop* loop, uint16_t port)
        : loop_(loop),
          listenFd_(createListenFd(port)),
          // 预留一个空闲fd，用于fd耗尽时应急腾位
          idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)),
          acceptChannel_(listenFd_) {
        acceptChannel_.setReadCallback([this] { handleRead(); });
    }
    ~Acceptor() {
        ::close(listenFd_);
        if (idleFd_ >= 0) ::close(idleFd_);
    }

    void setNewConnectionCallback(NewConnectionCallback cb) { cb_ = std::move(cb); }
    // 启动监听：把监听Channel注册到epoll
    void listen() {
        acceptChannel_.enableReading();
        loop_->updateChannel(&acceptChannel_);
    }

private:
    // 创建监听socket
    static int createListenFd(uint16_t port) {
        int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           IPPROTO_TCP);
        int opt = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 端口复用
        struct sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (::bind(lfd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); _exit(1); }
        if (::listen(lfd, 128) < 0) { perror("listen"); _exit(1); }
        return lfd;
    }

    // 处理新连接事件
    void handleRead() {
        for (;;) {
            struct sockaddr_in peer{}; socklen_t len = sizeof(peer);
            // 接受连接，直接设为非阻塞
            int cfd = ::accept4(listenFd_, (struct sockaddr*)&peer, &len,
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd >= 0) {
                char ip[INET_ADDRSTRLEN] = {0};
                ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                if (cb_) cb_(cfd, ip, ntohs(peer.sin_port)); // 交付上层
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 连接取完了
                // fd耗尽应急处理：释放预留fd→accept出连接并关闭→重新预留
                // 解决：fd耗尽时监听fd持续触发读事件导致CPU100%忙等
                if (errno == EMFILE || errno == ENFILE) {
                    ::close(idleFd_);
                    int fd = ::accept(listenFd_, nullptr, nullptr);
                    if (fd >= 0) ::close(fd);
                    idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                    break;
                }
                perror("accept4"); break;
            }
        }
    }

    EventLoop* loop_;
    int listenFd_, idleFd_;
    Channel acceptChannel_;
    NewConnectionCallback cb_;
};

// ================= TcpConnection：TCP连接封装 =================
// 状态机：kConnecting → kConnected → kDisconnecting → kDisconnected
// 双缓冲区：inputBuffer_收数据，outputBuffer_发数据
// 会话上下文：context_ 挂载任意业务数据，框架不感知具体类型
enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using Ptr = std::shared_ptr<TcpConnection>;
    using CloseCallback = std::function<void(const Ptr&)>;
    using MessageCallback = std::function<void(const Ptr&, Buffer*)>;

    TcpConnection(EventLoop* loop, int fd, std::string ip, uint16_t port)
        : loop_(loop), fd_(fd), ip_(std::move(ip)), port_(port),
          state_(kConnecting), channel_(fd) {
        // 绑定四类事件回调
        channel_.setReadCallback([this] { handleRead(); });
        channel_.setWriteCallback([this] { handleWrite(); });
        channel_.setCloseCallback([this] { handleClose(); });
        channel_.setErrorCallback([this] { handleError(); });
    }

    ~TcpConnection() {
        std::cout << "[Conn] dtor fd=" << fd_ << std::endl;
        ::close(fd_);
    }

    int fd() const { return fd_; }
    void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }

    // 连接级会话上下文：类型擦除，可挂载任意业务对象
    void setContext(std::shared_ptr<void> c) { context_ = std::move(c); }
    template <typename T>
    std::shared_ptr<T> getContext() const {
        return std::static_pointer_cast<T>(context_);
    }

    // 连接建立完成：切换状态、注册事件、启动生命周期守卫
    void connectEstablished() {
        state_ = kConnected;
        channel_.tie(shared_from_this());
        channel_.enableReading();
        loop_->updateChannel(&channel_);
        std::cout << "[Conn] established fd=" << fd_ << " " << ip_ << ":" << port_ << std::endl;
    }

    // 协议帧发送（零拷贝封包版本，利用预留头部）
    void sendPacket(uint16_t cmd, const std::string& body) {
        if (state_ == kDisconnected) return;
        // 直接在输出缓冲区上编码帧，利用prepend零拷贝加头部
        PacketCodec::encode(&outputBuffer_, cmd, body);
        // 没有待发数据时，尝试直接发送快速路径
        if (!channel_.isWriting()) {
            ssize_t nwrote = ::send(fd_, outputBuffer_.peek(), outputBuffer_.readableBytes(), MSG_NOSIGNAL);
            if (nwrote >= 0) {
                outputBuffer_.retrieve(nwrote);
                if (outputBuffer_.readableBytes() == 0) {
                    return; // 全部发完，无需开启写事件
                }
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    handleClose();
                    return;
                }
            }
            // 没发完，开启写事件等待下次可写
            channel_.enableWriting();
            loop_->updateChannel(&channel_);
        }
    }

    void forceClose() { handleClose(); } // 强制关闭

private:
    // 读事件处理：循环读取直到EAGAIN，读完交付上层
    void handleRead() {
        int err = 0;
        for (;;) {
            ssize_t n = inputBuffer_.readFd(fd_, &err);
            if (n > 0) {
                if (messageCallback_) messageCallback_(shared_from_this(), &inputBuffer_);
            } else if (n == 0) { // 对端发FIN，正常关闭
                handleClose(); return;
            } else {
                if (err == EAGAIN || err == EWOULDBLOCK) break; // 数据读完了
                handleClose(); return; // 读错误，关闭连接
            }
        }
    }

    // 写事件处理：从输出缓冲区取数据发送
    void handleWrite() {
        if (outputBuffer_.readableBytes() == 0) return;
        ssize_t n = ::send(fd_, outputBuffer_.peek(),
                          outputBuffer_.readableBytes(), MSG_NOSIGNAL);
        if (n > 0) {
            outputBuffer_.retrieve(n); // 移动读指针，零拷贝
            if (outputBuffer_.readableBytes() == 0) {
                channel_.disableWriting();
                loop_->updateChannel(&channel_);
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            handleClose(); // 写错误，关闭连接
        }
    }

    // 统一关闭入口：状态守卫保证幂等，可重复调用
    void handleClose() {
        if (state_ == kDisconnected) return;
        state_ = kDisconnected;
        channel_.disableAll();
        loop_->removeChannel(&channel_); // 先从epoll移除
        if (closeCallback_) closeCallback_(shared_from_this()); // 再通知上层销毁
    }

    // 错误事件处理
    void handleError() {
        int err = 0; socklen_t len = sizeof(err);
        ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
        std::cerr << "[Conn] EPOLLERR: " << strerror(err) << std::endl;
        handleClose();
    }

    EventLoop* loop_;
    int fd_;
    std::string ip_;
    uint16_t port_;
    StateE state_;
    Channel channel_;
    CloseCallback closeCallback_;
    MessageCallback messageCallback_;
    Buffer inputBuffer_;   // 输入缓冲区
    Buffer outputBuffer_;  // 输出缓冲区
    std::shared_ptr<void> context_; // 业务会话上下文，类型擦除
};

// ================= MessageDispatcher：消息分发器 =================
// 作用：根据命令字(cmd)查表路由到对应处理函数
// 优势：新增业务命令只需注册，不用修改底层代码，符合开闭原则
class MessageDispatcher {
public:
    using CommandHandler = std::function<void(const TcpConnection::Ptr&, uint16_t, const std::string&)>;
    // 注册指定命令的处理函数
    void registerHandler(uint16_t cmd, CommandHandler h) {
        handlers_[cmd] = std::move(h);
    }
    // 分发消息：按cmd查表调用
    void dispatch(const TcpConnection::Ptr& conn, uint16_t cmd, const std::string& body) {
        auto it = handlers_.find(cmd);
        if (it != handlers_.end())
            it->second(conn, cmd, body);
        else // 未知命令默认返回错误
            conn->sendPacket(kCmdError, "unknown command");
    }

private:
    std::unordered_map<uint16_t, CommandHandler> handlers_; // 命令号→处理函数
};

// ================= 业务层 =================
// 命令字定义
constexpr uint16_t kCmdEcho   = 1;
constexpr uint16_t kCmdLogin  = 2;
constexpr uint16_t kCmdChat   = 3;
constexpr uint16_t kCmdNotice = 4;
constexpr uint16_t kCmdError  = 0xFFFF;

// 业务会话数据：框架完全不感知，通过context挂载在连接上
struct ClientSession {
    std::string username;
    Clock::time_point lastActive; // 最后活跃时间，用于心跳检测
    bool loggedIn = false;
};

int main() {
    ::signal(SIGPIPE, SIG_IGN); // 忽略SIGPIPE，防止写已关闭socket导致进程退出

    EventLoop loop;
    Acceptor acceptor(&loop, 8080);
    TimerQueue timerQueue(&loop); // 定时器队列，和网络共用同一个epoll
    std::unordered_map<int, TcpConnection::Ptr> connections;
    PacketCodec codec;
    MessageDispatcher dispatcher;

    // 工具函数：更新连接最后活跃时间
    auto touchActive = [](const TcpConnection::Ptr& conn) {
        if (auto s = conn->getContext<ClientSession>())
            s->lastActive = Clock::now();
    };

    // ---------- 注册业务命令处理函数 ----------
    // 回声命令：原样返回
    dispatcher.registerHandler(kCmdEcho,
        [&](const TcpConnection::Ptr& conn, uint16_t cmd, const std::string& body) {
            touchActive(conn);
            conn->sendPacket(cmd, body);
        });

    // 登录命令：创建会话并挂载到连接
    dispatcher.registerHandler(kCmdLogin,
        [&](const TcpConnection::Ptr& conn, uint16_t, const std::string& body) {
            auto s = std::make_shared<ClientSession>();
            s->username = body;
            s->loggedIn = true;
            s->lastActive = Clock::now();
            conn->setContext(s); // 业务会话挂到连接上
            conn->sendPacket(kCmdLogin, "welcome " + body);
        });

    // 聊天命令：需要登录态，未登录返回错误
    dispatcher.registerHandler(kCmdChat,
        [&](const TcpConnection::Ptr& conn, uint16_t, const std::string& body) {
            auto s = conn->getContext<ClientSession>();
            if (!s || !s->loggedIn) {
                conn->sendPacket(kCmdError, "not logged in");
                return;
            }
            touchActive(conn);
            conn->sendPacket(kCmdChat, s->username + " 说: " + body);
        });

    // ---------- 分层接线：编解码 → 分发器 ----------
    codec.setFrameCallback(
        [&](const TcpConnection::Ptr& conn, uint16_t cmd, const std::string& body) {
            dispatcher.dispatch(conn, cmd, body);
        });

    // ---------- 新连接回调 ----------
    acceptor.setNewConnectionCallback(
        [&](int cfd, const std::string& ip, uint16_t port) {
            auto conn = std::make_shared<TcpConnection>(&loop, cfd, ip, port);
            // 连接关闭：从全局表移除
            conn->setCloseCallback([&](const TcpConnection::Ptr& c) {
                connections.erase(c->fd());
            });
            // 消息到达：交给编解码层切包
            conn->setMessageCallback([&codec](const TcpConnection::Ptr& c, Buffer* b) {
                codec.decode(c, b);
            });
            conn->connectEstablished();//注册进入epoll
            connections[cfd] = conn;

            // 一次性定时器：连接8秒后发送公告
            // 用weak_ptr：连接销毁后回调自动空转，不持有对象防止内存泄漏
            std::weak_ptr<TcpConnection> weak = conn;
            timerQueue.addTimer([weak] {
                if (auto c = weak.lock())
                    c->sendPacket(kCmdNotice, "connected 8s, 这是定时公告");
            }, 8s);
        });

    // 周期定时器：每秒扫描一次，登录后5秒无消息判定心跳超时，主动踢掉
    timerQueue.addTimer([&] {
        std::vector<TcpConnection::Ptr> dead; // 先收集再关闭，避免遍历删除迭代器失效
        auto now = Clock::now();
        for (auto& [fd, conn] : connections) {
            auto s = conn->getContext<ClientSession>();
            if (s && s->loggedIn && now - s->lastActive > 5s)
                dead.push_back(conn);
        }
        for (auto& c : dead) {
            std::cout << "[Timer] 心跳超时，踢掉 fd=" << c->fd() << std::endl;
            c->forceClose();
        }
    }, 1s, 1s);

    acceptor.listen();
    std::cout << "server on 8080 (登录后 5s 无消息踢人，8s 收公告)" << std::endl;
    loop.loop(); // 启动事件循环
    return 0;
}
```