//Glue.h是一个用户可以手动扩展的接口
//只需完成胶水代码就可以调用原生ABI
//在计算密集部分，使用原生ABI将会是非常高效的做法
//甚至可以将中断处理移入这个部分，内核部分极快的任务时间片轮转可以有效防止中断响应时间过长的情况
//该说不说很多设计都是我当初的无心之举，后来才发现还有隐藏优势和用途（笑
//
//注意：本文件不仅有声明，还有 ABIs 表和胶水函数的定义，
//所以同一个程序里只允许一个编译单元包含它（当前是 INTERPRETER.cpp）。
//若确实需要被多处包含，把这些定义改成 inline（ABIs 用函数内静态变量）即可。

#ifndef IO_INCLUDE
#include "IO_INCLUDE.h"
#endif

#include <Arduino.h>

#define ABI_QUANTITY 7

typedef void (*ABI)(uars_i8 *, uars_i16);
extern ars_i32 CalcResu[OS_MAX_TASK];  //用于存放调用完外部原生函数后的结果（如果有需要的话）
/* 文件 ABI 需要定位字节码的变量地址，由 Memory.cpp 提供 */
extern volatile uars_i8 *CurPhyMem[OS_MAX_TASK];

//ABI调用号
enum ABI_CALL {
	GPIOWRITE = 0,
	GPIOREAD,
	ARSTIMER,
	/* 以下仅当 USE_FILE_AND_UART=1 时可用 */
	FILE_OPEN,
	FILE_READ,
	FILE_WRITE,
	FILE_CLOSE
};

void gWrite(uars_i8 *, uars_i16);
void gRead(uars_i8 *, uars_i16);
void Timer(uars_i8 *, uars_i16);
void gFileOpen(uars_i8 *, uars_i16);
void gFileRead(uars_i8 *, uars_i16);
void gFileWrite(uars_i8 *, uars_i16);
void gFileClose(uars_i8 *, uars_i16);

ABI ABIs[ABI_QUANTITY] = {
	//指向胶水函数
	//使得ArtisanOS可以通过ABIs这个统一接口来调用外部原生函数
	[GPIOWRITE] = gWrite,
	[GPIOREAD] = gRead,
	[ARSTIMER] = Timer,
	[FILE_OPEN] = gFileOpen,
	[FILE_READ] = gFileRead,
	[FILE_WRITE] = gFileWrite,
	[FILE_CLOSE] = gFileClose
};

void gWrite(uars_i8 *SerializeParamStack, uars_i16 taskId) {
	int pin = *(int *)SerializeParamStack;
	int val = *(int *)(SerializeParamStack + 4);
	pinMode((uars_i8)pin, OUTPUT);
	digitalWrite((uars_i8)pin, (uars_i8)val);
}

void gRead(uars_i8 *SerializeParamStack, uars_i16 taskId) {
	int pin = *(int *)(SerializeParamStack);
	pinMode((uars_i8)pin, INPUT);
	ars_i32 read = digitalRead(pin);
	ARS_memset(&CalcResu[taskId], &read, 4);
}

void Timer(uars_i8 *SerializeParamStack, uars_i16 taskId) {
	uars_i32 time = millis();
	ARS_memset(&CalcResu[taskId], &time, 4);
}

/* ---------------- 文件操作的 ABI 包装 ----------------
 * 字节码不原生执行，文件访问必须经此受检通道，并由互斥量保护。
 * 会话是静态的（一次一个任务在用文件），省 RAM 也省得管理句柄表。
 *
 * 调用约定（先用 pushp 压参）：
 *   abi_invoke 3  压入 15 字节文件名 -> 打开(不存在则创建)，返回文件长度
 *   abi_invoke 4  压入 变量地址、长度 -> 从当前游标读入该变量，返回实读字节数
 *   abi_invoke 5  压入 变量地址、长度 -> 把该变量内容写到当前游标，返回实写字节数
 *   abi_invoke 6  无参 -> 关闭会话
 */
