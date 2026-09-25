# ArtisanOS

一个**语言导向**的极简操作系统：自研字节码虚拟机既是内核的核心组件，也是唯一的不受信代码执行边界。
配合带碎片合并的动态内存管理（**堆仿栈**）、协作式多任务调度、带跳转链表的**极简文件系统**与串口终端，
以及自创的 "ARS" 伪汇编语言与配套编译器。

A **language-oriented** minimal operating system: a hand-written bytecode VM that serves as
both the kernel's core component and the sole boundary for untrusted code — plus a
fragment-merging allocator (a **heap that behaves like a stack**), cooperative multi-tasking,
a tiny FAT-style flash filesystem with a serial terminal, and the original "ARS"
pseudo-assembly language with its own compiler.

> **可移植性 | Portability**
> 除 `Glue.h`（ABI 胶水层）与 `main.ino`（平台入口）之外，全部代码都是平台无关的纯 C，
> **几乎不需要修改**即可移植到任何 32 位平台，甚至可以直接作为普通进程跑在宿主操作系统上。
>
> Except for `Glue.h` (the ABI glue layer) and `main.ino` (the platform entry point), the entire
> codebase is platform-independent C. It ports to any 32-bit target **almost unmodified** — and it
> runs perfectly well as an ordinary user-space process on a host OS.

---

## 项目简介 | Project Overview

ArtisanOS 把一台机器变成一个运行自研字节码的"多任务宿主"：

- 编译器把 ARS 源码编译成紧凑、**与目标架构无关**的字节码（`.ars_bin`）
- 字节码可以随固件烧录（内置模式），也可以放在 FLASH 文件系统里由调度表加载（文件驱动模式）
- 解释器以轮转方式执行最多 8 个任务
- 每个任务的运行内存由一套"魔术字块头 + 空闲链表 + 前后合并"的分配器管理
- 一切外设访问都经过 ABI 胶水层查表，字节码自己碰不到硬件
- 上位机用 `ARSTerm.py` 通过串口上传 / 下载 / 删除程序，并可视化 FLASH 占用

ArtisanOS turns any 32-bit machine into a multi-tasking host for its own bytecode.
Compile ARS → load bytecode → run up to 8 tasks round-robin with a coalescing allocator,
with an on-chip filesystem that the scheduler reads its task list from.

**它当前被部署在 RP2040 上，但这不是它的身份。** RP2040 只是众多可能的后端之一。

**It currently runs on the RP2040 — but that is a deployment choice, not its identity.**

---

## 为什么它跨平台 | Why It Travels Well

### 1. 平台适配面只有两处

| 组件 | 是否需要改动 |
|---|---|
| `Compiler/Source/Compiler.c/.h` | ❌ 纯 C，无平台依赖，桌面端直接编译 |
| `Core/IO_INCLUDE.cpp/.h` | ❌ 只用自实现的 `ARS_memmove` / `ARS_memset`，不依赖 libc 以外的任何东西 |
| `Core/Memory.cpp/.h` | ❌ 纯 C。仅操作字节缓冲区 |
| `Core/INTERPRETER.cpp/.h` | ❌ 纯 C。只做指令译码与内存访问 |
| `Core/ARSSCHED.cpp/.h` | ❌ 纯 C。只依赖文件系统与解释器的公开接口 |
| `Core/ARSCMD.cpp/.h` | ❌ 纯 C。命令与协议全在这里，只调 `ARSUART` 的收发原语 + 文件系统/调度器 |
| `Core/ARSFS.cpp/.h` | ⚠️ 平台无关，但需要目标平台提供 3 个硬件原语 `arsfs_hw_read` / `arsfs_hw_erase` / `arsfs_hw_write`（RP2040 的实现已内置） |
| `Core/ARSUART.cpp/.h` | ⚠️ 串口传输层：依赖 `Serial`（Arduino 串口 API）。换链路（UART / TCP / 假串口）只改这一个文件，命令层不用动 |
| `Core/ByteCode.h` | ❌ 就是一个 `const unsigned char[]` |
| `Core/Glue.h` | ✅ **唯一**包含 `<Arduino.h>` 的地方（`pinMode`/`digitalWrite`/`digitalRead`/`millis`） |
| `Core/main.ino` | ✅ 平台入口：装载任务、提供调度节拍 |

**必须改写的只有两个文件**（`Glue.h`、`main.ino`），外加一处串口映射（`ARSUART.cpp`）。
其余原样编译。

### 2. 字节码与架构无关，一次编译到处运行

指令编码固定为 `1 字节操作码 + N × 4 字节参数`，数值按小端存放：

- 同一份 `.ars_bin` 可以直接喂给 RP2040、ESP32、STM32、桌面进程，**无需重编**
- 只要目标仍是 32 位小端，字节码就是通用的；换端序只需改编译器写字节的顺序
- 内存访问全部通过 `ARS_memmove`/`ARS_memset` 逐字节进行，**不依赖结构体对齐**
  （`Magic` 块头刻意用 `__attribute__((packed))` 并使用 `ARS_memmove` 读写，正是为此）

### 3. 可以作为一个进程跑在宿主系统上

这是最能说明问题的一点：整套东西可以被编成一个普通的宿主进程。

```c
// 平台层只需要提供这 4 个符号，其余原样编译
unsigned long millis(void);
void pinMode(int pin, int mode);
void digitalWrite(int pin, int val);
int digitalRead(int pin);

// 然后就像 main.ino 那样跑
init_mem_info();
loadTask(0);
loadTask(1);
while (running) {            // 对应 Arduino 的 loop()
    // 取指、译码、执行
}
```

文件系统也遵循同样的原则：它的平台相关部分被压缩成了**三个函数**
（`arsfs_hw_read` / `arsfs_hw_erase` / `arsfs_hw_write`）。
桌面回归测试只要提供一份"用 RAM 模拟 FLASH"的替身——**并且严格模拟"先擦后写"**
（未擦除就写要报错）——整个文件系统、串口命令与调度逻辑就都能在 PC 上跑，不必占用真机。
换句话说，**移植文件系统的工作量就是实现这三个原语**。

换句话说：**它是一个用户态可运行的操作系统层**。调试时不需要真机——
在 PC 上就能单步、能打印内存状态、能在 gdb 里下断点。

### 4. 移植清单

| 目标 | 要做的事 |
|---|---|
| 其它 Arduino 核心（ESP32 / STM32 / AVR） | 改 `Glue.h` 里的引脚 API 映射，`main.ino` 基本不动 |
| 裸机 MCU | 提供 `millis()`（或任意递增计数器）、GPIO 读写、扇区级 FLASH 擦写（`arsfs_hw_*`），自己写 `main()` |
| Linux / Windows / macOS 进程 | 提供 4 个平台桩函数（串口收发、`ARS_provision()`）+ 一份 RAM 版 `arsfs_hw_*`，用于开发、调试与回归测试 |
| WebAssembly / 模拟器 | 保留 `INTERPRETER.cpp` + `Memory.cpp`，前端只做输入输出 |

