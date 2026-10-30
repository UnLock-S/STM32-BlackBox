/* ─────────────────────────────────────────────────────────────────────────────
 *  diag_handlers.c —— UDS 数据层（DID / 例程 / 安全 / 暂存区）
 * ─────────────────────────────────────────────────────────────────────────────
 *  ★ 这是「手写实现」层：生成器只生成表，不覆盖本文件 ✓
 * ───────────────────────────────────────────────────────────────────────────── */
#include <string.h>

#include "app_uds.h"
#include "boot_cfg.h"
#include "boot_flash.h"
#include "bsp_sdcard.h"
#include "bsp_uart.h"
#include "main.h"
#include "sched.h"
#include "app_log.h"

#define STG_BLOCKS      4096u          /* 暂存区 2 MB（卡尾）*/
#define STG_CHUNK       512u

extern uint32_t SystemCoreClock;

static uint32_t s_stg_base;            /* 暂存区起始 LBA */
static uint32_t s_dl_size;
static uint32_t s_dl_written;
static uint8_t  s_dl_next_seq;
static uint8_t  s_blk[STG_CHUNK];

/* ★ 诊断计数（J-Link 直读）
 *   enter / pre / post 对比就能定位卡在 SD 写前/中/后 */
volatile uint8_t  g_ota_pct;
volatile uint32_t g_ota_tick;      /* ★ 最后一次进度更新的 tick（OLED 超时回退用）*/      /* ★ OTA 下载进度 0~100，0=非 OTA 状态（供 OLED 显示）*/
volatile uint32_t g_dl_enter;
volatile uint32_t g_dl_pre;
volatile uint32_t g_dl_post;

static uint32_t stg_base(void)
{
    const bsp_sd_info_t *si;

    if (s_stg_base != 0u) {
        return s_stg_base;
    }
    si = bsp_sdcard_info();
    if (si == NULL || si->blocks < (STG_BLOCKS * 2u)) {
        return 0u;
    }
    s_stg_base = si->blocks - STG_BLOCKS;
    return s_stg_base;
}

/* ── DID ─────────────────────────────────────────────────────────────────── */

int diag_read_did(uint16_t did, uint8_t *out, uint8_t *len)
{
    if (out == NULL || len == NULL) {
        return -1;
    }
    switch (did) {

    case 0xF186: {                                  /* App 版本（ASCII）*/
        const char *v = "SBB-APP-0.9.0";
        uint8_t n = (uint8_t) strlen(v);

        memcpy(out, v, n);
        *len = n;
        return 0;
    }

    case 0xF187: {                                  /* Bootloader 版本 */
        const char *v = "SBB-BOOT-0.9.0";
        uint8_t n = (uint8_t) strlen(v);

        memcpy(out, v, n);
        *len = n;
        return 0;
    }

    case 0xF190: {                                  /* 分区信息（16 B）*/
        uint32_t app   = BOOT_APP_ADDR;
        uint32_t amax  = BOOT_APP_MAX_SIZE;
        uint32_t meta  = BOOT_META_ADDR;
        uint32_t stg   = stg_base();

        memcpy(&out[0],  &app,  4u);
        memcpy(&out[4],  &amax, 4u);
        memcpy(&out[8],  &meta, 4u);
        memcpy(&out[12], &stg,  4u);
        *len = 16u;
        return 0;
    }

    case 0xF1A0: {                                  /* 暂存区状态（8 B）*/
        uint32_t size = s_dl_size;
        uint32_t wr   = s_dl_written;

        memcpy(&out[0], &size, 4u);
        memcpy(&out[4], &wr,   4u);
        *len = 8u;
        return 0;
    }

    default:
        return -1;
    }
}

/* ── G5 安全访问（ISO 14229 加固版）────────────────────────────────────
 *  · seed 由 UID96 + tick + 计数混合后 xorshift32 -> 每次不同、每板不同
 *  · key  = CRC32(seed_be || SECRET)，SECRET 在固件里
 *  · 连续 3 次错 key -> 锁 10 s，期间请求 seed 回 NRC 0x37
 *  · 未请求 seed 直接送 key -> NRC 0x35
 * 说明：SECRET 在 Flash 里，SWD 可读 —— 这是"台架级"防护（防误操作/防随手上位机），
 *       不是防逆向。量产要换成每 ECU 唯一密钥 + 服务端签发。
 */
#define SEC_SECRET_0  0x53u   /* 'S' */
#define SEC_SECRET_1  0x42u   /* 'B' */
#define SEC_SECRET_2  0x42u   /* 'B' */
#define SEC_SECRET_3  0x31u   /* '1' */
#define SEC_MAX_FAIL  3u
#define SEC_LOCKOUT_MS 10000u

static uint32_t s_sec_seed;
static uint8_t  s_sec_have_seed;
static uint8_t  s_sec_fail;
static uint32_t s_sec_lock_until;

