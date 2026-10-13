# Makefile 与 CMake

> `make` 是构建工具，**Makefile 是它读取的构建规则文件**，用来组织编译、链接和清理流程。本文命令以 Linux、GNU make 和 `g++` 为例。

## 1. 为什么需要 Makefile

项目文件较多时，每次手动输入 `g++ a.cpp b.cpp c.cpp ...` 不便于维护。把依赖关系和命令写进 Makefile 后，就可以用简单命令管理构建：

```bash
make        # 构建默认目标
make clean  # 执行自己定义的 clean 目标
```

**核心补充：**`make` 根据规则调用编译器，不是编译器本身；`clean` 也不是内置清理命令，需要在 Makefile 中定义。

## 2. 基本语法

```makefile
目标: 依赖文件
	Shell命令
```

| 部分 | 含义 |
| --- | --- |
| 目标 | 想生成的文件，如 `game_app`、`main.o`；也可以是 `clean` 等动作名称 |
| 依赖 | 构建目标所需要的源文件、头文件或其他目标 |
| 命令 | 真正执行编译、链接或清理的操作 |

> **命令行默认必须以一个真正的 Tab 开头。**不要用若干空格代替；复制示例时也要检查编辑器是否自动把 Tab 转为空格。

对于普通文件目标，**目标不存在，或任意普通依赖比目标新**时，`make` 通常会执行命令重建目标。构建前还会先检查、更新它依赖的目标。

## 3. 一个完整的 Makefile

假设项目包含 `main.cpp`、`player.cpp` 和 `player.h`，且两个 `.cpp` 都包含 `player.h`。在项目根目录创建名为 `Makefile` 的文件：

```makefile
game_app: main.o player.o
	g++ main.o player.o -o game_app

main.o: main.cpp player.h
	g++ -g -O0 -Wall -Wextra -c main.cpp -o main.o

player.o: player.cpp player.h
	g++ -g -O0 -Wall -Wextra -c player.cpp -o player.o

.PHONY: clean
clean:
	rm -f main.o player.o game_app
```

### 3.1 编译与链接

```text
main.cpp   ──编译──→ main.o   ─┐
                              ├──链接──→ game_app
player.cpp ──编译──→ player.o ─┘
```

| 参数 | 含义 |
| --- | --- |
| `-c` | 编译生成目标文件，暂不链接 |
| `-o 文件名` | 指定输出文件名 |
| `-g` | 生成调试信息 |
| `-O0` | 关闭大部分优化，便于调试 |
| `-Wall -Wextra` | 启用一组常用及额外警告，不代表开启全部警告 |

原笔记中的编译命令重复写了 `main.cpp`，这里已修正为只输入一次，并显式指定输出文件。

### 3.2 默认目标与伪目标

- **默认目标：**本例中，不带参数执行 `make` 会构建第一个普通目标 `game_app`，并按依赖顺序先生成两个 `.o` 文件。
- **伪目标：**`.PHONY: clean` 声明 `clean` 是一个动作。即使目录里存在名为 `clean` 的文件，执行 `make clean` 仍会运行清理命令。

