/* ─────────────────────────────────────────────────────────────────────────────
 *  app_ota.c —— SD 固件升级：探针 + 校验（应用部分按设计暂缓，见头文件）
 * ───────────────────────────────────────────────────────────────────────────── */

#include "app_ota.h"

#include <string.h>

#include "bsp_sdcard.h"
#include "bsp_uart.h"

#include <string.h>
#include "main.h"
#include "boot_cfg.h"
#include "boot_flash.h"

/* 镜像头（块 0，24 B 有效，其余补零）：
 *   +0  char[8] magic "SBBOTA01"
 *   +8  u32 version
 *   +12 u32 length
 *   +16 u32 crc32（对 payload 逐字节计算，多项式 0xEDB88320）
 *   +20 u32 reserved
 * payload 从 +512（下一块）开始，按 512 B 对齐存放。
 */
#define APP_OTA_HDR_SIZE     24u
#define APP_OTA_BLOCK        512u

static app_ota_info_t s_info;
static uint8_t        s_blk[APP_OTA_BLOCK];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
         | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* ── 设置错误串 ──
 * ★ 为什么不用 snprintf：本工程刻意不引 newlib 的 stdio —— snprintf 会把
 *   _malloc_r/_sbrk 拖进镜像（ADR-10 门禁会直接让构建失败）。所以自己拼。 */
static void set_err(const char *msg)
{
    size_t i = 0u;
    while (msg[i] != '\0' && i < (sizeof(s_info.err) - 1u)) {
        s_info.err[i] = msg[i];
        i++;
    }
    s_info.err[i] = '\0';
}

/* 前缀 + 无符号十进制 */
static void set_err_u32(const char *prefix, uint32_t v)
{
    char tmp[12];
    int  n = 0;

    if (v == 0u) {
        tmp[n++] = '0';
    }
    while (v != 0u && n < 12) {
        tmp[n++] = (char) ('0' + (v % 10u));
        v /= 10u;
    }

    size_t i = 0u;
    while (prefix[i] != '\0' && i < (sizeof(s_info.err) - 1u)) {
        s_info.err[i] = prefix[i];
        i++;
    }
    while (n > 0 && i < (sizeof(s_info.err) - 1u)) {
        s_info.err[i++] = tmp[--n];
    }
    s_info.err[i] = '\0';
}

int app_ota_probe(void)
{
    memset(&s_info, 0, sizeof(s_info));
    set_err("未探测");

    if (!bsp_sdcard_ready()) {
        set_err("SD 卡未就绪");
        return -1;
    }

    /* 读镜像头块 */
    if (bsp_sdcard_read(APP_OTA_FIRST_LBA, s_blk, 1u) != 0) {
        set_err("读镜像头失败");
        return -2;
    }

    if (memcmp(s_blk, APP_OTA_MAGIC, 8u) != 0) {
        set_err("magic 不匹配（卡上无镜像）");
        return -3;
    }
    s_info.magic_ok = true;
    s_info.present  = true;
    s_info.version  = rd_u32(&s_blk[8]);
    s_info.length   = rd_u32(&s_blk[12]);
    s_info.crc_stored = rd_u32(&s_blk[16]);

    if (s_info.length == 0u || s_info.length > (16u * 1024u * 1024u)) {
        set_err_u32("长度异常: ", s_info.length);
        return -4;
    }

    /* 流式算 CRC：按块读 payload */
    {
        uint32_t crc = 0xFFFFFFFFu;
        uint32_t remain = s_info.length;
        uint32_t lba = APP_OTA_FIRST_LBA + 1u;

        while (remain > 0u) {
            uint32_t take = (remain > APP_OTA_BLOCK) ? APP_OTA_BLOCK : remain;

            if (bsp_sdcard_read(lba, s_blk, 1u) != 0) {
                set_err_u32("读 payload 失败 @LBA ", lba);
                return -5;
            }
            crc = crc32_update(crc, s_blk, take);
            remain -= take;
            lba++;
        }
        s_info.crc_calc = ~crc;
    }

    s_info.crc_ok = (s_info.crc_calc == s_info.crc_stored);
    set_err(s_info.crc_ok ? "校验通过" : "CRC 不匹配");

    return s_info.crc_ok ? 0 : -6;
}

const app_ota_info_t *app_ota_info(void)
{
    return &s_info;
}

int app_ota_apply(void)
{
    boot_meta_t m;
    int rc;

    memset(&m, 0, sizeof(m));
    m.magic      = BOOT_META_MAGIC;
    m.state      = BOOT_META_STATE_REQUEST;
    m.boot_count = 0u;
    m.img_size   = 0u;
    m.img_crc    = 0u;
    m.hdr_crc    = boot_crc32((const uint8_t *) &m, 20u, 0u);

    bsp_uart_puts("[ota] 正在写升级标志到 0x0800FC00 ...\r\n");

    boot_flash_unlock();
    rc = boot_meta_write(&m, sizeof(m));
    boot_flash_lock();

    if (rc != 0) {
        bsp_uart_logf("[ota] ✗ 写标志失败 rc=%d\r\n", rc);
        return -3;
    }

    bsp_uart_puts("[ota] ✓ 标志已写，2 秒后软复位进入 bootloader\r\n");
    bsp_uart_puts("[ota]   bootloader 会从卡根目录读 SBB_APP.BIN 并搬运\r\n");
    HAL_Delay(2000u);
    NVIC_SystemReset();
    return 0;
}

void app_ota_report(void)
{
    const app_ota_info_t *o = app_ota_info();

    bsp_uart_puts("── OTA 探针 ────────────────────────────\r\n");
    bsp_uart_logf("  镜像 LBA   %lu\r\n", (unsigned long) APP_OTA_FIRST_LBA);
    bsp_uart_logf("  存在       %s\r\n", o->present ? "是" : "否");
    if (o->present) {
        bsp_uart_logf("  版本       %lu\r\n", (unsigned long) o->version);
        bsp_uart_logf("  长度       %lu B\r\n", (unsigned long) o->length);
        bsp_uart_logf("  CRC(头)    0x%08lX\r\n", (unsigned long) o->crc_stored);
        bsp_uart_logf("  CRC(算)    0x%08lX\r\n", (unsigned long) o->crc_calc);
        bsp_uart_logf("  校验       %s\r\n", o->crc_ok ? "✓ 通过" : "✗ 失败");
    }
    bsp_uart_logf("  说明       %s\r\n", o->err);
    bsp_uart_puts("────────────────────────────────────────\r\n");
}