#if USE_FILE_AND_UART
	/* TranslatePhyAddr 由 Memory.cpp 提供：虚拟偏移 -> 物理地址。
	 * 这里用 FindPhyMemOffByID 把 CurPhyMem 定位好后自行取址。 */

static char     fsSessName[NAME_LEN + 1];
static uars_i8  fsSessBuf[FILE_MAX];
static long     fsSessLen = 0;
static long     fsSessOff = 0;
static uars_i8  fsSessOpen = 0;

static void fsNameFromStack(uars_i8 *sp, char *out) {
	for (int i = 0; i < NAME_LEN; i++) out[i] = (char)sp[i];
	out[NAME_LEN] = 0;
	for (int i = NAME_LEN - 1; i >= 0 && out[i] == ' '; i--) out[i] = 0;
}

void gFileOpen(uars_i8 *sp, uars_i16 taskId) {
	fsNameFromStack(sp, fsSessName);
	arsfs_lock();
	ars_i8 rc = createFile(fsSessName);
	long n = 0;
	if (rc == FS_OK) {
		n = readFile(fsSessName, fsSessBuf, FILE_MAX);
		if (n < 0) { rc = (ars_i8)n; n = 0; }
	}
	arsfs_unlock();
	fsSessLen = n;
	fsSessOff = 0;                       /* 游标从头开始，便于顺序读 */
	fsSessOpen = (rc == FS_OK);
	CalcResu[taskId] = (rc == FS_OK) ? (ars_i32)n : (ars_i32)rc;
}

void gFileRead(uars_i8 *sp, uars_i16 taskId) {
	ars_i32 varOff = *(ars_i32 *)(sp);       /* 变量的虚拟偏移 */
	ars_i32 want = *(ars_i32 *)(sp + 4);
	CalcResu[taskId] = 0;
	if (!fsSessOpen || want <= 0) return;
	long left = fsSessLen - fsSessOff;
	if (left <= 0) return;
	if (want > left) want = left;
	if (FindPhyMemOffByID((uars_i8)taskId, (uars_i32)varOff) == 0) {
		ARS_memset((void *)CurPhyMem[taskId], fsSessBuf + fsSessOff, (uars_i32)want);
		fsSessOff += want;
		CalcResu[taskId] = want;
	}
}

void gFileWrite(uars_i8 *sp, uars_i16 taskId) {
	ars_i32 varOff = *(ars_i32 *)(sp);
	ars_i32 len = *(ars_i32 *)(sp + 4);
	CalcResu[taskId] = 0;
	if (!fsSessOpen || len <= 0) return;
	if (fsSessOff + len > FILE_MAX) len = FILE_MAX - (ars_i32)fsSessOff;
	if (len <= 0) return;
	if (FindPhyMemOffByID((uars_i8)taskId, (uars_i32)varOff) == 0) {
		ARS_memset(fsSessBuf + fsSessOff, (void *)CurPhyMem[taskId], (uars_i32)len);
		fsSessOff += len;
		if (fsSessOff > fsSessLen) fsSessLen = fsSessOff;
		arsfs_lock();
		ars_i8 rc = writeFile(fsSessName, (uars_i8 *)fsSessBuf, fsSessLen);
		arsfs_unlock();
		CalcResu[taskId] = (rc == FS_OK) ? len : (ars_i32)rc;
	}
}

void gFileClose(uars_i8 *sp, uars_i16 taskId) {
	(void)sp;
	fsSessOpen = 0;
	fsSessOff = 0;
	fsSessLen = 0;
	CalcResu[taskId] = 0;
}
#else
void gFileOpen(uars_i8 *sp, uars_i16 taskId) { (void)sp; CalcResu[taskId] = FS_EIO; }
void gFileRead(uars_i8 *sp, uars_i16 taskId) { (void)sp; CalcResu[taskId] = FS_EIO; }
void gFileWrite(uars_i8 *sp, uars_i16 taskId) { (void)sp; CalcResu[taskId] = FS_EIO; }
void gFileClose(uars_i8 *sp, uars_i16 taskId) { (void)sp; CalcResu[taskId] = 0; }
#endif
