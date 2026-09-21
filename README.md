# ArtisanOS

一个**语言导向**的极简操作系统：自研字节码虚拟机既是内核的核心组件，也是唯一的不受信代码执行边界。
配合带碎片合并的动态内存管理（**堆仿栈**）、协作式多任务调度，以及自创的 "ARS" 伪汇编语言与配套编译器。

A **language-oriented** minimal operating system: a hand-written bytecode VM that serves as
both the kernel's core component and the sole boundary for untrusted code — plus a
fragment-merging allocator (a **heap that behaves like a stack**), cooperative multi-tasking,
and the original "ARS" pseudo-assembly language with its own compiler.

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
- 字节码被装载进每个任务独立的代码页
- 解释器以轮转方式执行最多 8 个任务
- 每个任务的运行内存由一套"魔术字块头 + 空闲链表 + 前后合并"的分配器管理
- 一切外设访问都经过 ABI 胶水层查表，字节码自己碰不到硬件

ArtisanOS turns any 32-bit machine into a multi-tasking host for its own bytecode.
Compile ARS → load bytecode → run up to 8 tasks round-robin with a coalescing allocator.

**它当前被部署在 RP2040 上，但这不是它的身份。** RP2040 只是众多可能的后端之一。

**It currently runs on the RP2040 — but that is a deployment choice, not its identity.**

---

## 为什么它跨平台 | Why It Travels Well

### 1. 平台适配面只有两处

| 组件 | 是否需要改动 |
|---|---|
| `Compiler.c/.h` | ❌ 纯 C，无平台依赖，桌面端交叉编译即可 |
| `Core/main/IO_INCLUDE.cpp/.h` | ❌ 只用自实现的 `ARS_memmove` / `ARS_memset`，不依赖 libc 以外的任何东西 |
| `Core/main/Memory.cpp/.h` | ❌ 纯 C。仅操作字节缓冲区 |
| `Core/main/INTERPRETER.cpp/.h` | ❌ 纯 C。只做指令译码与内存访问 |
| `Core/main/ByteCode.h` | ❌ 就是一个 `const unsigned char[]` |
| `Core/main/Glue.h` | ✅ **唯一**包含 `<Arduino.h>` 的地方（`pinMode`/`digitalWrite`/`digitalRead`/`millis`） |
| `Core/main/main.ino` | ✅ 平台入口：装载字节码、提供调度节拍 |

**改写量：两个文件。** 其余是原样编译。

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

换句话说：**它是一个用户态可运行的操作系统层**。这意味着调试时不需要真机——
在 PC 上就能单步、能打印内存状态、能在 gdb 里下断点。开发期的这一条比什么都值钱。

### 4. 移植清单

| 目标 | 要做的事 |
|---|---|
| 其它 Arduino 核心（ESP32 / STM32 / AVR） | 改 `Glue.h` 里的引脚 API 映射，`main.ino` 基本不动 |
| 裸机 MCU | 提供 `millis()`（或任意递增计数器）、GPIO 读写，自己写 `main()` |
| Linux / Windows / macOS 进程 | 提供 4 个桩函数即可，用于开发与回归测试 |
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

- **内核 = 解释器 + 内存管理器 + 调度器**，三者加起来体积很小，全部可审计
- **用户态 = 字节码**，它没有指针、没有原生执行权，唯一的对外通道是 ABI 表
- **边界由语言运行时定义**，而不是由 MMU 定义——这正是语言导向 OS 的出发点
  （Inferno 的 Dis、Singularity 的 SIP 都在同一条脉络上）

这条路线带来的好处正好契合嵌入式：

- 不需要 MMU 就能定义"进程"的概念
- 应用不需要针对每个平台重新编译，字节码跨架构通用
- 内核很小、行为可复现，适合资源紧张且要求确定性的设备

需要诚实说明的是：**"隔离"目前是设计意图，尚未完全强制**。见文末「当前限制」。

---

## 主要特性 | Key Features

- **自研字节码虚拟机** — 31 条指令、类型化操作数（B/I/F）、专用结果寄存器 `CalcResu`
- **堆仿栈的动态内存管理** — `SPLT`/`FREE` 魔术字块头 + `Check` 守卫字 + 空闲链表 + 碎片合并
- **协作式多任务** — 8 个任务槽、轮转调度、每任务独立代码页与内存层级
- **ABI 胶水层** — 字节码不原生执行，一切硬件访问都经 `abi_invoke` 查表
- **字节码跨架构通用** — 一份 `.ars_bin` 喂给所有 32 位小端平台
- **可作为宿主进程运行** — 平台层仅需 4 个符号，便于开发、调试与回归测试
- **ARS 伪汇编** — 一门为这台 VM 量身定做、语法相当"有主见"的小语言

---

## 目录结构 | Project Structure

