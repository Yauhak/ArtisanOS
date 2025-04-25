//注意！！！
//这部分代码除了一些与自定义函数搭配的处理，其余部分目前几乎完全由DeepSeek生成
#ifndef DISK_FAT
	#include "DiskAndFAT.h"
#endif

/*
	FAT文件系统硬盘布局图
	|保留扇区|FAT1|FAT2|根目录|数据区（簇2开始）|
	write_file 的物理磁盘操作示例
	文件数据："Hello World" (11字节)
	所需簇数：1个簇（假设每簇1扇区=512字节）

	LBA计算：
	data_start_lba = 保留扇区 + FAT表占用 + 根目录占用
	cluster 3的LBA = data_start_lba + (3-2)*1 = data_start_lba +1

	磁盘写入：
	[簇3扇区]:
	偏移0x00: 48 65 6C 6C 6F 20 57 6F 72 6C 64 (Hello World)
	其余空间填充0
*/

uint16_t *g_fat = 0; // FAT表缓存
struct BootSector g_boot;
static uint8_t g_directory_cache[512]; // 当前加载的目录扇区缓存
static uint32_t g_current_directory_lba; // 当前目录的起始LBA

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
	if (ARS_inb(IDE_PORT_COMMAND) & 0x01)  // 检查ERR位
		return -1; // 读取失败
	// 读取512字节数据到缓冲区
	uint16_t *ptr = (uint16_t*)buffer;
	for (int i = 0; i < 256; i++)
		*ptr++ = ARS_inw(IDE_PORT_DATA);
	return 0;
}

