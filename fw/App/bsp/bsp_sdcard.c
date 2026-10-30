/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_sdcard.c —— SD 卡底层块驱动（SPI 模式）
 * ─────────────────────────────────────────────────────────────────────────────
 *  协议要点：
 *    · 上电后先给 ≥74 个 CLK（CS 拉高）让卡进入 SPI 模式
 *    · CMD0(GO_IDLE) → R1 应为 0x01
 *    · CMD8(SEND_IF_COND, 0x1AA) → R7；有回应 = SD v2
 *    · CMD55+ACMD41 轮询至 R1=0x00（HCS=1 表示支持高容量）
 *    · CMD58(READ_OCR) 读 CCS 位：1 = SDHC（块地址直用 LBA）
 *    · CMD9(SEND_CSD) 取容量
 *  数据事务：CMD17/18 读，CMD24/25 写，均以 0xFE 起始令牌 / 0xFE 数据令牌。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_sdcard.h"

#include <string.h>

#include "main.h"
#include "bsp_spi.h"
#include "bsp_uart.h"

#define SD_CMD0      0u
#define SD_CMD8      8u
#define SD_CMD9      9u
#define SD_CMD17     17u
#define SD_CMD18     18u
#define SD_CMD24     24u
#define SD_CMD25     25u
#define SD_CMD55     55u
#define SD_CMD58     58u
#define SD_ACMD41    41u

#define SD_TOKEN_DATA_START   0xFEu
#define SD_TOKEN_DATA_RESULT  0xE5u
#define SD_TOKEN_STOP_TRAN    0xFDu

#define SD_R1_READY           0x00u
#define SD_R1_IDLE            0x01u

#define SD_TIMEOUT_MS         500u
#define SD_DUMMY              0xFFu

static bsp_sd_info_t s_info;
static bool          s_inited;

/* ── 基础 SPI 小工具（保持短小，便于日后换成 DMA）── */

static uint8_t spi_rw(uint8_t out)
{
    uint8_t in = SD_DUMMY;
    (void) bsp_spi_xfer(&out, &in, 1u);
    return in;
}

static void spi_tx_ff(unsigned n)
{
    while (n-- > 0u) {
        (void) spi_rw(SD_DUMMY);
    }
}

/* 等待卡把 DO 拉低（R1 起始位）
 * ★ guard 兜底：若 HAL 节拍被屏蔽（见 app_start 的 BASEPRI 说明），
 *   基于 tick 的超时条件不成立，靠迭代上限保证一定会返回，绝不在此死循环。 */
static int wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    uint32_t guard = 0u;

    while ((HAL_GetTick() - t0) < timeout_ms) {
        if (spi_rw(SD_DUMMY) != 0xFFu) {
            return 0;
        }
        if (++guard > 400000u) {
            break;                      /* 兜底退出 */
        }
    }
    return -1;
}

/* 发命令并取 R1 */
static int sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *r1)
{
    uint8_t  buf[6];
    uint8_t  v;

    buf[0] = (uint8_t) (0x40u | cmd);
    buf[1] = (uint8_t) (arg >> 24);
    buf[2] = (uint8_t) (arg >> 16);
    buf[3] = (uint8_t) (arg >> 8);
    buf[4] = (uint8_t) (arg);
    buf[5] = (uint8_t) (crc | 0x01u);   /* 结束位 */

    (void) spi_rw(SD_DUMMY);
    /* ★ 逐字节全双工发送（与已验证成功的 sd raw 一致）*/
    for (uint8_t i = 0u; i < (uint8_t) sizeof(buf); i++) {
        (void) spi_rw(buf[i]);
    }

    /* ★★ 等 R1：逐字节读到第一个非 0xFF 的字节 —— 【它本身就是 R1】✓
     *
     * 原实现用 wait_ready()，它只返回"成功/失败"、把那个字节丢掉了 ✗，
     * 随后再读一次拿到的是下一个空闲字节 0xFF
     * -> R1 恒为 0xFF -> 每次 CMD0 都被判"无响应" ✗
     * （这就是 SD 初始化一直 -1 的真正根因；sd raw 因为是自己逐字节读的，所以能看到 0x01）
     */
    {
        uint32_t t0    = HAL_GetTick();
        uint32_t guard = 0u;

        for (;;) {
            v = spi_rw(SD_DUMMY);
            if (v != 0xFFu) {
                break;                      /* 这就是 R1 ✓ */
            }
            if (((HAL_GetTick() - t0) > SD_TIMEOUT_MS) || (++guard > 400000u)) {
                if (r1 != NULL) { *r1 = 0xFFu; }
                return -1;
            }
        }
        if (r1 != NULL) {
            *r1 = v;
        }
    }
    return 0;
}

