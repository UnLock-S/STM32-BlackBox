/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_flash.h —— 内部 Flash 擦写（OTA 升级用）
 * ─────────────────────────────────────────────────────────────────────────────
 *  STM32F103C8T6：64 KB Flash @0x08000000，页大小 1 KB，共 64 页。
 *  ⚠ TC-01 实测 0x1FFFF7E0 = 0x0040 → 64 KB（无 A/B 双槽空间）→ OTA 单槽。
 *
 *  ⚠ 重要限制（明天务必确认）
 *    单槽 OTA 在升级过程中掉电 = 应用被擦掉一半 = 变砖（只能 SWD 救回）。
 *    因此本驱动只做「擦→写→校验」，是否真的执行升级由 app_ota 的策略决定。
 *    要真正安全需要 bootloader + 备份区，那需要更大的 Flash 或换 MCU —— 已记录在
 *    docs/CUBEMX-TODO.md 的待决策项里。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_FLASH_H
#define SBB_BSP_FLASH_H

#include <stdint.h>

#define BSP_FLASH_BASE        0x08000000u
#define BSP_FLASH_SIZE        (64u * 1024u)
#define BSP_FLASH_PAGE_SIZE   1024u
#define BSP_FLASH_PAGE_COUNT  (BSP_FLASH_SIZE / BSP_FLASH_PAGE_SIZE)

/* 擦除 [addr, addr+len) 覆盖到的整页。len 会被向上取整到页边界 */
int      bsp_flash_erase(uint32_t addr, uint32_t len);

/* 写（自动按 半字 编程）。addr 需 2 字节对齐；目标区间必须已擦除 */
int      bsp_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);

/* 直接读（内存映射，无需解锁） */
int      bsp_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/* CRC32（多项式 0xEDB88320，初值 0xFFFFFFFF，结果取反）—— 用于升级镜像校验 */
uint32_t bsp_flash_crc32(uint32_t addr, uint32_t len);

#endif /* SBB_BSP_FLASH_H */
