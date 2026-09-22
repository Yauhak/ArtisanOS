#ifndef ARSFS_H
#include "ARSFS.h"
#endif

#if USE_FILE_AND_UART

typedef struct {
	uars_i32 magic;
	uars_i32 ver;
	FCB fcb[FCB_CNT];
	uars_i8 nextPg[BMP];
} Meta;

static Meta meta;
static uars_i8 sectorBuf[SECTOR];
static uars_i8 pgBuf[PG];
static volatile uars_i8 fsLock = 0;

static void pgFreeChain(int start);
static ars_i8 writeFileAt(int idx, const uars_i8 *src, long len);

static void namePack(ars_i8 *dst, const char *src) {
	int i = 0;
	for (; i < NAME_LEN && src[i]; i++) dst[i] = src[i];
	for (; i < NAME_LEN; i++) dst[i] = ' ';
}

static int nameEq(const ars_i8 *packed, const char *name) {
	for (int i = 0; i < NAME_LEN; i++) {
		char a = packed[i];
		char b = name[i];
		if (a == ' ') a = 0;
		if (a != b) return 0;
		if (a == 0) return 1;
	}
	return 1;
}

static uars_i32 pgOffset(uars_i8 index) {
	return DATA_OFF + (uars_i32)index * PG;
}
static int pgCount(long len) {
	return len > 0 ? (int)((len + PG - 1) / PG) : 0;
}

#define PG_PER_SEC (SECTOR / PG)

static int pgAlloc(void) {
	for (int i = 0; i < BMP; i++)
		if (meta.nextPg[i] == FREE_OR_DEL) {
			meta.nextPg[i] = EOF_PG;
			return i;
		}
	return -1;
}

static int pgAllocSector(int pages) {
	if (pages <= 0 || pages > PG_PER_SEC) return -1;
	for (int s = 0; s < BMP; s += PG_PER_SEC) {
		int i = 0;
		for (; i < PG_PER_SEC; i++)
			if (meta.nextPg[s + i] != FREE_OR_DEL) break;
		if (i < PG_PER_SEC) continue;
		for (i = 0; i < pages; i++)
			meta.nextPg[s + i] = (i + 1 < pages) ? (uars_i8)(s + i + 1) : EOF_PG;
		return s;
	}
	return -1;
}

static void pgFreeChain(int start) {
	int p = start;
	int guard = 0;
	while (p >= 0 && p < BMP && meta.nextPg[p] != FREE_OR_DEL && guard++ <= BMP) {
		int nx = meta.nextPg[p];
		meta.nextPg[p] = FREE_OR_DEL;
		if (nx == EOF_PG) break;
		p = nx;
	}
}

void arsfs_lock(void) {
	while (fsLock) {}
	fsLock = 1;
}
void arsfs_unlock(void) {
	fsLock = 0;
}

uars_i32 gFsBase = 0;
static uars_i32 gFsOff = 0;

#ifndef FLASH_XIP_BASE
#define FLASH_XIP_BASE 0x10000000UL
#endif

#define FS_MIN_GAP (FS_TOTAL + 65536u)

uars_i32 arsfsBindBase(void) {
	uars_i32 flashSize = 0;
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#ifdef PICO_FLASH_SIZE_BYTES
	flashSize = (uars_i32)PICO_FLASH_SIZE_BYTES;
#endif
#endif
	if (flashSize < FS_MIN_GAP || (flashSize & (flashSize - 1)) != 0) {
		flashSize = 2u * 1024u * 1024u;
	}
	gFsOff = (uars_i32)(flashSize - FS_TOTAL);
	gFsBase = FLASH_XIP_BASE + gFsOff;
	return gFsBase;
}

#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#include <Arduino.h>
#include <hardware/flash.h>
#include <hardware/sync.h>

static inline void flashBegin(void) {
	noInterrupts();
}
static inline void flashEnd(void) {
	interrupts();
}

ars_i8 arsfs_hw_read(uars_i32 off, uars_i8 *dst, uars_i32 len) {
	const uars_i8 *src = (const uars_i8 *)(gFsBase + off);
	for (uars_i32 i = 0; i < len; i++) dst[i] = src[i];
	return FS_OK;
}

ars_i8 arsfs_hw_erase(uars_i32 off, uars_i32 len) {
	(void)len;
	flashBegin();
	flash_range_erase(gFsOff + off, SECTOR);
	flashEnd();
	return FS_OK;
}

ars_i8 arsfs_hw_write(uars_i32 off, const uars_i8 *src, uars_i32 len) {
	flashBegin();
	flash_range_program(gFsOff + off, (const uint8_t *)src, len);
	flashEnd();
	return FS_OK;
}
#endif

static uars_i8 secBuf[SECTOR];

