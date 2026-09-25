#ifndef ARSUART_H
#define ARSUART_H

#include "IO_INCLUDE.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ArsLineFn)(char *line);

void arsuart_begin(uars_i32 baud);
void arsuart_setLineHandler(ArsLineFn fn);
void arsuart_poll(void); /* 推发送缓冲 + 收行 */
void arsuart_tick(void); 
void arsuart_txBegin(void); 
void arsuart_putc(char c);
void arsuart_puts(const char *s);
void arsuart_putu32(uars_i32 v); 
void arsuart_putLine(const char *s);
void arsuart_txDrain(void);
int arsuart_rxAvail(void); 
int arsuart_rxGet(void);
void arsuart_rxFlush(void);

#ifdef __cplusplus
}
#endif

#endif
