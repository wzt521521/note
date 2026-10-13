ASan，全称 AddressSanitizer，是一个专门检测内存访问错误的运行时检测工具。它不是操作系统功能，也不是 malloc 本身的一部分，而是由编译器配合 sanitizer runtime 一起实现的。你在编译时加上 -fsanitize=address，编译器就会在你的程序里插入额外的检查代码；程序运行时，再由 ASan 的运行库维护一套额外的内存状态信息。

ASan 的核心机制可以分成三层理解。
第一层是 shadow memory。ASan 不只使用你的正常内存，还额外维护一块“影子内存”，用来描述正常内存中哪些字节当前是合法可访问的、哪些是越界保护区、哪些已经被释放。可以粗略理解成：
应用程序内存
[合法][合法][合法][已释放][redzone]

           ↓ 对应

shadow memory
[ 00 ][ 00 ][ 00 ][ fd ][ fa ]

第二层是 编译器插桩 instrumentation
x = p[10];正常编译时，本质上直接计算地址然后读取。但启用 ASan 后，编译器会在真正访问之前插入类似这样的逻辑：
计算目标地址
↓
查对应 shadow memory
↓
这个地址当前可访问吗？
├─ 可以 → 执行真正的读写
└─ 不可以 → 调用 ASan 报错
所以 ASan 不需要等操作系统触发 segmentation fault。哪怕这个地址仍然位于一个 OS 允许读写的页面里，只要 ASan 的 shadow memory 认为它不属于合法对象，就能提前抓住。所以 ASan 能检测“对象边界”，而操作系统通常只能检测“页级权限”。

第三层是 redzone 和 quarantine，ASan 在申请内存时，不只是简单给你请求的那几字节，而会在对象周围布置保护区域。
例如：redzone | 你的对象 | redzone
这些 redzone 在 shadow memory 中被标记为不可访问。如果你向左或向右越界，就会碰到 redzone，因此很容易被发现。
对于 free，ASan 也不会简单地马上让这块内存重新参与普通分配，而经常先放进一个叫 quarantine 的隔离区，并在 shadow memory 中把它标记为“已释放”。所以在free后访问时，，ASan 查 shadow memory，看到这里是 freed region就会报错

ASan常会带来明显的内存开销和一定运行时性能开销，所以一般用于开发、测试、CI，而不是默认用于正式生产环境。