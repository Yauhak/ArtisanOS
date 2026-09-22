/* 串口命令处理 + FLASH 拓扑可视化。
 * 命令：update / get / del / ls / occ [text] / ver / format / reboot <名> / reboot_all
 * 命令词不区分大小写；reboot 也可以写成 restart。
 */
#ifndef ARSFS_H
	#include "ARSFS.h"
#endif
#ifndef ARSUART_H
	#include "ARSUART.h"
#endif

#if USE_FILE_AND_UART

#include <Arduino.h>
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
	#include <USB/PluggableUSBSerial.h>
#endif

#define CMD_MAX 64
#define UART_CHUNK 128

/* ---- 非阻塞串口输出 ----*/
#define TX_RING 8192u
#define TX_KEEP 1024u 
#define TX_PKT 63u
#define POLL_MAX_CMD 4 /* 一次 poll 最多处理几条命令 */

static char txRing[TX_RING];
static uars_i32 txHead = 0, txTail = 0;
static uars_i8 txOver = 0;

static uars_i32 txBacklog(void) {
	return (txHead >= txTail) ? (txHead - txTail) : (TX_RING - txTail + txHead);
}

void arsuart_txBegin(void) {
	if (txBacklog() > TX_KEEP) txTail = txHead; /* 丢弃上一条没人要的应答 */
	txOver = 0;
}

static void txPut(const char *s, uars_i32 n) {
	if (txOver) return;
	for (uars_i32 i = 0; i < n; i++) {
		uars_i32 nx = txHead + 1;
		if (nx >= TX_RING) nx = 0;
		if (nx == txTail) { txOver = 1; return; } /* 满了：这条应答剩下的部分不要了 */
		txRing[txHead] = s[i];
		txHead = nx;
	}
}

static void txDrain(void) {
	if (txHead == txTail) return;
	uars_i32 avail = (txHead > txTail) ? (txHead - txTail) : (TX_RING - txTail);
	if (avail > (uars_i32)TX_PKT) avail = (uars_i32)TX_PKT;
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
	uint32_t got = 0;
	_SerialUSB.send_nb((uint8_t *)(txRing + txTail), (uint32_t)avail, &got, true);
	if (!got) return; /* 上一包还没被宿主取走，下一轮再来 */
	txTail += (uars_i32)got;
#else
	for (uars_i32 i = 0; i < avail; i++) Serial.write((uars_i8)txRing[txTail + i]);
	txTail += avail;
#endif
	if (txTail >= TX_RING) txTail = 0;
}

static void txStr(const char *s) {
	uars_i32 n = 0;
	while (s[n]) n++;
	txPut(s, n);
}

static void txChar(char c) { txPut(&c, 1); }

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
	while (n--) txChar(t[n]);
}
/* 一条命令失败时，把接收缓冲里剩下的东西扔掉。 */
static void rxFlush(void) {
	unsigned long t0 = millis();
	while ((millis() - t0) < 120) {
		if (Serial.available()) { (void)Serial.read(); t0 = millis(); }
	}
	cmdLen = 0;
}

static void putLine(const char *s) { txStr(s); txStr("\r\n"); }

/* ---- FLASH 拓扑：占用标红、空闲标绿 ---- */
static void printOcc(int color) {
	txStr("USED ");
	putU32((uars_i32)arsfs_used());
	txStr(" FREE ");
	putU32((uars_i32)arsfs_free());
	txStr(color ? " (red=used green=free)\r\n" : " (*=used .=free)\r\n");
	for (int i = 0; i < BMP; i++) {
		int u = arsfs_page_used(i);
		if (color) txStr(u ? "\x1b[31m" : "\x1b[32m"); /* 红占用 / 绿空闲 */
		txChar((char)('0' + i / 100 % 10));
		txChar((char)('0' + i / 10 % 10));
		txChar((char)('0' + i % 10));
		if (color) txStr("\x1b[0m");
		else txChar(u ? '*' : '.');
		txStr((i % 8 == 7) ? "\r\n" : " ");
	}
	/* FCB 一览：start 为 FREE_OR_DEL 表示该目录项空闲 */
	txStr("FCB:\r\n");
	for (int i = 0; i < FCB_CNT; i++) {
		int st = arsfs_fcb_start(i);
		if (color) txStr(st == FREE_OR_DEL ? "\x1b[32m" : "\x1b[31m");
		if (st == FREE_OR_DEL) {
			txStr("FREE_OR_DEL");
		} else {
			for (int k = 0; k < NAME_LEN && arsfs_fcb_name(i, k) != ' '; k++)
				txChar(arsfs_fcb_name(i, k));
			txStr(" start=");
			putU32((uars_i32)st);
			txStr(" size=");
			putU32((uars_i32)arsfs_fcb_size(i));
		}
		if (color) txStr("\x1b[0m");
		txStr("\r\n");
	}
}

