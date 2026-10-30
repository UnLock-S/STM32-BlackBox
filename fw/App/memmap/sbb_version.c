/*
 * sbb_version.c -- 把版本指纹放进 .sbb_version 段
 *
 * 该段由 sbb_app.ld 固定链接到 0x08003A00（= app 基址 + 0x200），
 * 所以主机工具只要读镜像第 0x200 字节就能拿到版本号，
 * 完全不依赖文件名、也不需要 ELF 符号表。
 */
#include "sbb_version.h"

#define SBB_START_SEC_VERSION
#include "MemMap.h"

SBB_VAR_VERSION const sbb_fingerprint_t g_sbb_fingerprint = {
    .magic      = { SBB_FP_MAGIC0, SBB_FP_MAGIC1, SBB_FP_MAGIC2, SBB_FP_MAGIC3 },
    .major      = SBB_VER_MAJOR,
    .minor      = SBB_VER_MINOR,
    .patch      = SBB_VER_PATCH,
    .build      = SBB_VER_BUILD,
    .build_date = SBB_VER_DATE,
    .image_crc  = 0u,
    .label      = "SBB-F103C8T6",
    .sw_id      = SBB_VER_SW_ID,
    .rsvd       = 0u
};

#define SBB_STOP_SEC_VERSION
#include "MemMap.h"

/* 只读、无动态分配、可重入 */
const char *sbb_fingerprint_str(void)
{
    static char s_buf[16];
    static uint8_t s_made;

    if (s_made == 0u) {
        uint8_t i = 0u;
        uint8_t v[3];
        v[0] = (uint8_t) SBB_VER_MAJOR;
        v[1] = (uint8_t) SBB_VER_MINOR;
        v[2] = (uint8_t) SBB_VER_PATCH;

        for (uint8_t k = 0u; k < 3u; k++) {
            if (v[k] >= 100u) { s_buf[i++] = (char) ('0' + (v[k] / 100u)); }
            if (v[k] >= 10u)  { s_buf[i++] = (char) ('0' + ((v[k] / 10u) % 10u)); }
            s_buf[i++] = (char) ('0' + (v[k] % 10u));
            if (k < 2u) { s_buf[i++] = '.'; }
        }
        s_buf[i] = 0;
        s_made = 1u;
    }
    return s_buf;
}