> 需要留意的约束：`ars_i32` 假定 32 位，内存管理器与字节码都建立在这个前提上。
> 64 位宿主下请把 `ars_i32` 固定为 `int32_t`（不要直接用 `int` 的宽度假设）。

---

## 设计要点 | Design Highlights

### 堆仿栈：用堆实现栈的语义

这是整个项目里最值得单独拿出来讲的设计。

传统做法里，"函数调用"和"动态内存"是两套东西：调用栈由硬件/编译器管理，堆由分配器管理。
ArtisanOS 把二者**合并成一个堆**：每一次 `call` 都是一次堆分配，每一次 `ret` 都是一次堆释放。

```
[Magic 块头][4 字节返回地址][变量区 + 形参]
   ↑ SPLT/FREE      ↑ 上下文     ↑ 被调函数的作用域
```

- **`call`** → 在堆上分配一块，把"返回地址"和形参写进块内，`MemTail` 指向新块
- **`ret`** → 释放最末一块，从块内取出返回地址，`MemTail` 退回上一块
- **相邻空块会被主动合并**（`SuperFree`），因此反复调用不会产生外部碎片
- 每个任务有独立的 `MemHead`/`MemTail`，形成自己的"调用链"

得到的性质：

1. **作用域即生命周期**：变量活到它所属的那一层 `ret` 为止，语义上和栈完全一致。
2. **栈深度有限，但块可以复用**：因为块大小固定（编译期算出），释放的块能被下一次同签名调用
   直接复用——用堆的通用性换来了栈的可预测性。
3. **能被检查**：每个块都有魔术字与 `Check` 守卫字，越界破坏**可被发现**并尽量回收。
   纯硬件栈做不到这一点。
4. **天然支持变长与分离**：作用域层级、跨层返回、参数按值拷贝进新块，都是普通堆操作。

代价也很清楚：解释器要自己做内存管理，而且没有硬件级保护。这是一个自觉的取舍。

### VM 作为内核组件：语言导向操作系统

ArtisanOS 的"进程"就是一段字节码，"系统调用"就是一次 `abi_invoke` 查表。
这不是"在一个 OS 上跑个解释器"，而是把**语言运行时本身当作内核**：

- **内核 = 解释器 + 内存管理器 + 调度器 + 文件系统**，全部可审计
- **用户态 = 字节码**，它没有指针、没有原生执行权，唯一的对外通道是 ABI 表
  （连读写文件也必须经 ABI 通道，不能自己碰 FLASH）
- **边界由语言运行时定义**，而不是由 MMU 定义——这正是语言导向 OS 的出发点
  （Inferno 的 Dis、Singularity 的 SIP 都在同一条脉络上）

这条路线带来的好处正好契合嵌入式：

- 不需要 MMU 就能定义"进程"与"文件"的概念
- 应用不需要针对每个平台重新编译，字节码跨架构通用
- 内核很小、行为可复现，适合资源紧张且要求确定性的设备

需要诚实说明的是：**"隔离"目前是设计意图，尚未完全强制**。见文末「当前限制」。

---

## 主要特性 | Key Features

- **自研字节码虚拟机** — 31 条指令、类型化操作数（B/I/F）、专用结果寄存器 `CalcResu`
- **堆仿栈的动态内存管理** — `SPLT`/`FREE` 魔术字块头 + `Check` 守卫字 + 空闲链表 + 碎片合并
- **协作式多任务** — 8 个任务槽、轮转调度、每任务独立代码页与内存层级
- **极简 FLASH 文件系统** — 32 个目录项、128 个 512B 数据页（共 64KB）、
  用一张 128 字节的跳转链表（FAT 式）描述文件拓扑，无文件夹
- **文件驱动的调度器** — 从 `SCHEDULE` 文件按行读取程序名，装载后轮转执行
- **串口终端** — 上位机可上传 / 下载 / 删除程序，并可视化 FLASH 占用
- **ABI 胶水层** — 字节码不原生执行，一切硬件与文件访问都经 `abi_invoke` 查表
- **字节码跨架构通用** — 一份 `.ars_bin` 喂给所有 32 位小端平台
- **可作为宿主进程运行** — 平台层仅需 4 个符号 + 3 个 FLASH 原语，便于开发与回归测试
- **ARS 伪汇编** — 一门为这台 VM 量身定做、语法相当"有主见"的小语言

---

## 目录结构 | Project Structure

```
ArtisanOS/
├── Core/
│   ├── main.ino
│   ├── INTERPRETER.cpp/.h
│   ├── Memory.cpp/.h
│   ├── IO_INCLUDE.cpp/.h
│   ├── Glue.h
│   ├── ByteCode.h
│   ├── ARSFS.cpp/.h
│   ├── ARSSCHED.cpp/.h
│   ├── ARSUART.cpp/.h
│   └── ARSCMD.cpp/.h
├── Compiler/
│   ├── Demo/
│   │   ├── LEDFlash.txt
│   │   ├── LEDStream.txt
│   │   └── recursionTest.txt
│   └── Source/
│       ├── ARSIDE.html
│       └── Compiler.c/.h
├── Terminal/
│   └── ARSTerm.py
├── README.md
└── LICENSE
```

> 上位机终端在 `Terminal/` 下：`python Terminal/ARSTerm.py COM4`。

### 串口层与命令层是两个独立模块

`ARSUART`（传输）和 `ARSCMD`（命令）**互不 include、互不认识**，只靠一个回调接起来：

```
ARSUART  ── 只做字节进出 ──────────────────────────────
  arsuart_begin / poll            推发送缓冲 + 把收到的字节拼成整行
  arsuart_puts / putc / putu32    发送：只排队，绝不等待宿主
  arsuart_rxAvail / rxGet / rxFlush  接收：给命令层收二进制块用
  arsuart_setLineHandler(fn)      ← 整行回调挂在这里
                    ↑
                    │  main.ino 里一行接线：
                    │  arsuart_setLineHandler(arscmd_line);
                    ↓
ARSCMD   ── 只做协议与业务 ────────────────────────────
  arscmd_line(line)               收行 → 认命令 → 调 ARSFS / ARSSCHED
```

好处是两边都能单独替换和单独测试：换物理链路（UART / TCP / 假串口）只动 `ARSUART`，
加命令只动 `ARSCMD`；离线测试也可以只链 `ARSUART`（不注册回调就什么命令都不处理）。
命令层的所有应答都只用 `arsuart_put*` 这几个原语，所以它根本不知道底下是 USB 还是别的。

