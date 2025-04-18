#ifndef IO_INCLUDE
	#include "IO_INCLUDE.h"
#endif
//当前文本颜色
uint8_t text_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLACK);
//当前光标位置
uint16_t cursor_pos=0;
//屏幕数据缓冲区
uint8_t ScBuff[SCREEN_BUFFSIZE];
//屏幕缓冲区占用计数
uint16_t ScBuffCount = 0;

volatile void *ARS_memmove(volatile void *dest, volatile const void *src, int n) {
	if (dest == 0 || src == 0 || n == 0) {
		return dest; // 处理无效参数
	}
	volatile char *d = (volatile char *)dest;
	volatile const char *s = (volatile const char *)src;
	// 判断内存重叠方向：
	// - 如果目标在源的前面（dest < src），从前往后复制
	// - 如果目标在源的后面（dest > src），从后往前复制
	if (d < s) {
		// 从前往后逐字节复制
		for (int i = 0; i < n; i++) {
			d[i] = s[i];
			if(n>=ARS_strlen(s))d[i]=0;
		}
	} else {
		// 从后往前逐字节复制
		for (int i = n; i > 0; i--) {
			d[i - 1] = s[i - 1];
			if(n>=ARS_strlen(s))d[i]=0;
		}
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

char *ARS_strtok(char *str,const char delim){
	char *ptr=str;
	while(*ptr!=delim&&ptr<str+ARS_strlen(str))ptr++;
	if(*ptr==delim){
		*ptr=0;
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
void ARS_pc(char c) {
	volatile uint16_t* vram = (volatile uint16_t*)0xB8000;
	if (c == '\n') {
		cursor_pos = (cursor_pos / VGA_WIDTH + 1) * VGA_WIDTH;
		//退格键
	} else if (c == 8) {
		vram[cursor_pos--] = 0;
		ScBuff[ScBuffCount--] = 0;
	} else {
		vram[cursor_pos++] = (text_color << 8) | c;
	}
	//屏幕滚动处理
	if (cursor_pos >= VGA_HEIGHT * VGA_WIDTH) {
		ARS_memmove(vram, vram + VGA_WIDTH, (VGA_HEIGHT - 1)*VGA_WIDTH);
		cursor_pos -= VGA_WIDTH;
	}
	if (ScBuffCount < SCREEN_BUFFSIZE) {
		//屏幕缓冲区缓存处理
		ScBuff[ScBuffCount++]=c;
	} else {
		ARS_memmove(ScBuff, ScBuff + 1, SCREEN_BUFFSIZE - 1);
		ScBuff[SCREEN_BUFFSIZE - 1] = c;
	}
}

// PS/2键盘驱动
char ARS_gc() {
	if (ARS_inb(0x64) & 0x1) {
		char chr = ARS_inb(0x60);
		//有回显地接收键盘输入
		ARS_pc(chr);
		return chr;
	} else return -1;
}
