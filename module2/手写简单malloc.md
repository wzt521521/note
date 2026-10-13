区分块与页，一个是 allocator 视角下的动态分配单位，另一个是 OS 虚拟内存管理视角下的分页单位。allocator 主要管理进程虚拟地址空间中的 block/chunk；OS 主要按 page 管理虚拟地址映射，并通过页表映射到物理页框。

块结构体示例：

```cpp
struct BlockHeaders{
    size_t size;              // 当前块管理的用户内存区域大小
    BlockHeaders* next;       // 下一个块
    bool isFree;              // 当前块是否空闲
};
```

在 allocator 中，主要是面向虚拟地址空间中的 block 进行管理。

`malloc(size)` 的意义是申请一块至少 `size` 字节、虚拟地址连续的可用内存。虚拟地址连续不代表物理地址连续。

手写 malloc 的简单实现，主要是要解决：

```text
malloc
free
merge_memory
```

以及：

```text
first fit
split
alignment
```

前置知识：内存对齐。CPU 和数据类型通常对内存地址存在一定的对齐要求。可以简单理解为，如果数据没有按照合适边界对齐，CPU 可能需要额外的内存访问或硬件处理，因此 allocator 通常会进行内存对齐。代价是可能产生一定的内存浪费，也就是内部碎片。

使用：

```cpp
size_t align_up(size_t size);
```

这样的函数将请求大小向上取整到指定对齐边界。

例如：

```text
100 -> 112
200 -> 208
```

注意 `alignas` 是 C++ 的对齐说明符，不是向上取整函数。

内存池：定义一个 `memory_pool`，在程序静态存储区域中预留一块固定大小的连续虚拟内存：

```cpp
alignas(std::max_align_t)
static unsigned char memory_pool[LengthOfPool];
```

由于它是静态未显式初始化数组，通常位于 `.bss`。这里并不是通过 `brk/mmap` 动态向内核申请真实 heap，而是为了方便学习，我们把这 1MB 区域人为当作一个小型 heap。

我们后续所有的小型 `My_malloc` 都默认在这个内存池里面进行处理。

## 1. malloc 手写

```cpp
void* My_malloc(size_t size);
```

用户调用 malloc 函数传入想要的内存大小，假设为 100：

```cpp
My_malloc(100);
```

然后进入手写 malloc 函数。

核心思路：采用链表记录的方式，不断记录各个 block 的大小和状态。每次从 `Head` 开始找到第一个空闲并且大小足够的连续内存块，然后返回这段内存中用户区域的起始地址。

这就是 first fit。

核心伪代码，`HEADER_SIZE` 已经进行内存对齐：

```cpp
void* My_malloc(size_t size)
{
    size = align_up(size); // 将用户申请大小向上对齐

    BlockHeaders* cur = Head; // Head 固定指向第一个块头

    while(cur != nullptr){

        // 当前块处于空闲，并且大小足够
        if(cur->isFree && cur->size >= size){

            // 如果剩余空间足够放下一个 Header，
            // 并且还能留下至少一个最小对齐单位的可用空间
            if(cur->size >= size + HEADER_SIZE + ALIGN){

                unsigned char* newBlockAdd =
                    reinterpret_cast<unsigned char*>(cur)
                    + HEADER_SIZE
                    + size;

                BlockHeaders* newBlock =
                    reinterpret_cast<BlockHeaders*>(newBlockAdd);

                newBlock->isFree = true;
                newBlock->next = cur->next;
                newBlock->size =
                    cur->size - size - HEADER_SIZE;

                cur->next = newBlock;
                cur->size = size;
            }

            cur->isFree = false;

            // 返回当前 Header 后面的用户区域起始地址
            return reinterpret_cast<unsigned char*>(cur)
                   + HEADER_SIZE;
        }

        cur = cur->next;
    }

    return nullptr;
}
```

其中 split 后的结构大致是：

```text
[Header][用户内存][Header][剩余 free 内存]
```

Header 属于 allocator 自己管理，用户拿到的是 Header 后面的地址。

## 2. free 手写

```cpp
void My_free(void* ptr)
{
    if(ptr == nullptr){
        return;
    }

    // 找回之前的块头
    unsigned char* userAdd =
        reinterpret_cast<unsigned char*>(ptr);

    userAdd = userAdd - HEADER_SIZE;

    BlockHeaders* userHead =
        reinterpret_cast<BlockHeaders*>(userAdd);

    userHead->isFree = true;

    merge_memory();
}
```

因为：

```text
用户地址 = Header 地址 + HEADER_SIZE
```

所以：

```text
Header 地址 = 用户地址 - HEADER_SIZE
```

`free` 的意义并不是删除这块内存，而是将这个 block 标记为空闲，让 allocator 后续可以重新使用。

## 3. merge_memory

释放内存后可能出现：

```text
[free][free]
```

这两个 block 如果在虚拟地址上相邻，就可以进行合并，减少外部碎片。

```cpp
void merge_memory()
{
    BlockHeaders* cur = Head;

    while(cur != nullptr && cur->next != nullptr){

        if(cur->isFree && cur->next->isFree){

            cur->size =
                cur->size
                + cur->next->size
                + HEADER_SIZE;

            cur->next = cur->next->next;
        }
        else{
            cur = cur->next;
        }
    }
}
```

合并时需要把中间已经不再使用的 Header 空间也算回当前 free block：

```text
当前块大小
+ HEADER_SIZE
+ 下一个块大小
```

注意：如果合并成功，不要立刻执行：

```cpp
cur = cur->next;
```

因为可能存在：

```text
[free][free][free]
```

需要继续让当前合并后的大块和后面的 free block 再次合并。

当前实现的整体流程：

```text
init
↓
整个 memory_pool 初始化成一个大 free block

My_malloc
↓
align
↓
first fit
↓
split
↓
返回用户区域地址

My_free
↓
ptr - HEADER_SIZE
↓
找到 Header
↓
isFree = true
↓
merge_memory
```

需要注意：allocator 管理的是虚拟地址空间中的 block；OS 虚拟内存管理主要管理 page。malloc 要求返回的用户内存虚拟地址连续，但底层对应的物理页不要求连续。
在真正的系统中，往往没有我们定义的pool，用户每次通过malloc申请内存，则会通过brk拓展虚拟内存地址，而此时得到的连续虚拟内存就很像我们pool在程序中的定位。
所以，上述的代码存在的主要问题是忽略了页的存在，假设已经获得了一片很大的内存池自由分配，然而，在真实的系统中，往往会出现多次申请页的现象，也就是需要处理跨页的内部碎片。


并且需要注意到，为了追求内存对齐，有一部分内存牺牲，这也是需要解决的潜在问题


关于merge，只能回收两个连续的free内存，这也是一个潜在的问题