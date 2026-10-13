# 5.4 epoll LT（水平触发，epoll 的默认模式）

```cpp
// 5.4 epoll LT（水平触发，epoll 的默认模式）
#include <iostream>
#include <vector>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    // 提 fd 上限（同前）
    rlimit rl{};
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8080);
    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, 4096);

    // 1. epoll_create1：创建 epoll 实例，本身也是一个 fd
    int epfd = epoll_create1(0);

    // 2. epoll_ctl(ADD)：把 listenfd 注册进 epoll，关注可读
    //    没加 EPOLLET，就是默认的 LT（水平触发）
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = listenfd;       // data 是 union，5.4 先存 fd，5.9 改成存对象指针
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

    const int MAX_EVENTS = 1024; // epoll_wait 一次最多取回多少个就绪事件
    std::vector<epoll_event> events(MAX_EVENTS);

    int conn_count = 0;
    long long ready_per_sec = 0;
    long long wakeups_per_sec = 0;
    time_t last = time(nullptr);
    char buf[1024];

    auto close_conn = [&](int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);  // 先从 epoll 摘除
        close(fd);
        --conn_count;
    };

    while (true) {
        // 3. epoll_wait：返回的 events 数组里【只有就绪的 fd】
        //    空闲连接再多，这里也不遍历它们；nready 是就绪个数
        int nready = epoll_wait(epfd, events.data(), MAX_EVENTS, 100);
        if (nready < 0) {
            perror("epoll_wait");
            break;
        }

        ++wakeups_per_sec;
        ready_per_sec += nready;

        time_t now = time(nullptr);
        if (now != last) {
            std::cout << "[stat] conns=" << conn_count
                      << " wakeups/s=" << wakeups_per_sec
                      << " ready_events/s=" << ready_per_sec << "\n";
            ready_per_sec = 0;
            wakeups_per_sec = 0;
            last = now;
        }

        // 只循环就绪的 nready 个：O(就绪数)，与总连接数无关
        for (int i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;
            uint32_t evts = events[i].events;

            if (fd == listenfd) {
                // 新连接。LT 下本轮只 accept 一个也不会丢：
                // 只要队列里还有连接，listenfd 就一直保持可读，下轮 wait 立刻再报
                sockaddr_in caddr{};
                socklen_t len = sizeof(caddr);
                int cfd = accept(listenfd, (sockaddr*)&caddr, &len);

                epoll_event cev{};
                cev.events = EPOLLIN | EPOLLRDHUP;   // 默认 LT
                cev.data.fd = cfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                ++conn_count;
                if (conn_count % 100 == 0)
                    std::cout << "[accept] conns=" << conn_count << "\n";
            } else {
                // 真正的错误/挂起先处理；正常 FIN 在 LT 下表现为 EPOLLIN，recv 返回 0
                if (evts & (EPOLLERR | EPOLLHUP)) {
                    close_conn(fd);
                    continue;
                }
                if (evts & EPOLLIN) {
                    ssize_t r = recv(fd, buf, sizeof(buf), 0);
                    if (r > 0) {
                        // LT 下只读一次也不会丢数据：缓冲区没读完，
                        // epoll_wait 下一轮会继续报这个 fd 可读（5.6 会证明 ET 不行）
                        send(fd, buf, r, MSG_NOSIGNAL);
                    } else {
                        close_conn(fd);   // r==0 对端 FIN；r<0 出错
                    }
                }
            }
        }
    }

    close(epfd);
    close(listenfd);
    return 0;
}

```
epoll 的核心改进在于不再全量遍历所有连接，只处理就绪的连接，处理开销与总连接数无关。使用时先在内核中创建一个 epoll 管理实例，由一个文件描述符 epfd 作为该内核资源的访问句柄，然后把 listenfd 注册进这个 epoll 实例，所有注册的连接由内核统一维护。

代码包含用户态两层循环：最外层是 while 主循环，持续驱动事件处理，核心执行 `int nready = epoll_wait(epfd, events.data(), MAX_EVENTS, 100);` 来阻塞等待就绪事件，返回值 nready 就是本次待处理的事件总数。

epoll 有几个核心数据结构与变量需要明确：

- `std::vector<epoll_event> events(MAX_EVENTS)`：作为就绪事件的接收缓冲区，只用来存放本次需要处理的事件，长度固定为单次最大取回事件数，与总连接数无关。
- `struct epoll_event`：是用户态与内核交互事件信息的载体，包含两个字段：
  - `uint32_t events`：事件掩码，注册时表示要监听的事件，返回时表示实际发生的事件；
  - `union epoll_data data`：用户自定义透传数据，内核不做修改，注册时写入什么，事件返回时就原样带回。

epoll_wait 只会从数组下标 0 开始，依次覆盖写入本次就绪的 nready 个事件，不会清空整个数组；超出 nready 范围的位置会残留上一次的旧数据，因此第二层循环必须严格以 nready 为上限访问对应事件，不能越界，否则会读到过期残留数据。

通过这种设计，用户态可以直接拿到所有需要处理的连接，无需像 select 那样遍历全部连接逐个筛查；内核侧也无需轮询检查所有连接的状态，而是通过事件回调自动收集就绪连接，从根本上提升了高并发场景下的性能。

ET 模式下必须对 IO 事件进行循环处理：监听端 accept 要一次性循环接收完内核队列中所有待处理连接，客户端 recv 要一次性循环读完内核接收缓冲区的所有数据，否则剩余的连接和数据不会再触发新的事件通知，造成连接积压、数据滞留。同时 accept 和 recv 都必须工作在非阻塞模式下，否则循环读取到缓冲区或队列为空时，函数会直接阻塞挂起，导致整个事件循环卡死，完全偏离事件驱动的设计意图；循环的正常结束标志是函数返回 - 1 且 errno 为 EAGAIN/EWOULDBLOCK，若 recv 返回 0 则代表对端主动关闭连接。

