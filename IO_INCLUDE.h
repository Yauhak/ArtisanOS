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
	#define OS_FAT_LOAD_START OS_PHY_MEM_START+OS_MAX_MEM+128
	#define OS_EXE_LOAD_START OS_FAT_LOAD_START+FAT_TABLE+128//ARS特制EXE的加载的首地址

	volatile void *ARS_memmove(volatile void *dest, volatile const void *src, int n);
	volatile void *ARS_memset(volatile void *dest, volatile const void *byte, int n);
	uint16_t ARS_strlen(const uint8_t *str);
	uint8_t ARS_strcmp(const char *haystack, const char *needle, int len);
	char *ARS_strtok(char *str,const char delim);
	char toupper(char chr);
	static inline void ARS_outb(uint16_t port, uint8_t val);
	static inline char ARS_inb(uint16_t port);
	static inline char ARS_inw(uint16_t port);
	int8_t apm_supported();
	void apm_shutdown();
	void acpi_shutdown();
	void halt_cpu();
	void shutdown();
	void ARS_pc(uint8_t c,uint8_t id);
	char ARS_gc(uint8_t id);
	void DispBuff(const char *dest,uint16_t count,uint8_t id);
	void ToppingWindowById(uint8_t id);
	static void _int_to_str(int32_t num, char *buffer);
	static void _float_to_str(float num, char *buffer, int precision);

	//屏幕数据缓冲区
	uint8_t EXE_SCBUFF[OS_MAX_TASK][SCREEN_BUFFSIZE];
	//屏幕缓冲区占用计数
	uint16_t EXE_SC_POS[OS_MAX_TASK] = {0};
	volatile uint32_t ID=0;
	uint8_t TopWindowId;
#endif
