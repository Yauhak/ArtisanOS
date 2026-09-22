/* 串口命令处理 + FLASH 拓扑可视化。
 * 命令：update / get / del / ls / occ [text] / ver
 */
#ifndef ARSFS_H
	#include "ARSFS.h"
#endif
#ifndef ARSUART_H
	#include "ARSUART.h"
#endif

#if USE_FILE_AND_UART

#include <Arduino.h>

#define CMD_MAX 64
#define UART_CHUNK 128

static char cmdBuf[CMD_MAX];
static uars_i16 cmdLen = 0;
static uars_i8 upBuf[FILE_MAX];   /* 上传暂存 */
static uars_i8 downBuf[FILE_MAX]; /* 下载暂存 */

/* ---- 不依赖 snprintf 的数字输出 ---- */
static void putU32(uars_i32 v) {
	char t[12];
	int n = 0;
	if (!v) t[n++] = '0';
	while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n--) Serial.write((uars_i8)t[n]);
}
static void putLine(const char *s) { Serial.print(s); Serial.print("\r\n"); }

/* ---- FLASH 拓扑：占用标红、空闲标绿 ---- */
static void printOcc(int color) {
	Serial.print("USED ");
	putU32((uars_i32)arsfs_used());
	Serial.print(" FREE ");
	putU32((uars_i32)arsfs_free());
	Serial.print(color ? " (red=used green=free)\r\n" : " (*=used .=free)\r\n");
	for (int i = 0; i < BMP; i++) {
		int u = arsfs_page_used(i);
		if (color) Serial.print(u ? "\x1b[31m" : "\x1b[32m"); /* 红占用 / 绿空闲 */
		Serial.write((uars_i8)('0' + i / 100 % 10));
		Serial.write((uars_i8)('0' + i / 10 % 10));
		Serial.write((uars_i8)('0' + i % 10));
		if (color) Serial.print("\x1b[0m");
		else Serial.write((uars_i8)(u ? '*' : '.'));
		Serial.print((i % 8 == 7) ? "\r\n" : " ");
	}
	/* FCB 一览：start 为 FREE_OR_DEL 表示该目录项空闲 */
	Serial.print("FCB:\r\n");
	for (int i = 0; i < FCB_CNT; i++) {
		int st = arsfs_fcb_start(i);
		if (color) Serial.print(st == FREE_OR_DEL ? "\x1b[32m" : "\x1b[31m");
		if (st == FREE_OR_DEL) {
			Serial.print("FREE_OR_DEL");
		} else {
			for (int k = 0; k < NAME_LEN && arsfs_fcb_name(i, k) != ' '; k++)
				Serial.write((uars_i8)arsfs_fcb_name(i, k));
			Serial.print(" start=");
			putU32((uars_i32)st);
			Serial.print(" size=");
			putU32((uars_i32)arsfs_fcb_size(i));
		}
		if (color) Serial.print("\x1b[0m");
		Serial.print("\r\n");
	}
}

/* ---- update：声明长度 -> 分块接收 -> 落盘 ---- */
static void handleUpdate(char *args) {
	char *sp = args;
	while (*sp == ' ') sp++;
	char *name = sp;
	while (*sp && *sp != ' ') sp++;
	if (!*sp) { putLine("ERR args"); return; }
	*sp++ = 0;
	while (*sp == ' ') sp++;
	long len = 0;
	while (*sp >= '0' && *sp <= '9') len = len * 10 + (*sp++ - '0');
	if (len < 0 || len > FILE_MAX) { putLine("ERR size"); return; }

	Serial.print("RDY\r\n");
	long got = 0;
	while (got < len) {
		long want = len - got;
		if (want > UART_CHUNK) want = UART_CHUNK;
		long n = 0;
		unsigned long t0 = millis();
		while (n < want && (millis() - t0) < 2000) {
			if (Serial.available()) { upBuf[got + n] = (uars_i8)Serial.read(); n++; t0 = millis(); }
		}
		if (n < want) { putLine("ERR timeout"); return; }
		got += n;
		Serial.print("ACK ");
		putU32((uars_i32)got);
		Serial.print("\r\n");
	}
	arsfs_lock();
	createFile(name); /* 不存在则先建目录项：writeFile 只负责写已有文件 */
	ars_i8 rc = writeFile(name, (uars_i8 *)upBuf, len);
	arsfs_unlock();
	putLine(rc == FS_OK ? "OK" : "ERR write");
}

