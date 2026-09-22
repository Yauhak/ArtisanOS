/* 文件驱动的调度器：
 *   恒定读取 FCB[0]（名称 SCHEDULE）的内容，每行一个文件名（15 字节空格补齐），
 *   按行把它们加载到不同任务槽并轮转执行。
 */
#ifndef ARSFS_H
	#include "ARSFS.h"
#endif
#ifndef ARSSCHED_H
	#include "ARSSCHED.h"
#endif
#include "Memory.h"      /* OS_EXE_LOAD_START 等内存布局宏 */
#include "INTERPRETER.h" /* call / interprete */

#if USE_FILE_AND_UART

uars_i8 gSchedTasks = 0;   /* 最近一次装载成功的任务数 */

extern ars_i32 CalcResu[OS_MAX_TASK];
extern int needJump[OS_MAX_TASK];
extern volatile uars_i8 *CurCmd[OS_MAX_TASK];

#define SCHED_NAME "SCHEDULE"
#define SCHED_BUFSZ (FILE_MAX)

/* 每条指令携带的参数个数，顺序与 Opcode 枚举严格一致 */
static const char schedParamQ[] = { 2, 2, 2, 2, 1,
                                    1, 2, 2, 2, 2,
                                    2, 2, 2, 2, 2,
                                    2, 1, 1, 1, 0,
                                    3, 2, 1, 2, 2,
                                    1, 1, 1, 2, 2,
                                    0 };

/* 把一行（最多 15 字节，空格补齐）拷成 C 字符串，去掉尾部空白 */
static void trimName(const uars_i8 *src, char *dst) {
	int n = 0;
	for (int i = 0; i < NAME_LEN && src[i] && src[i] != '\r' && src[i] != '\n'; i++) dst[n++] = (char)src[i];
	while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) n--;
	dst[n] = 0;
}

/* 确保 FCB[0] 是名为 SCHEDULE 的文件；不存在则创建并搬进 0 号目录项。
 * 0 号位被别的文件占用时退化为按名字查找——按名字读同样能工作。 */
static ars_i8 loadScheduleFile(void) {
	if (arsfs_find(SCHED_NAME) == 0) return FS_OK;      /* 已在 0 号位 */
	ars_i8 rc = createFileAt(SCHED_NAME, 0);            /* 创建或搬运到 0 号位 */
	if (rc == FS_OK) return FS_OK;
	if (arsfs_find(SCHED_NAME) >= 0) return FS_OK;      /* 位子被占，但表还在 */
	return createFile(SCHED_NAME);
}

ars_i8 arssched_load(void) {
	static uars_i8 sched[SCHED_BUFSZ];
	static uars_i8 prog[FILE_MAX];

	/* 调度要求 SCHEDULE 恒为 FCB[0] */
	if (loadScheduleFile() != FS_OK) return 0;

	long n = readFile(SCHED_NAME, sched, SCHED_BUFSZ);
	if (n <= 0) return 0;

	uars_i8 task = 0;
	long i = 0;
	while (i < n && task < OS_MAX_TASK) {
		char name[NAME_LEN + 1];
		uars_i8 line[NAME_LEN];
		int k = 0;
		while (i < n && sched[i] != '\n' && sched[i] != '\r' && k < NAME_LEN) line[k++] = sched[i++];
		while (i < n && sched[i] != '\n') i++;   /* 丢弃超长部分 */
		if (i < n) i++;                          /* 跳过换行 */
		if (k == 0) continue;
		for (int j = k; j < NAME_LEN; j++) line[j] = ' ';
		trimName(line, name);
		if (!name[0]) continue;

		long got = readFile(name, prog, FILE_MAX);
		if (got <= 0) continue;                  /* 找不到或缺内容则跳过 */
		if (got > OS_MAX_SGL_PG) continue;       /* 超出任务代码页 */

		ARS_memset((void *)OS_EXE_LOAD_START(task), prog, (uars_i32)got);
		call(0, (ars_i32 *)OS_EXE_LOAD_START(task), task);
		needJump[task] = 0;
		task++;
	}
	gSchedTasks = task;
	return (ars_i8)task;
}

void arssched_loop(void) {
	static uars_i8 tid = 0;
	if (CurCmd[tid]) {
		uars_i8 ins = *CurCmd[tid]++;
		uars_i8 op = ins >> 3;
		ars_i32 params[3] = { 0, 0, 0 };
		uars_i8 argc = (uars_i8)schedParamQ[op];
		if (argc) ARS_memset(params, (const void *)CurCmd[tid], sizeof(ars_i32) * argc);
		interprete(ins, params, tid);
		if (!needJump[tid] || (op == JMP_T && !CalcResu[tid]))
			CurCmd[tid] += sizeof(ars_i32) * argc;
		needJump[tid] = 0;
	}
	tid = (tid + 1) % OS_MAX_TASK;
}

#else
uars_i8 gSchedTasks = 0;
ars_i8 arssched_load(void) { return 0; }
void arssched_loop(void) { }
#endif
