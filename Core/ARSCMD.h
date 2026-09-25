#ifndef ARSCMD_H
#define ARSCMD_H

#include "IO_INCLUDE.h"

/* ============================== 命令层 ==============================
 * 共 14 条：
 *   update / get / del / ls / occ [text] / ver / format
 *   reboot <名> / reboot_all
 *   start <名> / kill <名> / killid <ID> / tasks
 * ================================================================== */
#ifdef __cplusplus
extern "C" {
#endif

void arscmd_begin(void);      /* 命令层自身状态初始化 */
void arscmd_line(char *line); /* 串口层收到一整行就调这个 */

#ifdef __cplusplus
}
#endif

#endif
