/* 文件系统 + 串口终端总开关：把下面的 0 改成 1 即可启用。
 * 启用后：ARSFS 文件系统、串口命令（update/get/del/ls/occ）、
 *         以及"读取 SCHEDULE 文件、按行装载任务"的文件驱动调度器。
 * 也可以用编译选项 -DUSE_FILE_AND_UART=1 覆盖。 */
#ifndef USE_FILE_AND_UART
	#define USE_FILE_AND_UART 1
#endif

#ifndef IO_INCLUDE
#define IO_INCLUDE

//下列define的值也是用户可自定义的
#define OS_MAX_TASK 8       //最大可“多进程”执行8个程序
#define OS_MAX_PARAM 128    //子程序传入参数的总长度的最大值(128)
#define OS_MAX_SGL_PG 2048  //每个程序代码体积的最大值为2048B
#define OS_MAX_MEM 8192     //系统“应用程序”层内存空间总量为8192B

typedef enum {
	MOV,        //将参数2（立即数或地址，BYTE,INT,FLOAT）存入参数1表示的地址内存
	SETARRAY,   //设置数组元素
	READARRAY,  //读取数组某个下标的元素
	INITARRAY,  //初始化数组（为数组元素批量赋值）
	PUSH,       //将CalcResu的值保存于参数表示的地址中
	PUSHP,      //为子程序传入参数
	ADD,        //参数1 + 参数2 → CalcResu
	SUB,        //减法
	MUL,        //乘法
	DIV,        //除法
	EQ,         //判断参数1 == 参数2，结果存CalcResu
	LT,         //参数1 < 参数2
	GT,         //参数1 > 参数2
	LE,         //参数1 <= 参数2
	GE,         //参数1 >= 参数2
	NE,         //参数1 != 参数2
	//使用ARS特制编程语言编写的程序中各项LABEL和FUNC的位置将由编译器进行推定
	JMP,         //无条件跳转到相应的跳转标签
	JMP_T,       //条件跳转（如果CalcResu!=0则跳转）
	CALL,        //子程序跳转
	RET,         //子程序返回
	BIT_AOX,     //按位与、或
	BIT_MOV,     //按位移
	ABI_INVOKE,  //调用Glue层的ABI
	REG_WRITE,   //向指定地址的寄存器写入一字节指定值
	REG_READ,    //在指定地址的寄存器读取一字节指定值
	VAL,         //设置临时计算寄存器
	TO_INT,      //将内存中的浮点数转换为整数存入 CalcResu
	TO_FLOAT,    //将内存中的整数转换为浮点数存入 CalcResu
	IPC_SEND,
	IPC_RECV,
	HLT,  //程序结束标记
} Opcode;

typedef signed char ars_i8;
typedef short ars_i16;
typedef int ars_i32;
typedef unsigned char uars_i8;
typedef unsigned short uars_i16;
typedef unsigned int uars_i32;

volatile void *ARS_memmove(volatile void *dest, volatile const void *src, int n);
volatile void *ARS_memset(volatile void *dest, volatile const void *byte, int n);
uars_i16 ARS_strlen(const char *str);
uars_i8 ARS_strcmp(const char *haystack, const char *needle, int len);
uars_i32 ARS_strtok(char *str, const char delim);
float copyIntToFloat(int x);
int copyFloatToInt(float x);

/* 平台/应用层提供：把出厂示例铺进文件系统（首次启动与 format 之后调用）。 */
void ARS_provision(void);

#include "ARSFS.h"
#include "ARSSCHED.h"
#include "ARSUART.h" /* 串口传输层 */
#include "ARSCMD.h"  /* 命令层（只依赖 ARSUART / ARSFS / ARSSCHED） */

#endif /* IO_INCLUDE */
