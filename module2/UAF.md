主程序：
int main()
{
    int* p = new int(42);

    std::cout << "before delete: " << *p << '\n';
    std::cout << "address      : " << p << '\n';

    delete p;

    std::cout << "after delete : " << *p << '\n';  // UAF

    return 0;
}

before delete: 42
address      : 0x565e535412b0
after delete : 1709520193

delete 前是 42，delete 后读出来变成了 1709520193。这说明 delete p 之后，那块内存已经被 allocator 接管，它可能立刻在原位置写入自己的管理信息，所以原来的 42 被覆盖了。
关键点是：p 这个变量本身还保存着原地址 0x565e535412b0，但这个地址已经不再对应一个“合法存在的 int 对象”。
后面的 *p 仍然能读，是因为那个虚拟页还映射着、权限也允许读取；但从 C++ 语义上，这已经是 Use After Free，属于未定义行为。
free/delete 结束的是对象和分配块的有效所有权，不一定立刻撤销对应虚拟页映射；因此悬空指针可能暂时还能访问，但内容已经不可靠。