/* ─────────────────────────────────────────────────────────────────────────────
 *  boot_fat.c —— 极简 FAT32 只读实现
 * ───────────────────────────────────────────────────────────────────────────── */
#include <string.h>

#include "boot_fat.h"
#include "bsp_sdcard.h"

#define SEC_SIZE        512u
#define FAT_EOC         0x0FFFFFF8u

static uint8_t  s_sec[SEC_SIZE];      /* 通用扇区缓冲 */

static uint32_t s_part_lba;           /* 分区起始 LBA */
static uint32_t s_fat_lba;            /* FAT 表起始 LBA */
static uint32_t s_data_lba;           /* 数据区起始 LBA */
static uint32_t s_root_clus;
static uint8_t  s_sec_per_clus;

/* 当前文件状态 */
static uint32_t s_clus;               /* 当前簇 */
static uint32_t s_clus_index;         /* 簇内第几个扇区 */
static uint32_t s_size;               /* 文件总字节 */
static uint32_t s_done;               /* 已读出字节 */
static int      s_eof;

static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | ((uint16_t) p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

uint32_t fat_part_lba(void)  { return s_part_lba; }
uint32_t fat_data_lba(void)  { return s_data_lba; }
uint32_t fat_root_clus(void) { return s_root_clus; }

static uint32_t clus_to_lba(uint32_t clus)
{
    return s_data_lba + (clus - 2u) * (uint32_t) s_sec_per_clus;
}

static int read_sec(uint32_t lba, uint8_t *buf)
{
    return (bsp_sdcard_read(lba, buf, 1u) == 0) ? 0 : -1;
}

/* 取 FAT 表里某个簇的下一簇 */
static int fat_next(uint32_t clus, uint32_t *next)
{
    uint32_t off  = clus * 4u;
    uint32_t lba  = s_fat_lba + off / SEC_SIZE;
    uint32_t idx  = off % SEC_SIZE;

    if (read_sec(lba, s_sec) != 0) {
        return -1;
    }
    *next = rd32(&s_sec[idx]) & 0x0FFFFFFFu;
    return 0;
}

int fat_mount(void)
{
    uint32_t lba;
    uint32_t fat_sz;
    uint8_t  num_fats;

    s_part_lba = 0u;

    /* ① MBR（也可能是"超软盘"布局：LBA 0 直接是 BPB） */
    if (read_sec(0u, s_sec) != 0) {
        return -1;
    }
    if (memcmp(&s_sec[82], "FAT32", 5u) == 0) {
        s_part_lba = 0u;                       /* 无分区表 */
    } else {
        const uint8_t *pe = &s_sec[446];
        uint8_t type = pe[4];

        if (type != 0x0Bu && type != 0x0Cu) {
            return -2;                          /* 不是 FAT32 分区 */
        }
        s_part_lba = rd32(&pe[8]);
    }

    /* ② BPB */
    if (read_sec(s_part_lba, s_sec) != 0) {
        return -3;
    }
    if (memcmp(&s_sec[82], "FAT32", 5u) != 0) {
        return -4;
    }
    if (rd16(&s_sec[11]) != SEC_SIZE) {
        return -5;                              /* 只支持 512 B/扇区 */
    }
    s_sec_per_clus = s_sec[13];
    fat_sz         = rd32(&s_sec[36]);
    num_fats       = s_sec[16];
    s_root_clus    = rd32(&s_sec[44]);

    if (s_sec_per_clus == 0u || fat_sz == 0u || num_fats == 0u) {
        return -6;
    }

    s_fat_lba  = s_part_lba + rd16(&s_sec[14]);
    s_data_lba = s_fat_lba + (uint32_t) num_fats * fat_sz;

    lba = 0u;                                   /* 避免未用变量告警 */
    (void) lba;
    return 0;
}

int fat_open(const char name83[11], uint32_t *size_out)
{
    uint32_t clus = s_root_clus;
    uint32_t guard = 0u;

    s_eof = 1;

    while (clus >= 2u && clus < FAT_EOC && guard++ < 64u) {
        uint32_t lba = clus_to_lba(clus);
        uint8_t  s;

        for (s = 0u; s < s_sec_per_clus; s++) {
            uint32_t ent;

            if (read_sec(lba + s, s_sec) != 0) {
                return -1;
            }
            for (ent = 0u; ent < SEC_SIZE; ent += 32u) {
                const uint8_t *e = &s_sec[ent];
                uint8_t attr;

                if (e[0] == 0x00u) {
                    return -2;                  /* 目录结束 */
                }
                if (e[0] == 0xE5u) {
                    continue;                   /* 已删除 */
                }
                attr = e[11];
                if (attr == 0x0Fu) {
                    continue;                   /* 长文件名条目 */
                }
                if (attr & 0x08u) {
                    continue;                   /* 卷标 */
                }
                if (memcmp(e, name83, 11u) != 0) {
                    continue;
                }
                /* 命中 */
                s_clus = (uint32_t) rd16(&e[26]) | ((uint32_t) rd16(&e[20]) << 16);
                s_size = rd32(&e[28]);
                s_done = 0u;
                s_clus_index = 0u;
                s_eof = (s_size == 0u) ? 1 : 0;
                if (size_out != NULL) {
                    *size_out = s_size;
                }
                return (s_clus >= 2u) ? 0 : -3;
            }
        }

        {
            uint32_t next;
            if (fat_next(clus, &next) != 0) {
                return -4;
            }
            clus = next;
        }
    }
    return -5;
}

int fat_read_block(uint8_t *buf)
{
    uint32_t remain;

    if (buf == NULL) {
        return -1;
    }
    if (s_eof != 0 || s_done >= s_size) {
        return 0;
    }

    if (s_clus_index >= s_sec_per_clus) {
        uint32_t next;
        if (fat_next(s_clus, &next) != 0) {
            return -2;
        }
        if (next >= FAT_EOC) {
            s_eof = 1;
            return 0;
        }
        s_clus = next;
        s_clus_index = 0u;
    }

    if (read_sec(clus_to_lba(s_clus) + s_clus_index, buf) != 0) {
        return -3;
    }
    s_clus_index++;

    remain = s_size - s_done;
    if (remain < SEC_SIZE) {
        memset(&buf[remain], 0xFF, SEC_SIZE - remain);   /* 尾块补 0xFF（Flash 空态）*/
        s_done = s_size;
    } else {
        s_done += SEC_SIZE;
    }
    return 1;
}
