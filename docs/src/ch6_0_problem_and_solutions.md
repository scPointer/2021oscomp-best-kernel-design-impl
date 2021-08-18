# 遇到的主要问题与解决方法

## 寄存器和 BSS 段初始化

使用 verilator 仿真 FPGA 代码时，发现 lrv-rust-bl 启动器无任何输出。打开仿真调试输出会在日志文件中输出每一条指令的执行结果和影响的寄存器值，日志行数轻松达到数十万甚至上百万之多。
于是在代码中插入 `ebreak` 、`nop` 、`csrr zero, xxx` 等正常情况下不会出现的指令，结合反汇编定位问题代码，并探查 CSR 内的数值。

结果发现，仿真时内存和寄存器均为随机初始化，CSR 中甚至可能出现非法值——如 `mideleg` 寄存器中 M 态中断对应的位为 1 ，意味着将 M 态中断委托到了 S 态处理，这是规范所不允许的；CPU 会在写入和读取该寄存器时做检查，但并不会处理随机初始化的情况。于是在启动代码中，将所有通用寄存器清零，CSR 写入一个合法的值。

另一方面，代码中使用的未初始化的全局变量位于 bss 和 sbss 段中，但程序链接脚本中缺少了 sbss 段，没有在程序启动阶段正常清零，导致一些全局变量初始化失败。在链接脚本中加入 sbss 段解决了这一问题。

## 中断处理函数中的死锁问题

编写串口驱动时，将串口对象包装在 `Mutex` 结构中，以确保任何时候只有一段执行流能够访问串口。但在单线程情况下，当主程序获取锁之后，串口仍然可能产生中断而进入中断处理程序，中断处理函数会再次尝试获取串口锁而进入等待；中断处理函数不返回时主程序不会继续执行，因而无法释放锁，导致出现死锁。

解决方案主要有四个，一是在主程序获取锁之前关闭中断，释放锁后再打开；二是将串口对象所有权保持在主程序中，中断处理程序通过一个静态原子变量通知主程序有中断产生，并禁用中断，主程序读取该变量，发现有中断时再调用串口的中断处理函数，并重新打开中断；
三是想办法将串口对象改造为无锁的，如主程序仅访问一个 `heapless::spsc` 的无锁队列，中断处理程序负责访问串口硬件；四是将中断处理函数移到单独的线程中执行。

rCore-N 中目前还没有多线程支持，故四不可行；三在主程序试图通过串口输出时可能有问题，因为单纯将数据写入软件缓冲区不能无法发起串口传输；一和二可行度较高，目前主要采用的是方案一。

## 多核改造中的死锁问题

在 rCore-N 的改造中，遇到了一个死锁问题：

进程 1 在退出时，内核获取 1 的锁来访问它的信息，同时要获取 0 号进程的锁，将进程 1 的子进程放到 0 号进程的子进程列表中，以避免资源不能正确地释放。同时，0 号进程会调用 `wait_pid` 系统调用，查看自己的子进程是否有运行结束的，来释放资源。而在 `wait_pid` 中，会需要获取调用者（0 号进程）与目标进程的锁，来访问它们的信息。

在单核环境下，由于上述两种过程都发生于内核态，且不允许嵌套中断，便不会发生死锁问题。而在多核环境下，0 号进程运行在一个核上，调用 `wait_pid` 去查看子进程 1 号；1 号进程运行在另一个核上，正在退出。这时就很有可能分别持有 0 号和 1 号的锁，要拿另一方的锁来查看或更改信息，从而进入死锁状态。

我们采用加一个大锁的方式来解决此问题，即当一段程序需要获取多个进程的锁时，需要先尝试获取大锁。在获取了大锁之后才会进入获取多个进程的锁的程序，避免了上述多段程序同时需要获取多个进程的锁的问题。

具体到代码上，简略后如下：

```rust
pub fn sys_waitpid(pid: isize, exit_code_ptr: *mut i32) -> isize {
    // 获取大锁
    let _ = WAIT_LOCK.lock();
    // 获取当前进程的锁
    ...
    // 遍历子进程，获取锁来读取状态
    ...
}

pub fn exit_current_and_run_next(exit_code: i32) {
    // 获取大锁
    let wl = WAIT_LOCK.lock();
    // 获取要退出进程的锁
    ...
    // 获取 0 号进程的锁
    ...
    // 释放所有锁
    drop(wl);
    ...
}
```

## 多核切换带来的 PLIC 上下文变动问题

在处理用户态外部中断时，有如下两种写法：

```rust
while let Some(irq) = Plic::claim(get_context(hart_id(), 'U')) {
    ext_intr_handler(irq, false);
    Plic::complete(get_context(hart_id(), 'U'), irq);
}
```

```rust
loop {
    let context = get_context(hart_id(), 'U');
    if let Some(irq) = Plic::claim(context) {
        ext_intr_handler(irq, false);
        Plic::complete(get_context(context, 'U'), irq);
    } else {
        break;
    }
}

```

主要区别在于，前者在 PLIC 领取和完成中断时，都会调用 `hart_id()` 和 `get_context()` 获取 PLIC 上下文编号，而后者将一次 `get_context` 的结果存入临时变量中，使用两次。乍看之下两种写法似乎等价，后者还能够节省两个函数调用；但实际上只有第一种写法能够正常运行。

问题的一方面在于，用户态的中断处理函数仍然可能被内核时钟中断而进入调度器；而相应的，进入内核中断处理函数时会关闭内核中断，此时不会响应时钟中断。这是很合理的：用户态的中断处理函数不应该能够突破基于时间片的多进程切换机制。这意味着 PLIC 领取和完成的代码可能在不同的硬件线程上执行。

问题的另一方面与 PLIC 的上下文设计以及内核在切换进程时的 PLIC 配置有关。假设进程领取了 A 外设的中断，硬件线程 1 的用户态对应的上下文编号为 3 ，硬件线程 2 的用户态对应的上下文编号为 6 （这里仅为方便举例，并不符合实际的上下文编号规则）。
当进程在硬件线程 1 上领取到 A 外设中断并离开时，内核会在 3 号上下文禁用 A 外设的中断使能，以避免随后在 1 上运行的进程被 A 外设中断；当进程被调度到硬件线程 2 上运行时，内核会在 6 号上下文中启用 A 外设中断。

PLIC 并不要求领取和完成在同一上下文中，但是只有中断在相应上下文中被使能时，领取和完成才是有效的。如果进程在硬件线程 2 上运行时，仍然向 PLIC 提交 3 号上下文的完成请求，由于此上下文中 A 外设没有被使能，故此次完成无效，A 外设的中断会保持屏蔽状态，进程便再也无法收到该中断。相反，进程向 PLIC 提交 6 号上下文的完成请求是有效的，A 外设中断屏蔽被解除，程序能够继续正常运行。

因此，用户程序提交完成请求时，应始终使用当前上下文提交；或者在所有上下文中都提交一遍。