/* ---- update：声明长度 -> 分块接收 -> 落盘 ---- */
static void handleUpdate(char *args) {
	char *sp = args;
	while (*sp == ' ') sp++;
	char *name = sp;
	while (*sp && *sp != ' ') sp++;
	if (!*sp) { putLine("ERR args"); rxFlush(); return; }
	*sp++ = 0;
	while (*sp == ' ') sp++;
	long len = 0;
	while (*sp >= '0' && *sp <= '9') len = len * 10 + (*sp++ - '0');
	if (len < 0 || len > FILE_MAX) { putLine("ERR size"); rxFlush(); return; }

	arsuart_txBegin();
	txStr("RDY\r\n");
	txDrain(); /* 主机在等 RDY，先把这一包推出去 */
	long got = 0;
	while (got < len) {
		long want = len - got;
		if (want > UART_CHUNK) want = UART_CHUNK;
		long n = 0;
		unsigned long t0 = millis();
		while (n < want && (millis() - t0) < 5000) {
			if (Serial.available()) { upBuf[got + n] = (uars_i8)Serial.read(); n++; t0 = millis(); }
			txDrain(); 
		}
		if (n < want) { putLine("ERR timeout"); rxFlush(); return; }
		got += n;
		arsuart_txBegin();
		txStr("ACK ");
		putU32((uars_i32)got);
		txStr("\r\n");
		txDrain();
	}
	arsfs_lock();
	createFile(name); /* 不存在则先建目录项：writeFile 只负责写已有文件 */
	ars_i8 rc = writeFile(name, (uars_i8 *)upBuf, len);
	arsfs_unlock();
	arsuart_txBegin();
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
	txStr("DATA ");
	putU32((uars_i32)n);
	txStr("\r\n");
	const char *hex = "0123456789ABCDEF";
	for (long i = 0; i < n; i++) {
		uars_i8 b = downBuf[i];
		txChar(hex[(b >> 4) & 0xF]);
		txChar(hex[b & 0xF]);
		if ((i & 31) == 31) txStr("\r\n");
	}
	txStr("\r\nOK\r\n");
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
	txStr("LS\r\n");
	txStr(buf);
	txStr("OK\r\n");
}

/* ---- 命令分发 ---- */
static void dispatch(char *line) {
	char *sp = line;
	while (*sp == ' ') sp++;
	char *cmd = sp;
	while (*sp && *sp != ' ') sp++;
	if (*sp) *sp++ = 0;
	if (!cmd[0]) return;
	for (char *p = cmd; *p; p++)
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) { rxFlush(); return; }
	arsuart_txBegin();
	for (char *p = cmd; *p; p++)
		if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
	/* ARS_strcmp 返回 0 表示前 len 个字节相同 */
	int isUpdate = (ARS_strcmp(cmd, "update", 6) == 0);
	int isGet = (ARS_strcmp(cmd, "get", 3) == 0);
	int isDel = (ARS_strcmp(cmd, "del", 3) == 0);
	int isLs = (ARS_strcmp(cmd, "ls", 2) == 0);
	int isOcc = (ARS_strcmp(cmd, "occ", 3) == 0);
	int isFmt = (ARS_strcmp(cmd, "format", 6) == 0);
	int isVer = (ARS_strcmp(cmd, "ver", 3) == 0);
	/* restart 是 reboot 的同义词 */
	int isReboot = (ARS_strcmp(cmd, "reboot", 6) == 0 || ARS_strcmp(cmd, "restart", 7) == 0);
	int isRebootAll = (ARS_strcmp(cmd, "reboot_all", 10) == 0 || ARS_strcmp(cmd, "restart_all", 11) == 0);
	if (isUpdate) handleUpdate(sp);
	else if (isGet) handleGet(sp);
	else if (isDel) handleDel(sp);
	else if (isLs) handleLs();
	else if (isVer) {
		txStr("ARS FS_VER=");
		putU32(FS_VER);
		txStr(" TASKS=");
		putU32((uars_i32)gSchedTasks);
		txStr("\r\n");
	}
	else if (isOcc) {
		/* occ 走 ANSI 颜色；occ text 输出不含任何转义码的纯文本 */
		int color = (sp[0] != 't' && sp[0] != 'T');
		arsfs_lock();
		printOcc(color);
		arsfs_unlock();
		putLine("OK");
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
		putLine(rc == FS_OK ? "OK" : "ERR format");
	}
	else if (isReboot) {
		/* reboot <名字>  只重启那一个任务（它必须正在调度计划里）
		 * reboot_all     重置整个堆并按 SCHEDULE 重新装载全部任务 */
		if (isRebootAll || (sp[0] == 'a' && sp[1] == 'l' && sp[2] == 'l')) {
			arssched_rebootAll();
			putLine("OK");
		} else if (!sp[0]) {
			putLine("ERR args");
		} else {
			ars_i8 rc = arssched_restart(sp);
			putLine(rc == FS_OK ? "OK" : "ERR noent");
		}
	}
	else putLine("ERR unknown");
}

void arsuart_begin(uars_i32 baud) {
	Serial.begin(baud);
	unsigned long t0 = millis();
	while ((millis() - t0) < 300) { }
	cmdLen = 0;
	txHead = 0;
	txTail = 0;
	txOver = 0;
}

void arsuart_poll(void) {
	txDrain();
	int served = 0;
	while (Serial.available() && served < POLL_MAX_CMD) {
		char c = (char)Serial.read();
		if (c == '\r') continue;
		if (c == '\n') {
			cmdBuf[cmdLen] = 0;
			dispatch(cmdBuf);
			cmdLen = 0;
			served++;
		} else if (cmdLen < CMD_MAX - 1) {
			cmdBuf[cmdLen++] = c;
		}
	}
	txDrain();
}

void arsuart_tick(void) { arsuart_poll(); }

#else
void arsuart_begin(uars_i32 baud) { (void)baud; }
void arsuart_poll(void) { }
void arsuart_tick(void) { }
void arsuart_txBegin(void) { }
#endif
