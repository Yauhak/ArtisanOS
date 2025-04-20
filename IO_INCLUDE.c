#ifndef IO_INCLUDE
	#include "IO_INCLUDE.h"
#endif
//当前文本颜色
uint8_t text_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLACK);
//当前光标位置
uint16_t cursor_pos = 0;

//两个函数共同用于置顶窗口的显示
//在切换窗口时可以用到
void DispBuff(const char *dest, uint16_t count, uint8_t id) {
	for (uint16_t i = 0; i < count; i++) {
		ARS_pc(dest[i], id);
	}
}

void ToppingWindowById(uint8_t id) {
	DispBuff(EXE_SCBUFF[id], EXE_SC_POS[id], id);
}

volatile void *ARS_memmove(volatile void *dest, volatile const void *src, int n) {
	if (dest == (void*)0 || src == (void*)0 || n <= 0) return dest;
	volatile char *d = (volatile char *)dest;
	volatile const char *s = (volatile const char *)src;
	if (d < s) {
		for (int i = 0; i < n; i++) d[i] = s[i];
	} else {
		for (int i = n - 1; i >= 0; i--) d[i] = s[i];
	}
	return dest;
}

volatile void *ARS_memset(volatile void *dest, volatile const void *byte, int n) {
	if (dest == 0 || byte == 0 || n == 0) {
		return dest; // 处理无效参数
	}
	volatile char *d = (volatile char *)dest;
	volatile const char *s = (volatile const char *)byte;
	for (int i = 0; i < n; i++) {
		*d++ = *s;
	}
	return dest;
}


uint16_t ARS_strlen(const uint8_t *str) {
	const uint8_t *ptr = str;
	uint16_t len = 0;
	while (*ptr++ != 0)len++;
	return len;
}

uint8_t ARS_strcmp(const char *haystack, const char *needle, int len) {
	if (*needle == '\0') {
		return 1;
	}
	for (int i = 0; i < len; i++) {
		if (haystack[i] != needle[i]) {
			return 1;
		}
	}
	return 0;
}

char *ARS_strtok(char *str, const char delim) {
	char *ptr = str;
	while (*ptr != delim && ptr < str + ARS_strlen(str))ptr++;
	if (*ptr == delim) {
		*ptr = 0;
	}
	return str;
}

//调用内联汇编的一些功能，实现端口读写
static inline void ARS_outb(uint16_t port, uint8_t val) {
	asm volatile("outb %0,%1"::"a"(val), "Nd"(port));
}

static inline char ARS_inb(uint16_t port) {
	uint8_t rt;
	asm volatile("inb %1,%0":"=a"(rt):"Nd"(port));
	return rt;
}

static inline char ARS_inw(uint16_t port) {
	uint16_t rt;
	asm volatile("inw %1,%0":"=a"(rt):"Nd"(port));
	return rt;
}

//输出一个字符
//可以处理换行和退格情况
//还有光标处理
void ARS_pc(char c, uint8_t id) {
	volatile uint16_t* vram;
	//一般程序窗口
	if (id >= 0 && id < OS_MAX_TASK) {
		vram = (volatile uint16_t*)(VGA_START + VGA_WIDTH * 5);
		//标题栏
	} else if (id == 0xFF) {
		vram = (volatile uint16_t*)VGA_START;
	}
	//若为换行符
	if (c == '\n') {
		cursor_pos = (cursor_pos / VGA_WIDTH + 1) * VGA_WIDTH;
		//退格键
		//这部分其实用来处理键盘的BackSpace
	} else if (c == 8) {
		vram[cursor_pos--] = 0;
		EXE_SCBUFF[id][EXE_SC_POS[id]--] = 0;
	} else {
		vram[cursor_pos++] = (text_color << 8) | c;
	}
	//屏幕滚动处理
	//（程序窗口）
	if (cursor_pos >= VGA_HEIGHT * VGA_WIDTH && id >= 0 && id < OS_MAX_TASK) {
		ARS_memmove(vram, vram + VGA_WIDTH, (VGA_HEIGHT - 5)*VGA_WIDTH);
		cursor_pos -= VGA_WIDTH;
	}
	if (EXE_SC_POS[id] < SCREEN_BUFFSIZE) {
		//屏幕缓冲区缓存处理
		if (c != 8)EXE_SCBUFF[id][EXE_SC_POS[id]++] = c;
	} else {
		ARS_memmove(EXE_SCBUFF[id], EXE_SCBUFF[id] + 1, SCREEN_BUFFSIZE - 1);
		if (c != 8)EXE_SCBUFF[id][SCREEN_BUFFSIZE - 1] = c;
	}
	//光标位置更新
	ARS_outb(0x3D4, 0x0F);
	ARS_outb(0x3D5, cursor_pos & 0xFF);
	ARS_outb(0x3D4, 0x0E);
	ARS_outb(0x3D5, (cursor_pos >> 8) & 0xFF);
}

// PS/2键盘驱动
char ARS_gc(uint8_t id) {
	if (ARS_inb(0x64) & 0x1) {
		char chr = ARS_inb(0x60);
		//有回显地接收键盘输入
		ARS_pc(chr, id);
		return chr;
	} else return -1;
}