static uint32_t sec_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < n; i++) {
        c ^= p[i];
        for (uint8_t k = 0u; k < 8u; k++) {
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

int diag_security_locked(void)
{
    return (sched_tick() < s_sec_lock_until) ? 1 : 0;
}

uint32_t diag_security_seed(void)
{
    uint8_t  buf[4];
    uint32_t x = HAL_GetUIDw0() ^ (HAL_GetUIDw1() << 11) ^ (HAL_GetUIDw2() << 3);
    uint32_t now = sched_tick();

    x ^= now;
    x ^= (uint32_t) s_sec_fail << 21;
    x ^= 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;      /* xorshift32 */
    if (x == 0u) { x = 0x12345678u; }

    s_sec_seed = x;
    s_sec_have_seed = 1u;
    memcpy(buf, &x, 4u);
    (void) buf;
    return x;
}

int diag_security_check(const uint8_t *key, uint8_t len)
{
    uint32_t now  = sched_tick();
    uint8_t  buf[8];
    uint32_t want, got;

    if (now < s_sec_lock_until) {
        return -2;                                  /* NRC 0x37 延时未到 */
    }
    if (len < 4u || s_sec_have_seed == 0u) {
        return -1;                                  /* NRC 0x35 */
    }

    buf[0] = (uint8_t) (s_sec_seed >> 24);
    buf[1] = (uint8_t) (s_sec_seed >> 16);
    buf[2] = (uint8_t) (s_sec_seed >> 8);
    buf[3] = (uint8_t) (s_sec_seed);
    buf[4] = SEC_SECRET_0; buf[5] = SEC_SECRET_1;
    buf[6] = SEC_SECRET_2; buf[7] = SEC_SECRET_3;
    want = sec_crc32(buf, 8u);

    got = ((uint32_t) key[0] << 24) | ((uint32_t) key[1] << 16) |
          ((uint32_t) key[2] << 8)  | (uint32_t) key[3];

    s_sec_have_seed = 0u;                            /* seed 一次性 */
    if (got != want) {
        if (++s_sec_fail >= SEC_MAX_FAIL) {
            s_sec_fail = 0u;
            s_sec_lock_until = now + SEC_LOCKOUT_MS;
            bsp_uart_logf("[uds] security: %u fails, lockout 10s\r\n", SEC_MAX_FAIL);
        }
        return -1;
    }
    s_sec_fail = 0u;
    return 0;
}


/* ── 例程 ───────────────────────────────────────────────────────────────── */

int diag_routine_start(uint16_t rid, const uint8_t *in, uint8_t in_len,
                       uint8_t *out, uint8_t *out_len)
{
    (void) in;
    (void) in_len;
    *out_len = 0u;

    switch (rid) {

    case 0xFF00u: {                                 /* 擦暂存区 */
        uint32_t base = stg_base();

        if (base == 0u) {
            return -2;
        }
        /* ★ 用 s_blk（静态）而不是 512 B 栈数组 —— 栈只有 1 KB ✗ */
        memset(s_blk, 0xFF, STG_CHUNK);              /* 清头块 = 暂存区作废 ✓ */
        if (bsp_sdcard_write(base, s_blk, 1u) != 0) {
            return -3;
        }
        diag_dl_abort();                             /* 擦除 = 放弃旧会话 ✓ */
        bsp_uart_logf("[uds] 暂存区已擦除 @LBA %lu\r\n", (unsigned long) base);
        out[0]   = 0x00u;
        *out_len = 1u;
        return 0;
    }

    case 0xFF01u: {                                 /* 校验暂存区镜像 */
        uint32_t base = stg_base();
        uint32_t crc;
        uint8_t *p;

        if (base == 0u) {
            return -2;
        }
        if (s_dl_size == 0u || s_dl_size > 64u * 1024u) {
            return -4;
        }
        if (bsp_sdcard_read(base + 1u, s_blk, 1u) != 0) {
            return -3;
        }
        p = s_blk;                                   /* 镜像头 32 B */
        crc = boot_crc32(p, s_dl_size, 0u);
        bsp_uart_logf("[uds] 校验: %lu B crc=0x%08lX\r\n",
                      (unsigned long) s_dl_size, (unsigned long) crc);
        memcpy(&out[0], &crc, 4u);
        *out_len = 4u;
        return 0;
    }

    default:
        return -1;
    }
}

/* ── 下载：写卡尾裸块 ───────────────────────────────────────────────────── */

/* ★ 31 01 FF00（擦暂存区）等于显式放弃之前的下载会话 */
void diag_dl_abort(void)
{
    s_dl_size    = 0u;
    s_dl_written = 0u;
    s_dl_next_seq = 1u;
    g_ota_pct    = 0u;
}

int diag_dl_begin(uint32_t total)
{
    uint32_t base = stg_base();

    if (base == 0u) {
        bsp_uart_puts("[uds] ✗ SD 不可用，无法下载\r\n");
        return -1;
    }
    /* ★ G1 重入保护：上一次下载还没结束（大小已设、写入未完成）时拒绝新的 34。
     *   没有这道检查，第二个上位机/重发的 34 会把 s_dl_size/written/next_seq
     *   直接覆盖，两个会话的块写进同一暂存区 -> 镜像必然损坏。 */
    /* 只有【活跃】的下载才拒绝：距最后一次进度更新 < 8 s。
     * 中断留下的残留状态（拔线/上位机崩）超时后允许覆盖，
     * 否则一个中断就会把后续所有下载永久锁死。 */
    if ((s_dl_size != 0u) && (s_dl_written < s_dl_size) &&
        ((uint32_t) (sched_tick() - g_ota_tick) < 8000u)) {
        bsp_uart_logf("[uds] ✗ 已有下载在进行 %lu/%lu，拒绝新请求\n",
                      (unsigned long) s_dl_written, (unsigned long) s_dl_size);
        return -3;
    }

    if (total == 0u || total > BOOT_APP_MAX_SIZE) {
        bsp_uart_logf("[uds] ✗ 长度非法 %lu\r\n", (unsigned long) total);
        return -2;
    }
    s_dl_size    = total;
    s_dl_written = 0u;
    s_dl_next_seq = 1u;
    g_ota_pct = 1u;                  /* 进入 OTA，OLED 切到进度屏 */
    g_ota_tick = sched_tick();
    app_log_pause();                 /* ★ 下载期间不写日志，避免与 OTA 争 SD 卡 */
    bsp_uart_logf("[uds] 下载开始 %lu B -> LBA %lu\r\n",
                  (unsigned long) total, (unsigned long) (base + 1u));
    return 0;
}

int diag_dl_block(uint8_t seq, const uint8_t *buf, uint16_t len)
{
    uint32_t base = stg_base();
    uint32_t off  = s_dl_written;
    uint16_t n    = len;

    g_dl_enter++;

    if (base == 0u || buf == NULL || len == 0u) {
        return -1;
    }
    if (seq != s_dl_next_seq) {                      /* 序号必须连续 ✓ */
        bsp_uart_logf("[uds] ✗ 序号错 期望%u 收到%u\r\n", s_dl_next_seq, seq);
        return -2;
    }
    if ((off + n) > s_dl_size) {
        n = (uint16_t) (s_dl_size - off);
    }
    if (n > STG_CHUNK) {
        return -3;
    }

    /* 不足一块补 0xFF（不写入镜像以外的垃圾）*/
    if (n < STG_CHUNK) {
        memset(&s_blk[0], 0xFF, STG_CHUNK);
        memcpy(s_blk, buf, n);
        g_dl_pre++;
        if (bsp_sdcard_write(base + 1u + (off / STG_CHUNK), s_blk, 1u) != 0) {
            return -4;
        }
        g_dl_post++;
    } else {
        g_dl_pre++;
        if (bsp_sdcard_write(base + 1u + (off / STG_CHUNK), buf, 1u) != 0) {
            return -4;
        }
        g_dl_post++;
    }

    s_dl_written = off + n;
    if (s_dl_size != 0u) {
        g_ota_pct = (uint8_t) ((s_dl_written * 100u) / s_dl_size);
        g_ota_tick = sched_tick();
        if (g_ota_pct == 0u) { g_ota_pct = 1u; }
    }
    s_dl_next_seq = (uint8_t) ((seq + 1u) & 0xFFu);
    return 0;
}

int diag_dl_finish(void)
{
    uint32_t base = stg_base();
    boot_meta_t m;
    const uint8_t *h;
    uint32_t hdr_crc;

    if (base == 0u || s_dl_size == 0u) {
        return -1;
    }
    if (s_dl_written != s_dl_size) {
        bsp_uart_logf("[uds] ✗ 长度不符 %lu/%lu\r\n",
                      (unsigned long) s_dl_written, (unsigned long) s_dl_size);
        return -2;
    }

    /* 读回头部并校验，避免把坏镜像交给 bootloader */
    if (bsp_sdcard_read(base + 1u, s_blk, 1u) != 0) {
        return -3;
    }
    h = s_blk;
    if (memcmp(h, "SBBIMG", 6u) != 0) {
        bsp_uart_puts("[uds] ✗ 镜像头不是 SBBIMG\r\n");
        return -4;
    }
    hdr_crc = boot_crc32(h, 24u, 0u);
    if (hdr_crc != *(const uint32_t *) &h[24]) {
        bsp_uart_puts("[uds] ✗ 镜像头 CRC 错\r\n");
        return -5;
    }

    memset(&m, 0, sizeof(m));
    m.magic      = BOOT_META_MAGIC;
    m.state      = BOOT_META_STATE_REQUEST;
    m.img_size   = s_dl_size;
    m.img_crc    = *(const uint32_t *) &h[16];
    m.boot_count = 0u;
    m.hdr_crc    = boot_crc32((const uint8_t *) &m, 20u, 0u);

    boot_flash_unlock();
    if (boot_meta_write(&m, sizeof(m)) != 0) {
        boot_flash_lock();
        return -6;
    }
    boot_flash_lock();

    app_log_resume();
    g_ota_pct = 100u;                /* OLED 显示 100% 直到复位 */
    g_ota_tick = sched_tick();
    bsp_uart_logf("[uds] ✓ 下载完成 %lu B，已置升级标志（复位后由 boot 搬运）\r\n",
                  (unsigned long) s_dl_size);
    return 0;
}
