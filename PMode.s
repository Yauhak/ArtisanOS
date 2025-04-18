.code16
.section .text
.global _stage2

_stage2:
    # 关闭中断
    cli

    # 加载GDT
    lgdt (gdt_desc)

    # 启用保护模式
    movl %cr0, %eax
    orl $0x01, %eax
    movl %eax, %cr0

    # 远跳转刷新流水线
    ljmp $0x08, $protected_mode

.code32
protected_mode:
    # 初始化段寄存器
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movl $0x7C00, %esp        # 栈指针（可调整）

    # 从磁盘加载内核到 0x100000（LBA模式）
    movl $0x100000, %edi      # 目标地址
    movl $66, %ecx            # 从第66扇区开始（第二阶段后）
    movl $256, %ebx           # 读取256个扇区（128KB内核）
    call load_kernel

    # 跳转到内核入口
    jmp *%edi

# LBA磁盘读取（保护模式）
load_kernel:
    pusha
    movl %ecx, %eax
    movl $0x1F2, %edx
    outb %al, %dx             # 扇区数
    inc %edx

    movl %ecx, %eax
    outb %al, %dx             # LBA低8位
    inc %edx
    shr $8, %eax
    outb %al, %dx             # LBA中8位
    inc %edx
    shr $8, %eax
    outb %al, %dx             # LBA高8位
    inc %edx
    or $0xE0, %al
    outb %al, %dx             # 驱动器号
    inc %edx
    mov $0x20, %al
    outb %al, %dx             # 发送读命令

.wait:
    inb %dx, %al
    test $0x08, %al
    jz .wait

    mov $256, %ecx            # 每次读256字（512字节）
    mov $0x1F0, %dx
.read:
    inw %dx, %ax
    stosw
    loop .read

    dec %ebx
    jnz load_kernel
    popa
    ret

# GDT定义
gdt:
    .quad 0x0000000000000000   # 空描述符
    .quad 0x00CF9A000000FFFF   # 代码段（基址0，界限4GB，DPL=0）
    .quad 0x00CF92000000FFFF   # 数据段（同上）
gdt_desc:
    .word . - gdt - 1          # GDT界限
    .long gdt                  # GDT基址