---

## 构建与运行 | Build & Run

### 1. 编译 ARS 编译器 | Build the compiler

```bash
gcc Compiler/Source/Compiler.c -o arscc
```

### 2. 编译 ARS 程序 | Compile an ARS program

```bash
./arscc Compiler/Demo/LEDStream.txt
# -> Compiler/Demo/LEDStream.ars_bin，并打印字节码 dump
```

也可以用 `ARSIDE.html`：浏览器直接打开，粘贴/拖入源码，点「编译」导出 `output.ars_bin`，
控制台会给出 `main` 头部地址、各标签地址与字节码 dump。两个编译器的输出经过校验是
**逐字节一致**的。

### 3. 两种运行模式 | Two run modes

`Core/IO_INCLUDE.h` 顶部的 `USE_FILE_AND_UART` 决定用哪一套：

```c
#ifndef USE_FILE_AND_UART
  #define USE_FILE_AND_UART 1     /* 0 = 固件内置模式，1 = 文件驱动模式 */
#endif
```

也可以用编译选项覆盖：`-DUSE_FILE_AND_UART=0`。

| | `0` 固件内置模式 | `1` 文件驱动模式（当前默认） |
|---|---|---|
| 程序来源 | `ByteCode.h` 里的数组，随固件烧录 | FLASH 文件系统里的文件 |
| 需要的组件 | 解释器 + 内存管理器 | 再加上 `ARSFS` / `ARSSCHED` / `ARSUART` / `ARSCMD` |
| 任务表 | `main.ino` 里写死 | 读 `SCHEDULE` 文件，每行一个文件名 |
| 外设通道 | `abi_invoke 0/1/2`（GPIO、定时器） | 再加 `3/4/5/6`（文件读写） |

关掉开关时 `ARSFS.cpp`、`ARSUART.cpp`、`ARSCMD.cpp` 都会整个编译成空文件——文件系统的
接口只被 `ARSCMD` / `ARSSCHED` / `Glue.h` 内部使用，而它们各自的 `#else` 已经把这条路
堵死了。所以**不留空实现**：
万一将来有人在关闭状态下误用，会在**链接期**直接报未定义符号，而不是运行期悄悄返回 `FS_EIO`。
唯一必须保留空实现的是 `Glue.h` 的那四个 `gFile*`，因为字节码的 ABI 表是**无条件编译**的。

**内置模式**默认装载两个任务：

| 任务 | 程序 | 行为 |
|---|---|---|
| 0 | `LED_Flash` | 读 GPIO13（光敏电阻模块 DO），为 1 时点亮 GPIO6 的 LED，否则熄灭 |
| 1 | `LED_Stream` | GPIO18/19/20 三个 LED 轮流点亮，形成流水灯 |

**文件驱动模式**首次启动会检测到 `SCHEDULE` 为空，自动把上面两个程序写进文件系统
并生成调度表，所以刷完固件立刻就能看到效果；之后可以用串口终端随意替换。

### 4. 烧录与连接 | Flash & connect

1. 用 Arduino IDE 打开 `Core/main/main.ino`，开发板选 **Raspberry Pi Pico**，点上传。
   也可以按住 BOOTSEL 插 USB，把生成的 `main.ino.uf2` 拖进 `RPI-RP2` 盘。
2. 烧录完成后设备会枚举成一个 USB CDC 串口（`2E8A:00C0`）。
3. 上位机连接：

```bash
pip install pyserial
python ARSTerm.py COM4          # Windows
python ARSTerm.py /dev/ttyACM0  # Linux
```

> 注意：**串口监视器会独占串口**。Arduino IDE 的串口监视器开着时，
> 外部终端和上传工具都打不开同一个口，用之前先关掉（IDE 会自动重开监视器，
> 所以最稳的是连 IDE 一起关）。

---

## 文件系统 | The Filesystem

一个**扁平、定长、无文件夹**的极简文件系统，约 400 行，不依赖 libc。

### 存储布局

固定占用 FLASH **末尾 68KB**（`FS_TOTAL = 69632` 字节）：

```
偏移 0        +------------------------+
              | FCB[32]                |  32 × 20 = 640B   目录项
偏移 640      +------------------------+
              | nextPg[128]            |  128B             跳转链表
偏移 768      +------------------------+
              | 保留（补到扇区边界）      |
偏移 4096     +------------------------+  <- DATA_OFF
              | page 0    (512B)       |
              | page 1    (512B)       |  共 128 页 = 64KB
              | ...                    |  每页全部是数据，页内无任何元数据
偏移 69632    +------------------------+
```

**基地址按芯片实际容量算**，不写死：取 `FLASH 末尾 FS_TOTAL 字节`。
2MB 的 Pico 上就是 `0x101EF000`；4MB / 8MB / 16MB 板子会自动落在各自末尾。
容量读不到时保守回退到 2MB —— 猜小只是浪费尾部空间，绝不会越界。

### 拓扑：128 字节的跳转链表（FAT 式）

文件由哪些页组成、以什么顺序组成，全部记在 `nextPg[128]` 这一张表里：
**下标是"本页"，值是"下一页"**。

```
nextPg[i] == FREE_OR_DEL (0xFE)   页 i 空闲
nextPg[i] == EOF_PG      (0xFF)   页 i 是文件最后一页
nextPg[i] == 0..127               页 i 的下一页是 nextPg[i]
```

于是读文件就是一路跳过去：

```
FCB.start = 3
  page 3 ──► page 5 ──► page 9 ──► EOF
nextPg[3]=5  nextPg[5]=9  nextPg[9]=0xFF
```

`FCB` 只需记 `start`（首页）和 `size`（实际字节数，末页可能有填充）。
写入顺序也是刻意的：**先写数据页，最后再落盘元数据**。中途掉电时表里那些页仍算空闲，
不会留下指向脏数据的坏链。

### 分配策略与风险（掉电 / 复位）

FLASH 的擦除粒度是 **4KB 扇区**，比页大 8 倍，"改一个页"实际是
"读回整扇区 → 改 → 擦掉 → 写回"。同一个扇区里可能同时住着好几个文件的页，
而这个窗口**不是原子的**——窗口里断一次电，同扇区里别人的数据会一起没。
这是为了简单直接接受的风险，不为此增加独占扇区之类的额外机制。

分配策略就是最朴素的**首次适配**：从 0 号页往后扫，遇到第一个空闲页就用掉
（`pgAlloc`）。文件页不要求连续（没有外部碎片），加链也只是改 RAM 里的 `nextPg`，
不需要回头重写上一页。保证"只要还有空闲页就一定写得进去"。