static uint8_t crc7_ok(uint8_t cmd) { return (cmd == SD_CMD0) ? 0x95u : 0x01u; }

static void sd_cs(bool assert)
{
    bsp_spi_cs(assert);
    if (!assert) {
        (void) spi_rw(SD_DUMMY);        /* 释放后补 8 个时钟 */
    }
}

/* 读单块 */
static int sd_read_block(uint32_t lba, uint8_t *buf)
{
    uint8_t  r1;
    uint32_t t0;

    if (sd_cmd(SD_CMD17, lba, crc7_ok(SD_CMD17), &r1) != 0 || r1 != SD_R1_READY) {
        return -1;
    }

    /* 等数据令牌 0xFE */
    t0 = HAL_GetTick();
    for (;;) {
        uint8_t v = spi_rw(SD_DUMMY);
        if (v == SD_TOKEN_DATA_START) {
            break;
        }
        if ((HAL_GetTick() - t0) > SD_TIMEOUT_MS) {
            return -2;
        }
    }

    (void) bsp_spi_recv(buf, 512u);
    (void) spi_rw(SD_DUMMY);            /* CRC16，忽略 */
    (void) spi_rw(SD_DUMMY);
    return 0;
}

/* 写单块 */
static int sd_write_block(uint32_t lba, const uint8_t *buf)
{
    uint8_t r1;
    uint8_t resp;

    if (sd_cmd(SD_CMD24, lba, crc7_ok(SD_CMD24), &r1) != 0 || r1 != SD_R1_READY) {
        return -1;
    }

    (void) spi_rw(SD_DUMMY);
    (void) spi_rw(SD_TOKEN_DATA_START);
    (void) bsp_spi_send(buf, 512u);
    (void) spi_rw(SD_DUMMY);            /* 伪 CRC16 */
    (void) spi_rw(SD_DUMMY);

    resp = spi_rw(SD_DUMMY);
    if ((resp & 0x1Fu) != 0x05u) {      /* 0b00101 = Data accepted */
        return -2;
    }

    /* 等卡写完（DO 拉低期间为忙） */
    if (wait_ready(SD_TIMEOUT_MS) != 0) {
        return -3;
    }
    return 0;
}

/* ── 公开接口 ── */

int bsp_sdcard_raw_probe(uint8_t *resp, uint32_t max)
{
    static const uint8_t cmd0[6] = { 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x95u };
    uint32_t i;

    if (resp == NULL || max < 16u) { return -1; }
    memset(resp, 0, max);

    (void) bsp_spi_set_speed(BSP_SPI_SPEED_SLOW);
    bsp_spi_cs(false);
    for (i = 0u; i < 12u; i++) { (void) spi_rw(0xFFu); }
    resp[0] = spi_rw(0xFFu);
    bsp_spi_cs(true);
    for (i = 0u; i < 6u; i++) { resp[1u + i] = spi_rw(cmd0[i]); }
    for (i = 0u; i < 8u; i++) { resp[7u + i] = spi_rw(0xFFu); }
    bsp_spi_cs(false);
    return 0;
}

