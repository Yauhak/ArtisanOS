#ifndef IDT
	#include"IDT.h"
#endif
struct idt_entry idt[256]; // IDT共256个条目
// 将PIC的IRQ0-7映射到中断号0x20-0x27
volatile uint32_t tick = 0;
volatile uint32_t id=0;//当前执行的程序的id

void remap_pic() {
	ARS_outb(0x20, 0x11);  // ICW1：初始化主PIC
	ARS_outb(0xA0, 0x11);  // ICW1：初始化从PIC
	ARS_outb(0x21, 0x20);  // ICW2：主PIC中断基址0x20
	ARS_outb(0xA1, 0x28);  // ICW2：从PIC中断基址0x28
	ARS_outb(0x21, 0x04);  // ICW3：主PIC IRQ2连接从PIC
	ARS_outb(0xA1, 0x02);
	ARS_outb(0x21, 0x01);  // ICW4：8086模式
	ARS_outb(0xA1, 0x01);
}

void sw_schedule(){
	id=(id+1)%64;//64:OS_MAX_TASK
}

void timer_handler() {
	tick++;
	ARS_outb(0x20, 0x20);  // 发送EOI到主PIC
	// 可选：任务调度或其他高级功能
	if (tick % 100 == 0) {
		sw_schedule();
	}
}

void register_irq0() {
	// 获取中断处理程序的32位地址
	uint32_t handler_addr = (uint32_t)timer_handler;
	// 设置IDT条目
	idt[0x20].offset_low = handler_addr & 0xFFFF;   // 低16位
	idt[0x20].selector = 0x08;                      // 内核代码段选择子
	idt[0x20].type_attr = 0x8E;                     // 32位中断门，DPL=0
	idt[0x20].offset_high = (handler_addr >> 16) & 0xFFFF; // 高16位
}

void load_idt() {
	struct idt_ptr idt_ptr;
	idt_ptr.limit = sizeof(idt) - 1;
	idt_ptr.base = (uint32_t)&idt;
	asm volatile ("lidt (%0)" : : "r"(&idt_ptr));
}

