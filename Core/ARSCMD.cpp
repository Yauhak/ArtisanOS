#ifndef ARSFS_H
	#include "ARSFS.h"
#endif
#ifndef ARSSCHED_H
	#include "ARSSCHED.h"
#endif
#ifndef ARSUART_H
	#include "ARSUART.h"
#endif
#ifndef ARSCMD_H
	#include "ARSCMD.h"
#endif

#if USE_FILE_AND_UART

#include <Arduino.h>

#define UART_CHUNK 128 /* update 每块多少字节 */

static uars_i8 upBuf[FILE_MAX];   /* 上传暂存 */
static uars_i8 downBuf[FILE_MAX]; /* 下载暂存 */

void arscmd_begin(void) { }

static char *firstArg(char *args) {
	while (*args == ' ') args++;
	char *p = args;
	while (*p && *p != ' ') p++;
	*p = 0;
	return args;
}

static void printOcc(int color) {
	arsuart_puts("USED ");
	arsuart_putu32((uars_i32)arsfs_used());
	arsuart_puts(" FREE ");
	arsuart_putu32((uars_i32)arsfs_free());
	arsuart_puts(color ? " (red=used green=free)\r\n" : " (*=used .=free)\r\n");
	for (int i = 0; i < BMP; i++) {
		int u = arsfs_page_used(i);
		if (color) arsuart_puts(u ? "\x1b[31m" : "\x1b[32m"); /* 红占用 / 绿空闲 */
		arsuart_putc((char)('0' + i / 100 % 10));
		arsuart_putc((char)('0' + i / 10 % 10));
		arsuart_putc((char)('0' + i % 10));
		if (color) arsuart_puts("\x1b[0m");
		else arsuart_putc(u ? '*' : '.');
		arsuart_puts((i % 8 == 7) ? "\r\n" : " ");
	}
	/* FCB 一览：start 为 FREE_OR_DEL 表示该目录项空闲 */
	arsuart_puts("FCB:\r\n");
	for (int i = 0; i < FCB_CNT; i++) {
		int st = arsfs_fcb_start(i);
		if (color) arsuart_puts(st == FREE_OR_DEL ? "\x1b[32m" : "\x1b[31m");
		if (st == FREE_OR_DEL) {
			arsuart_puts("FREE_OR_DEL");
		} else {
			for (int k = 0; k < NAME_LEN && arsfs_fcb_name(i, k) != ' '; k++)
				arsuart_putc(arsfs_fcb_name(i, k));
			arsuart_puts(" start=");
			arsuart_putu32((uars_i32)st);
			arsuart_puts(" size=");
			arsuart_putu32((uars_i32)arsfs_fcb_size(i));
		}
		if (color) arsuart_puts("\x1b[0m");
		arsuart_puts("\r\n");
	}
}

static void handleUpdate(char *args) {
	char *sp = args;
	while (*sp == ' ') sp++;
	char *name = sp;
	while (*sp && *sp != ' ') sp++;
	if (!*sp) { arsuart_putLine("ERR args"); arsuart_rxFlush(); return; }
	*sp++ = 0;
	while (*sp == ' ') sp++;
	long len = 0;
	while (*sp >= '0' && *sp <= '9') len = len * 10 + (*sp++ - '0');
	if (len < 0 || len > FILE_MAX) { arsuart_putLine("ERR size"); arsuart_rxFlush(); return; }

	arsuart_txBegin();
	arsuart_puts("RDY\r\n");
	arsuart_txDrain();
	long got = 0;
	while (got < len) {
		long want = len - got;
		if (want > UART_CHUNK) want = UART_CHUNK;
		long n = 0;
		unsigned long t0 = millis();
		while (n < want && (millis() - t0) < 5000) {
			int ci = arsuart_rxGet();
			if (ci >= 0) { upBuf[got + n] = (uars_i8)ci; n++; t0 = millis(); }
			arsuart_txDrain(); 
		}
		if (n < want) { arsuart_putLine("ERR timeout"); arsuart_rxFlush(); return; }
		got += n;
		arsuart_txBegin();
		arsuart_puts("ACK ");
		arsuart_putu32((uars_i32)got);
		arsuart_puts("\r\n");
		arsuart_txDrain();
	}
	arsfs_lock();
	createFile(name); /* 不存在则先建目录项：writeFile 只负责写已有文件 */
	ars_i8 rc = writeFile(name, (uars_i8 *)upBuf, len);
	arsfs_unlock();
	arsuart_txBegin();
	arsuart_putLine(rc == FS_OK ? "OK" : "ERR write");
}

static void handleGet(char *args) {
	char *name = firstArg(args);
	arsfs_lock();
	long n = readFile(name, (uars_i8 *)downBuf, FILE_MAX);
	arsfs_unlock();
	if (n < 0) { arsuart_putLine("ERR noent"); return; }
	arsuart_puts("DATA ");
	arsuart_putu32((uars_i32)n);
	arsuart_puts("\r\n");
	const char *hex = "0123456789ABCDEF";
	for (long i = 0; i < n; i++) {
		uars_i8 b = downBuf[i];
		arsuart_putc(hex[(b >> 4) & 0xF]);
		arsuart_putc(hex[b & 0xF]);
		if ((i & 31) == 31) arsuart_puts("\r\n");
	}
	arsuart_puts("\r\nOK\r\n");
}

static void handleDel(char *args) {
	char *name = firstArg(args);
	arsfs_lock();
	ars_i8 rc = delFile(name);
	arsfs_unlock();
	arsuart_putLine(rc == FS_OK ? "OK" : "ERR noent");
}

