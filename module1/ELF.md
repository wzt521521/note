# ELF 文件结构与常用分析命令

## 0. 常用命令速查：file / readelf / nm / objdump / ldd

| 命令 | 主要作用 | 常用方式 |
|---|---|---|
| `file xxx` | 快速判断文件类型、架构、位数、是否动态链接等 | `file app`、`file libtest.so` |
| `readelf` | 直接按照 ELF 格式读取 ELF 内部结构，最适合查看 Header、Section、Segment、Symbol、Dynamic、Relocation | `readelf -h app`：ELF Header；`readelf -S app`：Section Header Table；`readelf -l app`：Program Header / Segment；`readelf -s app`：Symbol Table；`readelf -r app`：重定位；`readelf -d app`：动态链接信息；`readelf -a app`：综合查看 |
| `nm` | 简洁地查看符号表，适合快速寻找函数、变量等符号 | `nm app`；`nm -C app`：C++ 符号反修饰；`nm -D libxxx.so`：查看动态符号 |
| `objdump` | 更偏向“内容分析”，特别适合反汇编和查看 Section 内容 | `objdump -d app`：反汇编可执行代码；`objdump -S app`：源码与汇编混合（需要调试信息）；`objdump -s app`：查看 Section 原始内容；`objdump -h app`：Section 简表；x86 下可用 `objdump -d -M intel app` |
| `ldd xxx` | 查看运行时需要加载哪些共享库 | `ldd app`、`ldd libxxx.so` |

### 简单记忆

```text
file      → “这是什么文件？”
readelf   → “这个 ELF 内部结构是什么？”
nm        → “里面有哪些符号？”
objdump   → “里面的机器代码/实际内容是什么？”
ldd       → “运行时依赖哪些动态库？”
```

其中最核心的是 readelf。学习 ELF 时可以优先记：

```bash
readelf -h xxx      # ELF Header
readelf -S xxx      # Section Header Table
readelf -l xxx      # Program Header Table / Segment
readelf -s xxx      # Symbol Table
readelf -r xxx      # Relocation
readelf -d xxx      # Dynamic
```

---

## 1. ELF 文件

```text
ELF 文件
├─ ELF Header
│  └─ 整个 ELF 的总说明：类型、架构、入口地址、Program Header 在哪里、Section Header 在哪里
├─ Program Header Table
│  └─ 描述 Segment：运行时哪些文件内容映射进内存、地址、大小、权限等
├─ 各种 Section 内容
│  ├─ .text       机器指令
│  ├─ .rodata     只读常量
│  ├─ .data       已初始化的全局/静态数据
│  ├─ .bss        未初始化或零初始化的全局/静态数据
│  ├─ .symtab     完整符号表
│  ├─ .strtab     .symtab 等使用的字符串表
│  ├─ .dynsym     动态符号表
│  ├─ .dynstr     动态符号使用的字符串表
│  ├─ .dynamic    动态链接信息
│  ├─ .rela.*     重定位信息
│  └─ 其他 Section
└─ Section Header Table
   └─ Section 的目录：每个 Section 的名字、位置、大小、类型、属性等
```

最重要的第一层认知是：ELF Header 是总目录；Section 是文件内部按“功能”组织的数据；Section Header Table 是这些 Section 的目录；Program Header Table 则是操作系统加载程序时使用的视图。

---

## 2. ELF 的两种视角：链接视角与运行视角

### 链接视角：Section

### 运行视角：Segment

再进一步，可以把 ELF 看成“两种视角”。链接视角主要看 Section：.text、.data、.symtab、重定位信息等，编译器和链接器更关心这些；运行视角主要看 Segment，而 Segment 是由 Program Header 描述的，操作系统会把若干 Section 组合成 Segment，然后映射进进程虚拟内存。

### Section ≠ Segment

这是 ELF 中非常关键的关系：

```text
Section → 文件内部的“功能分类”
Segment → 运行时的“内存映射单位”
```

它们不是一一对应的。

通常一个 Segment 会包含多个 Section

所以一个很关键的关系是：Section ≠ Segment。Section 更像“文件里的功能分类”，Segment 更像“运行时要怎么装进内存”。一个 Segment 里通常可以包含多个 Section。

---

## 3. ELF 与 .o、.so、可执行程序的关系

ELF 是格式；.o、.so、可执行程序是 ELF 的不同用途和类型；结构框架相似，但内部内容和使用重点不同。

它们的总体结构是相似的，因为都遵循 ELF 格式，都会有 ELF Header，也可能有 Section、Section Header Table、Program Header Table 等；但不是每一种 ELF 都必须完整具备所有结构，而且具体有哪些 Section、Program Header 会因用途不同而变化。比如 .o 主要是给链接器使用，所以重点是 .text、.data、.symtab、.rela.* 等 Section，通常还没有形成最终的运行时内存布局；而可执行文件和 .so 需要被加载进内存，因此会有 Program Header，用来描述 Segment 和加载方式。

可以把 ELF 想成一个统一的“文件容器规范”，不同用途往里面放不同内容：player.cpp.o 是“等待链接的 ELF”，libgame_shared.so 是“可被动态加载的 ELF”，game_app 是“可启动运行的 ELF”。它们像是同一种文件格式下的不同产品，而不是完全不同的格式。

---

## 4. Section 与 Symbol Table

Section 存实际内容，Symbol Table 记录“名字 → 内容所在位置”的映射。

### 符号表symtab

nm 也是看符号表的工具，但比 readelf -s 更简洁

---

## 5. Section Header Table 与 Symbol Table 的定位关系

Section Header Table 和 Symbol Table 是两套不同的索引，它们通过符号表里的 `Ndx` 字段联系起来。

### Section Header Table

Section Header Table 描述的是 **Section 本身**，例如：

```text
第14个 Section = .text
Address = 0x1060
```

它告诉我们：第 14 个节是 `.text`，这个节的起始虚拟地址是 `0x1060`。

### Symbol Table

Symbol Table 描述的是 **符号**，例如：

```text
show_player()
Ndx   = 14
Value = 0x1119
Size  = 36
Type  = FUNC
```

这里 `Ndx=14` 表示：`show_player()` 这个符号定义在第 14 个 Section，也就是 `.text`。`Value=0x1119` 表示这个函数在当前 ELF 中对应的地址。

### 定位关系

因此定位关系是：

```text
show_player()
    ↓
Symbol Table：Ndx = 14
    ↓
Section Header Table：第14节 = .text
    ↓
.text 起始地址 = 0x1060
    ↓
show_player 地址 = 0x1119
```

如果想计算 `show_player()` 在 `.text` 内部距离节起点多远，可以计算：

```text
0x1119 - 0x1060 = 0xB9
```

也就是说，`show_player()` 位于 `.text` 起点之后 `0xB9` 字节的位置。

需要注意：**`Value=0x1119` 不是简单的“.text 内部偏移”，而是这个符号在该 ELF 中对应的地址值。** `.text` 内部偏移需要用 `符号 Value - .text 起始 Address` 计算。

### 一句话总结

**`Ndx` 用来确定“符号属于哪个 Section”，`Value` 用来确定“符号在地址空间中的位置”；如果想知道它在该 Section 内部的偏移，就用 `Value - Section Address`。**