### 自愈

开机时如果 `SCHEDULE` 的内容是**全 `0xFF`**（典型的"那一页被擦掉了"），
固件会判定文件系统没铺好，把 `SCHEDULE` / `LEDFLASH` / `LEDSTREAM` 重新写一遍。
否则会出现"文件在 `ls` 里看得见、内容却是空的、一个任务都装载不到、板子看起来像死了"的状态。

### 目录项 FCB

```c
typedef struct FCB {
  ars_i8  fileName[15];   /* 定长 15 字节，不足用空格补齐，无扩展名 */
  uars_i8 start;          /* 首页下标；FREE_OR_DEL 空闲，EOF_PG 空文件 */
  ars_i16 size;           /* 实际长度（字节） */
  uars_i8 attr;           /* 预留属性位 */
} FCB;                      /* 20 字节 */
```

- 最多 **32 个文件**，单一命名空间，没有文件夹
- 单个文件上限 `FILE_MAX = 2048` 字节（正好一个任务代码页）
- 第 0 号目录项**固定留给调度表 `SCHEDULE`**

### 格式化与版本

元数据扇区开头有 `MAGIC`（`'ARFS'`）和 `FS_VER`。挂载时两者都要对得上，否则自动重新格式化。
文件系统格式变了却没人重建，症状是"文件系统看起来在跑，但内容全是错的"。

### 首次启动的预置内容

文件驱动模式下，如果 `SCHEDULE` 还是空的（或者内容已被擦成全 `0xFF`），
固件会把内置的两个示例写进去：

```
SCHEDULE  ->  "LEDFLASH\nLEDSTREAM\n"
LEDFLASH  ->  LED_Flash  的字节码（96 字节）
LEDSTREAM ->  LED_Stream 的字节码（210 字节）
```

这一手同时充当了文件系统写通路的自检：刷完固件如果流水灯亮了，
说明初始化、分配、页写、元数据落盘、调度装载这一整条链路都是通的。

**`format` 就是"恢复出厂设置"**：清空文件系统 → 铺回上面这三份 → 立刻按新的
`SCHEDULE` 重启任务。一条命令回到干净可用状态，不需要再断电重上。
（耗时不到 1 秒——要写十几次 FLASH。）

### 串口输出不会阻塞固件 | Output Never Blocks

设备侧**不再用 `Serial.print` 输出**。Arduino mbed 核心的 `USBCDC::send()` 内部是
`write_op.wait(NULL)`——无限等待，宿主一旦停止排空 CDC（串口监视器暂停或关闭、
终端进程被杀、驱动缓冲塞满）就永久卡住，固件随之僵死。

现在的做法是**排队 + 主循环里推**，全程一个字节都不等：

- `txPut()` 只把字节写进一个 8KB 的环形缓冲（`txRing`），立刻返回；
- `arsuart_poll()` 每轮调用 `txDrain()`，用 `USBCDC::send_nb()`（非阻塞原语，
  `PluggableUSBSerial.h` 里是 public 的）把放得下的字节丢给底层，放不下就下一轮再说；
- **一次最多推 63 字节**（`TX_PKT`）。这是踩出来的坑：写入长度正好等于端点最大包
  （64 字节）时，主机侧不认为传输结束，整包会被 CDC 驱动扣住不交给 `read()`，
  直到下一包才一起吐出来——症状就是"应答要等下一条命令才冒出来"，
  而且只在应答总长恰好是 64 的倍数时出现。让每包都是小于 64 字节的短包（63）即可；
- 宿主正常读 → 一个字节都不会丢（8KB 积压余量，最长的 `occ` 应答也装得下）；
  宿主不读 → 缓冲满了就丢掉这条应答的剩余部分（`txOver`），
  下一条命令开始时会丢弃超过 1KB 的陈年积压（`TX_KEEP`）。**固件照跑不误**。

另外 `arsuart_poll()` 一次最多处理 `POLL_MAX_CMD = 4` 条命令就返回，
免得命令洪泛把任务饿死。

> 实践建议：用 `ARSTerm.py` 或 Windows Terminal 之类的终端，别用 IDE 的串口监视器
> 长期挂着——它一暂停读取，应答会被丢掉（但设备不会卡）。

### 串口排障 | When the serial link misbehaves

症状与成因是**一一对应**的，照这张表看：

| 你看到的 | 真正的原因 | 怎么办 |
|---|---|---|
| `打不开串口`、`Access denied` | 串口被独占：Arduino IDE 的串口监视器（关掉它会自动重开）、上一个被 Ctrl-C 杀掉的终端还没放手 | 关掉占口的程序；还不行就**拔插一次板子** |
| `Cannot configure port ... OSError(22, '信号灯超时时间已到' / ERROR_SEM_TIMEOUT 121)` | 上位机在**频繁改 `ser.timeout`**：pyserial 每次赋值都发一次 `SetCommState`，而 CDC 那边只要还有一次写没落地，这个调用就会等 5 秒然后报错，并把这个口**卡死到下一次 USB 复位** | 一条会话里**只设一次** `timeout`，之后靠"截止时间"控制超时（`ARSTerm.py` 里的 `POLL_TIMEOUT` 就是这么用的）。卡死了就拔插，或在设备管理器里把那个 USB 串口设备**禁用再启用**（不用断电） |
| 应答**错位**：`update` 报的错每次都不一样（`设备未就绪: ARS FS_VER=...`、`传输中断: DATA 210`…） | 上一条命令的应答只读了一半（Ctrl-C、超时、口卡过），残留字节让之后每条命令都对不上号 | 设备侧：出错路径会 `rxFlush()` 丢掉接收缓冲剩下的数据，命令行里混进不可打印字节就静默丢弃；上位机侧：每条命令前 `drain()`，`update` 则一直读到 `RDY` 为止，中途的杂音跳过 |
| 上传到一半 `传输中断：没等到 ACK` | 单块 128 字节等了 5 秒还没到（宿主在刷屏 / 口被抢） | 设备已把缓冲里的残留丢掉，**重新发一次命令**即可，不用断电 |
| `ls` / `occ` 的输出看着"少了一截" | 宿主长时间不读，设备主动丢弃了这条应答的剩余部分（宁可丢应答也不死等） | 正常；确认终端在一直读就行 |
| 固件像死了一样，USB 还在但什么都不回 | 基本是宿主那侧把口卡住了（见上面第 2 条），先拔插试试 | 代码里已经**没有**任何看门狗/自恢复逻辑，设备侧不会自己复位 |

