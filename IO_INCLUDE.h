#ifndef IO_INCLUDE
	#define IO_INCLUDE

	#define int8_t char
	#define int16_t short
	#define int32_t int
	#define uint8_t unsigned char
	#define uint16_t unsigned short
	#define uint32_t unsigned int

	#define VGA_START 0xB8000
	#define VGA_WIDTH 80
	#define VGA_HEIGHT 25

	#define COLOR_BLACK   0x0
	#define COLOR_BLUE    0x1
	#define COLOR_GREEN   0x2
	#define COLOR_CYAN    0x3
	#define COLOR_RED     0x4
	#define COLOR_MAGENTA 0x5
	#define COLOR_YELLOW  0x6
	#define COLOR_WHITE   0x7

	#define KEY_UP 0x80
	#define KEY_DOWN 0x81
	#define KEY_LEFT 0x82
	#define KEY_RIGHT 0x83
	#define NOT_EXT_KEY 0x85

	#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)
	#define SCREEN_BUFFSIZE 4096
	#define FAT_TABLE 5000//FAT表大小（事实上在FAT12中FAT表一般不会超过5000字节）
	#define OS_MAX_TASK 64//最大可“多进程”执行64个程序
	#define OS_MAX_PARAM 128//子程序传入参数的最大量（128）
	#define OS_MAX_MEM 64*1024*1025//最大占用内存64MB（外加缓冲内存1KB）
	#define OS_PHY_MEM_START 0x00250000//从物理内存的第2.5MB开始暂定为OS可操作的内存的首地址
	//在文件读取过程中也会将一些扇区信息、FAT表等暂存于此处
	#define MAX_COMPILECODE 0x00100000//编译ARSLanguage的最大代码大小为1MB
	#define OS_FAT_LOAD_START OS_PHY_MEM_START+OS_MAX_MEM+128
	#define COMPILE_START OS_FAT_LOAD_START+FAT_TABLE+128//ArtisanOS内置编译器编译ARSLanguage时编译代码存放首地址
	#define OS_EXE_LOAD_START COMPILE_START+MAX_COMPILECODE+128
#endif

//现将单字节0x00作为扩展操作码标识
typedef enum {
	EXCODE = 0x00,
	PCHR, // 输出一个字符（参数为立即数或地址）
	PSTR,// 输出一个字符串（参数为地址，输出直到内容为'\0'）
	PVAL,//输出数值（DWORD,INT,FLOAT,参数为立即数或地址）
	KEYINPUT, // 从键盘接收消息，存放到参数所表示的地址中
	VALINPUT,//从键盘输入数值
	READFILE,//从磁盘读取文件，参数1表示读取缓冲区的地址，参数2表示文件名（也是文件名字符串存放的地址），参数3表示读入大小（簇数）
	WRITEFILE,//写文件
	DEL_FILE,//删除文件
	MOV, // 将参数2（立即数或地址，BYTE,DWORD,INT,FLOAT）存入参数1表示的地址内存
	IVKARRAY,//访问数组元素
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

enum {
	BIT_AOX = 0x01,
	BIT_MOV,
	EX_END
} ex_Opcode;

volatile void *ARS_memmove(volatile void *dest, volatile const void *src, int n);
volatile void *ARS_memset(volatile void *dest, volatile const void *byte, int n);
uint16_t ARS_strlen(const char *str);
uint8_t ARS_strcmp(const char *haystack, const char *needle, int len);
char *ARS_strtok(char *str, const char delim);
char toupper(char chr);
static inline void ARS_outb(uint16_t port, uint8_t val);
static inline char ARS_inb(uint16_t port);
static inline char ARS_inw(uint16_t port);
int8_t apm_supported();
void apm_shutdown();
void acpi_shutdown();
void halt_cpu();
void shutdown();
void ARS_pc(uint8_t c, uint8_t id);
char ARS_gc(uint8_t id, uint8_t ifshowoff);
void DispBuff(const char *dest, uint16_t count, uint8_t id);
void ToppingWindowById(uint8_t id);
static void _int_to_str(int32_t num, char *buffer);
static void _float_to_str(float num, char *buffer, int precision);
static double str_to_double(char *p);
float tranIntToFloat(int x);
int tranFloatToInt(float x);

//屏幕数据缓冲区
uint8_t EXE_SCBUFF[OS_MAX_TASK][SCREEN_BUFFSIZE];
//屏幕缓冲区占用计数
uint16_t EXE_SC_POS[OS_MAX_TASK] = {0};
volatile uint32_t ID = 0;
uint8_t TopWindowId;