/* ---- get：读文件 -> HEX 分块下发 ---- */
static void handleGet(char *args) {
	char *sp = args;
	while (*sp == ' ') sp++;
	char *name = sp;
	while (*sp && *sp != ' ') sp++;
	*sp = 0;
	arsfs_lock();
	long n = readFile(name, (uars_i8 *)downBuf, FILE_MAX);
	arsfs_unlock();
	if (n < 0) { putLine("ERR noent"); return; }
	Serial.print("DATA ");
	putU32((uars_i32)n);
	Serial.print("\r\n");
	const char *hex = "0123456789ABCDEF";
	for (long i = 0; i < n; i++) {
		uars_i8 b = downBuf[i];
		Serial.write((uars_i8)hex[(b >> 4) & 0xF]);
		Serial.write((uars_i8)hex[b & 0xF]);
		if ((i & 31) == 31) Serial.print("\r\n");
	}
	Serial.print("\r\nOK\r\n");
}

static void handleDel(char *args) {
	char *sp = args;
	while (*sp == ' ') sp++;
	char *name = sp;
	while (*sp && *sp != ' ') sp++;
	*sp = 0;
	arsfs_lock();
	ars_i8 rc = delFile(name);
	arsfs_unlock();
	putLine(rc == FS_OK ? "OK" : "ERR noent");
}

static void handleLs(void) {
	char buf[FCB_CNT * (NAME_LEN + 1)];
	arsfs_lock();
	arsfs_ls(buf, sizeof(buf));
	arsfs_unlock();
	Serial.print("LS\r\n");
	Serial.print(buf);
	Serial.print("OK\r\n");
}

/* ---- 命令分发 ---- */
static void dispatch(char *line) {
	char *sp = line;
	while (*sp == ' ') sp++;
	char *cmd = sp;
	while (*sp && *sp != ' ') sp++;
	if (*sp) *sp++ = 0;
	if (!cmd[0]) return;
	/* ARS_strcmp 返回 0 表示前 len 个字节相同 */
	int isUpdate = (ARS_strcmp(cmd, "update", 6) == 0);
	int isGet = (ARS_strcmp(cmd, "get", 3) == 0);
	int isDel = (ARS_strcmp(cmd, "del", 3) == 0);
	int isLs = (ARS_strcmp(cmd, "ls", 2) == 0);
	int isOcc = (ARS_strcmp(cmd, "occ", 3) == 0);
	int isFmt = (ARS_strcmp(cmd, "format", 6) == 0);
	int isVer = (ARS_strcmp(cmd, "ver", 3) == 0);
	if (isUpdate) handleUpdate(sp);
	else if (isGet) handleGet(sp);
	else if (isDel) handleDel(sp);
	else if (isLs) handleLs();
	else if (isVer) {
		Serial.print("ARS FS_VER=");
		putU32(FS_VER);
		Serial.print(" TASKS=");
		putU32((uars_i32)gSchedTasks);
		putLine("");
	}
	else if (isOcc) {
		/* occ 走 ANSI 颜色；occ text 输出不含任何转义码的纯文本 */
		int color = (sp[0] != 't' && sp[0] != 'T');
		arsfs_lock();
		printOcc(color);
		arsfs_unlock();
		putLine("OK");
	}
	else if (isFmt) { /* 重建文件系统：清空所有目录项与位图 */
		arsfs_lock();
		ars_i8 rc = format();
		arsfs_unlock();
		putLine(rc == FS_OK ? "OK" : "ERR format");
	}
	else putLine("ERR unknown");
}

void arsuart_begin(uars_i32 baud) {
	Serial.begin(baud);
	/* 给 USB 串口一点枚举时间；不依赖 !Serial，避免无宿主机时卡死启动 */
	unsigned long t0 = millis();
	while ((millis() - t0) < 300) { }
	cmdLen = 0;
}

void arsuart_poll(void) {
	while (Serial.available()) {
		char c = (char)Serial.read();
		if (c == '\r') continue;
		if (c == '\n') {
			cmdBuf[cmdLen] = 0;
			dispatch(cmdBuf);
			cmdLen = 0;
		} else if (cmdLen < CMD_MAX - 1) {
			cmdBuf[cmdLen++] = c;
		}
	}
}

void arsuart_tick(void) { arsuart_poll(); }

#else
void arsuart_begin(uars_i32 baud) { (void)baud; }
void arsuart_poll(void) { }
void arsuart_tick(void) { }
#endif
