#ifndef ARSSCHED_H
#define ARSSCHED_H

#include "IO_INCLUDE.h"

/* 基于文件的调度：
 *   FCB[0] 固定为 SCHEDULE，其内容为若干行文件名。
 *   启动时按行取出文件名，逐个加载到任务槽并轮转执行。
 * 任务槽号（0..OS_MAX_TASK-1）就是命令里的 "ID"。
 */
#ifdef __cplusplus
extern "C" {
#endif

ars_i8 arssched_load(void);   /* 读取 SCHEDULE 并装载各任务，返回装入的任务数 */
void   arssched_loop(void);   /* 轮转执行一条指令 */
extern uars_i8 gSchedTasks;   /* 当前存活任务数，供诊断查询 */

/* 重启：
 *   arssched_restart(名字)  只重启"名字"对应的那个任务（没在跑则失败）
 *   arssched_rebootAll()    重置整个堆并重新读 SCHEDULE 装载全部任务
 */
ars_i8 arssched_restart(const char *name);
ars_i8 arssched_rebootAll(void);

/* 手动起停（与 SCHEDULE 文件解耦，只影响"现在谁在跑"）：
 *   arssched_start(名字)  从文件系统读一个程序，装进第一个空闲槽位并开始跑
 *   arssched_kill(名字)   杀掉所有这个名字的任务（同名可能不止一个）
 *   arssched_killId(ID)   按槽位号杀（只杀一个）
 *   arssched_alive(ID)    该槽位是否有活任务（1/0）
 *   arssched_name(ID)     该槽位跑的程序名（空串表示没有）
 *   arssched_count()      当前存活任务数
 * 注意：kill 只杀运行中的实例，不删文件、也不改 SCHEDULE，
 * 所以 reboot_all 还会把它按 SCHEDULE 装回来；要永久移除就改 SCHEDULE 或 del 文件。 */
ars_i8 arssched_start(const char *name);
ars_i8 arssched_kill(const char *name);
ars_i8 arssched_killId(uars_i8 id);
int    arssched_alive(int id);
const char *arssched_name(int id);
int    arssched_count(void);

#ifdef __cplusplus
}
#endif

#endif
