/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_sdcard.h —— SD 卡底层块驱动（SPI 模式，无文件系统）
 * ─────────────────────────────────────────────────────────────────────────────
 *  接线：CS=PA4(软件片选/SD_CS)、SCK=PA5、MISO=PA6、MOSI=PA7（.ioc 已配）
 *
 *  ★ 速率策略（SD 规范强制）
 *    初始化阶段 ≤ 400 kHz → 本驱动在 init 里先切到 BSP_SPI_SPEED_SLOW（0.281 MHz），
 *    识别完成后切到 BSP_SPI_SPEED_FAST（9 MHz）。
 *
 *  ⚠ 硬件未验证：无卡在位时 init 会失败并返回负值，代码路径已按此分支写好。
 *    明天插卡后用控制台 `sd init` 实测；若首字节不是 0x01/0x00，多半是片选/引脚
 *    或上电 74 时钟不足。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_SDCARD_H
#define SBB_BSP_SDCARD_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BSP_SD_TYPE_NONE = 0,
    BSP_SD_TYPE_SD1,      /* SD v1.x 标准容量 */
    BSP_SD_TYPE_SD2,      /* SD v2.0 标准容量 */
    BSP_SD_TYPE_SDHC      /* SDHC/SDXC 高容量（块地址） */
} bsp_sd_type_t;

typedef struct {
    bsp_sd_type_t type;
    bool          ready;
    uint32_t      blocks;      /* 总块数（512 B/块） */
    uint32_t      capacity_kb;
} bsp_sd_info_t;

/* 上电初始化。返回 0 成功；负值见错误码注释 */
int  bsp_sdcard_init(void);
/* ★ 原始探测（诊断）：resp[0]=MISO空闲电平，resp[1..]=CMD0 回读字节 */
int  bsp_sdcard_raw_probe(uint8_t *resp, uint32_t max);
bool bsp_sdcard_ready(void);
const bsp_sd_info_t *bsp_sdcard_info(void);

/* 块读写（每块 512 B）。返回 0 成功 */
int  bsp_sdcard_read(uint32_t lba, uint8_t *buf, uint32_t count);
int  bsp_sdcard_write(uint32_t lba, const uint8_t *buf, uint32_t count);

/* 控制台自检：跑一遍 CMD0/CMD8 等并打印过程 */
int  bsp_sdcard_selftest(void);

#endif /* SBB_BSP_SDCARD_H */
