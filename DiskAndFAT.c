//注意！！！
//这部分代码除了一些与自定义函数搭配的处理，其余部分目前几乎完全由DeepSeek生成
#ifndef DISK_FAT
	#include "DiskAndFAT.h"
#endif

/*
	FAT文件系统硬盘布局图
	|保留扇区|FAT1|FAT2|根目录|数据区（簇2开始）|
*/

uint16_t *g_fat = 0; // FAT表缓存
struct BootSector g_boot;

// 等待硬盘就绪（非忙状态）
static void ide_wait_ready() {
	while (ARS_inb(IDE_PORT_COMMAND) & 0x80); // 等待BSY位清零
}

// 初始化IDE控制器
void ide_init() {
	ide_wait_ready();
	ARS_outb(IDE_PORT_DRIVE_HEAD, 0xE0); // 选择主硬盘，LBA模式
}

// 读取单个扇区到内存
int ide_read_sector(uint32_t lba, void *buffer) {
	ide_wait_ready();
	// 设置LBA地址
	ARS_outb(IDE_PORT_SECTOR_CNT, 1);       // 读取1个扇区
	ARS_outb(IDE_PORT_LBA_LOW, lba & 0xFF);
	ARS_outb(IDE_PORT_LBA_MID, (lba >> 8) & 0xFF);
	ARS_outb(IDE_PORT_LBA_HIGH, (lba >> 16) & 0xFF);
	ARS_outb(IDE_PORT_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); // LBA高4位
	// 发送读取命令
	ARS_outb(IDE_PORT_COMMAND, IDE_CMD_READ);
	// 等待数据就绪
	ide_wait_ready();
	if (ARS_inb(IDE_PORT_COMMAND) & 0x01) { // 检查ERR位
		return -1; // 读取失败
	}
	// 读取512字节数据到缓冲区
	uint16_t *ptr = (uint16_t*)buffer;
	for (int i = 0; i < 256; i++) {
		*ptr++ = ARS_inw(IDE_PORT_DATA);
	}
	return 0;
}

// 初始化 FAT12 文件系统
int fat12_init() {
	// 读取引导扇区
	if (ide_read_sector(0, &g_boot) != 0) {
		return -1;
	}
	// 验证 FAT12 签名
	if (g_boot.boot_signature_end != 0xAA55) {
		return -2;
	}
	// 加载 FAT 表到内存
	uint32_t fat_start = g_boot.reserved_sectors;
	g_fat = (uint16_t*)OS_EXE_LOAD_START; // FAT表加载首地址
	for (int i = 0; i < g_boot.sectors_per_fat; i++) {
		if (ide_read_sector(fat_start + i, (uint8_t*)g_fat + i * 512) != 0) {
			return -3;
		}
	}
	return 0;
}

// 获取数据区起始 LBA
static uint32_t data_start_lba() {
	return g_boot.reserved_sectors + 
	(g_boot.fat_count * g_boot.sectors_per_fat) +
	//向上取整
	((g_boot.root_entries * 32 + 511) / 512);
}

// 获取下一簇号
static uint16_t get_next_cluster(uint16_t cluster) {
	//一簇=12字节=3/2个DWORD
	uint16_t fat_offset = cluster + (cluster / 2);
	uint16_t next_cluster = g_fat[fat_offset];
	//当前簇为奇数簇
	if (cluster % 2) {
		//取高12位
		next_cluster >>= 4;
	} else {
		//低12位
		next_cluster &= 0x0FFF;
	}
	return next_cluster;
}

// 查找文件（支持路径如 "DIR1\DIR2\FILE.TXT"）
uint16_t find_file(const char *path) {
	char *token;
	char temp_path[256];
	ARS_memmove(temp_path, path,ARS_strlen(path));
	uint16_t current_cluster = 0; // 0 表示从根目录开始
	struct DirEntry *entry;
	token = ARS_strtok(temp_path, '\\');
	while (token != (void *)0) {
		// 在 current_cluster 对应的目录中查找 token
		entry = find_entry_in_directory(current_cluster, token);
		if (entry == (void *)0) {
			return 0; // 路径不存在
		}
		if ((entry->attributes & 0x10) == 0 && token != (void *)0) {
			return 0; // 非目录但路径未结束
		}
		current_cluster = entry->first_cluster;
		token = ARS_strtok(token+ARS_strlen(token), '\\');
	}
	return current_cluster;
}

// 在指定目录簇中查找条目
struct DirEntry *find_entry_in_directory(uint16_t cluster, const char *name) {
	uint8_t buffer[512];
	uint32_t lba;
	// 根目录的特殊处理（cluster=0）
	if (cluster == 0) {
		lba = g_boot.reserved_sectors + 
		(g_boot.fat_count * g_boot.sectors_per_fat);
	} else {
		lba = data_start_lba() + (cluster - 2) * g_boot.sectors_per_cluster;
	}
	// 读取目录的每个簇
	while (1) {
		ide_read_sector(lba, buffer);
		for (int i = 0; i < 16; i++) { // 每扇区 16 个条目
			struct DirEntry *entry = (struct DirEntry*)(buffer + i * 32);
			if (entry->filename[0] == 0x00) return (void *)0; // 条目结束
			if (entry->filename[0] == 0xE5) continue;    // 已删除
			// 生成 8.3 格式的短文件名
			char short_name[12];
			ARS_memmove(short_name, entry->filename, 8);
			ARS_memmove(short_name + 8, entry->ext, 3);
			short_name[11] = '\0';
			// 比较文件名（不忽略大小写）
			if (ARS_strcmp(short_name, name, 11) == 0) {
				return entry;
			}
		}
		// 查找下一个簇
		if (cluster == 0) break; // 根目录无簇链
		cluster = get_next_cluster(cluster);
		lba = data_start_lba() + (cluster - 2) * g_boot.sectors_per_cluster;
	}
	
	return (void *)0;
}

// 读取文件到内存
int read_file(uint16_t cluster, void *buffer) {
	// 计算数据区起始 LBA
	uint32_t data_start = g_boot.reserved_sectors + 
	(g_boot.fat_count * g_boot.sectors_per_fat) +
	((g_boot.root_entries * 32 + 511) / 512);
	// 遍历簇链
	uint8_t *ptr = (uint8_t*)buffer;
	while (cluster < 0xFF8) { // 0xFF8~0xFFF 表示文件结束
		uint32_t lba = data_start + (cluster - 2) * g_boot.sectors_per_cluster;
		ide_read_sector(lba, ptr);
		ptr += 512 * g_boot.sectors_per_cluster;
		// 查找下一簇
		uint16_t fat_offset = cluster + (cluster / 2);
		cluster = g_fat[fat_offset];
		if (cluster & 0x8000) { // 处理 12 位簇号
			cluster = (cluster & 0x0FFF);
		}
	}
	return 0;
}