```
ArtisanOS/
├── Core/
│   └── main/
│       ├── main.ino          # 【平台相关】入口：装载任务 + 指令调度器
│       ├── INTERPRETER.cpp/.h# 指令实现与 opcode 分发表（平台无关）
│       ├── Memory.cpp/.h     # 运行内存管理：分配/释放/合并（平台无关）
│       ├── IO_INCLUDE.cpp/.h # 基础类型、Opcode 枚举、内存工具函数（平台无关）
│       ├── Glue.h            # 【平台相关】ABI 胶水层，用户扩展点
│       └── ByteCode.h        # 内置程序的字节码数组（平台无关）
├── Compiler/
│   ├── Compiler.c/.h         # ARS 编译器（纯 C，平台无关）
│   ├── LEDFlash.txt          # 示例：光敏电阻控制 LED
│   ├── LEDStream.txt         # 示例：流水灯
│   ├── recursion.txt         # 示例：递归 / 子程序调用
│   ├── array.txt             # 示例：数组读写
│   └── *.ars_bin             # 编译产物
├── ARSIDE.html               # 单文件网页 IDE（同一套编译逻辑的 JS 版 + 语法高亮）
└── README.md
```

---

## 构建与运行 | Build & Run

### 1. 编译 ARS 编译器 | Build the compiler

```bash
gcc Compiler/Compiler.c -o arscc
```

### 2. 编译 ARS 程序 | Compile an ARS program

```bash
./arscc Compiler/LEDStream.txt
# -> Compiler/LEDStream.ars_bin，并打印字节码 dump
```

也可以用 `ARSIDE.html`：浏览器直接打开，粘贴/拖入源码，点「编译」导出 `output.ars_bin`，
控制台会给出 `main` 头部地址、各标签地址与字节码 dump。两个编译器的输出经过校验是
**逐字节一致**的。

### 3. 运行 | Run

**嵌入式**：把字节码数组贴进 `Core/main/ByteCode.h`，用 Arduino IDE 上传到 Pico，上电即运行。

`main.ino` 默认装载两个任务：

| 任务 | 程序 | 行为 |
|---|---|---|
| 0 | `LED_Flash` | 读 GPIO13（光敏电阻模块 DO），为 1 时点亮 GPIO6 的 LED，否则熄灭 |
| 1 | `LED_Stream` | GPIO18/19/20 三个 LED 轮流点亮，形成流水灯 |

**宿主机（推荐用于开发）**：把 4 个平台符号做成桩函数，`INTERPRETER.cpp` 与 `Memory.cpp`
原样编译，即可在 PC 上单步调试、打印堆状态、做回归测试。项目就是以这种方式完成功能验证的。

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

调用前用 `pushp` 依次压入参数（4 字节对齐），调用后参数栈自动清空。

```
pushp I 6        ; 引脚
pushp I 1        ; 电平
abi_invoke 0     ; digitalWrite(6, 1)
```

`Glue.h` 是留给使用者的扩展点，也是移植时唯一必须改的文件之一：
加一个函数、在 `ABI_CALL` 里加一个枚举、在 `ABIs[]` 里登记即可。

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

光敏电阻控制 LED（`Compiler/LEDFlash.txt`）：

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

流水灯（`Compiler/LEDStream.txt`）演示了子程序延时、数组保存引脚号与循环取模：

```
	pushp I $delay
	call delay_ms
```

更多样例见 `Compiler/` 目录下的 `*.txt`。

---

## ARS 语言的设计要点（请留意）| Design Quirks

1. **长度 = 元素数 - 1**，声明数组时不要忘记加一。
2. **`read_array` / `set_array` 只有 3 个参数**，读写都经过 `CalcResu`。
3. **`init_array` 的"元素数量"同样是元素数 - 1**。
4. **没有单字节运算指令**，字节参与运算前先 `mov I` 读进整型。
5. **所有比较结果都在 `CalcResu`**，`jmp_t` 只看它。

这些约定换来的是极小的编译器与解释器：`Compiler.c` 是单文件纯 C，解释器在极小的内存占用下
同时实现多任务调度与带碎片合并的内存管理——也因此才能跨平台随意移植。

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
- **32 位假设**：`ars_i32` 与内存管理器的块布局都建立在 32 位寻址上。
- **无文件系统**：内置字节码以数组形式随固件烧录；`.ars_bin` 需要自行接入加载通道。

---

## License

MIT License

---

## 致谢 | Thanks

本项目由 [Yauhak](https://github.com/Yauhak) 原创开发，欢迎各种建议、Issue、PR 与讨论。

---

**如果你喜欢这种把虚拟机、内存管理器和小语言全部握在自己手里，
并且希望它能跑在任何一台机器上的开发方式，
欢迎一起把这个"属于自己的微型操作系统世界"继续做下去。**