static void handleLs(void) {
	char buf[FCB_CNT * (NAME_LEN + 1)];
	arsfs_lock();
	arsfs_ls(buf, sizeof(buf));
	arsfs_unlock();
	arsuart_puts("LS\r\n");
	arsuart_puts(buf);
	arsuart_puts("OK\r\n");
}

static void handleTasks(void) {
	arsuart_puts("TASKS ");
	arsuart_putu32((uars_i32)arssched_count());
	arsuart_puts("\r\n");
	for (int t = 0; t < OS_MAX_TASK; t++) {
		if (!arssched_alive(t)) continue;
		arsuart_putu32((uars_i32)t);
		arsuart_putc(' ');
		arsuart_puts(arssched_name(t));
		arsuart_puts("\r\n");
	}
	arsuart_puts("OK\r\n");
}

static void handleStart(char *args) {
	char *name = firstArg(args);
	if (!name[0]) { arsuart_putLine("ERR args"); return; }
	ars_i8 rc = arssched_start(name);
	if (rc == FS_OK) arsuart_putLine("OK");
	else if (rc == FS_ENOENT) arsuart_putLine("ERR noent");
	else if (rc == FS_EBUSY) arsuart_putLine("ERR busy");
	else arsuart_putLine("ERR full");
}

static void handleKill(char *args) {
	char *name = firstArg(args);
	if (!name[0]) { arsuart_putLine("ERR args"); return; }
	arsuart_putLine(arssched_kill(name) == FS_OK ? "OK" : "ERR noent");
}

static void handleKillId(char *args) {
	char *sp = args;
	while (*sp == ' ') sp++;
	if (*sp < '0' || *sp > '9') { arsuart_putLine("ERR args"); return; }
	int id = 0;
	while (*sp >= '0' && *sp <= '9') id = id * 10 + (*sp++ - '0');
	if (id < 0 || id >= OS_MAX_TASK) { arsuart_putLine("ERR noent"); return; }
	arsuart_putLine(arssched_killId((uars_i8)id) == FS_OK ? "OK" : "ERR noent");
}


void arscmd_line(char *line) {
	for (char *p = line; *p; p++)
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) { arsuart_rxFlush(); return; }

	char *sp = line;
	while (*sp == ' ') sp++;
	char *cmd = sp;
	while (*sp && *sp != ' ') sp++;
	if (*sp) *sp++ = 0;
	if (!cmd[0]) return;
	arsuart_txBegin();
	for (char *p = cmd; *p; p++)
		if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
	int isUpdate = (ARS_strcmp(cmd, "update", 6) == 0);
	int isGet = (ARS_strcmp(cmd, "get", 3) == 0);
	int isDel = (ARS_strcmp(cmd, "del", 3) == 0);
	int isLs = (ARS_strcmp(cmd, "ls", 2) == 0);
	int isOcc = (ARS_strcmp(cmd, "occ", 3) == 0);
	int isFmt = (ARS_strcmp(cmd, "format", 6) == 0);
	int isVer = (ARS_strcmp(cmd, "ver", 3) == 0);
	int isTasks = (ARS_strcmp(cmd, "tasks", 5) == 0);
	int isStart = (ARS_strcmp(cmd, "start", 5) == 0);
	int isKillId = (ARS_strcmp(cmd, "killid", 6) == 0);
	int isKill = (ARS_strcmp(cmd, "kill", 4) == 0);
	int isReboot = (ARS_strcmp(cmd, "reboot", 6) == 0 || ARS_strcmp(cmd, "restart", 7) == 0);
	int isRebootAll = (ARS_strcmp(cmd, "reboot_all", 10) == 0 || ARS_strcmp(cmd, "restart_all", 11) == 0);
	if (isUpdate) handleUpdate(sp);
	else if (isGet) handleGet(sp);
	else if (isDel) handleDel(sp);
	else if (isLs) handleLs();
	else if (isTasks) handleTasks();
	else if (isKillId) handleKillId(sp);
	else if (isKill) handleKill(sp);
	else if (isStart) handleStart(sp);
	else if (isVer) {
		arsuart_puts("ARS FS_VER=");
		arsuart_putu32(FS_VER);
		arsuart_puts(" TASKS=");
		arsuart_putu32((uars_i32)arssched_count()); 
		arsuart_puts("\r\n");
	}
	else if (isOcc) {
		int color = (sp[0] != 't' && sp[0] != 'T');
		arsfs_lock();
		printOcc(color);
		arsfs_unlock();
		arsuart_putLine("OK");
	}
	else if (isFmt) {
		/* 恢复出厂设置：清空 -> 铺回出厂示例 -> 立刻按新的 SCHEDULE 重启任务 */
		arsfs_lock();
		ars_i8 rc = format();
		arsfs_unlock();
		if (rc == FS_OK) {
			ARS_provision();
			arssched_rebootAll();
		}
		arsuart_putLine(rc == FS_OK ? "OK" : "ERR format");
	}
	else if (isReboot) {
		/* reboot <名字>  只重启那一个任务（它必须正跑着）
		 * reboot_all     重置整个堆并按 SCHEDULE 重新装载全部任务 */
		if (isRebootAll || (sp[0] == 'a' && sp[1] == 'l' && sp[2] == 'l')) {
			arssched_rebootAll();
			arsuart_putLine("OK");
		} else if (!sp[0]) {
			arsuart_putLine("ERR args");
		} else {
			ars_i8 rc = arssched_restart(firstArg(sp));
			arsuart_putLine(rc == FS_OK ? "OK" : "ERR noent");
		}
	}
	else arsuart_putLine("ERR unknown");
}

#else /* USE_FILE_AND_UART == 0 */

void arscmd_begin(void) { }
void arscmd_line(char *line) { (void)line; }

#endif