// 写入单个扇区到硬盘
int ide_write_sector(uint32_t lba, void *buffer) {
	ide_wait_ready();
	// 设置LBA地址（与读取相同）
	ARS_outb(IDE_PORT_SECTOR_CNT, 1);
	ARS_outb(IDE_PORT_LBA_LOW, lba & 0xFF);
	ARS_outb(IDE_PORT_LBA_MID, (lba >> 8) & 0xFF);
	ARS_outb(IDE_PORT_LBA_HIGH, (lba >> 16) & 0xFF);
	ARS_outb(IDE_PORT_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
	// 发送写入命令
	ARS_outb(IDE_PORT_COMMAND, IDE_CMD_WRITE);
	// 写入512字节数据
	uint16_t *ptr = (uint16_t*)buffer;
	for (int i = 0; i < 256; i++) {
		asm volatile("outw %0, %1" : : "a"(*ptr++), "Nd"(IDE_PORT_DATA));
	}
	// 等待操作完成
	ide_wait_ready();
	return (ARS_inb(IDE_PORT_COMMAND) & 0x01) ? -1 : 0; // 检查错误位
}

// 将FAT表缓存写回磁盘
void flush_fat() {
	uint32_t fat_start = g_boot.reserved_sectors;
	for (int i = 0; i < g_boot.sectors_per_fat; i++)
		ide_write_sector(fat_start + i, (uint8_t*)g_fat + i * 512);
}

// 查找空闲簇
static uint16_t find_free_cluster() {
	for (uint16_t cluster = 2; cluster < 0xFF6; cluster++)
		if (get_next_cluster(cluster) == FAT_FREE_CLUSTER) return cluster;
	return 0; // 无空闲簇
}

// 分配簇链（返回起始簇号）
static uint16_t allocate_cluster_chain(int sectors_needed) {
	uint16_t first_cluster = 0;
	uint16_t prev_cluster = 0;
	for (int i = 0; i < sectors_needed; i++) {
		uint16_t cluster = find_free_cluster();
		if (!cluster) return 0; // 分配失败
		// 标记为已分配
		uint16_t fat_offset = cluster + (cluster / 2);
		if (cluster & 1)
			g_fat[fat_offset] = (FAT_END_CLUSTER << 4) | (g_fat[fat_offset] & 0x000F);
		else
			g_fat[fat_offset] = (g_fat[fat_offset] & 0xF000) | FAT_END_CLUSTER;
		if (!first_cluster) first_cluster = cluster;
		if (prev_cluster) link_clusters(prev_cluster, cluster);
		prev_cluster = cluster;
	}
	return first_cluster;
}

// 链接两个簇
static void link_clusters(uint16_t prev, uint16_t next) {
	uint16_t fat_offset = prev + (prev / 2);
	if (prev & 1)
		g_fat[fat_offset] = (next << 4) | (g_fat[fat_offset] & 0x000F);
	else
		g_fat[fat_offset] = (g_fat[fat_offset] & 0xF000) | next;
}

// 辅助函数：查找或创建目录条目
static struct DirEntry* find_or_create_entry(const char *path) {
	char parent_path[256];
	char filename[13];
	// 分离父目录路径和文件名
	const char *last_slash = ARS_strtok((char *)path, '\\');
	if (!last_slash) {
		// 根目录下直接创建
		ARS_memmove(filename, path, ARS_strlen(path));
		parent_path[0] = '\0';
	} else {
		ARS_memmove(parent_path, path, last_slash - path);
		parent_path[last_slash - path] = '\0';
		ARS_memmove(filename, last_slash + 1, ARS_strlen(last_slash + 1));
	}
	// 1. 查找父目录
	uint16_t parent_cluster = find_file(parent_path);
	if (parent_cluster == 0 && parent_path[0] != '\0')
		return (void *)0; // 父目录不存在
	// 2. 转换8.3文件名格式
	char short_name[12] = {0};
	char *dot = ARS_strtok(filename, '.');
	if (dot) {
		// 处理主文件名（最多8字符）
		ARS_memmove(short_name, filename, 8 < dot - filename ? 8 : dot - filename);
		// 处理扩展名（最多3字符）
		ARS_memmove(short_name + 8, dot + 1, 3);
	} else
		ARS_memmove(short_name, filename, 8);
	// 填充空格并转大写
	for (int i = 0; i < 11; i++) {
		if (short_name[i] == '\0') short_name[i] = ' ';
		if (i < 8 && short_name[i] == ' ') {
			// 主文件名不足8字符时后续填空格
			for (int j = i; j < 8; j++) short_name[j] = ' ';
		}
		short_name[i] = toupper(short_name[i]);
	}
	// 3. 在父目录中查找空闲条目
	uint32_t lba;
	uint8_t sector_buffer[512];
	struct DirEntry *free_entry = (void *)0;
	// 根目录特殊处理
	if (parent_cluster == 0) {
		lba = g_boot.reserved_sectors +
		      (g_boot.fat_count * g_boot.sectors_per_fat);
		uint32_t root_sectors = (g_boot.root_entries * 32 + 511) / 512;
		// 遍历根目录所有扇区
		for (uint32_t s = 0; s < root_sectors; s++) {
			ide_read_sector(lba + s, sector_buffer);
			// 检查每个条目
			for (int i = 0; i < 16; i++) {
				struct DirEntry *entry = (struct DirEntry*)(sector_buffer + i * 32);
				// 找到空闲槽位
				if (entry->filename[0] == 0x00 || entry->filename[0] == 0xE5) {
					free_entry = entry;
					goto found_slot;
				}
				// 检查是否已存在同名文件
				if (ARS_strcmp(entry->filename, short_name, 11) == 0) {
					return entry; // 返回已存在条目
				}
			}
		}
	} else {
		// 子目录处理（需要遍历簇链）
		uint16_t current_cluster = parent_cluster;
		do {
			lba = data_start_lba() + (current_cluster - 2) * g_boot.sectors_per_cluster;
			// 遍历簇内所有扇区
			for (int s = 0; s < g_boot.sectors_per_cluster; s++) {
				ide_read_sector(lba + s, sector_buffer);
				for (int i = 0; i < 16; i++) {
					struct DirEntry *entry = (struct DirEntry*)(sector_buffer + i * 32);
					if (entry->filename[0] == 0x00 || entry->filename[0] == 0xE5) {
						free_entry = entry;
						goto found_slot;
					}
					if (ARS_strcmp(entry->filename, short_name, 11) == 0) {
						return entry;
					}
				}
			}
			current_cluster = get_next_cluster(current_cluster);
		} while (current_cluster < 0xFF8);
	}
	// 目录已满（根目录）或需要扩展（子目录）
	if (!free_entry) {
		if (parent_cluster == 0) return (void *)0; // 根目录已满
		// 为子目录分配新簇
		uint16_t new_cluster = allocate_cluster_chain(1);
		if (!new_cluster) return (void *)0;
		// 链接到目录簇链
		link_clusters(parent_cluster, new_cluster);
		// 初始化新簇为全零
		ARS_memset(sector_buffer, 0, 512);
		for (int s = 0; s < g_boot.sectors_per_cluster; s++) {
			ide_write_sector(data_start_lba() + (new_cluster - 2)*g_boot.sectors_per_cluster + s,
			                 sector_buffer);
		}
		free_entry = (struct DirEntry*)sector_buffer;
	}
found_slot:
	// 4. 初始化新条目
	ARS_memmove(free_entry->filename, short_name, 11);
	free_entry->attributes = 0x20; // 归档属性
	free_entry->first_cluster = 0; // 稍后填充
	free_entry->file_size = 0;
	// 5. 写回修改后的扇区
	update_directory_entry(free_entry);
	return free_entry;
}

// 辅助函数：更新目录条目到磁盘
static void update_directory_entry(struct DirEntry *entry) {
	// 计算物理地址
	uint32_t sector_offset = ((uint32_t)entry - (uint32_t)g_directory_cache) / 512;
	uint32_t lba = g_current_directory_lba + sector_offset;
	// 读取整个扇区
	uint8_t sector_buffer[512];
	ide_read_sector(lba, sector_buffer);
	// 计算条目在扇区内的偏移
	uint32_t entry_offset = ((uint32_t)entry - (uint32_t)g_directory_cache) % 512;
	// 拷贝更新后的条目数据
	ARS_memmove(sector_buffer + entry_offset, entry, sizeof(struct DirEntry));
	// 写回磁盘
	ide_write_sector(lba, sector_buffer);
}

// 写入文件到指定路径
int write_file(const char *path, void *data, uint32_t size) {
	// 步骤1：查找或创建目录条目
	struct DirEntry *entry = find_or_create_entry(path);
	if (!entry) return -1; // 目录不存在或无法创建
	// 步骤2：计算需要的簇数量
	int sectors_needed = (size + 511) / 512;
	int clusters_needed = (sectors_needed + g_boot.sectors_per_cluster - 1) /
	                      g_boot.sectors_per_cluster;
	// 步骤3：分配簇链
	uint16_t first_cluster = allocate_cluster_chain(clusters_needed);
	if (!first_cluster) return -2; // 空间不足
	// 步骤4：写入数据
	uint8_t *ptr = (uint8_t*)data;
	uint16_t current_cluster = first_cluster;
	uint32_t bytes_written = 0;
	while (current_cluster < FAT_END_CLUSTER && bytes_written < size) {
		uint32_t lba = data_start_lba() + (current_cluster - 2) * g_boot.sectors_per_cluster;
		for (int i = 0; i < g_boot.sectors_per_cluster; i++) {
			ide_write_sector(lba + i, ptr + bytes_written);
			bytes_written += 512;
			if (bytes_written >= size) break;
		}
		current_cluster = get_next_cluster(current_cluster);
	}
	// 步骤5：更新目录条目
	entry->first_cluster = first_cluster;
	entry->file_size = size;
	update_directory_entry(entry);
	// 步骤6：刷新FAT表
	flush_fat();
	return 0;
}

// 初始化 FAT12 文件系统
int fat12_init() {
	// 读取引导扇区
	if (ide_read_sector(0, &g_boot) != 0)
		return -1;
	// 验证 FAT12 签名
	if (g_boot.boot_signature_end != 0xAA55)
		return -2;
	// 加载 FAT 表到内存
	uint32_t fat_start = g_boot.reserved_sectors;
	g_fat = (uint16_t*)OS_FAT_LOAD_START; // FAT表加载首地址
	for (int i = 0; i < g_boot.sectors_per_fat; i++) {
		if (ide_read_sector(fat_start + i, (uint8_t*)g_fat + i * 512) != 0)
			return -3;
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
	if (cluster & 1)
		//取高12位
		next_cluster >>= 4;
	else
		//低12位
		next_cluster &= 0x0FFF;
	return next_cluster;
}

// 查找文件（支持路径如 "DIR1\DIR2\FILE.TXT"）
uint16_t find_file(const char *path) {
	char *token;
	char temp_path[256];
	ARS_memmove(temp_path, path, ARS_strlen(path));
	uint16_t current_cluster = 0; // 0 表示从根目录开始
	struct DirEntry *entry;
	token = ARS_strtok(temp_path, '\\');
	while (token != (void *)0) {
		// 在 current_cluster 对应的目录中查找 token
		entry = find_entry_in_directory(current_cluster, token);
		if (entry == (void *)0)
			return 0; // 路径不存在
		if ((entry->attributes & 0x10) == 0 && token != (void *)0)
			return 0; // 非目录但路径未结束
		current_cluster = entry->first_cluster;
		token = ARS_strtok(token + ARS_strlen(token), '\\');
	}
	return current_cluster;
}

// 在指定目录簇中查找条目
struct DirEntry *find_entry_in_directory(uint16_t cluster, const char *name) {
	char Fname[11];
	ARS_memmove(Fname, name, 11);
	uint8_t buffer[512];
	uint32_t lba;
	uint8_t isRoot = 0;
	// 根目录的特殊处理（cluster=0）
	if (cluster == 0) {
		lba = g_boot.reserved_sectors +
		      (g_boot.fat_count * g_boot.sectors_per_fat);
		isRoot = 1;
	} else
		lba = data_start_lba() + (cluster - 2) * g_boot.sectors_per_cluster;
	uint32_t root_sectors = (g_boot.root_entries * 32 + 511) / 512; // 每个条目32字节，向上取整到512字节扇区
	uint32_t root_count = 0;
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
			for (int i = 0; i < 11; i++) {
				short_name[i] = toupper(short_name[i]);
				Fname[i] = toupper(Fname[i]);
			}
			// 比较文件名（忽略大小写）
			if (ARS_strcmp(short_name, Fname, 11) == 0)
				return entry;
		}
		root_count++;
		if (isRoot && root_count >= 14) break; // 根目录最大224个条目（14扇区）
		if (!isRoot) {
			cluster = get_next_cluster(cluster);
			lba = data_start_lba() + (cluster - 2) * g_boot.sectors_per_cluster;
		} else
			lba++;
	}
	return (void *)0;
}

// 读取文件到内存（需指定读取扇区数）
int read_file(uint16_t cluster, void *buffer, int sect_count) {
	// 计算数据区起始 LBA
	uint32_t data_start = g_boot.reserved_sectors +
	                      (g_boot.fat_count * g_boot.sectors_per_fat) +
	                      ((g_boot.root_entries * 32 + 511) / 512);
	// 遍历簇链
	uint8_t *ptr = (uint8_t*)buffer;
	int init_sectC = 0;
	while (cluster < 0xFF8 && init_sectC < sect_count) { // 0xFF8~0xFFF 表示文件结束
		uint32_t lba = data_start + (cluster - 2) * g_boot.sectors_per_cluster;
		ide_read_sector(lba, ptr);
		ptr += 512 * g_boot.sectors_per_cluster;
		// 查找下一簇
		uint16_t fat_offset = cluster + (cluster / 2);
		cluster = g_fat[fat_offset];
		if (cluster & 0x8000) // 处理 12 位簇号
			cluster = (cluster & 0x0FFF);
		init_sectC++;
	}
	return 0;
}
