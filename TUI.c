#include "TUI.h"
/*
	标题栏
	============...=
	|           ...|
	| ..ArtisanOS..|
	|           ...|
	============...=
*/
void draw_title(char *str, uint8_t lenStr) {
	for (int i = 0; i < 80; i++) {
		ARS_pc('=',0xFF);
	}
	ARS_pc('\n',0xFF);
	ARS_pc('|',0xFF);
	for (int i = 0; i < 78; i++) {
		ARS_pc(' ',0xFF);
	}
	ARS_pc('|',0xFF);
	ARS_pc('\n',0xFF);
	ARS_pc('|',0xFF);
	for (int i = 0; i < (78 - lenStr) / 2; i++) {
		ARS_pc(' ',0xFF);
	}
	for (int i = 0; i < lenStr; i++) {
		ARS_pc(*str++,0xFF);
	}
	for (int i = 0; i < (78 - lenStr) / 2; i++) {
		ARS_pc(' ',0xFF);
	}
	ARS_pc('|',0xFF);
	ARS_pc('\n',0xFF);
	ARS_pc('|',0xFF);
	for (int i = 0; i < 78; i++) {
		ARS_pc(' ',0xFF);
	}
	ARS_pc('|',0xFF);
	ARS_pc('\n',0xFF);
	for (int i = 0; i < 80; i++) {
		ARS_pc('=',0xFF);
	}
	ARS_pc('\n',0xFF);
}

//选中效果
//选中的行会由黑底白字变成白底黑字
//大概这个效果：|  [][][][][]...[][]  |，[]表示反色部分
void mark_line(uint8_t line) {
	volatile uint16_t* vram = (volatile uint16_t*)0xB8000;
	//跳过标题栏和该行的前三个字符
	//为了美观，前三个字符表示边界
	vram += (4 + line) * VGA_WIDTH + 3;
	uint8_t color = MAKE_COLOR(COLOR_BLACK, COLOR_WHITE);
	for (int i = 0; i < VGA_WIDTH - 6; i+=2) {
		//高八位表颜色，低八位表文字
		*vram = color << 8 | (*vram & 0xFF);
	}
}
