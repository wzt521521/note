这块可以压缩成下面这几个核心点。

`pipe()` 创建一个内核管道：
```cpp
int pipefd[2];
pipe(pipefd);
```
其中 `pipefd[0]` 是读端，`pipefd[1]` 是写端。通常在 `fork()` 前创建，这样父子进程会继承指向同一个 Pipe 的文件描述符。若父进程只写、子进程只读，就应该分别关闭不用的一端：
```cpp
// parent
close(pipefd[0]);

// child
close(pipefd[1]);
```

`write()` 原型可以先理解成：
```cpp
ssize_t write(int fd, const void* buf, size_t count);
```
三个参数分别是：`fd` 表示往哪个文件描述符写，`buf` 是数据起始地址，`count` 是希望写入多少字节。例如：
```cpp
const char* msg = "hello";
ssize_t n = write(pipefd[1], msg, 5);
```
返回值 `n` 表示**实际写入的字节数**；成功时 `n >= 0`，失败返回 `-1`。以后做严谨的网络程序时，也不能无脑假设一次 `write()` 永远把所有数据写完。

`read()`：
```cpp
ssize_t read(int fd, void* buf, size_t count);
```
`fd` 是从哪里读，`buf` 是把数据放到哪里，`count` 表示**最多读取多少字节**。例如：
```cpp
char buf[1024];
ssize_t n = read(pipefd[0], buf, sizeof(buf));
```
这里 `1024` 的意思不是“必须等满 1024 字节才返回”，而是“这次最多给我 1024 字节”。

默认阻塞模式下，`read()` 最关键的语义是：如果当前一个字节都没有，并且还有写端存在，它会阻塞；一旦有数据可读，即使只有 5 字节，也可以立即返回 `5`；如果 Pipe 已经没有数据，而且所有写端都关闭，则返回 `0`，这代表 EOF；失败返回 `-1`。

所以我们刚刚讨论的核心问题是：**缓冲区足够大，不代表一次 `read()` 一定能读完整个数据流。**例如发送方最终会写：
```text
helloworldlinux
```
总共 15 字节，接收方即使：
```cpp
char buf[4096];
read(fd, buf, sizeof(buf));
```
如果执行 `read()` 的那一刻 Pipe 里只有 `"hello"`，那么这次完全可能只返回：
```text
n = 5
```
后面的 `"worldlinux"` 要靠后续 `read()` 才能拿到。因此：
```text
buf 足够大 ≠ 数据已经全部到达 ≠ 一次 read 一定读完
```

也因此常见代码会循环：
```cpp
while (true) {
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n > 0) {
        // 处理本次读到的 n 个字节
    } else if (n == 0) {
        // EOF：所有写端关闭，并且数据已经读完
        break;
    } else {
        // error
        break;
    }
}
```

Pipe 还要记住一个重要性质：它是**字节流**。发送方：
```cpp
write(fd, "hello", 5);
write(fd, "world", 5);
write(fd, "linux", 5);
```
接收方不一定对应三次 `read()`。可能一次读到 `"helloworldlinux"`，也可能分成 `"hello"`、`"world"`、`"linux"`，甚至其他拆分。因此：
```text
一次 write ≠ 一次 read
一次 read ≠ 一条完整业务消息
```
真正的“消息边界”要由应用层自己定义，比如换行符、固定长度、`length + body`。这个思想以后会直接延伸到 TCP、粘包/半包和 Reactor。