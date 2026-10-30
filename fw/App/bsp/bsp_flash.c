/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_flash.c —— 内部 Flash 擦写（HAL）
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_flash.h"

#include <stdbool.h>
#include <string.h>

#include "main.h"

/* ── 简易 CRC32（位运算实现，不占表空间；速度对本用途足够）── */
uint32_t bsp_flash_crc32(uint32_t addr, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0u; i < len; i++) {
        uint8_t byte = *(volatile uint8_t *) (addr + i);
        crc ^= byte;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

static bool addr_ok(uint32_t addr, uint32_t len)
{
    if (addr < BSP_FLASH_BASE) {
        return false;
    }
    if ((uint64_t) addr + (uint64_t) len > (uint64_t) (BSP_FLASH_BASE + BSP_FLASH_SIZE)) {
        return false;
    }
    return true;
}

int bsp_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (buf == NULL || !addr_ok(addr, len)) {
        return -1;
    }
    memcpy(buf, (const void *) addr, len);
    return 0;
}

int bsp_flash_erase(uint32_t addr, uint32_t len)
{
    FLASH_EraseInitTypeDef er;
    uint32_t               page_err = 0u;
    uint32_t               first;
    uint32_t               npages;

    if (len == 0u || !addr_ok(addr, len)) {
        return -1;
    }

    first  = (addr - BSP_FLASH_BASE) / BSP_FLASH_PAGE_SIZE;
    npages = ((len + BSP_FLASH_PAGE_SIZE - 1u) / BSP_FLASH_PAGE_SIZE);

    if ((first + npages) > BSP_FLASH_PAGE_COUNT) {
        return -2;
    }

    er.TypeErase   = FLASH_TYPEERASE_PAGES;
    er.PageAddress = BSP_FLASH_BASE + (first * BSP_FLASH_PAGE_SIZE);
    er.NbPages     = npages;

    (void) HAL_FLASH_Unlock();

    /* 清掉可能的旧错误标志，否则 HAL 直接返回错误 */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);

    if (HAL_FLASHEx_Erase(&er, &page_err) != HAL_OK) {
        (void) HAL_FLASH_Lock();
        return -3;                      /* page_err 里是出错页 */
    }

    (void) HAL_FLASH_Lock();
    return 0;
}

int bsp_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0u || !addr_ok(addr, len)) {
        return -1;
    }
    if ((addr & 1u) != 0u) {
        return -2;                      /* 半字编程要求 2 字节对齐 */
    }

    (void) HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);

    for (uint32_t i = 0u; i < len; i += 2u) {
        uint16_t hw = (uint16_t) data[i];
        if ((i + 1u) < len) {
            hw |= (uint16_t) ((uint16_t) data[i + 1u] << 8);
        } else {
            hw |= 0xFF00u;              /* 尾部单字节：高位补 0xFF（擦除态） */
        }

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, (uint64_t) hw) != HAL_OK) {
            (void) HAL_FLASH_Lock();
            return -3;
        }
    }

    (void) HAL_FLASH_Lock();
    return 0;
}