int bsp_sdcard_init(void)
{
    uint8_t  r1 = 0xFFu;
    uint8_t  r7[4];
    uint32_t t0;

    memset(&s_info, 0, sizeof(s_info));
    s_inited = false;

    /* 慢速 + CS 高 + ≥74 个时钟（上电序列） */
    (void) bsp_spi_set_speed(BSP_SPI_SPEED_SLOW);
    bsp_spi_cs(false);
    spi_tx_ff(10u);

    /* CMD0 → 进 SPI 模式，期望 R1 = 0x01 */
    sd_cs(true);
    {
        int rc0 = sd_cmd(SD_CMD0, 0u, 0x95u, &r1);

        bsp_uart_logf("[sdbg] CMD0 rc=%d R1=0x%02X\r\n", rc0, (unsigned) r1);
        /* ★ CMD0 的 R1 合法值有两个:
         *     0x01 = 卡进入 idle 态（首次上电的常态）
         *     0x00 = 卡本来就已在 idle/transfer 态（例如刚初始化过）
         *   R1 的高 7 位是错误位，必须全 0 ✓
         *   原来的 r1 != 0x01 判据太死 ✗ —— 第二次跑 sd test 就会误判失败 */
        if (rc0 != 0 || (r1 & 0xFEu) != 0u) {
            sd_cs(false);
            return -1;
        }
    }

    /* CMD8 → 判 SD v2 */
    if (sd_cmd(SD_CMD8, 0x1AAu, 0x87u, &r1) == 0 && r1 == SD_R1_IDLE) {
        (void) bsp_spi_recv(r7, 4u);
        if (r7[3] != 0xAAu) {
            sd_cs(false);
            return -2;                  /* 电压不匹配 */
        }
        s_info.type = BSP_SD_TYPE_SD2;
        bsp_uart_logf("[sdbg] CMD8 OK -> SD v2\r\n");
    } else {
        s_info.type = BSP_SD_TYPE_SD1;
        bsp_uart_logf("[sdbg] CMD8 无响应 -> SD v1\r\n");
    }

    /* ACMD41 轮询 */
    t0 = HAL_GetTick();
    {
        uint32_t guard = 0u;
        do {
            if (sd_cmd(SD_CMD55, 0u, crc7_ok(SD_CMD55), &r1) != 0) {
                sd_cs(false);
                return -3;
            }
            if (sd_cmd(SD_ACMD41, (s_info.type == BSP_SD_TYPE_SD1) ? 0u : 0x40000000u,
                       crc7_ok(SD_ACMD41), &r1) != 0) {
                sd_cs(false);
                return -4;
            }
            if ((HAL_GetTick() - t0) > 2000u || ++guard > 4000u) {
                sd_cs(false);
                return -5;              /* 卡一直不 ready（guard 是 tick 失效时的兜底） */
            }
        } while (r1 != SD_R1_READY);
    }

    /* CMD58 → CCS 位（高容量） */
    if (s_info.type == BSP_SD_TYPE_SD2) {
        if (sd_cmd(SD_CMD58, 0u, crc7_ok(SD_CMD58), &r1) == 0 && r1 == SD_R1_READY) {
            (void) bsp_spi_recv(r7, 4u);
            if ((r7[0] & 0x40u) != 0u) {
                s_info.type = BSP_SD_TYPE_SDHC;
            }
        }
    }

    sd_cs(false);

    /* 识别完成 → 升到高速 */
    (void) bsp_spi_set_speed(BSP_SPI_SPEED_FAST);

    /* CMD9 读 CSD 取容量（简化解析，足够显示用；精确值可后续补） */
    {
        uint8_t csd[16];
        sd_cs(true);
        if (sd_cmd(SD_CMD9, 0u, crc7_ok(SD_CMD9), &r1) == 0 && r1 == SD_R1_READY) {
            uint32_t tt;
            for (tt = HAL_GetTick(); (HAL_GetTick() - tt) < SD_TIMEOUT_MS; ) {
                if (spi_rw(SD_DUMMY) == SD_TOKEN_DATA_START) {
                    break;
                }
            }
            (void) bsp_spi_recv(csd, 16u);
            (void) spi_rw(SD_DUMMY);
            (void) spi_rw(SD_DUMMY);

            if (s_info.type == BSP_SD_TYPE_SDHC) {
                uint32_t c_size = ((uint32_t) (csd[7] & 0x3Fu) << 16)
                                | ((uint32_t) csd[8] << 8) | csd[9];
                s_info.blocks = (c_size + 1u) * 1024u;      /* 单位 512 B */
            } else if (s_info.type == BSP_SD_TYPE_SD2) {
                uint32_t c_size = ((uint32_t) (csd[6] & 0x03u) << 10)
                                | ((uint32_t) csd[7] << 2)
                                | ((uint32_t) (csd[8] & 0xC0u) >> 6);
                s_info.blocks = (c_size + 1u) * 1024u;
            } else {
                uint32_t c_size = ((uint32_t) (csd[6] & 0x03u) << 10)
                                | ((uint32_t) csd[7] << 2)
                                | ((uint32_t) (csd[8] & 0xC0u) >> 6);
                uint32_t mult   = (uint32_t) ((csd[9] & 0x03u) << 1)
                                | ((csd[10] & 0x80u) >> 7);
                uint32_t bl_len = csd[5] & 0x0Fu;
                uint32_t n      = (c_size + 1u) << (mult + 2u);
                s_info.blocks = (n * (1u << bl_len)) / 512u;
            }
            s_info.capacity_kb = s_info.blocks / 2u;
        }
        sd_cs(false);
    }

    s_info.ready = true;
    s_inited = true;
    return 0;
}