> 为什么不做"看门狗自恢复"？试过两轮：硬件看门狗会把 `reset_usb_boot()`（1200bps
> touch，IDE 下载靠它）搞坏——看门狗使能位不会被普通复位清掉，板子会被从 BOOTSEL
> 里踢出来；软件停摆检测则会在宿主只是"读得慢"的时候误判、把任务全重启。
> 现在改成"输出永不阻塞 + 出错就丢同步、宿主重发"，比自恢复更可预测。

---

## 调度器 | The Scheduler

`Core/ARSSCHED.cpp` 把"任务表"也变成一个文件：

1. 确保 0 号目录项是 `SCHEDULE`（不存在则创建；若 0 号位被别的文件占了，退化为按名字查找）
2. 逐行读出文件名（每行 15 字节、空格补齐、无扩展名）
3. 依次读取对应文件的字节码，装入 `exeMem[任务号]`
4. 调用 `call()` 建立该任务的初始作用域，然后轮转执行

```
SCHEDULE 文件内容（每行一个文件名）:
LEDFLASH
LEDSTREAM
```

于是"改任务列表"不需要重新编译固件，只要用串口改一个文件。

### 重启 / 起停任务 | Restart / start / kill

**上传文件不会自动重启任何东西**——`update` / `del` / `format` 只改 FLASH，
跑着的任务照旧。要生效就在终端里下命令：

| 命令 | 作用 |
|---|---|
| `reboot <名字>` | **只重启这一个任务**，别的任务完全不受影响（指针都不动）。名字必须正在跑，否则回 `ERR noent` |
| `reboot_all` | 重置整个堆，按 `SCHEDULE` 重新装载全部任务。**改 SCHEDULE 后用这个** |
| `tasks` | 列出当前存活任务与它们的 ID（ID 就是任务槽号 0..7） |
| `start <名字>` | 手动启用一个任务：从文件系统读该程序，装进**第一个空闲槽位**并开始跑 |
| `kill <名字>` | 杀掉**所有**这个名字的任务（同名可能不止一个） |
| `killid <ID>` | 按槽位号杀（只杀一个，用 `tasks` 查 ID） |

`reboot <名字>` 走的是 `ReArrangeMemAndTask()`：把该任务占的内存块整条释放
（相邻空块照样合并）、槽位清零，然后**先把新程序读进缓冲区**、读成功才动手，
最后重新 `call()`。所以：

- 上传一个新版本的程序 → `reboot 那个名字` → 立刻跑新版本，**流水灯不会被打断**；
- 文件读不到时原样不动，不会把一个正在跑的任务原地弄死；
- 只释放属于它的块，别的任务的内存和状态一个字节都不碰。

`reboot_all` 则是 `init_mem_info()`（真正的整堆重置）+ `arssched_load()`。
离线测试里反复 `reboot` 50 次来盯"单任务释放是否干净"这一点。

`start` / `kill` 是**和 `SCHEDULE` 文件解耦**的一对，玩的是"现在谁在跑"：

- `start LEDSTREAM` → 装进第一个空槽位（可能不是它上次的 ID），回 `OK`；
  文件不存在回 `ERR noent`，8 个槽位占满回 `ERR full`；可以重复启动同名任务。
- `kill LEDSTREAM` → **所有**叫 LEDSTREAM 的任务一起杀（8 个槽位全扫一遍），
  释放它们占的内存、清空槽位、名字也清掉，回 `OK`；一个都没杀到回 `ERR noent`。
  `killid 1` 则只杀 1 号槽位那一个。
- **同名任务是可能的**：`start` 会拒绝重复启动（`ERR busy`），但 `SCHEDULE` 里同一个
  文件名写两行就会装出两个同名任务，之后 `reboot_all` 也会照样装两个。所以 `kill`
  是按名字"全杀"，要精确杀某一个就用 `tasks` 查出 ID 再 `killid`。
- **只杀运行中的实例**：文件还在，`SCHEDULE` 也没改。
  所以 `kill X` 之后来个 `reboot_all`，X 会按 `SCHEDULE` 又被装回来。
  要"永久移除"，就改 `SCHEDULE`（或 `del` 掉文件）再 `reboot_all`。

```
tasks
  TASKS 2
  0 LEDFLASH
  1 LEDSTREAM
  OK
```

> 字节码通过 ABI 写文件**不会**触发任何重启，只有终端的 reboot/start/kill 命令会。

---

## 串口终端与协议 | Terminal & Protocol

### 上位机命令

`ARSTerm.py`（仓库根目录，依赖 `pyserial`）：

| 命令 | 作用 |
|---|---|
| `update <localfile> <dest_name>` | 上传本地文件到设备，不存在则创建 |
| `get <dest_name>` | 下载文件，保存为 `<8位随机数>.txt`（HEX 文本） |
| `del <dest_name>` | 删除文件 |
| `ls` | 列出所有文件名 |
| `occ [text]` | 可视化 FLASH 占用：加 `text` 输出纯文本，否则红=占用 / 绿=空闲 |
| `ver` | 查询固件元数据版本、当前存活任务数 |
| `tasks` | 列出存活任务与它们的 ID |
| `start <name>` | 手动启用一个任务（装进第一个空闲槽位，不改 `SCHEDULE`） |
| `kill <name>` | 杀掉正在跑的那个任务（只杀实例） |
| `killid <ID>` | 按任务 ID 杀 |
| `reboot <name>` | 只重启正在跑的那一个任务（上传完新程序后用这个） |
| `reboot_all` | 重置整个堆并按 `SCHEDULE` 重新装载全部任务 |
| `format` | **恢复出厂设置**：清空、铺回出厂示例、并立刻重启任务（终端里会二次确认） |
| `help` / `quit` | 帮助 / 退出 |

设备侧的命令名与之一一对应。

> **大小写随便敲。** 命令词（`REBOOT` / `Reboot` / `reboot`）和文件名/任务名
> （`LEDStream` 命中 `LEDSTREAM`）都**不区分大小写**

出错时的几种应答要分清：

| 应答 | 含义 |
|---|---|
| `ERR unknown` | 命令词不认识 |
| `ERR noent` | 命令对了，但那个名字不存在（文件没有 / 任务没在跑） |
| `ERR args` | 命令对了但参数不够（比如 `reboot` 没给名字、`killid` 没给数字） |
| `ERR full` | `start` 时 8 个任务槽位全占着 |
| `ERR write` | 上传的数据落盘失败（文件系统满 / 底层出错） |

### 行协议（便于自己写客户端）

设备**不回显、不提示符**，全部应答以 `\r\n` 结尾：

