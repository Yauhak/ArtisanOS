#ifndef IO_INCLUDE
	#include "IO_INCLUDE.h"
#endif

#ifndef INTERPRETER
	#define INTERPRETER
	#define MENU_MAXSIZE 32
#endif

struct menu {
	uint8_t is_registered;//是否注册了菜单结构
	uint8_t id;//哪个程序需要启用菜单结构
	uint8_t handle;//这里相当于菜单的ID
	uint8_t menu_len;//菜单项目数
	uint8_t *menu_prompt[MENU_MAXSIZE];//菜单最大32个选项，该指针指向不同选项提示语句的地址
	uint32_t action[MENU_MAXSIZE];//每个选项对应的动作（函数）
} __attribute__((packed));
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
int8_t invoke_array(uint8_t ParamType, int32_t *params, uint16_t taskId);
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
//扩展操作码
int8_t bit_and_or_xor(uint8_t ParamType, int32_t *params, uint16_t taskId);
int8_t bit_move(uint8_t ParamType, int32_t *params, uint16_t taskId);

int8_t interprete(uint8_t cmdAndPmTp, int32_t *params, uint16_t taskId, uint8_t is_ex_op);
