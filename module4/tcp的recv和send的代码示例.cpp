// 标准输入输出流库，用于控制台打印运行日志与状态信息
#include <iostream>
// POSIX 标准系统调用头文件，提供 close() 等文件描述符操作接口
#include <unistd.h>
// 网络地址转换头文件，提供 htons/htonl 等主机字节序与网络字节序的转换函数
#include <arpa/inet.h>
// Socket 核心 API 头文件，提供套接字创建、绑定、监听、收发等全部核心函数及结构体定义
#include <sys/socket.h>

int main()
{
    // ==============================================================
    // 整体设计：单线程阻塞式 TCP Echo 回显服务器
    // 核心功能：监听本机 8888 端口，接受客户端连接后，将客户端发来的数据原样回发
    // 模型特点：采用阻塞 IO 模型，同一时间只能处理一个客户端连接，串行处理请求
    // ==============================================================

    // 1. 创建 TCP 套接字文件描述符
    // 函数作用：在内核中申请并初始化一个套接字实例，返回对应的文件描述符供后续操作
    // 参数1：AF_INET     —— 指定使用 IPv4 协议族
    // 参数2：SOCK_STREAM —— 指定流式套接字类型，对应 TCP 协议（提供可靠、面向连接的字节流传输）
    // 参数3：0           —— 协议编号，流式套接字默认对应 TCP，填 0 由系统自动选择
    // 返回值：成功返回非负整数文件描述符；失败返回 -1 并设置全局错误码 errno
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket"); // 打印错误信息，格式为 "socket: 具体错误描述"
        return 1;
    }

    // 2. 初始化服务端地址结构体
    // 结构体 sockaddr_in：IPv4 专用地址结构体，用于存储 IP 地址和端口号
    // 设计意图：配置服务端要绑定的网络地址，作为后续 bind 系统调用的入参
    sockaddr_in server_addr{}; // 零初始化结构体，避免内存脏数据影响字段值
    server_addr.sin_family = AF_INET;        // 地址族字段，必须与 socket() 入参一致，标识 IPv4 协议
    server_addr.sin_port = htons(8888);      // 端口号：通过 htons 将主机字节序转为网络字节序（大端）
                                             // 网络协议统一使用大端字节序，屏蔽不同主机的字节序差异
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
    // IP 地址：INADDR_ANY 表示监听本机所有网卡的 IP 地址
    // htonl 将 32 位整数从主机字节序转为网络字节序
    // 设计意图：不绑定某一张具体网卡，任意网卡收到的连接请求都能被处理

    // 3. 绑定套接字与本地地址端口
    // 函数作用：将创建好的套接字与指定的 IP 地址、端口号进行绑定
    // 设计意图：让内核建立「套接字文件描述符 ↔ 本机 IP+端口」的映射关系，客户端才能通过地址找到该服务
    // 参数1：listen_fd —— 待绑定的监听套接字文件描述符
    // 参数2：reinterpret_cast<sockaddr*>(&server_addr) —— 地址结构体指针
    //        历史兼容原因：socket 通用接口使用 sockaddr* 抽象类型，实际传入 IPv4 的 sockaddr_in* 需要强转
    // 参数3：sizeof(server_addr) —— 地址结构体的实际字节长度
    // 返回值：成功返回 0；失败返回 -1 并设置 errno
    if (bind(
            listen_fd,
            reinterpret_cast<sockaddr*>(&server_addr),
            sizeof(server_addr)
        ) == -1) {
        perror("bind");
        close(listen_fd); // 异常退出前必须关闭已创建的套接字，防止文件描述符资源泄漏
        return 1;
    }

    // 4. 将套接字设置为被动监听状态
    // 函数作用：把主动套接字转换为被动监听套接字，内核开始接受该端口上的 TCP 连接请求
    // 设计意图：调用 listen 后，内核才会维护该端口的连接队列，处理客户端的 TCP 三次握手
    // 参数1：listen_fd —— 已完成绑定的套接字文件描述符
    // 参数2：128 —— backlog 参数，指定「已完成三次握手但未被 accept 取出」的连接队列最大长度
    //        高并发场景下调大该值可容纳更多待处理连接，避免连接请求被直接丢弃
    // 返回值：成功返回 0；失败返回 -1 并设置 errno
    if (listen(listen_fd, 128) == -1) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    std::cout << "server listening on port 8888...\n";

    // 5. 阻塞等待并接受客户端连接
    // 函数作用：从已完成三次握手的连接队列中取出一个连接，创建独立的连接套接字用于数据通信
    // 设计意图：分离「监听新连接」和「数据收发」两个职责
    //   - listen_fd：始终负责监听端口，只用于接受新连接，不参与业务数据读写
    //   - conn_fd：每个客户端对应一个独立的连接套接字，专门用于和该客户端进行数据交互
    // 参数1：listen_fd —— 处于监听状态的套接字文件描述符
    // 参数2：reinterpret_cast<sockaddr*>(&client_addr) —— 输出参数，用于存储已连接客户端的 IP+端口信息
    // 参数3：&client_addr_len —— 输入输出参数：传入时为结构体大小，返回时为实际地址数据的长度
    // 返回值：成功返回新的连接套接字描述符；失败返回 -1 并设置 errno
    // 阻塞特性：如果连接队列为空，accept 会阻塞挂起，直到有新客户端完成连接
    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    int conn_fd = accept(
        listen_fd,
        reinterpret_cast<sockaddr*>(&client_addr),
        &client_addr_len
    );
    if (conn_fd == -1) {
        perror("accept");
        close(listen_fd);
        return 1;
    }
    std::cout << "client connected\n";

    // 6. Echo 回显循环：持续接收客户端数据并原样回发
    // 设计意图：实现 Echo 服务核心逻辑——客户端发送什么内容，服务端就原封不动返回什么
    char buffer[1024]; // 数据接收缓冲区，大小 1024 字节，用于暂存从网络收到的数据
    while (true) {
        // recv 函数：从连接套接字中读取对端发送的数据
        // 参数1：conn_fd     —— 客户端对应的连接套接字文件描述符
        // 参数2：buffer       —— 接收数据的内存缓冲区指针
        // 参数3：sizeof(buffer) —— 缓冲区最大可容纳的字节数
        // 参数4：0            —— 接收标志位，0 表示默认阻塞接收模式
        // 返回值 n：
        //   > 0：实际读取到的字节数
        //   = 0：对端（客户端）正常关闭了 TCP 连接（四次挥手完成）
        //   < 0：读取过程出错，全局 errno 被设置为具体错误码
        ssize_t n = recv(conn_fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            // send 函数：向连接套接字发送数据
            // 参数1：conn_fd —— 客户端对应的连接套接字
            // 参数2：buffer  —— 待发送数据的缓冲区指针
            // 参数3：n       —— 实际要发送的字节数（只发送本次收到的 n 字节，而非整个缓冲区）
            // 参数4：0       —— 发送标志位，0 表示默认阻塞发送模式
            // 返回值：成功返回实际发送的字节数；失败返回 -1 并设置 errno
            ssize_t sent = send(conn_fd, buffer, n, 0);
            if (sent == -1) {
                perror("send");
                break; // 发送出错，终止通信循环
            }
        } else if (n == 0) {
            // 客户端主动断开连接，正常退出循环
            std::cout << "client disconnected\n";
            break;
        } else {
            // recv 调用出错，打印错误后退出
            perror("recv");
            break;
        }
    }

    // 7. 关闭套接字，释放内核资源
    // 设计意图：通信结束后必须显式关闭文件描述符，否则会造成文件描述符泄漏，耗尽系统资源
    // 关闭顺序：先关闭业务连接套接字，再关闭监听套接字
    close(conn_fd);   // 关闭与当前客户端的连接套接字，释放连接相关资源
    close(listen_fd); // 关闭监听套接字，停止接受新的连接请求
    return 0;
}