| 请求 | 应答 |
|---|---|
| `update <name> <len>` | `RDY` → 每 128 字节一块（单块最多等 5 秒），每块回 `ACK <已收字节数>` → 全部收完落盘后 `OK`；超时或写失败回 `ERR ...` |
| `get <name>` | `DATA <len>` → 每 32 字节一行的十六进制 → 空行 → `OK`；不存在回 `ERR noent` |
| `del <name>` | `OK` / `ERR noent` |
| `ls` | `LS` → 逐行文件名 → `OK` |
| `tasks` | `TASKS <存活数>` → 每行 `<ID> <名字>` → `OK` |
| `start <name>` | `OK` / `ERR noent`（文件不存在）/ `ERR full`（槽位满）/ `ERR args` |
| `kill <name>` | `OK`（杀掉一个或多个同名任务）/ `ERR noent`（一个都没在跑）/ `ERR args` |
| `killid <ID>` | `OK` / `ERR noent`（槽位空或 ID 越界）/ `ERR args`（没给数字） |
| `reboot <name>` | `OK` / `ERR noent`（没在跑）/ `ERR args`（没给名字） |
| `reboot_all` | `OK` |
| `format` | `OK`（会花约 1 秒：清空 + 铺回出厂示例 + 重启任务）/ `ERR format` |
| `occ` | `USED <n> FREE <m>` → 128 页的页图（每行 8 页）→ `FCB:` → 32 行目录项 → `OK` |
| `ver` | `ARS FS_VER=<n> TASKS=<n>` |

写客户端时记住三条：

1. **每条命令之前先把接收缓冲读干净**（`ARSTerm.py` 的 `drain()`）。只要读到一点点
   上一条命令的残留，后面每条命令都会错开一位，报错会变得"随机"。
2. **不要反复改 `ser.timeout`**（见上文「串口排障」），用固定的小超时 + 截止时间。
3. `update` 的每条应答都可能是 `ERR ...`，见到就**停下来**并把缓冲读干净，
   不要继续灌数据——继续灌只会让设备把二进制当成命令行。

关于 `occ` 的颜色：设备发的是**真正的 ANSI 转义序列**（`0x1B`），不是字面文本。
不支持 ANSI 的终端（例如 Arduino IDE 的串口监视器）会把它原样显示出来，
所以另外提供了不带任何转义码的 `occ text`：

```
occ text
  USED 3 FREE 125 (*=used .=free)
  000* 001* 002* 003. 004. 005. 006. 007.
  008. 009. 010. 011. ...
  FCB:
  SCHEDULE start=2 size=19
  LEDFLASH start=0 size=96
  LEDSTREAM start=1 size=210
  FREE_OR_DEL
  ...
```

### 字节码读写文件

文件驱动模式下，字节码也能访问文件，但**必须经过 ABI 查表**（`Glue.h`），
并且由互斥量保护 —— 字节码自己碰不到 FLASH：

| 调用号 | 函数 | 调用约定（先用 `pushp` 压参） |
|---|---|---|
| 3 | `FILE_OPEN` | 压入 15 字节文件名 → 打开（不存在则创建），返回文件长度 |
| 4 | `FILE_READ` | 压入 变量地址、长度 → 从当前游标读入该变量，返回实读字节数 |
| 5 | `FILE_WRITE` | 压入 变量地址、长度 → 写到当前游标，返回实写字节数 |
| 6 | `FILE_CLOSE` | 关闭会话 |

---

## 程序在内存中的布局 | Program Layout

编译器产出的镜像分三段，`call` 依赖这个布局定位入口：

```
[0..3]      main 函数头偏移 H（写在程序最前面）
[4..7]      变量区大小（main 被当作普通函数布局：头部 4 字节 + 紧跟其后的代码）
[8 ...]     各子程序的代码
[H]         main 的变量区大小
[H+4 ...]   main 的第一条指令
```

每个子程序头部同样有一个 4 字节的"变量区大小"字段。
解释器装载时只需把镜像基地址交给 `call`，它读出 `H` 后正好落在 `H+4`，即 main 的第一条指令。

---

## ARS 语言 | The ARS Language

### 变量声明：长度 = 元素数 - 1

```
$变量名 类型 长度
```

类型为 `B`(1 字节) / `I`(4 字节) / `F`(4 字节)。**长度不是初值，而是"元素数 - 1"**：

```
mem
  $flag  B 0     ; 1 个字节
  $x     I 0     ; 1 个 int
  $vec   I 2     ; 3 个 int
  $y     F 2     ; 3 个 float
end_mem
```

实际占用 `(长度 + 1) × 类型大小` 字节。`init_array` 的"元素数量"沿用同一约定。

### 结果寄存器 CalcResu

所有运算、比较、读取、类型转换的结果都写入 `CalcResu`，再由 `push` 落盘或由 `jmp_t` 判断：

```
add I $a $b        ; CalcResu = $a + $b
push I $sum        ; $sum = CalcResu
eq I $x 10         ; CalcResu = ($x == 10)
jmp_t is_ten       ; CalcResu != 0 时跳转
```

### 数组：只用 3 个参数

```
init_array I $vec 2 5 6 7   ; 初始化 3 个元素：5, 6, 7
read_array I $vec 1         ; CalcResu = $vec[1]
set_array  I $vec 0         ; $vec[0] = CalcResu
```

### 子程序

```
fn delay_ms
  mem
    $len I 0
  end_mem
  ...
  ret
endfn

main
  mem
  end_mem
  pushp I 100          ; 压入实参
  call delay_ms
  hlt
endmain
```

`pushp` 把实参按序压入参数栈，`call` 在堆上新分配一层并把实参拷贝进被调函数的变量区，
`ret` 释放该层内存并跳回调用点。

### 单字节运算限制

所有算术/比较/位运算都按 4 字节处理，**不存在针对单字节的运算指令**
（早期版本的 `ext_byte` 已被移除）。字节要参与运算，请先用 `mov I` 把它读进整型变量：

```
mov I $temp $byteVar     ; 整型 ← 字节（低字节在最低位）
mul I $temp 2
mov B $byteVar $temp     ; 反过来：截回单字节
```

配合 `bit_aox` / `bit_move` 就能手工完成符号扩展、掩码、移位等操作。

---

## 运行时机制 | Runtime Behaviour

### 指令编码

```
操作码字节 = (opcode << 3) | ParamType
```

低 3 位 `ParamType` 由各指令自行解释，常见含义：

- `bit0`：某操作数是立即数(0)还是地址(1)
- `bit1-2`：类型（0=B, 1=I, 2=F）或浮点标志
- `INITARRAY` 的参数是**变长**的，末尾跟着 `[标签字节][数据]` 组成的初始化表

### 调度与内存

