/* ─────────────────────────────────────────────────────────────────────────────
 *  boot_update.h —— 从 SD 卡上的 SBB_APP.BIN 搬运固件
 * ─────────────────────────────────────────────────────────────────────────────
 *  镜像格式（由 tools/make_ota_image.py 生成，32 B 头 + payload）:
 *    0   : "SBBIMG\0\0"
 *    8   : u32 version       (仅记录)
 *    12  : u32 payload_size
 *    16  : u32 payload_crc32 (与 zlib.crc32 一致)
 *    20  : u32 app_base      (必须 == 0x08003000)
 *    24  : u32 header_crc32  (覆盖 0..23)
 *    28  : u32 reserved
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef SBB_BOOT_UPDATE_H
#define SBB_BOOT_UPDATE_H

#include <stdint.h>

/* 执行升级。返回 0 = 成功（已写入并通过 CRC 校验），<0 = 失败原因。
 * 失败时**不清标志**，下次上电会再试一次 ✓ */
int boot_update_run(void);

#endif /* SBB_BOOT_UPDATE_H */