bool bsp_sdcard_ready(void) { return s_inited; }

const bsp_sd_info_t *bsp_sdcard_info(void) { return &s_info; }

int bsp_sdcard_read(uint32_t lba, uint8_t *buf, uint32_t count)
{
    if (!s_inited || buf == NULL || count == 0u) {
        return -1;
    }
    if (s_info.type != BSP_SD_TYPE_SDHC) {
        lba *= 512u;                    /* 标准容量卡用字节地址 */
    }

    sd_cs(true);
    for (uint32_t i = 0u; i < count; i++) {
        if (sd_read_block(lba + (i * 512u), buf + (i * 512u)) != 0) {
            sd_cs(false);
            return -2;
        }
    }
    sd_cs(false);
    return 0;
}

int bsp_sdcard_write(uint32_t lba, const uint8_t *buf, uint32_t count)
{
    if (!s_inited || buf == NULL || count == 0u) {
        return -1;
    }
    if (s_info.type != BSP_SD_TYPE_SDHC) {
        lba *= 512u;
    }

    sd_cs(true);
    for (uint32_t i = 0u; i < count; i++) {
        if (sd_write_block(lba + (i * 512u), buf + (i * 512u)) != 0) {
            sd_cs(false);
            return -2;
        }
    }
    sd_cs(false);
    return 0;
}

int bsp_sdcard_selftest(void)
{
    int rc;

    bsp_uart_logf("[sd] 自检开始（慢速 %s）\r\n",
                  (bsp_spi_speed() == BSP_SPI_SPEED_SLOW) ? "0.281MHz" : "9MHz");
    rc = bsp_sdcard_init();
    if (rc != 0) {
        bsp_uart_logf("[sd] ✗ 初始化失败，错误码 %d\r\n", rc);
        bsp_uart_logf("      检查: 卡在位? CS=PA4? 上电 74 时钟? 供电 3V3?\r\n");
        return rc;
    }

    bsp_uart_logf("[sd] ✓ 初始化成功\r\n");
    bsp_uart_logf("      类型: %s\r\n",
                  (s_info.type == BSP_SD_TYPE_SDHC) ? "SDHC" :
                  (s_info.type == BSP_SD_TYPE_SD2)  ? "SD v2" : "SD v1");
    bsp_uart_logf("      容量: %lu KB (%lu MB)\r\n",
                  (unsigned long) s_info.capacity_kb,
                  (unsigned long) (s_info.capacity_kb / 1024u));
    bsp_uart_logf("      块数: %lu\r\n", (unsigned long) s_info.blocks);
    return 0;
}