- 最多 `OS_MAX_TASK = 8` 个任务，每任务独立代码页 `OS_MAX_SGL_PG = 2048` 字节
- 运行内存总量 `OS_MAX_MEM = 8192` 字节，块头为 `Magic` 结构（含 `Check` 守卫字）
- 子程序返回时释放该层内存；相邻空闲块会被合并，抑制外部碎片
- 每任务参数栈 `OS_MAX_PARAM = 128` 字节

### ABI 胶水层

字节码**不原生执行**，没有指针，唯一的对外通道是 `abi_invoke` 查表调用 `Glue.h` 中的函数：

| 调用号 | 函数 | 说明 |
|---|---|---|
| 0 | `gWrite` | `digitalWrite(pin, val)` |
| 1 | `gRead` | `digitalRead(pin)` → `CalcResu` |
| 2 | `Timer` | `millis()` → `CalcResu` |
| 3–6 | 文件操作 | 见上文「字节码读写文件」 |

调用前用 `pushp` 依次压入参数（4 字节对齐），调用后参数栈自动清空。

```
pushp I 6        ; 引脚
pushp I 1        ; 电平
abi_invoke 0     ; digitalWrite(6, 1)
```

`Glue.h` 是留给使用者的扩展点，也是移植时唯一必须改的文件之一：
加一个函数、在 `ABI_CALL` 里加一个枚举、在 `ABIs[]` 里登记即可。
注意 `Glue.h` 里不止有声明还有定义，**同一个程序只允许一个编译单元包含它**（当前是 `INTERPRETER.cpp`）。

---

## 指令集参考 | Instruction Set

共 31 条。`$名字` 表示变量地址，裸数字表示立即数。

### 数据传送

| 指令 | 语法 | 说明 |
|---|---|---|
| `mov` | `mov 类型 目标 源` | 同类型赋值；源可为 `$地址` 或立即数 |
| `push` | `push 类型 $目标` | 把 `CalcResu` 写入内存 |
| `val` | `val 类型 值/$地址` | 把值载入 `CalcResu` |

### 数组

| 指令 | 语法 | 说明 |
|---|---|---|
| `init_array` | `init_array 类型 $数组 元素数-1 [值...]` | 批量初始化，值可为立即数或 `$地址` |
| `read_array` | `read_array 类型 $数组 下标` | 元素 → `CalcResu` |
| `set_array` | `set_array 类型 $数组 下标` | `CalcResu` → 元素 |

### 运算与比较（结果均入 `CalcResu`）

| 指令 | 语法 |
|---|---|
| `add` / `sub` / `mul` / `div` | `add I $a $b`、`mul F $x 3.14` |
| `eq` / `ne` / `lt` / `le` / `gt` / `ge` | `eq I $x 10` |
| `bit_aox` | `bit_aox A\|O\|X 操作数1 操作数2` |
| `bit_move` | `bit_move L\|R 操作数 位数`（VM 内部操作码名为 `BIT_MOV`） |

整数除法的商向零舍入；除数为 0 返回 `DIV_BY_0`。

### 控制流

| 指令 | 语法 | 说明 |
|---|---|---|
| `jmp` | `jmp 标签` | 无条件跳转 |
| `jmp_t` | `jmp_t 标签` | `CalcResu != 0` 时跳转 |
| `call` | `call 子程序名` | 调用子程序（堆上分配新作用域） |
| `ret` | `ret` | 返回调用点并释放该层内存 |
| `hlt` | `hlt` | 结束程序并释放全部资源 |

标签用 `lb 名字` 声明。

### 类型转换

| 指令 | 语法 | 说明 |
|---|---|---|
| `to_int` | `to_int $浮点变量` | 浮点 → 整型（截断），入 `CalcResu` |
| `to_float` | `to_float $整型变量` | 整型 → 浮点，入 `CalcResu`（位模式不变） |

### 系统接口

| 指令 | 语法 | 说明 |
|---|---|---|
| `abi_invoke` | `abi_invoke 调用号/$地址` | 调用 `Glue.h` 中的 ABI |
| `reg_write` | `reg_write $地址 值` | 向物理地址写 1/4 字节（4 字节参数按位拷贝） |
| `reg_read` | `reg_read $目标 地址` | 从物理地址读 1/4 字节 |
| `ipc_send` | `ipc_send 目标任务 值` | 向指定任务投递消息，返回是否成功 |
| `ipc_recv` | `ipc_recv $目标` | 取走自己的消息（无消息则写 0） |

### 浮点技巧

`mov F $intVar $floatVal` 会按位拷贝，把浮点的位模式搬进整型变量；
`reg_write` 再把它按位写出去，就绕开了"参数类型"的限制。

---

## 示例 | Examples

光敏电阻控制 LED（`Compiler/Demo/LEDFlash.txt`，编译后 96 字节）：

```
main
  mem
    $sensor I 0
    $val I 0
  end_mem
  mov I $sensor 13
  mov I $val 0
  lb loop
    pushp I $sensor
    abi_invoke 1        ; digitalRead(13) -> CalcResu
    push I $val
    eq I $val 1
    jmp_t led_on
    pushp I 6           ; 灭
    pushp I 0
    abi_invoke 0
    jmp loop
    lb led_on
      pushp I 6       ; 亮
      pushp I 1
      abi_invoke 0
  jmp loop
  hlt
endmain
```

流水灯（`Compiler/Demo/LEDStream.txt`，编译后 210 字节）演示了子程序延时、数组保存引脚号与循环取模：

```
fn delay_ms
  mem
    $len I 0
    $now I 0
    $start I 0
    $mid I 0
  end_mem
  ;2号ABI为系统时钟
  abi_invoke 2
  push I $start
  lb loop
    abi_invoke 2
    push I $now
    sub I $now $start
    push I $mid
    ge I $mid $len
    jmp_t finish
  jmp loop
  lb finish
    ret
endfn

main
  mem
    $delay I 0
    $count I 0
    $ele I 0
    $array I 2
  end_mem
  mov I $delay 100
  mov I $count 0
  ;三个LED分别接在18、19、20号引脚上
  init_array I $array 3 18 19 20
  lb loop_main
    ;依次点亮当前LED
    read_array I $array $count
    push I $ele
    pushp I $ele
    pushp I 1
    ;0号ABI为digitalWrite
    abi_invoke 0
    pushp I $delay
    call delay_ms
    pushp I $ele
    pushp I 0
    abi_invoke 0
    ;切换到下一个LED，越界则回到第一个
    add I $count 1
    push I $count
    gt I $count 2
    jmp_t clean
  jmp loop_main
  lb clean
    mov I $count 0
  jmp loop_main
  hlt
  endmain
```

更多样例见 `Compiler/Demo/` 目录。

---

## 重要注意事项 | Notes

