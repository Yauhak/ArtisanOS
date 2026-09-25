/* ============================ 串口传输层 ============================
 * 只做两件事：把要发的字节排队推出去、把收到的字节拼成整行交出去。
 * 一个命令名字都不认识——命令全在 ARSCMD.cpp。
 *
 * 为什么不用 Serial.print：mbed 的 USBCDC::send() 内部是 write_op.wait(NULL)，
 * 宿主一旦停止排空 CDC（串口监视器暂停、终端被杀、缓冲塞满），固件就被永远
 * 卡在那一行。这里所有发送都只往 RAM 里排队，一个字节都不等。
 *
 * 为什么一次只推 63 字节：写入长度正好等于端点最大包（64 字节）时，
 * 主机侧不认为一次传输结束，整包会被 CDC 驱动扣住不交给 read()，
 * 直到下一包才一起吐出来——留 1 字节让每包都是短包即可。
 * ==================================================================== */
#ifndef ARSUART_H
	#include "ARSUART.h"
#endif

#if USE_FILE_AND_UART

#include <Arduino.h>
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
	#include <USB/PluggableUSBSerial.h>
#endif

#define CMD_MAX 64 /* 一行最长多少字节 */

#define TX_RING 8192u
#define TX_KEEP 1024u 
#define TX_PKT 63u
#define POLL_MAX_CMD 4 /* 一次 poll 最多处理几条命令 */

static char txRing[TX_RING];
static uars_i32 txHead = 0, txTail = 0;
static uars_i8 txOver = 0;

static ArsLineFn lineFn = 0;
static char cmdBuf[CMD_MAX];
static uars_i16 cmdLen = 0;

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

void arsuart_putc(char c) { txPut(&c, 1); }

void arsuart_puts(const char *s) {
	uars_i32 n = 0;
	while (s[n]) n++;
	txPut(s, n);
}

void arsuart_putu32(uars_i32 v) {
	char t[12];
	int n = 0;
	if (!v) t[n++] = '0';
	while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n--) arsuart_putc(t[n]);
}

void arsuart_putLine(const char *s) {
	arsuart_puts(s);
	arsuart_puts("\r\n");
}

void arsuart_txDrain(void) {
	if (txHead == txTail) return;
	uars_i32 avail = (txHead > txTail) ? (txHead - txTail) : (TX_RING - txTail);
	if (avail > (uars_i32)TX_PKT) avail = (uars_i32)TX_PKT;
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
	uint32_t got = 0;
	_SerialUSB.send_nb((uint8_t *)(txRing + txTail), (uint32_t)avail, &got, true);
	if (!got) return;
	txTail += (uars_i32)got;
#else
	for (uars_i32 i = 0; i < avail; i++) Serial.write((uars_i8)txRing[txTail + i]);
	txTail += avail;
#endif
	if (txTail >= TX_RING) txTail = 0;
}

int arsuart_rxAvail(void) { return Serial.available(); }

int arsuart_rxGet(void) {
	if (!Serial.available()) return -1;
	return (int)Serial.read();
}

void arsuart_rxFlush(void) {
	unsigned long t0 = millis();
	while ((millis() - t0) < 120) {
		if (Serial.available()) { (void)Serial.read(); t0 = millis(); }
	}
	cmdLen = 0;
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

void arsuart_setLineHandler(ArsLineFn fn) { lineFn = fn; }

void arsuart_poll(void) {
	arsuart_txDrain();
	int served = 0;
	while (Serial.available() && served < POLL_MAX_CMD) {
		char c = (char)Serial.read();
		if (c == '\r') continue;
		if (c == '\n') {
			cmdBuf[cmdLen] = 0;
			if (cmdLen && lineFn) lineFn(cmdBuf);
			cmdLen = 0;
			served++;
		} else if (cmdLen < CMD_MAX - 1) {
			cmdBuf[cmdLen++] = c;
		}
	}
	arsuart_txDrain();
}

void arsuart_tick(void) { arsuart_poll(); }

#else /* USE_FILE_AND_UART == 0：整套串口层编译成空壳 */

void arsuart_begin(uars_i32 baud) { (void)baud; }
void arsuart_setLineHandler(ArsLineFn fn) { (void)fn; }
void arsuart_poll(void) { }
void arsuart_tick(void) { }
void arsuart_txBegin(void) { }
void arsuart_putc(char c) { (void)c; }
void arsuart_puts(const char *s) { (void)s; }
void arsuart_putu32(uars_i32 v) { (void)v; }
void arsuart_putLine(const char *s) { (void)s; }
void arsuart_txDrain(void) { }
int arsuart_rxAvail(void) { return 0; }
int arsuart_rxGet(void) { return -1; }
void arsuart_rxFlush(void) { }

#endif
