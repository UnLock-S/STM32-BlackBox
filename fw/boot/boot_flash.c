/* ─────────────────────────────────────────────────────────────────────────────
 *  boot_flash.c —— 内部 Flash 擦写 + CRC32 + 标志页
 * ─────────────────────────────────────────────────────────────────────────────
 *  ★ 为什么 bootloader 写 App 区是安全的:
 *    代码在 0x08000000..0x08002FFF 执行，只擦写 0x08003000 之后 ——
 *    STM32F1 是单 bank，同 bank 擦写期间不能取指，所以"自己改自己"会挂 ✗
 *    （这正是 App 不能自升级、必须靠 bootloader 的原因）
 * ───────────────────────────────────────────────────────────────────────────── */
#include <string.h>

#include "boot_flash.h"
#include "boot_cfg.h"

#include "main.h"

int boot_flash_unlock(void)
{
    HAL_FLASH_Unlock();
    return 0;
}

void boot_flash_lock(void)
{
    HAL_FLASH_Lock();
}

int boot_flash_erase(uint32_t addr, uint32_t size)
{
    FLASH_EraseInitTypeDef e;
    uint32_t err = 0u;
    uint32_t pages;

    if (size == 0u) {
        return -1;
    }
    pages = (size + BOOT_PAGE_SIZE - 1u) / BOOT_PAGE_SIZE;

    memset(&e, 0, sizeof(e));
    e.TypeErase   = FLASH_TYPEERASE_PAGES;
    e.PageAddress = addr;
    e.NbPages     = pages;

    if (HAL_FLASHEx_Erase(&e, &err) != HAL_OK) {
        return -2;
    }
    return 0;
}

int boot_flash_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (buf == NULL || (len & 1u) != 0u) {
        return -1;
    }
    for (i = 0u; i < len; i += 2u) {
        uint16_t hw = (uint16_t) (buf[i] | ((uint16_t) buf[i + 1u] << 8));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, (uint64_t) hw) != HAL_OK) {
            return -2;
        }
        /* 读回校验：Flash 编程失败往往不报错，只能读回比 ✗ */
        if (*(volatile uint16_t *) (addr + i) != hw) {
            return -3;
        }
    }
    return 0;
}

uint32_t boot_crc32(const uint8_t *p, uint32_t len, uint32_t seed)
{
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    uint32_t i;
    uint32_t b;

    if (p == NULL) {
        return 0u;
    }
    for (i = 0u; i < len; i++) {
        crc ^= (uint32_t) p[i];
        for (b = 0u; b < 8u; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) (-(int32_t) (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ── 标志页 ───────────────────────────────────────────────────────────────── */

int boot_meta_erase(void)
{
    return boot_flash_erase(BOOT_META_ADDR, BOOT_META_PAGE_SIZE);
}

int boot_meta_write(const void *rec, uint32_t len)
{
    int rc;

    if (rec == NULL || len == 0u || len > BOOT_META_PAGE_SIZE) {
        return -1;
    }
    rc = boot_meta_erase();
    if (rc != 0) {
        return -2;
    }
    return boot_flash_write(BOOT_META_ADDR, (const uint8_t *) rec, (len + 1u) & ~1u);
}
