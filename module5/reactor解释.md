面试时要讲清楚技术选型，踩过的坑，以及原理

reactor设计模式下的io多路复用多线程模型

建议从main开始阅读
角色定位
1.EventLoop baseLoop
作为监听的主线程，注册了epoll实例，负责监听新连接，并且将新连接下发到子线程，采用轮询下发
2.TcpServer server(&baseLoop, 8080, 4);
创建一个核心管理类，负责管理各类实例，包括最核心的几类
a.eventloopthreadpool，直接持有线程池对象unique_ptr<class EventLoopThreadPool> pool_
b.每个连接的上下文：std::vector<std::shared_ptr<LoopContext>> contexts_
c.acceptor对象本身
3.业务分发器dispatcher
4.解码器本身Packet codec

以及各种工具函数和注入函数，对于touch和dispatcher，不过多阐述
需要确保对各类的角色有清晰的认知
tcpserver是管理类，负责进行管理
acceptor是监听类，注册到baseloop里面作为普通的网络IO事件，同样运行channel来进行工作，但是acceptor不拥有tcpconnection
tcpconnection是连接类，负责每一个客户端的连接，每处理一个新连接就要创建一个对应的tcpconnection，负责对这个连接进行持续的管理
buffer，packet作为缓冲区和协议的定义，用来对tcp接收的内容进行处理
eventloopthread对应每个子线程一个，用来持有子线程
eventloop用来对应每个子线程一个，子线程的栈持有enentloop，并且进行loop函数的调用
eventloopthreadpool作为线程池本身，内部通过指针持有所有的子线程实例，而子线程实例持有子线程栈上持有的loop，通过操作loop的wakeup等读写函数，可以操作子线程，线程本身不可直接操作，需要通过各类函数来让线程进行代码跳转执行需要的行为
关系大致如下
//EventLoopThreadPool
//├── EventLoopThread[0]
//│       └── std::thread[0]
//│               └── EventLoop[0]
//├── EventLoopThread[1]
//│       └── std::thread[1]
//│               └── EventLoop[1]
//└── EventLoopThread[2]
//		  └── std::thread[2]
//				  └── EventLoop[2]
///每个线程拥有一个epoll实例，负责处理连接，也就是每个线程可以对应多个连接
//所有权关系是：
//
//```
//EventLoopThreadPool
//owns N 个 EventLoopThread
//
//EventLoopThread
//owns std::thread
//
//子线程 threadFunc 的栈
//owns EventLoop
//```

loopcontext作为每个线程拥有的loop的独自上下文，loopcontext内部记录了这个线程的timer和对应的连接

现在假设main已经运行到了server.start()，成功创建了对应数量的线程，并且开启了监听新连接，baseloop由main函数持有，被传入acceptor，成功建立了监听事件，但是此时还没有开启baseloop的loop循环开始等待epoll

baseLoop.Loops();开始等待epoll

假设新客户端申请连接:
此时baseloop的loop函数中正在执行，int n = epoll_wait(epollfd, vecOfevent.data(),static_cast<int>(vecOfevent.size()), -1);
监听到了新的连接，返回n，从baseloop的事件数组里面取出来事件，找到事件对应的channel，然后运行acceptor的handleread，拿出连接后交付上层回调函数，上层回调函数位于TcpServer::newConnection，此时就用到了多线程，先取出一个创建好的线程的eventloop，然后通过map查找对应的上下文空间，对应的上下文空间里面就有计时器，管理连接的容器等等。此时新连接仍然没有被放到此线程对应的epoll实例里面，通过ioLoop->queueInLoop注入lambda函数，来让这个线程自己执行任务，让线程自己在读取任务的时候执行任务，任务的内容就是lambda函数体的内容，大概就是创建tcpconnection，并且注入各类回调，创建完成后调用conn->connectionSuccess();conn->connectionSuccess();内部调用loop_->updateChannel(&ch);让线程自己的loop进行创建新连接的事件epoll注册。此时就算这个线程成功的执行了任务队列里面的任务，并且成功了管理了这个新连接，负责这个连接后面的IO

现在假设这个连接开始传输数据：
数据到达对应的注册在epoll中的实例，会唤醒对应的loop，相当于唤醒对应的线程，线程会执行处理，此时与单线程模型区别不大
