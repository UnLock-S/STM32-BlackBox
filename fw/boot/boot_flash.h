/* ─────────────────────────────────────────────────────────────────────────────
 *  boot_flash.h —— 内部 Flash 擦写 + CRC32（bootloader 与 App 共用）
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef SBB_BOOT_FLASH_H
#define SBB_BOOT_FLASH_H

#include <stdint.h>

int      boot_flash_unlock(void);
void     boot_flash_lock(void);

/* 擦除 App 区前 size 字节所覆盖的页（1 KB/页） */
int      boot_flash_erase(uint32_t addr, uint32_t size);

/* 半字编程，len 必须是偶数（镜像长度由打包工具保证 4 字节对齐） */
int      boot_flash_write(uint32_t addr, const uint8_t *buf, uint32_t len);

/* 标准 CRC32（多项式 0xEDB88320，与 Python zlib.crc32 一致） */
uint32_t boot_crc32(const uint8_t *p, uint32_t len, uint32_t seed);

/* 标志页读/写（0x0800FC00，1 KB 独立页） */
int      boot_meta_write(const void *rec, uint32_t len);
int      boot_meta_erase(void);

#endif /* SBB_BOOT_FLASH_H */
