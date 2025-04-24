#ifndef IO_INCLUDE
	#include "IO_INCLUDE.h"
#endif

#define IFISTOPWINDOW if(TopWindowId==id)

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

// 整数转字符串（十进制）
static void _int_to_str(int32_t num, char *buffer) {
	int i = 0;
	int is_negative = 0;

	if (num < 0) {
		is_negative = 1;
		num = -num;
	}
	// 处理0的特殊情况
	if (num == 0) {
		buffer[i++] = '0';
	} else {
		while (num > 0) {
			buffer[i++] = (num % 10) + '0';
			num /= 10;
		}
	}
	if (is_negative) {
		buffer[i++] = '-';
	}
	buffer[i] = '\0';
	// 反转字符串
	int len = i;
	for (int j = 0; j < len / 2; j++) {
		char temp = buffer[j];
		buffer[j] = buffer[len - j - 1];
		buffer[len - j - 1] = temp;
	}
}

// 浮点数转字符串（固定精度）
static void _float_to_str(float num, char *buffer, int precision) {
	int integer_part = (int)num;
	float decimal_part = num - integer_part;
	// 处理整数部分
	_int_to_str(integer_part, buffer);
	int len = ARS_strlen(buffer);
	buffer[len++] = '.';  // 添加小数点
	// 处理小数部分（按精度转换）
	for (int i = 0; i < precision; i++) {
		decimal_part *= 10;
		int digit = (int)decimal_part;
		buffer[len++] = digit + '0';
		decimal_part -= digit;
	}
	buffer[len] = '\0';
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
//可以处理窗口置顶或否的情况
//还有光标处理
// 在 IO_INCLUDE.c 中更新 ARS_pc 函数
void ARS_pc(uint8_t c, uint8_t id) {
	volatile uint16_t* vram;
	uint16_t screen_width = VGA_WIDTH;
	uint16_t screen_height = VGA_HEIGHT;
	// 根据窗口类型选择显存区域
	if (id >= 0 && id < OS_MAX_TASK) {  // 程序窗口
		vram = (volatile uint16_t*)(VGA_START + VGA_WIDTH * 5);  // 程序窗口从第5行开始
	} else if (id == 0xFF) {            // 标题栏
		vram = (volatile uint16_t*)VGA_START;                    // 标题栏占用前5行
		screen_height = 5;  // 标题栏高度固定为5行
	} else {
		return;  // 无效ID直接返回
	}
	// 处理特殊字符
	IFISTOPWINDOW//注意！！！
	//该宏用于判断窗口是否为置顶窗口
	//若非置顶窗口则将所有输出重定向到输出缓冲区中而非显存中
	switch (c) {
		case '\n':  // 换行符处理
			cursor_pos = ((cursor_pos / screen_width) + 1) * screen_width;
			break;
		case 8:     // 退格键
			if (cursor_pos > 0) {
				cursor_pos--;
				vram[cursor_pos] = (text_color << 8) | ' ';  // 用空格覆盖退格
			}
			break;
		case KEY_UP:    // 上键
			cursor_pos = (cursor_pos >= screen_width) ? cursor_pos - screen_width : 0;
			break;
		case KEY_DOWN:  // 下键
			cursor_pos = (cursor_pos + screen_width < screen_width * screen_height)
			             ? cursor_pos + screen_width
			             : cursor_pos;
			break;
		case KEY_LEFT:  // 左键
			cursor_pos = (cursor_pos > 0) ? cursor_pos - 1 : 0;
			break;
		case KEY_RIGHT: // 右键
			cursor_pos = (cursor_pos + 1 < screen_width * screen_height)
			             ? cursor_pos + 1
			             : cursor_pos;
			break;
		default:        // 普通字符
			vram[cursor_pos++] = (text_color << 8) | c;
			break;
	}
	// 屏幕滚动处理（仅程序窗口）
	IFISTOPWINDOW
	if (id >= 0 && id < OS_MAX_TASK) {
		if (cursor_pos >= screen_width * screen_height) {
			// 向上滚动一行
			ARS_memmove(vram, vram + screen_width, (screen_height - 1) * screen_width * sizeof(uint16_t));
			cursor_pos -= screen_width;
			// 清空新行
			for (int i = 0; i < screen_width; i++) {
				vram[cursor_pos + i] = (text_color << 8) | ' ';
			}
		}
	}
	// 更新屏幕缓冲区
	if (id >= 0 && id < OS_MAX_TASK) {
		if (EXE_SC_POS[id] < SCREEN_BUFFSIZE) {
			EXE_SCBUFF[id][EXE_SC_POS[id]++] = c;
		} else {
			// 缓冲区满时循环覆盖
			ARS_memmove(EXE_SCBUFF[id], &EXE_SCBUFF[id][1], SCREEN_BUFFSIZE - 1);
			EXE_SCBUFF[id][SCREEN_BUFFSIZE - 1] = c;
		}
	}
	// 更新光标位置
	ARS_outb(0x3D4, 0x0F);
	ARS_outb(0x3D5, cursor_pos & 0xFF);
	ARS_outb(0x3D4, 0x0E);
	ARS_outb(0x3D5, (cursor_pos >> 8) & 0xFF);
}

// PS/2键盘驱动
char ARS_gc(uint8_t id) {
	if (ARS_inb(0x64) & 0x1) {
		char chr = ARS_inb(0x60);
		static uint8_t is_ext = 0;
		//扩展码（用于上下左右键）
		if (chr == 0xE0) {
			is_ext = 1;
			return -1;
		}
		//如果存在扩展码
		if (is_ext) {
			is_ext = 0;
			switch (1) {
				case 0x48:
					return KEY_UP;
					break;
				case 0x50:
					return KEY_DOWN;
					break;
				case 0x4B:
					return KEY_LEFT;
					break;
				case 0x4D:
					return KEY_RIGHT;
					break;
				default:
					return NOT_EXT_KEY;
					break;
			}
		}
		//有回显地接收键盘输入
		if (chr >= ' ' && chr <= '~')
			ARS_pc(chr, id);
		return chr;
	} else return -1;
}
