#ifndef ARSSCHED_H
#define ARSSCHED_H

#include "IO_INCLUDE.h"

/* 基于文件的调度：
 *   FCB[0] 固定为 SCHEDULE，其内容为若干行文件名。
 *   启动时按行取出文件名，逐个加载到任务槽并轮转执行。
 */
#ifdef __cplusplus
extern "C" {
#endif

ars_i8 arssched_load(void);   /* 读取 SCHEDULE 并装载各任务，返回装入的任务数 */
void   arssched_loop(void);   /* 轮转执行一条指令（等价于原来 loop() 的一轮） */
extern uars_i8 gSchedTasks;   /* 最近一次装载成功的任务数，供诊断查询 */

#ifdef __cplusplus
}
#endif

#endif
