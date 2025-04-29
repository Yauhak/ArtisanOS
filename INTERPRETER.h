#ifndef IO_INCLUDE
	#include "IO_INCLUDE.h"
#endif

#ifndef INTERPRETER
	#define INTERPRETER
	#define MENU_MAXSIZE 32
#endif

struct menu{
	uint8_t is_registered;//是否注册了菜单结构
	uint8_t id;//哪个程序需要启用菜单结构
	uint8_t handle;//这里相当于菜单的ID
	uint8_t menu_len;//菜单项目数
	uint8_t *menu_prompt[MENU_MAXSIZE];//菜单最大32个选项，该指针指向不同选项提示语句的地址
	uint32_t action[MENU_MAXSIZE];//每个选项对应的动作（函数）
}__attribute__((packed));
typedef struct menu menu;

/*
	该部分为“天工”操作系统的程序执行逻辑部分
	通过该系统独有的字节码虚拟机执行已编译为字节码的程序
	同时该虚拟机可以实现简单的时间片轮转和任务调度
	来实现“操作系统”的要求
*/

/*
	“天工”操作系统拥有特殊的“PE”文件结构
	【程序文件声明（标识头）】【代码表】
	·标识头是常量（0x41,0x52,0x53,0x45,0x58,0x45，对应字符“ARSEXE”）
	·代码表则被存放在程序主体,包含了一系列字节码命令
*/

typedef enum {
	PCHR = 0x00, // 输出一个字符（参数为立即数或地址）
	PSTR,// 输出一个字符串（参数为地址，输出直到内容为'\0'）
	PVAL,//输出数值（DWORD,INT,FLOAT,参数为立即数或地址）
	KEYINPUT, // 从键盘接收消息，存放到参数所表示的地址中
	VALINPUT,//从键盘输入数值
	READFILE,//从磁盘读取文件，参数1表示读取缓冲区的地址，参数2表示文件名（也是文件名字符串存放的地址），参数3表示读入大小（簇数）
	WRITEFILE,//写文件
	DEL_FILE,//删除文件
	MOV, // 将参数2（立即数或地址，BYTE,DWORD,INT,FLOAT）存入参数1表示的地址内存
	PUSH,//将CalcResu的值保存于参数表示的地址中
	PUSHP,//为子程序传入参数
	EQ, // 判断参数1 == 参数2，结果存CalcResu
	LT, // 参数1 < 参数2
	GT, // 参数1 > 参数2
	LE, // 参数1 <= 参数2
	GE, // 参数1 >= 参数2
	NE, // 参数1 != 参数2
	//使用ARS特制编程语言编写的程序中各项LABEL和FUNC的位置将由编译器进行推定
	JMP, // 无条件跳转到相应的跳转标签
	JMP_T, // 条件跳转（如果CalcResu!=0则跳转）
	CALL,//子程序跳转
	RET,//子程序返回
	ADD, // 参数1 + 参数2 → CalcResu
	SUB, // 减法
	MUL, // 乘法
	DIV, // 除法
	MENU_HANG,//挂载菜单
	MENU_REG,//注册菜单选项
	MENU_SHOW,//显示菜单
	HLT, // 程序结束标记
} Opcode;

//内存读写需要考虑多任务情况
//每个函数应显式要求提供程序ID
//以防止时间片轮转后ID切换，导致读取其他程序的内存
int8_t pchr(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t pstr(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t pval(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t key_input(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t val_input(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t _read_file(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t _write_file(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t _del_file(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t mov(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t push(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t pushp(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t call(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t ret(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t conds(uint8_t cmdAndPmTp, int32_t *params, uint16_t taskId);
int8_t jmp(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t jmp_t(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t calc(uint8_t cmdAndPmTp, int32_t *params, uint16_t taskId);
int8_t menu_hang(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t menu_reg(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t menu_show(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t hlt(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t interprete(uint8_t cmd, int32_t *params, uint16_t taskId);
