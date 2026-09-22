#ifndef ARSFS_H
#define ARSFS_H

#include "IO_INCLUDE.h"

/* ============================ 存储布局 ============================
 * 一块连续的 FLASH 区，按页组织，无文件夹，扁平命名空间。
 *
 *   +------------------+ <- 基地址（取 FLASH 末尾 FS_TOTAL 字节）
 *   | FCB[0..31]       |  32 * 20 = 640B   文件控制块（下标 0 固定为 SCHEDULE）
 *   | nextPg[0..127]   |  128B             跳转链表，见下
 *   | 保留              |  补到 4096B
 *   +------------------+ <- DATA_OFF，数据区起点
 *   | page 0           |  PG=512 字节，整页都是数据
 *   | ...              |  共 BMP=128 页
 *   +------------------+
 *
 * 拓扑由 nextPg 这张跳转表描述（类似 FAT，下标是本页，值是下一页）：
 *   nextPg[i] == FREE_OR_DEL  页 i 空闲
 *   nextPg[i] == EOF_PG       页 i 是文件最后一页
 *   nextPg[i] == 0..127       页 i 的下一页是 nextPg[i]
 * FCB.start 是首页下标，沿 nextPg 一路跳到 EOF_PG 就是整个文件。
 *   FCB.start == FREE_OR_DEL 表示目录项空闲；== EOF_PG 表示文件为空（无数据页）。
 * FCB.size 只用于记实际字节数（末页可能有填充）。
 *
 * 两个哨兵值在两张表里的分工（页号合法范围 0..127，所以永不与哨兵撞车）：
 *   FREE_OR_DEL   nextPg[i]：页 i 空闲       FCB.start：该目录项空闲
 *   EOF_PG        nextPg[i]：页 i 是链尾     FCB.start：文件存在但还没有数据页
 * 注意两者不可互换：空文件的 start 不能写成 FREE_OR_DEL，否则它就跟"空闲目录项"
 * 无法区分了。所以 FREE_OR_DEL 才是"两张表通用的空闲标记"，EOF_PG 不是。
 *
 * 分配一页时（pgAlloc）立刻把它置成 EOF_PG：既保证这一页不会被重复分配，
 * 也让每页天生就是"只有尾节点的短链"；写文件时再用 nextPg[上一页] = 本页 覆盖它。
 * writeFileAt 里另外显式写了一次链尾，链的终止不依赖 pgAlloc 的副作用。
 *
 * 于是：每页 512 字节全部可用，文件页不要求连续（没有外部碎片），
 * 加链也只是改 RAM 里的 nextPg，不需要回头重写上一页。
 *
 * 写文件时优先把它的页放进【整个扇区都空闲】的地方（pgAllocSector），
 * 找不到才退化为逐页分配：
 *   - 独占扇区时，擦除只会动到自己的页，写完再逐页编程，完全不需要"读-改-擦-写"；
 *   - 于是页写过程中被打断（复位 / 拔插 / 掉电）最多丢掉正在写的这个文件，
 *     而它的目录项是最后才提交的，所以结果只是"这个文件还是空的"，别人毫发无伤；
 *   - 退化为共用扇区时没有这个保证，但回退路径保证了"只要还有空闲页就一定写得进去"。
 *
 * 底层必须先擦后写，而擦除粒度是 4KB 扇区（比页大），所以 arsfs_flash_write
 * 统一走"读回整扇区 -> 覆盖对应区间 -> 擦除扇区 -> 整扇区写回"（只用于元数据
 * 与回退路径）。注意是【覆盖】而不是按位与：目标是从 FLASH 读回的旧内容，
 * 按位与会把上一版数据残留在页里。
 * ================================================================ */

#define FS_TOTAL (SECTOR + (uars_i32)BMP * PG)

extern uars_i32 gFsBase;
#define FILE_FLASH_START_OFF 0          /* 相对基地址的偏移 */

#define SECTOR 4096
#define PG 512
#define BMP 128
#define FCB_CNT 32
#define NAME_LEN 15
#define FILE_MAX OS_MAX_SGL_PG          /* 单个文件不得超过一个任务代码页 */

#define DATA_OFF SECTOR                 /* 数据页起始偏移（扇区对齐） */
#define FIRST_PG_OFF DATA_OFF

#define FREE_OR_DEL 0xFE
#define EOF_PG 0xFF
#define MAGIC 0x53465241UL  /* 'ARFS' 小端 */
#define FS_VER 4            /* 元数据版本：改动语义时递增，旧盘会被强制重建 */

#define FS_OK 0
#define FS_ENOENT (-20)   /* 文件不存在 */
#define FS_EFULL (-21)    /* 目录项或页已满 */
#define FS_ECORRUPT (-22) /* 结构损坏 */
#define FS_ETOOBIG (-23)  /* 超过单文件上限 */
#define FS_EBUSY (-24)    /* 互斥量被占用 */
#define FS_EIO (-25)      /* 底层读写失败 */

typedef struct FCB {
	ars_i8 fileName[NAME_LEN]; /* 不足 15 字节用空格补齐，无扩展名 */
	uars_i8 start;             /* 首页下标；FREE_OR_DEL 空闲，EOF_PG 空文件 */
	ars_i16 size;              /* 实际文件长度（字节） */
	uars_i8 attr;              /* 预留属性位 */
} FCB;

#ifdef __cplusplus
extern "C" {
#endif

ars_i8 format(void);          /* 擦除并重建元数据区 */
ars_i8 arsfs_init(void);      /* 挂载；未格式化或版本不符则自动格式化 */
uars_i32 arsfsBindBase(void); /* 按实际 FLASH 容量算出文件系统基地址（内部调用） */
void arsfs_lock(void);        /* 文件操作互斥（协作式，忙等让出） */
void arsfs_unlock(void);

int arsfs_find(const char *name);            /* 返回 FCB 下标，-1 表示没有 */
int arsfs_ls(char *out, int cap);            /* 列出文件名，返回写入长度 */
int arsfs_used(void);                        /* 已占用页数 */
int arsfs_free(void);                        /* 空闲页数 */

int arsfs_page_used(int page);               /* 该页是否被占用 */
int arsfs_fcb_start(int i);                  /* FCB[i].start；FREE_OR_DEL 表示空闲 */
int arsfs_fcb_size(int i);
char arsfs_fcb_name(int i, int k);           /* 第 k 个字符（空格填充） */

ars_i8 createFile(const char *name);
ars_i8 delFile(const char *name);
long readFile(const char *name, uars_i8 *dst, long cap);          /* 返回实际读出长度 */
ars_i8 writeFile(const char *name, const uars_i8 *src, long len); /* 覆盖写 */

ars_i8 createFileAt(const char *name, int slot);
ars_i8 writeFileAtSlot(int slot, const uars_i8 *src, long len);

ars_i8 arsfs_hw_read(uars_i32 off, uars_i8 *dst, uars_i32 len);
ars_i8 arsfs_hw_erase(uars_i32 off, uars_i32 len);
ars_i8 arsfs_hw_write(uars_i32 off, const uars_i8 *src, uars_i32 len);

ars_i8 arsfs_flash_read(uars_i32 off, uars_i8 *dst, uars_i32 len);
ars_i8 arsfs_flash_erase(uars_i32 off, uars_i32 len);
ars_i8 arsfs_flash_write(uars_i32 off, const uars_i8 *src, uars_i32 len);

#ifdef __cplusplus
}
#endif

#endif
