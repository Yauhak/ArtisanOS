#ifndef ARSUART_H
#define ARSUART_H

#include "IO_INCLUDE.h"

/* 串口命令处理：
 *   文本命令以 '\n' 结尾；二进制数据由 "update" 命令声明长度后按块传输。
 *   应答行一律以 "OK"/"ERR"/"RDY"/"DATA" 开头，便于终端解析。
 */
#ifdef __cplusplus
extern "C" {
#endif

void arsuart_begin(uars_i32 baud);
void arsuart_poll(void);      /* 每轮调度调用一次，非阻塞 */
void arsuart_tick(void);      /* 兼容别名 */
void arsuart_wdtReport(uars_i8 flag); /* 平台层告知：本次启动是否由看门狗超时引起 */

#ifdef __cplusplus
}
#endif

#endif
