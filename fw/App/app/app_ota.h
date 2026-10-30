/* ─────────────────────────────────────────────────────────────────────────────
 *  app_ota.h —— SD 卡固件升级（单槽，受 TC-01 的 64 KB 限制）
 * ─────────────────────────────────────────────────────────────────────────────
 *  ⚠⚠ 诚实说明（重要，请先读）
 *
 *  TC-01 实测本芯片 64 KB Flash（0x1FFFF7E0 = 0x0040），**没有 A/B 双槽空间**。
 *  因此真正的「自升级」在当前硬件上做不到安全实现，原因是**架构性**的：
 *
 *    1. F1 在擦/写 Flash 期间无法从被擦写的同一块 Flash 取指令 —— 写 Flash 的
 *       代码必须搬进 RAM 执行（经典 RAM function 方案）。
 *    2. 单槽自升级在擦除后、写完前掉电 = 应用被毁 = 只能 SWD 救回。
 *    3. 要安全就需要 bootloader + 备份区，而 64 KB 放不下两个 30 KB 的应用。
 *
 *  → 本模块当前做到「**探针 + 校验**」这一步（读 SD 上的镜像、验 magic/length/CRC、
 *    报告版本与校验结果），这一部分**是可测且安全的**。
 *    `app_ota_apply()` 明确返回「未实现 + 原因」，**不会去动 Flash** ——
 *    宁可功能缺失也不能在无硬件验证的情况下写坏应用。
 *
 *  → 要真正上线，需要先做一个决策（已列在 docs/CUBEMX-TODO.md 的待决项）：
 *      (a) 换 Flash 更大的 MCU（如 F103RC 256 KB → 双槽 + bootloader）
 *      (b) 保留单槽，接受升级期间掉电变砖的风险，并用 RAM function 实现
 *      (c) 放弃 OTA，只留 SD 日志
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_APP_OTA_H
#define SBB_APP_OTA_H

#include <stdbool.h>
#include <stdint.h>

/* 镜像放在 SD 的这两个块（避开日志区，从卡尾往前放；主机侧按块写入） */
#ifndef APP_OTA_FIRST_LBA
#define APP_OTA_FIRST_LBA   0x00040000u   /* 256 MB 卡上属于尾部预留区 */
#endif

#define APP_OTA_MAGIC       "SBBOTA01"

typedef struct {
    bool     present;        /* SD 上是否存在镜像 */
    bool     magic_ok;
    bool     crc_ok;
    uint32_t version;        /* 镜像里的版本号（单调递增，主机侧写入） */
    uint32_t length;         /* 镜像字节数 */
    uint32_t crc_stored;     /* 镜像头里记录的 CRC32 */
    uint32_t crc_calc;       /* 设备端算出的 CRC32 */
    char     err[40];        /* 失败原因 */
} app_ota_info_t;

/* 读 SD 上的镜像头并流式校验 CRC。返回 0 = 校验通过 */
int  app_ota_probe(void);
const app_ota_info_t *app_ota_info(void);

/* 尝试应用升级。当前返回 -2（未实现，见头文件说明），且不会写 Flash。 */
int  app_ota_apply(void);

void app_ota_report(void);

#endif /* SBB_APP_OTA_H */
