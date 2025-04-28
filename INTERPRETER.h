#ifndef IO_INCLUDE
	#include "IO_INCLUDE.h"
#endif

#ifndef INTERPRETER
	#define INTERPRETER
	#define OUT_BOUND 1
	#define ID_ERR 2
	#define HEAD_ERR 3
	#define BAD_MEM_TRACE 4
	#define DIV_BY_0 5
	#define OUT_PARAM_BOUND 6
	#define INVALID_LEN 7
	#define NEED_APPEND_TO_TAIL 8
	#define MEM_CLEAN_PARTLY 9
	#define NO_FREE_MEM 10
	#define NO_MEM_TAIL 11
	#define NO_MEM_HEAD 12
	#define BAD_FREE_BLOCK 13

	#define SPLIT "SPLT" //魔术字：已被程序占位
	#define FREE "FREE" //魔术字：程序内存已被清空
	#define CHECK 1145141919
	#define RESERVED_BLOCKSIZE 3
#endif

struct ParamStack {
	int8_t Type;
	union {
		int8_t BYTE;
		int16_t DWORD;
		int32_t INT;
		float FLOAT;
	} DATA __attribute__((aligned(4))); // 4字节对齐
};

typedef struct ParamStack ParamStack;

struct Magic {
	char MagicHead[4];
	int32_t Reserved;//保留字
	//用于应对内存越界访问的缓冲
	uint8_t id;
	int32_t Check;
	//Check为守卫标识
	//最后的内存防线
	//若连此值都被破坏则认为该段内存完全损坏
	uint32_t len;
	//len=数据块长度+3字节缓冲
	//不包括魔术字头
	volatile uint8_t *last_block;
	volatile uint8_t *next_block;
	//注意
	//此处的作用域等级只是方便虚拟机追踪内存分配情况
	//“应用程序”的字节码中若没有限制则可以访问全局内存
	//作用域的规划将交给ARS字节码编译器处理
	int level;
} __attribute__((packed));

typedef struct Magic Magic;

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
	MOV_BYTE, // 将参数2（立即数或地址，大小为一个字节）存入参数1表示的地址内存
	MOV_DWRD, // 将参数2（立即数或地址，大小为二个字节）存入参数1表示的地址内存
	MOV_INT, // 将参数2（立即数或地址，大小为四个字节）存入参数1表示的地址内存
	MOV_FLOAT,//将参数2（立即数或地址，FLOAT类型）存入参数1表示的地址内存
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
	HLT, // 程序结束标记
} Opcode;

int8_t HigherInt16(int16_t x);
int8_t LowerInt16(int16_t x);
int16_t HigherInt32(int x);
int16_t LowerInt32(int x);
int16_t Tran8To16(int8_t higher, int8_t lower);
int Tran16To32(int16_t higher, int16_t lower);
//内存读写需要考虑多任务情况
//每个函数应显式要求提供程序ID
//以防止时间片轮转后ID切换，导致读取其他程序的内存
void ReadByteMem(uint8_t *Recv,uint8_t id);
int8_t findByteWithAddr(uint8_t id);
int16_t findDByteWithAddr(uint8_t id);
int findIntWithAddr(uint8_t id);
void setByte(int8_t byteText,uint8_t id);
void setDByte(int16_t DbyteText,uint8_t id);
void setInt(int32_t intText,uint8_t id) ;
int8_t ReArrangeMemAndTask(uint8_t id);
int8_t DelLastFuncMem(uint8_t id);
int8_t SuperFree(Magic *block);
int findFreeMemById(uint8_t id, int allocLen, int level);
uint8_t FindPhyMemOffByID(uint8_t id, uint32_t offset);
void init_mem_info();
int8_t interprete(uint8_t cmd, int32_t *params, uint16_t taskId);
