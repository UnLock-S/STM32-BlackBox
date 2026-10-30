/* ─────────────────────────────────────────────────────────────────────────────
 *  boot_fat.h —— 极简 FAT32 只读（只为从根目录取一个文件）
 * ─────────────────────────────────────────────────────────────────────────────
 *  为什么不用现成的 FatFs:
 *    体积。bootloader 只有 12 KB，这里只需要"打开根目录下一个 8.3 名字的文件，
 *    按块顺序读出来"，用不到写、目录树、长文件名、时间戳。
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef SBB_BOOT_FAT_H
#define SBB_BOOT_FAT_H

#include <stdint.h>

/* 挂载：解析 MBR/BPB。/  返回 0 = 成功（必须是 FAT32 且每扇区 512 B） */
int fat_mount(void);

/* 在根目录找 8.3 名字（11 字节，不足补空格，如 "SBB_APP BIN"）。
 * 成功返回 0，并给出文件字节数与首簇 */
int fat_open(const char name83[11], uint32_t *size_out);

/* 顺序读取：每次 512 B。返回 1 = 读到一块，0 = 读完，<0 = 出错 */
int fat_read_block(uint8_t *buf);

/* 诊断信息 */
uint32_t fat_part_lba(void);
uint32_t fat_data_lba(void);
uint32_t fat_root_clus(void);

#endif /* SBB_BOOT_FAT_H */
