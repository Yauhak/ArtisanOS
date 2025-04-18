.code16                     # 16位实模式
.section .text
.global _start

_start:
    # 初始化段寄存器
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp        # 栈指针指向MBR下方

    # 启用A20地址线（绕过BIOS兼容性问题）
    call enable_a20

    # 加载第二阶段加载器（从磁盘第2扇区开始）
    movw $0x1000, %ax        # 目标地址 ES:BX = 0x1000:0x0000
    movw %ax, %es
    xorw %bx, %bx
    movb $0x02, %ah          # 读磁盘功能号
    movb $64, %al            # 读取扇区数（根据内核大小调整）
    movw $0x0002, %cx        # 柱面0, 扇区2
    movb $0x00, %dh          # 磁头0
    int $0x13                # 调用BIOS磁盘中断
    jc disk_error            # 失败则跳转

    # 跳转到第二阶段加载器（保护模式）
    ljmp $0x1000, $0x0000

# 启用A20地址线（通过键盘控制器）
enable_a20:
    cli
    call a20_wait
    movb $0xAD, %al
    outb %al, $0x64
    call a20_wait
    movb $0xD0, %al
    outb %al, $0x64
    call a20_wait2
    inb $0x60, %al
    pushw %ax
    call a20_wait
    movb $0xD1, %al
    outb %al, $0x64
    call a20_wait
    popw %ax
    orb $0x02, %al
    outb %al, $0x60
    call a20_wait
    movb $0xAE, %al
    outb %al, $0x64
    sti
    ret

a20_wait:
    inb $0x64, %al
    testb $0x02, %al
    jnz a20_wait
    ret

a20_wait2:
    inb $0x64, %al
    testb $0x01, %al
    jz a20_wait2
    ret

# 磁盘错误处理
disk_error:
    movw $error_msg, %si
    call print_string
    hlt

# 打印字符串（实模式）
print_string:
    lodsb
    orb %al, %al
    jz done
    movb $0x0E, %ah
    int $0x10
    jmp print_string
done:
    ret

error_msg: .asciz "Disk Error!"
.fill 510-(.-_start), 1, 0   # 填充至510字节
.word 0xAA55                 # MBR签名
