/*
 * sbb_version.h -- 镜像内固定的软件指纹（Application SW Fingerprint）
 */
#ifndef SBB_VERSION_H
#define SBB_VERSION_H

#include <stdint.h>

#include "sbb_version_cfg.h"

/* 主机侧（uds_web.py / make_ota_image.py）按镜像偏移 0x200 读取，
 * 结构体必须保持 4 字节对齐、字段顺序不可改动。 */
typedef struct {
    uint8_t  magic[4];      /* "SBBF"                                  0 */
    uint8_t  major;         /*                                         4 */
    uint8_t  minor;         /*                                         5 */
    uint8_t  patch;         /*                                         6 */
    uint8_t  build;         /*                                         7 */
    uint32_t build_date;    /* 0xYYYYMMDD                              8 */
    uint32_t image_crc;     /* 0 = 未回填（打包脚本可写）             12 */
    char     label[20];     /* "SBB-F103C8T6"                         16 */
    uint16_t sw_id;         /* 软件版本 ID                            36 */
    uint16_t rsvd;          /* 对齐                                   38 */
} sbb_fingerprint_t;        /* 40 bytes, 镜像偏移 0x200..0x227 */

#define SBB_FP_MAGIC0 'S'
#define SBB_FP_MAGIC1 'B'
#define SBB_FP_MAGIC2 'B'
#define SBB_FP_MAGIC3 'F'

/* 镜像内固定偏移（相对 app 基址 0x08003800） */
#define SBB_FP_IMAGE_OFFSET  0x200u

extern const sbb_fingerprint_t g_sbb_fingerprint;

/* "1.1.0" 形式，静态缓冲，不做动态分配 */
const char *sbb_fingerprint_str(void);

#endif /* SBB_VERSION_H */
