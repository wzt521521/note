# 什么是 Lambda

lambda本质是一个编译器替你生成的仿函数类，那什么是仿函数？

## 仿函数示例

```
struct PrintId {
    int id;                  // 携带的状态
    PrintId(int i) : id(i) {}
    void operator()() const {      // 让对象可被 () 调用
        std::cout << id;
    }
};
PrintId p(5);
p();   // 看着像调函数，实际是 p.operator()()
```

这就是仿函数类，`p()`看似是调用函数，实则是使用了p重载的`()`语法

---

## Lambda 的本质：编译器生成的匿名闭包类

基础示例：

```
int id = 5;
auto p = [id] { std::cout << id; };
p();
```

编译器大致生成等价代码：

```
class __Closure {
    int id;             // 值捕获 id → 成员
public:
    __Closure(int i) : id(i) {}
    void operator()() const { std::cout << id; }
};
__Closure p(id);
```

对应关系：

- **捕获的变量** → 闭包类的**成员变量**；
- **捕获动作**（值 / 引用）→ 构造闭包时怎么初始化这些成员；
- **lambda 花括号里的代码** → `operator()` 的函数体。

---

## std::function 的作用

`std::function` 是 "装任意仿函数的盒子"：

```
std::function<void()> f;
f = PrintId(5);                     // 装手写仿函数
f = []{ std::cout << "x"; };    // 装 lambda
f();                                // 不关心里面具体是哪种类
```

这就是lambda的本质，更深入一点

---

## 深入：值捕获与引用捕获

### 值捕获

```
int x = 10;
auto f = [x](int a) { return x + a; };
f(5);
```

这个过程是怎么样的？如下：

```
class __Closure {
    int x;                             // 第一句话：捕获 x → 类里多了字段 int x
public:
    __Closure(int x_) : x(x_) {}    // 第二句话：构造时，把外部 x 的值复制给字段 x
    int operator()(int a) const { return x + a; }
};
```

### 引用捕获

如果是引用捕获呢？如下：

```
int x = 10;
auto g = [&x] { x = 99; };
g();   // 外部 x 变成 99
```

```
class __ClosureRef {
    int& x;                            // 第一句话：捕获 &x → 字段类型是"引用 int&"
public:
    __ClosureRef(int& x_) : x(x_) {} // 第二句话：构造时不复制值，把引用绑定到外部 x
    void operator()() const { x = 99; }
};
```

也就是在捕获的时候要把lambda看作一个崭新的匿名类，这个时候就方便理解了许多，例如理解循环引用

---

## 循环引用完整示例

```
#include <iostream>
#include <memory>
#include <functional>
// 需继承 enable_shared_from_this 才能在类内获取自身的 shared_ptr
class Test : public std::enable_shared_from_this<Test> {
public:
    Test()  { std::cout << "Test 构造函数调用\n"; }
    ~Test() { std::cout << "Test 析构函数调用\n"; }

    void setCallback() {
        // lambda 按值捕获自身的 shared_ptr，引用计数 +1
        auto self = shared_from_this();
        m_callback = [self]() {
            std::cout << "回调执行\n";
        };
    }
private:
    std::function<void()> m_callback; // 对象本身持有这个回调
};

int main() {
    std::shared_ptr<Test> obj = std::make_shared<Test>();
    obj->setCallback();

    // 此时引用计数为 2：obj 本身 + lambda 捕获的 self
    std::cout << "引用计数: " << obj.use_count() << '\n';

    // main 结束，obj 离开作用域，引用计数减为 1
    // 但 m_callback 里的 lambda 仍持有 shared_ptr
    // 对象不销毁 → 回调不释放 → shared_ptr 不释放 → 对象不销毁，循环形成
    return 0;
}
```

讲解：
main结束以后，因为m_callback 里的 lambda 仍持有 shared_ptr，导致了obj不会被回收，然后obj不回收，就导致m_callback始终存在，循环引用出现

在编译阶段，发现了这个 lambda，就已经完成了匿名类的生成了，虽然这个匿名类是按照这个 lambda 函数生成的，但是并没有为此生成一个对象，等到 main 函数执行到这里的时候，lambda 执行了针对这个匿名类的初始化，完成了引用捕获的初始化，但是虽然有了一个实例对象，还没有把这个对象赋值给任何一个用户态对象，一直到 cb_=cb 才被正式拷贝赋值，里面的函数体始终没有执行除非调用 cb

如果多个cb1,cb2,cb3都被cb赋值，就要看具体的捕获方式，如果是引用捕获，则会操作同一个变量，但是析构和构造都是各管各自的