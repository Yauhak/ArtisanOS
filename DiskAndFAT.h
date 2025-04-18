#ifndef DISK_FAT
	#define DISK_FAT
	#include"IO_INCLUDE.h"
	
	#define IDE_PORT_DATA        0x1F0
	#define IDE_PORT_ERROR       0x1F1
	#define IDE_PORT_SECTOR_CNT  0x1F2
	#define IDE_PORT_LBA_LOW     0x1F3
	#define IDE_PORT_LBA_MID     0x1F4
	#define IDE_PORT_LBA_HIGH    0x1F5
	#define IDE_PORT_DRIVE_HEAD  0x1F6
	#define IDE_PORT_COMMAND     0x1F7
	#define IDE_CMD_READ         0x20
	#define IDE_CMD_WRITE        0x30

//此处由DeepSeek生成
// FAT12 引导扇区结构（共 512 字节）
#pragma pack(push, 1) // 禁用内存对齐
struct BootSector {
	// -------- BIOS 参数块（BPB）--------
	uint8_t  jump_code[3];      // 跳转指令（0xEB 0x3C 0x90）
	char     oem_name[8];       // OEM 标识符（如 "MSWIN4.1"）
	uint16_t bytes_per_sector;  // 每扇区字节数（通常 512）
	uint8_t  sectors_per_cluster; // 每簇扇区数（通常 1）
	uint16_t reserved_sectors;  // 保留扇区数（包括引导扇区）
	uint8_t  fat_count;         // FAT 表数量（通常 2）
	uint16_t root_entries;      // 根目录条目数（通常 224）
	uint16_t total_sectors;     // 总扇区数（≤ 65535）
	uint8_t  media_type;        // 介质类型（0xF8 表示硬盘）
	uint16_t sectors_per_fat;   // 每个 FAT 表占用的扇区数
	uint16_t sectors_per_track; // 每磁道扇区数（CHS 参数）
	uint16_t head_count;        // 磁头数（CHS 参数）
	uint32_t hidden_sectors;    // 隐藏扇区数（分区起始 LBA）
	uint32_t large_total_sectors; // 总扇区数（> 65535 时使用）
	
	// -------- 扩展 BIOS 参数块（EBPB）--------
	uint8_t  drive_number;      // 驱动器号（0x00=软盘，0x80=硬盘）
	uint8_t  reserved;          // 保留
	uint8_t  boot_signature;    // 扩展引导标记（0x29）
	uint32_t volume_id;         // 卷序列号
	char     volume_label[11]; // 卷标（空格填充）
	char     file_system[8];    // 文件系统类型（"FAT12   "）
	uint8_t  boot_code[448];    // 引导代码（未使用则为 0）
	uint16_t boot_signature_end; // 引导扇区结束标记（0xAA55）
};
#pragma pack(pop)

#pragma pack(push, 1)
struct DirEntry {
	char     filename[8];      // 文件名（空格填充，不足补空格）
	char     ext[3];           // 扩展名（同上）
	uint8_t  attributes;       // 文件属性（0x20=普通文件，0x10=目录）
	uint8_t  reserved_nt;      // Windows NT 保留（通常为 0）
	uint8_t  creation_time_ms; // 创建时间的毫秒（通常为 0）
	uint16_t creation_time;    // 创建时间（格式：hh:mm:ss，各5/6位）
	uint16_t creation_date;    // 创建日期（格式：yyyy:mm:dd，各7/4/5位）
	uint16_t last_access_date; // 最后访问日期（同上）
	uint16_t first_cluster_hi; // 起始簇号高 16 位（FAT12 中为 0）
	uint16_t modify_time;      // 最后修改时间
	uint16_t modify_date;       // 最后修改日期
	uint16_t first_cluster;     // 起始簇号（低 16 位）
	uint32_t file_size;        // 文件大小（字节）
};
#pragma pack(pop)

static void ide_wait_ready();
void ide_init();
int ide_read_sector(uint32_t lba, void *buffer);
int fat12_init();
static uint32_t data_start_lba();
static uint16_t get_next_cluster(uint16_t cluster);
uint16_t find_file(const char *path);
struct DirEntry *find_entry_in_directory(uint16_t cluster, const char *name);
int read_file(uint16_t cluster, void *buffer);
#endif
