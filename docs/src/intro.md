# 软硬协同的用户态中断

## 目标

本项目在现有 [RISC-V 用户态中断扩展规范草案](https://five-embeddev.com/riscv-isa-manual/latest/n.html) 的基础上将其进一步完善，提出一种符合该规范的模拟器和 FPGA 实现，并基于用户态中断在内核中实现优化的信号和 io_uring 等跨进程通信机制，展示其设计和性能优势。

## 项目设计

项目架构设计如下：

![arch](assets/proj.svg)

- 模拟器：修改 QEMU 5.0，在其中添加 N 扩展支持
- FPGA：基于中科院计算所的[标签化 RISC-V 架构](https://github.com/LvNA-system/labeled-RISC-V/tree/master/fpga)，添加 N 扩展支持
- 启动器与 SBI：在 FPGA 平台上使用基于 RustSBI 开发的 [lrv-rust-bl](https://github.com/Gallium70/lrv-rust-bl) ，在 QEMU 上 uCore-SMP 系统使用 OpenSBI ，rCore 系统使用 RustSBI
- 操作系统：选择 [uCore-SMP](https://github.com/TianhuaTao/uCore-SMP) 和 [rCore-N](https://github.com/duskmoon314/rCore-N)

### 文件结构

```
.
├── README.md
├── docs                docs in mdbook structure
├── Labeled-uCore-SMP   uCore SMP with labeled RISC-V support
├── lrv-rust-bl         Labeled RISC-V fpga bootloader based on RustSBI
├── qemu                qemu modified by Campbell He
├── qemu-build          folder holding qemu build artifacts
├── rCore-N             rCore with N extension
└── rv-csr-test         N extension simple test program
```

## 开发进展

### QEMU 与 FPGA

- [x] 在 QEMU 中添加 N 扩展支持
- [x] 在 FPGA 开发板上部署标签化 RISC-V 架构
- [ ] 增加 N 扩展的 Chisel 代码

### 操作系统

- rCore
  - [x] 添加 N 扩展支持
  - [ ] 适配 FPGA 平台
  - [ ] 适配标签机制
  - [ ] 多核支持
  - [x] 实现信号机制
  - [ ] 实现 io_uring
- uCore-SMP
  - [ ] 添加 N 扩展支持
  - [x] 适配 FPGA 平台
  - [x] 适配标签机制
  - [ ] 实现信号机制
  - [ ] 实现 io_uring

### 应用程序

- [x] 验证 N 扩展正常工作
- [x] 信号机制测例
- [ ] io_uring 测例