epollout的处理：
server不光要处理接收事件（epollin），还要在接收之后处理发送数据，如果发送缓冲区满了怎么办呢，这里涉及到了epollout

过程如下：
三个关键：
1.std::map<int, std::string> outbuf;仓库，存储发送的数据
2.函数queue_send，业务发送的统一入口：数据先存储到仓库outbuf，然后调用函数flush_out发送对应的fd的数据，发不完就针对这个fd标记epollout
3.函数flush_out，需要参数fd，用来发送仓库里面对应的函数，相当于业务封装的send函数

接收数据后调用发送，先进行queue_send ,会先存储数据到outbuf，然后调用flush_out，如果成功发送，则会正常退出，如果发送区满了，则标记epollout。问题来了，如果f调用lush_out的过程中发送到一半发送缓冲区满了怎么办，无需担心，outbuf是实时更新的，如果发送到一半，则outbuf则会擦除已经发送的部分，只留下剩下尚未发送的部分，不会出现重复发送已经发送过的数据的问题。该fd被标记为epollout之后，则会退出，不会阻塞，等待下次epoll重新获取该事件，确保了et下正常运转

等到server下一轮重新获取事件，对应的fd被标记为epollout，则会直接调用flush_out，传入对应的fd继续发送，如果成功发送则清空epollout事件，否则继续保留事件，等待下一次继续尝试发送，这也确保了et下的正常运行

示例代码：

// 5.8 EPOLLOUT：发送缓冲区满后注册写事件，实现异步发送

```cpp
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    rlimit rl{};
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblocking(listenfd);
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8080);
    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, 4096);

    int epfd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    ev.data.fd = listenfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

    const std::string BIG(1024 * 1024, 'B');   // 收到 "big" 请求时回复 1MB，用来撑满 sndbuf

    std::map<int, std::string> outbuf;         // fd -> 没发完的数据（5.9 收进 Connection）
    std::vector<epoll_event> events(1024);
    int conn_count = 0;
    char buf[1024];

    // 重新设置 fd 关注的事件；want_out=true 时额外关注 EPOLLOUT
    auto change_events = [&](int fd, bool want_out) {
        epoll_event e{};
        e.events = EPOLLIN | EPOLLRDHUP | EPOLLET | (want_out ? EPOLLOUT : 0);
        e.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
    };

    // 尽力发送 outbuf[fd] 里的排队数据
    // 返回：0=全部发完  1=sndbuf 满，没发完  -1=出错
    auto flush_out = [&](int fd) -> int {
        auto it = outbuf.find(fd);
        if (it == outbuf.end() || it->second.empty()) return 0;
        std::string& data = it->second;
        while (!data.empty()) {
            ssize_t n = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
            if (n > 0) {
                data.erase(0, n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 1;                    // 发送缓冲区满，剩余数据留在 outbuf
            } else {
                perror("send");
                return -1;
            }
        }
        return 0;                            // 全部发完
    };

    // 业务发送的统一入口：数据先入队，再尽力发，发不完就挂 EPOLLOUT
    auto queue_send = [&](int fd, const std::string& s) -> int {
        outbuf[fd].append(s);
        int rc = flush_out(fd);
        if (rc == 1) {
            change_events(fd, true);         // 注册写事件：sndbuf 腾空后内核通知我
            std::cout << "[out ] fd=" << fd << " sndbuf 满，注册 EPOLLOUT，排队 "
                      << outbuf[fd].size() << " 字节\n";
        }
        return rc;
    };

    auto close_conn = [&](int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        outbuf.erase(fd);
        --conn_count;
    };

    while (true) {
        int nready = epoll_wait(epfd, events.data(), events.size(), 100);
        if (nready < 0) { perror("epoll_wait"); break; }

        for (int i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;
            uint32_t evts = events[i].events;

            if (fd == listenfd) {
                while (true) {
                    sockaddr_in caddr{};
                    socklen_t len = sizeof(caddr);
                    int cfd = accept(listenfd, (sockaddr*)&caddr, &len);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }
                    set_nonblocking(cfd);
                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;  // 默认不关注 EPOLLOUT
                    cev.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                    ++conn_count;
                    std::cout << "[accept] fd=" << cfd << "\n";
                }
                continue;
            }

            bool closed = (evts & (EPOLLERR | EPOLLHUP));

            // 读：5.7 的循环读到 EAGAIN
            if (!closed && (evts & EPOLLIN)) {
                while (true) {
                    ssize_t r = recv(fd, buf, sizeof(buf), 0);
                    if (r > 0) {
                        int rc;
                        if (r >= 3 && memcmp(buf, "big", 3) == 0)
                            rc = queue_send(fd, BIG);       // 演示：回 1MB
                        else
                            rc = queue_send(fd, std::string(buf, r));  // 普通 echo
                        if (rc < 0) { closed = true; break; }
                    } else if (r == 0) {
                        closed = true;
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        closed = true;
                        break;
                    }
                }
            }

            // 写：sndbuf 腾空，继续把排队数据发出去
            if (!closed && (evts & EPOLLOUT)) {
                int rc = flush_out(fd);
                if (rc == 0) {
                    change_events(fd, false);  // ★ 发完立刻取消 EPOLLOUT，否则一直报可写
                    std::cout << "[out ] fd=" << fd << " 排队数据全部发完，取消 EPOLLOUT\n";
                } else if (rc < 0) {
                    closed = true;
                }
            }

            if (closed) close_conn(fd);
        }
    }

    close(epfd);
    close(listenfd);
    return 0;
}
```