# ArtisanOS（开发中）

ArtisanOS 是一个轻量级的实验性操作系统，通过自定义的虚拟机与操作系统核心深度绑定，实现了程序管理、内存分配和文件操作的统一管理。该项目基于 C 和汇编语言编写，旨在探索操作系统与虚拟机的集成设计，同时为教学和研究提供一个简单易扩展的平台。

## 功能特性

### 核心功能
- **自定义虚拟机**：
  - 通过自定义字节码运行用户程序。
  - 支持基础指令集，如算术运算、条件跳转、函数调用等。
  - 实现了时间片轮转的任务调度。

- **文件系统 (FAT12)**：
  - 支持文件的读取、写入和删除操作。
  - 基于 FAT12 文件系统实现，适合小型存储设备。

- **内存管理**：
  - 使用魔术字和守卫机制保护内存安全。
  - 支持多任务环境下的内存分配和释放。

- **多任务支持**：
  - 基于简单的时间片轮转调度，支持多个任务同时运行。
  - 使用自定义中断处理程序实现任务切换。

- **图形用户界面 (TUI)**：
  - 提供简单的文本用户界面。
  - 支持标题栏绘制和选项高亮显示。

### 技术细节
- **引导加载器**：
  - 包括主引导记录 (MBR) 和第二阶段加载器。
  - 支持从实模式切换到保护模式，并加载内核。

- **硬件交互**：
  - 使用汇编语言实现低级硬件操作，如 A20 地址线启用和磁盘读写。
  - 提供简单的 PS/2 键盘输入支持。

---

## 项目结构

```
ArtisanOS/
├── MBR.s            # 主引导记录代码 (16 位汇编)
├── PMode.s          # 第二阶段加载器代码 (16 位汇编)
├── DiskAndFAT.c     # 文件系统相关实现
├── Memory.c         # 内存管理模块
├── IDT.c            # 中断描述表及任务调度
├── INTERPRETER.c    # 字节码虚拟机及指令集实现
├── TUI.c            # 文本用户界面
└── IO_INCLUDE.c     # 输入输出管理及硬件抽象
```

---

## 编译与运行（注意！该项目并未完全完成，以下内容只是一个模板）

### 环境要求
- GCC (支持交叉编译，目标平台为 x86)
- 汇编工具 (如 `nasm`)
- 磁盘镜像工具 (如 `qemu` 或 `bochs`)

### 编译步骤
1. 克隆项目：
   ```bash
   git clone https://github.com/Yauhak/ArtisanOS.git
   cd ArtisanOS
   ```

2. 编译引导加载器和内核：
   ```bash
   nasm -f bin MBR.s -o mbr.bin
   nasm -f bin PMode.s -o loader.bin
   gcc -m32 -ffreestanding -c *.c
   ld -m elf_i386 -T linker.ld -o kernel.bin *.o
   ```

3. 创建磁盘镜像：
   ```bash
   dd if=/dev/zero of=disk.img bs=512 count=2880
   dd if=mbr.bin of=disk.img conv=notrunc
   dd if=loader.bin of=disk.img seek=1 conv=notrunc
   dd if=kernel.bin of=disk.img seek=66 conv=notrunc
   ```

4. 运行系统：
   ```bash
   qemu-system-i386 -fda disk.img
   ```

---

## 使用说明

系统启动后，将显示 ArtisanOS 的标题界面以及简单的 TUI 界面。用户可以通过键盘输入和虚拟机指令交互，运行编译后的字节码程序。

### 示例字节码程序
`ARSLanguage.txt` 中包含了一个简单的字节码程序示例：
```txt
main
    var
        x int 0 10;
        y int 0 11;
    end_var;
    eq x,y;
    jmp_t true
    pstr "x 不等于 y"
    :true
        pstr "x 等于 y"
    hlt
```

将其编译为 ArtisanOS 支持的字节码后加载运行。

---

## 贡献

欢迎对该项目提出意见或建议！请通过 [GitHub Issues](https://github.com/Yauhak/ArtisanOS/issues) 提交问题，或发起 Pull Request 贡献代码。

---

## 许可证

本项目基于GPL3.0许可证开源。详情请见 [LICENSE](LICENSE) 文件。
