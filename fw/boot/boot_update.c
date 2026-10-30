/* ─────────────────────────────────────────────────────────────────────────────
 *  boot_update.c —— SD 镜像 -> 内部 Flash
 * ───────────────────────────────────────────────────────────────────────────── */
#include <string.h>

#include "boot_update.h"
#include "boot_cfg.h"
#include "boot_flash.h"
#include "boot_fat.h"

#include "main.h"
#include "bsp_uart.h"
#include "bsp_sdcard.h"

#define OTA_FILE_NAME83   "SBB_APP BIN"     /* 11 字节固定宽度 */
#define OTA_HDR_SIZE      32u
#define BLK               512u

typedef struct {
    uint8_t  magic[8];
    uint32_t version;
    uint32_t size;
    uint32_t crc;
    uint32_t app_base;
    uint32_t hdr_crc;
    uint32_t reserved;
} __attribute__((packed)) ota_hdr_t;

static uint8_t s_blk[BLK];
static uint32_t s_stg_base;

int boot_update_run(void)
{
    ota_hdr_t hdr;
    uint32_t  fsize = 0u;
    uint32_t  written = 0u;
    uint32_t  blkidx  = 2u;   /* ★ 显式块号：第 0 块=LBA+0(头)，负载从 LBA+1 开始读
                              *   千万不能用 written/512 算 ✗
                              *   首块只补充 480 字节负载，480/512=0 -> 会重读首块 -> CRC 必错 */
    uint32_t  crc = 0u;
    uint32_t  remain;
    int       rc;

    /* ⓪ SD 必须在这里先初始化 ✗
     *   boot_main.c 里"SD 探测"那段在"升级请求"之后，所以直接调 fat_mount()
     *   时 bsp_sdcard_init() 还没跑过 -> s_inited=false -> 读扇区返回 -1
     *   -> fat_mount rc=-1（真机现象：标志读到了、搬运却报挂载失败）*/
    {
        int i;
        for (i = 0; i < 10; i++) {
            if (bsp_sdcard_init() == 0) {
                break;
            }
            HAL_Delay(200u);
        }
        if (!bsp_sdcard_ready()) {
            bsp_uart_puts("[boot] \u2717 SD \u672a\u5c31\u7eea\uff0c\u65e0\u6cd5\u8bfb\u955c\u50cf\r\n");
            return -9;
        }
    }

    /* ① 直接读【卡尾裸块暂存区】
     *   ★ OTA 下载把镜像写在 LBA (blocks-4096)+1 起，不用文件系统
     *   （之前这里走 FAT32 找 SBB_APP.BIN，与 App 写的裸块对不上 ✗）*/
    {
        const bsp_sd_info_t *si = bsp_sdcard_info();

        if (si == NULL || si->blocks < (4096u * 2u)) {
            bsp_uart_puts("[boot] ✗ SD 信息异常\r\n");
            return -10;
        }
        s_stg_base = si->blocks - 4096u;
        fsize = 0u;
        bsp_uart_logf("[boot] 暂存区 @LBA %lu\r\n", (unsigned long) s_stg_base);
    }

    /* ② 读头 */
    rc = (bsp_sdcard_read(s_stg_base + 1u, s_blk, 1u) == 0) ? 1 : -1;
    if (rc != 1) {
        bsp_uart_logf("[boot] \u2717 \u8bfb\u6587\u4ef6\u5934\u5931\u8d25 rc=%d\r\n", rc);
        return -12;
    }
    memcpy(&hdr, s_blk, sizeof(hdr));

    if (memcmp(hdr.magic, "SBBIMG", 6u) != 0) {
        bsp_uart_puts("[boot] \u2717 \u56fa\u4ef6\u5934\u6807\u8bc6\u4e0d\u5bf9\uff08\u4e0d\u662f SBBIMG\uff09\r\n");
        return -13;
    }
    if (boot_crc32(s_blk, 24u, 0u) != hdr.hdr_crc) {
        bsp_uart_puts("[boot] \u2717 \u56fa\u4ef6\u5934 CRC \u9519\r\n");
        return -14;
    }
    if (hdr.app_base != BOOT_APP_ADDR) {
        bsp_uart_logf("[boot] \u2717 \u56fa\u4ef6\u57fa\u5740 0x%08lX != 0x%08lX\r\n",
                      (unsigned long) hdr.app_base, (unsigned long) BOOT_APP_ADDR);
        return -15;
    }
    if (hdr.size == 0u || hdr.size > BOOT_APP_MAX_SIZE) {
        bsp_uart_logf("[boot] \u2717 \u56fa\u4ef6\u5927\u5c0f\u5f02\u5e38 %lu B (max %lu)\r\n",
                      (unsigned long) hdr.size, (unsigned long) BOOT_APP_MAX_SIZE);
        return -16;
    }

    bsp_uart_logf("[boot] \u56fa\u4ef6: v%lu  %lu B  crc=0x%08lX\r\n",
                  (unsigned long) hdr.version, (unsigned long) hdr.size,
                  (unsigned long) hdr.crc);

    /* ③ 擦除 */
    boot_flash_unlock();

    bsp_uart_puts("[boot] \u64e6\u9664 App \u533a...\r\n");
    rc = boot_flash_erase(BOOT_APP_ADDR, hdr.size);
    if (rc != 0) {
        boot_flash_lock();
        bsp_uart_logf("[boot] \u2717 \u64e6\u9664\u5931\u8d25 rc=%d\r\n", rc);
        return -18;
    }

    /* ④ 写入：文件第一块已读入 s_blk，其中 32..511 是负载开头 */
    bsp_uart_puts("[boot] \u5199\u5165...\r\n");
    {
        uint32_t chunk = BLK - OTA_HDR_SIZE;          /* 480 */

        if (hdr.size < chunk) {
            chunk = hdr.size;
        }
        crc = boot_crc32(&s_blk[OTA_HDR_SIZE], chunk, crc);
        rc = boot_flash_write(BOOT_APP_ADDR + written, &s_blk[OTA_HDR_SIZE], (chunk + 1u) & ~1u);
        if (rc != 0) {
            boot_flash_lock();
            bsp_uart_logf("[boot] \u2717 \u5199\u5165\u5931\u8d25 @0x%08lX rc=%d\r\n",
                          (unsigned long) (BOOT_APP_ADDR + written), rc);
            return -19;
        }
        written += chunk;
    }

    while (written < hdr.size) {
        uint32_t chunk;

        rc = (bsp_sdcard_read(s_stg_base + blkidx, s_blk, 1u) == 0) ? 1 : -1;
        if (rc != 1) {
            boot_flash_lock();
            bsp_uart_logf("[boot] \u2717 \u8bfb\u5361\u5931\u8d25\u5728 %lu/%lu rc=%d\r\n",
                          (unsigned long) written, (unsigned long) hdr.size, rc);
            return -20;
        }

        remain = hdr.size - written;
        chunk  = (remain < BLK) ? remain : BLK;

        crc = boot_crc32(s_blk, chunk, crc);

        rc = boot_flash_write(BOOT_APP_ADDR + written, s_blk, (chunk + 1u) & ~1u);
        if (rc != 0) {
            boot_flash_lock();
            bsp_uart_logf("[boot] \u2717 \u5199\u5165\u5931\u8d25 @0x%08lX rc=%d\r\n",
                          (unsigned long) (BOOT_APP_ADDR + written), rc);
            return -21;
        }
        written += chunk;
        blkidx++;          /* ★ 必须自增：否则永远重复读同一块（CRC 固定错的根因）*/

        if ((written % (8u * 1024u)) == 0u || written == hdr.size) {
            bsp_uart_logf("[boot]   %lu/%lu B  %lu%%\r\n",
                          (unsigned long) written, (unsigned long) hdr.size,
                          (unsigned long) ((written * 100u) / hdr.size));
        }
    }

    boot_flash_lock();

    /* ⑤ 校验 */
    if (crc != hdr.crc) {
        bsp_uart_logf("[boot] \u2717 CRC \u4e0d\u5339\u914d: \u7b97\u51fa 0x%08lX / \u5934\u91cc 0x%08lX\r\n",
                      (unsigned long) crc, (unsigned long) hdr.crc);
        bsp_uart_puts("[boot]   \u6807\u5fd7\u4fdd\u7559\uff0c\u4e0b\u6b21\u4e0a\u7535\u91cd\u8bd5\uff08\u8bf7\u68c0\u67e5\u5361\u91cc\u7684\u6587\u4ef6\uff09\r\n");
        return -22;
    }

    bsp_uart_logf("[boot] \u2713 \u5199\u5165\u5b8c\u6210\u5e76\u6821\u9a8c\u901a\u8fc7  %lu B  crc=0x%08lX\r\n",
                  (unsigned long) written, (unsigned long) crc);
    return 0;
}
