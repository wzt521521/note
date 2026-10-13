int main()
{
    int* p = new int(42);

    std::cout << "address: " << p << '\n';

    delete p;

    std::cout << "first delete done\n";

    delete p;   // Double Free

    std::cout << "second delete done\n";

    return 0;
}

address: 0x5e1ece4b12b0
first delete done
free(): double free detected in tcache 2
Aborted (core dumped)

第一次 delete p 成功后，这个 chunk 已经被 glibc allocator 放回了自己的空闲管理结构；第二次再 delete p，allocator 发现“这块内存已经处于空闲状态”，于是报出 free(): double free detected in tcache 2，随后 Aborted (core dumped) 终止进程。

这里的 tcache 是 glibc 为线程维护的一种小块空闲缓存，可以把它先理解成“allocator 管理已释放小块的一层缓存”。第一次释放后，这个 chunk 很可能已经进入 tcache；第二次释放同一个地址时，glibc 检查到异常，所以主动中止。

UAF 是你绕过 allocator，直接继续读写旧地址，所以 allocator 未必立刻知道；Double Free 是你再次调用 delete/free，控制权重新进入 allocator，因此它有机会检查元数据并发现错误。