1. **`flash_range_erase/program` 的第一个参数是 FLASH 偏移，不是 XIP 地址。**
   传 `0x101EF000` 这种 XIP 地址会命中 pico-sdk 的
   `hard_assert(flash_offs + count <= PICO_FLASH_SIZE_BYTES)`，固件在 `setup()` 里就停住，
   主机将会报告"**无法识别的 USB 设备**"。
   `ARSFS.cpp` 里因此把两者分开：`gFsBase`（XIP 地址，只用于读）与 `gFsOff`（偏移，用于擦写）。
2. **擦写 FLASH 期间不能在 FLASH 上取指，必须关中断。**
   所以文件系统的挂载与写入要等 USB 枚举完成（`main.ino` 里推迟到第一次 `loop()` 并先等 1.5 秒），
   否则主机在枚举途中拿不到描述符，同样表现为"无法识别"。
3. **`ARS_memset` / `ARS_memmove` 是"拷贝"语义，第二个参数是源指针**，不是 libc 的 `memset`。
   传 0 会直接返回、什么都不做——一个静默的空操作曾经让位图没被清零，
   结果 128 页全被当成"已占用"，所有写入都返回 `FS_EFULL`。
4. **`ars_i8` 是显式 `signed char`。** 某些 Arduino 核心（mbed）带 `-funsigned-char`，
   裸 `char` 会让同一份字节码在设备上跑出与宿主不同的结果；显式写出符号性后，
   移植到任何平台语义都相同。
5. **"先擦后写"的擦除粒度是扇区，不是页。** 让多个文件共用一个扇区，就等于让它们
   共享一次"擦掉再写回"的窗口——窗口里断一次电，同扇区所有人的数据一起没。
   症状是"上传一个文件，结果另外几个文件变成空的"，离病因非常远。
   这一版为了简单**直接接受了这个风险**（首次适配分配，见上文「分配策略与风险」）。
6. **mbed 核心的 `USBCDC` 输出有两个坑。**
   一是 `send()` 内部 `wait(NULL)`，宿主停止读取就把固件堵死；
   二是**写入长度正好等于端点最大包（64 字节）时，主机侧不认为传输结束**，
   整包会被 CDC 驱动扣住，直到下一包才吐出来——症状是"应答要等下一条命令才冒出来"，
   而且只在应答长度恰好是 64 的倍数时才出现（`ls` 刚好 64 字节时最容易撞上）。
   所以输出走 `send_nb` 排队、主循环里推，并且**一次最多推 63 字节**。
7. **上位机侧：一条会话里只 `ser.timeout = X` 一次。** pyserial 每次给 `timeout`
   赋值都会走一次 `SetCommState`；CDC 那边只要还有一次写没落地，这个调用就会
   卡满 5 秒然后抛 `ERROR_SEM_TIMEOUT(121)`，并且**把这个串口卡死到下一次 USB 复位**
   （之后连 `serial.Serial(...)` 都打不开）。表现为"上传到一半串口炸了、每次错得
   还不一样"。`ARSTerm.py` 用固定 `POLL_TIMEOUT` + 截止时间来避免这一点。
8. **一条命令失败之后必须把接收缓冲清干净。** 上传超时留下的那几百字节会被当成
   命令行去解析，从那一刻起每条命令都错开一位、报错看起来随机。
   设备侧出错路径会 `rxFlush()`，并且把含不可打印字节的"命令行"静默丢掉；
   上位机侧则每条命令前 `drain()`、`update` 时读到 `RDY` 为止。

---

## ARS 语言的设计要点（请留意）| Design Quirks

1. **长度 = 元素数 - 1**，声明数组时不要忘记加一。
2. **`read_array` / `set_array` 只有 3 个参数**，读写都经过 `CalcResu`。
3. **`init_array` 的"元素数量"同样是元素数 - 1**。
4. **没有单字节运算指令**，字节参与运算前先 `mov I` 读进整型。
5. **所有比较结果都在 `CalcResu`**，`jmp_t` 只看它。

这些约定换来的是极小的编译器与解释器：`Compiler.c` 是单文件纯 C，解释器在极小的内存占用下
同时实现多任务调度、带碎片合并的内存管理与一个文件系统——也因此才能跨平台随意移植。

---

## 当前限制 | Current Limitations

诚实地说清楚边界，比宣称"内核"更有价值：

- **协作式而非抢占式**：调度器在 `loop()` 中每条指令后让出 CPU，并不掌握定时器中断。
  好消息是每条字节码指令都很短，卡死整个系统的风险被限制在单条指令内。
- **单地址空间，隔离靠约定**：任务间没有 MMU/MPU 边界，也没有硬件特权级。
  内存管理器能**发现**被破坏的块（魔术字 + `Check` 守卫字）并尽量回收，但这是容错，不是保护。
- **解释器的越界检查尚未全部强制**：部分内存接口在越界时返回错误码，而调用方未检查。
  把每个偏移查询的返回值变成硬约束，是这套"语言基隔离"设计兑现承诺的关键一步——
  而且由于字节码本就不原生执行，这一步**不需要 MMU 也能做到**。
- **文件系统是无事务的**：元数据每次都整扇区重写，写入顺序刻意保证"掉电不会留坏链"，
  但没有日志/双份目录，掉电仍可能丢掉最后一次操作。
- **文件系统容量固定**：32 个目录项、128 个数据页、单文件 2048 字节，无子目录，
  文件名定长 15 字节且不分大小写之外的任何命名规则。
- **FLASH 写入会短暂关中断**：一次页写入要读-改-擦-写整个 4KB 扇区，
  这段时间（约几十毫秒）CPU 不响应中断，USB 批量传输靠 NAK 重试扛过去。
  每次 `update` 落盘要写十几次扇区，所以"上传完紧接着的几百毫秒"里串口是抖的，
  上位机别把超时卡得太紧（`ARSTerm.py` 单块给 5 秒）。
- **串口输出依赖主循环推进**：应答是在 `loop()` 里一包一包推出去的，
  所以任何长时间霸占主循环的东西（某条指令里的长忙等）都会让输出变慢。
  目前内置任务都是短指令，实测 `ver` 往返 5ms 上下。
- **32 位假设**：`ars_i32` 与内存管理器的块布局都建立在 32 位寻址上。

---

## License

MIT License

---

## 致谢 | Thanks

本项目由 [Yauhak](https://github.com/Yauhak) 原创开发，欢迎各种建议、Issue、PR 与讨论。

---

**如果你喜欢这种把虚拟机、内存管理器、文件系统和小语言全部握在自己手里，
并且希望它能跑在任何一台机器上的开发方式，
欢迎一起把这个"属于自己的微型操作系统世界"继续做下去。**
