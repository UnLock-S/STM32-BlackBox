/* ─────────────────────────────────────────────────────────────────────────────
 *  boot_cfg.h —— Bootloader / App 共用的 Flash 分区约定
 * ─────────────────────────────────────────────────────────────────────────────
 *  分区（STM32F103C8T6，64 KB，页大小 1 KB）:
 *
 *    0x08000000 ┌──────────────────────────┐
 *               │ Bootloader     ( 14 KB)  │  永不擦写 —— 掉电不会变砖的根基
 *    0x08003800 ├──────────────────────────┤
 *               │ App            (50 KB)   │  OTA 搬运的目标
 *    0x0800FC00 ├──────────────────────────┤
 *               │ Meta / 升级标志 ( 1 KB)  │  最后一页，单独操作
 *    0x08010000 └──────────────────────────┘
 *
 *  ★ 为什么 Meta 放最后一页而不是紧跟 Bootloader:
 *    STM32F1 的页擦除以 1 KB 为单位，把标志放在独立页里，
 *    "擦 App 区" 永远不会碰到标志 —— 掉电后标志仍在，Bootloader 下次开机重试 ✓
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef SBB_BOOT_CFG_H
#define SBB_BOOT_CFG_H

#include <stdint.h>

#define BOOT_SELF_ADDR        0x08000000u
#define BOOT_SELF_SIZE        (14u * 1024u)

#define BOOT_APP_ADDR         0x08003800u
#define BOOT_APP_MAX_SIZE     (50u * 1024u)      /* 0x08003800 .. 0x0800FC00 */
#define BOOT_APP_END          (BOOT_APP_ADDR + BOOT_APP_MAX_SIZE)

#define BOOT_META_ADDR        0x0800FC00u
#define BOOT_META_PAGE_SIZE   1024u
#define BOOT_PAGE_SIZE        1024u

/* 标志页内容。magic 正确且 state == BOOT_META_STATE_REQUEST 才认为"要求升级"。 */
#define BOOT_META_MAGIC       0x5342424Du        /* 'M''B''B''S' */
#define BOOT_META_STATE_IDLE    0u
#define BOOT_META_STATE_REQUEST 0x5AA5C33Cu

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t img_size;      /* 镜像字节数 */
    uint32_t img_crc;       /* 镜像 CRC32（ASCII 路径下由 App 计算并写入） */
    uint32_t boot_count;    /* Bootloader 启动次数（诊断用） */
    uint32_t hdr_crc;       /* 上面 5 个字段的 CRC32 */
} boot_meta_t;

#endif /* SBB_BOOT_CFG_H */