仅写 `clean:` 不能可靠地避免同名文件干扰，应显式声明 `.PHONY`。[参考：GNU make 伪目标](https://www.gnu.org/s/make/manual/html_node/Phony-Targets.html)

## 4. 增量构建：只重建受影响的目标

基于上面的完整示例：

| 操作 | 再执行 `make` 时的行为 |
| --- | --- |
| 第一次构建，产物不存在 | 编译两个 `.cpp`，再链接生成程序 |
| 未修改文件，产物均为最新 | 不重复构建，通常提示目标已是最新 |
| 修改 `main.cpp` | 重建 `main.o`，再重新链接 |
| 修改 `player.cpp` | 重建 `player.o`，再重新链接 |
| 修改 `player.h` | 重建两个依赖它的 `.o`，再重新链接 |
| 执行 `make clean` 后再构建 | 重新生成被清理的产物 |

> **关键是依赖关系写完整。**`make` 不会自动理解 C++ 中的全部 `#include`。本例显式列出头文件；漏写依赖可能导致修改头文件后没有重编。较大项目可以通过 GCC 的 `-MMD -MP` 生成 `.d` 依赖文件，再由 Makefile 引入。

时间戳机制也不会自动追踪编译选项的变化。如果只修改了编译参数，本例可能需要执行 `make clean` 后再 `make`。

## 5. 核心补充：变量与自动变量

重复出现的编译器和选项可以提取为变量：

```makefile
CXX = g++
CXXFLAGS = -g -O0 -Wall -Wextra

main.o: main.cpp player.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

| 写法 | 含义 | 在上述规则中的值 |
| --- | --- | --- |
| `$(CXX)` | 引用编译器变量 | `g++` |
| `$(CXXFLAGS)` | 引用 C++ 编译选项变量 | `-g -O0 -Wall -Wextra` |
| `$@` | 当前目标名称 | `main.o` |
| `$<` | 第一个依赖 | `main.cpp` |
| `$^` | 所有依赖，去重 | `main.cpp player.h` |

链接规则可写成 `$(CXX) $^ -o $@`，前提是依赖列表里确实都是要传给链接命令的输入文件。[参考：GNU make 手册](https://www.gnu.org/software/make/manual/make.html)

## 6. CMake 与 make 的关系

手写 Makefile 时，流程是：

```text
Makefile → make → 编译器与链接器 → 可执行程序
```

采用 CMake，且选择 Makefile 生成器时，流程是：

```text
CMakeLists.txt → cmake 配置并生成 → Makefile → make → 编译器与链接器
```

**CMake 是构建系统生成器，不是编译器。**你描述项目的源文件、目标、库和编译选项，CMake 再生成对应构建系统需要的文件。

CMake 不一定生成 Makefile，也可以生成 Ninja 构建文件或 Visual Studio 工程。它有助于维护跨平台构建配置，但平台专用命令和库仍需妥善处理。

### 6.1 最小 `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)
project(GameApp LANGUAGES CXX)

add_executable(game_app main.cpp player.cpp)
```

本例最低要求 CMake 3.16。`add_executable` 声明可执行目标及其源文件；源文件包含的头文件通常由生成的构建系统配合编译器跟踪依赖。

### 6.2 推荐流程：源码与构建产物分开

在含有 `CMakeLists.txt` 的项目根目录运行：

```bash
cmake -S . -B build    # 配置项目，将构建文件生成到 build 目录
cmake --build build   # 调用底层构建工具，执行编译和链接
```

| 命令或参数 | 含义 |
| --- | --- |
| `-S .` | 指定当前目录为源码目录 |
| `-B build` | 指定构建目录；目录不存在时会创建 |
| `cmake --build build` | 构建已配置的 build 目录，无需先进入该目录 |

`cmake --build` 会调用对应的底层构建工具，因此换用 Ninja 等生成器时，仍可使用这一构建入口。

### 6.3 原笔记中的 `cmake ..`

下面演示在一个新项目中显式使用 Unix Makefiles 生成器：

```bash
mkdir build
cd build
cmake -G "Unix Makefiles" ..
make
```

此处 `..` 指向含有 `CMakeLists.txt` 的上一级源码目录，当前 `build` 目录用于存放生成的文件。**`cmake ..` 不会切换工作目录，也不会直接完成编译链接。**省略 `-G` 时使用默认或已缓存的生成器，不能保证生成 Makefile。[参考：CMake 命令行文档](https://cmake.org/cmake/help/latest/manual/cmake.1.html)

---

**记忆重点：**Makefile 写规则，make 按依赖与时间戳构建；CMake 先生成构建系统，再通过 `cmake --build` 发起实际构建。