ars_i8 arsfs_flash_read(uars_i32 off, uars_i8 *dst, uars_i32 len) {
	return arsfs_hw_read(off, dst, len);
}

ars_i8 arsfs_flash_erase(uars_i32 off, uars_i32 len) {
	return arsfs_hw_erase(off, len);
}

ars_i8 arsfs_flash_write(uars_i32 off, const uars_i8 *src, uars_i32 len) {
	uars_i32 sec = off / SECTOR;
	if (arsfs_hw_read(sec * SECTOR, secBuf, SECTOR) != FS_OK) return FS_EIO;
	ARS_memset(secBuf + (off - sec * SECTOR), src, (int)len);
	if (arsfs_hw_erase(sec * SECTOR, SECTOR) != FS_OK) return FS_EIO;
	if (arsfs_hw_write(sec * SECTOR, secBuf, SECTOR) != FS_OK) return FS_EIO;
	return FS_OK;
}

static ars_i8 metaFlush(void) {
	for (int i = 0; i < SECTOR; i++) sectorBuf[i] = 0xFF;
	ARS_memset(sectorBuf, &meta, sizeof(Meta));
	return arsfs_flash_write(0, sectorBuf, SECTOR);
}

static ars_i8 metaLoad(void) {
	if (arsfs_flash_read(0, sectorBuf, SECTOR) != FS_OK) return FS_EIO;
	ARS_memset(&meta, sectorBuf, sizeof(Meta));
	return (meta.magic == MAGIC && meta.ver == FS_VER) ? FS_OK : FS_ECORRUPT;
}

int arsfs_find(const char *name) {
	for (int i = 0; i < FCB_CNT; i++) {
		if (meta.fcb[i].start == FREE_OR_DEL) continue;
		if (nameEq(meta.fcb[i].fileName, name)) return i;
	}
	return -1;
}

static int fcbAlloc(void) {
	for (int i = 0; i < FCB_CNT; i++)
		if (meta.fcb[i].start == FREE_OR_DEL) return i;
	return -1;
}

int arsfs_used(void) {
	int n = 0;
	for (int i = 0; i < BMP; i++)
		if (meta.nextPg[i] != FREE_OR_DEL) n++;
	return n;
}

int arsfs_free(void) {
	return BMP - arsfs_used();
}

int arsfs_page_used(int page) {
	return (page >= 0 && page < BMP && meta.nextPg[page] != FREE_OR_DEL) ? 1 : 0;
}

int arsfs_fcb_start(int i) {
	return (i >= 0 && i < FCB_CNT) ? meta.fcb[i].start : FREE_OR_DEL;
}

int arsfs_fcb_size(int i) {
	return (i >= 0 && i < FCB_CNT) ? meta.fcb[i].size : 0;
}

char arsfs_fcb_name(int i, int k) {
	if (i < 0 || i >= FCB_CNT || k < 0 || k >= NAME_LEN) return ' ';
	return meta.fcb[i].fileName[k];
}

int arsfs_ls(char *out, int cap) {
	int n = 0;
	for (int i = 0; i < FCB_CNT; i++) {
		if (meta.fcb[i].start == FREE_OR_DEL) continue;
		for (int k = 0; k < NAME_LEN && meta.fcb[i].fileName[k] != ' '; k++)
			if (n < cap - 1) out[n++] = meta.fcb[i].fileName[k];
		if (n < cap - 1) out[n++] = '\n';
	}
	if (n < cap) out[n] = 0;
	return n;
}

int arsfs_nthName(int i, char *out) {
	int seen = 0;
	for (int k = 0; k < FCB_CNT; k++) {
		if (meta.fcb[k].start == FREE_OR_DEL) continue;
		if (seen++ == i) {
			for (int j = 0; j < NAME_LEN; j++) out[j] = meta.fcb[k].fileName[j];
			out[NAME_LEN] = 0;
			return 0;
		}
	}
	return -1;
}

ars_i8 format(void) {
	for (int i = 0; i < BMP; i++) meta.nextPg[i] = FREE_OR_DEL;
	meta.magic = MAGIC;
	meta.ver = FS_VER;
	for (int i = 0; i < FCB_CNT; i++) {
		for (int k = 0; k < NAME_LEN; k++) meta.fcb[i].fileName[k] = ' ';
		meta.fcb[i].start = FREE_OR_DEL;
		meta.fcb[i].size = 0;
		meta.fcb[i].attr = 0;
	}
	return metaFlush();
}

ars_i8 arsfs_init(void) {
	arsfsBindBase();
	if (metaLoad() == FS_OK) return FS_OK;
	if (format() != FS_OK) return FS_EIO;
	return metaLoad();
}

