strace -e trace=brk,mmap,munmap ./build/malloc_trace

trace 可以观察进程发出的系统调用。
我们这里故意只看 brk、mmap、munmap，否则输出会非常杂。
brk 可以改变传统 heap 的末端位置；mmap 可以创建新的虚拟内存映射；
munmap 可以撤销映射。它们正好对应 malloc 和内核之间最重要的几种交互。

整理：
ubuntu\@wzt:\~$ pmap 596allocator 的空闲块管理
得到一个关键信息：
0000646b8d943000    132K rw---   [ anon ]
这里意义是heap的大小为132K

然后执行
```cpp
int main()
{
    std::cout << "before malloc" << std::endl;

    void* p1 = malloc(16);
    std::cout << "malloc 16 bytes: " << p1 << std::endl;

    void* p2 = malloc(1024);
    std::cout << "malloc 1 KB: " << p2 << std::endl;

    void* p3 = malloc(1024 * 1024);
    std::cout << "malloc 1 MB: " << p3 << std::endl;
allocator 的空闲块管理
    free(p1);
    free(p2);
    free(p3);

    return 0;
}
```
通过strace -e trace=brk,mmap,munmap ./build/malloc_trace观察内存过程

得到的核心信息为：
brk(NULL)                               = 0x5942ae3e8000
brk(0x5942ae409000)                     = 0x5942ae409000

before malloc
malloc 16 bytes: 0x5942ae3fa6c0
malloc 1 KB: 0x5942ae3fa6e0

mmap(NULL, 1052672, PROT_READ|PROT_WRITE,
     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
                                        = 0x769ca14ff000

malloc 1 MB: 0x769ca14ff010

munmap(0x769ca14ff000, 1052672)         = 0

先看 brk(NULL)：brk(NULL) = 0x5942ae3e8000

这里可以把 brk 理解成“传统 heap 的末端边界”。传 NULL 基本是在询问当前 program break 在哪里。
于是当时 heap 顶端大约在：0x5942ae3e8000

然后通过brk(0x5942ae409000) = 0x5942ae409000把program break 从：0x5942ae3e8000拓展到了0x5942ae409000

差值正好是0x21000 = 132 KiB

这就是pmap得到的堆的大小：[heap] 132K

所以

glibc allocator 需要一块初始 heap
            ↓
调用 brk 扩展 program break
            ↓
得到大约 132 KiB
            ↓
/proc/PID/maps 里看到 [heap]

然后执行malloc(16)和malloc(1024)，并没有出现新的brk或者mmap调用
这就意味着并没有找内核，而是直接根据之前分配的132K，来进行切割分配，把132K里面的空闲内存进行切割
malloc 16 bytes = 0x5942ae3fa6c0
malloc 1 KB     = 0x5942ae3fa6e0
二者差值为32B，这说明 allocator 不会简单地按照“用户申请多少，就挨着切多少”还要考虑：对齐，块大小，allocator元数据等等

然后进入重点：malloc(1024 * 1024);
此时 strace 立刻出现：mmap(NULL, 1052672,
     PROT_READ|PROT_WRITE,
     MAP_PRIVATE|MAP_ANONYMOUS,
     -1, 0)
= 0x769ca14ff000

也就是glibc 没有把传统 [heap] 再往后扩大，而是向内核申请了一块匿名虚拟内存映射

内核返回的地址是0x769ca14ff000，而malloc 最终返回：0x769ca14ff010，也就是中间差了16B
这16个B被allocator用来保存这块 chunk 的管理信息，这也解释了一个以后极其重要的问题：free(ptr) 只拿到一个指针，为什么却知道应该释放多大的内存？
因为 allocator 往往在你看不到的位置保存了元数据。可以在这16B的地方找到大小等信息

执行free(p3)看到了munmap(0x769ca14ff000, 1052672) = 0，但是free(p1);free(p2);并没有看到brk(...)把 heap 缩回去。
这就说明了，free不等于一定立刻把内存归还操作系统，而是暂时归还到allocator 的空闲块管理，继续存在132K里面

malloc() 本身不是系统调用。 这是很多初学者容易误解的地方。malloc 是 glibc 提供的用户态函数。它可能完全不触发系统调用，也可能在需要更多内存时内部调用 brk/mmap。

核心结论有 5 个：第一，malloc 不是系统调用，而是 glibc 的用户态分配器接口。 第二，小块 malloc 通常优先从 allocator 已经掌握的 heap 空间里切块，不会每次都找内核。第三，当现有 heap 空间不够时，glibc 可以通过 brk 扩大传统 [heap]，实测看到它多次以大约 132 KiB 的粒度扩展。第四，大块分配可能直接走 mmap，你申请 1 MiB 时就看到匿名 mmap，释放时对应 munmap。第五，free 不等于“立刻把内存还给操作系统”，小块通常先回到 allocator 的空闲结构；只有在合适条件下，比如 heap 顶部形成较大连续空闲区域，glibc 才可能通过 brk 收缩一部分 heap。

malloc/free 管的是用户态 chunk，brk/mmap/munmap 管的是进程和内核之间更大粒度的虚拟内存获取与归还。