static ars_i8 writeFileAt(int idx, const uars_i8 *src, long len) {
	if (idx < 0 || idx >= FCB_CNT) return FS_ENOENT;
	if (len < 0 || len > FILE_MAX) return FS_ETOOBIG;

	pgFreeChain(meta.fcb[idx].start);
	meta.fcb[idx].start = EOF_PG;
	meta.fcb[idx].size = 0;

	int pages = pgCount(len);
	int head = -1, prev = -1;
	long done = 0;
	int sect = pgAllocSector(pages);
	if (sect >= 0 && arsfs_flash_erase(pgOffset((uars_i8)sect), SECTOR) != FS_OK) {
		pgFreeChain(sect);
		metaFlush();
		return FS_EIO;
	}
	for (int i = 0; i < pages; i++) {
		int p;
		if (sect >= 0) {
			p = sect + i;
		} else {
			p = pgAlloc();
			if (p < 0) {
				pgFreeChain(head);
				metaFlush();
				return FS_EFULL;
			}
			if (prev >= 0) meta.nextPg[prev] = (uars_i8)p;
			meta.nextPg[p] = EOF_PG;
		}
		if (head < 0) head = p;
		prev = p;

		long chunk = len - done;
		if (chunk > PG) chunk = PG;
		for (int k = 0; k < PG; k++) pgBuf[k] = 0xFF;
		ARS_memset(pgBuf, src + done, (uars_i32)chunk);
		ars_i8 rc = (sect >= 0) ? arsfs_hw_write(pgOffset((uars_i8)p), pgBuf, PG)
		                        : arsfs_flash_write(pgOffset((uars_i8)p), pgBuf, PG);
		if (rc != FS_OK) {
			pgFreeChain(head);
			metaFlush();
			return FS_EIO;
		}
		done += chunk;
	}
	if (head >= 0) {
		meta.fcb[idx].start = (uars_i8)head;
		meta.fcb[idx].size = (ars_i16)len;
	}
	return metaFlush();
}

ars_i8 createFile(const char *name) {
	if (!name || !name[0]) return FS_ENOENT;
	if (arsfs_find(name) >= 0) return FS_OK;
	int idx = fcbAlloc();
	if (idx < 0) return FS_EFULL;
	namePack(meta.fcb[idx].fileName, name);
	meta.fcb[idx].start = EOF_PG;
	meta.fcb[idx].size = 0;
	meta.fcb[idx].attr = 0;
	return metaFlush();
}

ars_i8 createFileAt(const char *name, int slot) {
	if (slot < 0 || slot >= FCB_CNT) return FS_EFULL;
	int cur = arsfs_find(name);
	if (cur == slot) return FS_OK;
	if (meta.fcb[slot].start != FREE_OR_DEL) return FS_EBUSY;
	if (cur >= 0) {
		meta.fcb[slot] = meta.fcb[cur];
		for (int k = 0; k < NAME_LEN; k++) meta.fcb[cur].fileName[k] = ' ';
		meta.fcb[cur].start = FREE_OR_DEL;
		meta.fcb[cur].size = 0;
	} else {
		namePack(meta.fcb[slot].fileName, name);
		meta.fcb[slot].start = EOF_PG;
		meta.fcb[slot].size = 0;
		meta.fcb[slot].attr = 0;
	}
	return metaFlush();
}

ars_i8 writeFileAtSlot(int slot, const uars_i8 *src, long len) {
	return writeFileAt(slot, src, len);
}

ars_i8 delFile(const char *name) {
	int idx = arsfs_find(name);
	if (idx < 0) return FS_ENOENT;
	pgFreeChain(meta.fcb[idx].start);
	for (int k = 0; k < NAME_LEN; k++) meta.fcb[idx].fileName[k] = ' ';
	meta.fcb[idx].start = FREE_OR_DEL;
	meta.fcb[idx].size = 0;
	return metaFlush();
}

ars_i8 writeFile(const char *name, const uars_i8 *src, long len) {
	int idx = arsfs_find(name);
	if (idx < 0) return FS_ENOENT;
	return writeFileAt(idx, src, len);
}

long readFile(const char *name, uars_i8 *dst, long cap) {
	int idx = arsfs_find(name);
	if (idx < 0) return FS_ENOENT;
	long want = meta.fcb[idx].size;
	if (want <= 0) return 0;
	if (want > cap) want = cap;
	long got = 0;
	int p = meta.fcb[idx].start;
	int guard = 0;
	while (p >= 0 && p < BMP && got < want && guard++ <= BMP) {
		if (arsfs_flash_read(pgOffset((uars_i8)p), pgBuf, PG) != FS_OK) return FS_EIO;
		long chunk = want - got;
		if (chunk > PG) chunk = PG;
		ARS_memset(dst + got, pgBuf, (uars_i32)chunk);
		got += chunk;
		int nx = meta.nextPg[p];
		if (nx == EOF_PG) break;
		p = nx;
	}
	return got;
}
#endif
