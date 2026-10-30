/* ─────────────────────────────────────────────────────────────────────────────
 *  app_uds.c —— ISO-TP(连续帧) + UDS 服务机
 * ───────────────────────────────────────────────────────────────────────────── */
#include <string.h>

#include "app_uds.h"
#include "bsp_can.h"
#include "bsp_uart.h"
#include "main.h"

/* ── ISO-TP 接收状态 ─────────────────────────────────────────────────────── */
static uint8_t  s_rx_buf[UDS_MAX_MSG];
static uint16_t s_rx_len;          /* 本次消息总长 */
static uint16_t s_rx_got;
static uint8_t  s_rx_seq;
static bool     s_rx_active;
static uint8_t  s_rx_fc_cnt;
static uint8_t  s_rx_bs;            /* 本次流控的 BS（发出的值）*/       /* 距上次流控已收几帧（BS 计数）*/

/* ── ISO-TP 发送状态 ─────────────────────────────────────────────────────── */
static uint8_t  s_tx_buf[UDS_MAX_MSG];
static uint16_t s_tx_len;
static uint16_t s_tx_off;
static uint8_t  s_tx_seq;
static bool     s_tx_wait_fc;

/* ── 会话/安全 ───────────────────────────────────────────────────────────── */
static uint8_t  s_session = 1u;             /* 1=默认 2=编程 */
static bool     s_unlocked;
static uint32_t s_reset_at;                 /* 非 0 = 到点复位 */
static uint32_t s_seed;

/* ── 统计 ───────────────────────────────────────────────────────────────── */
static uint32_t s_req_cnt;
static uint32_t s_nrc_cnt;

/* ★ 诊断计数（J-Link 按符号读，不占 UART）*/
volatile uint32_t g_uds_cf_cnt;
volatile uint32_t g_uds_msg_cnt;
volatile uint32_t g_uds_seq_err;
volatile uint32_t g_uds_last_got;

uint32_t app_uds_seed(void)     { return s_seed; }
uint32_t app_uds_requests(void) { return s_req_cnt; }
uint32_t app_uds_nrc(void)      { return s_nrc_cnt; }

/* ══════════════════════════ ISO-TP 发送 ══════════════════════════ */

static void tp_send_frame(const uint8_t *d, uint8_t n);   /* 前置声明 */

/* ★ 流控帧：BS=4 / STmin=2ms
 *   必须【每收满 BS 帧再发一次】✗ 只在首帧发一次的话，
 *   测试仪发完 4 帧就停下来等 FC -> 双方死等（实测现象：'块间隔的流控帧丢失'）*/
static void tp_send_fc(void)
{
    uint8_t fc[3] = { 0x30u, 0x00u, 0x00u };   /* BS=0: 一次放开，靠测试仪匀速发 */

    /* ★ TX 邮箱满时 bsp_can_send 会失败，之前被忽略 -> FC 丢失
     *   -> 测试仪在 BS=1 下死等。这里重试 50 次（约 5 ms）。 */
    {
        int i;
        uint8_t f8[8];
        for (i = 0; i < 50; i++) {
            memset(f8, 0xCC, sizeof(f8));
            memcpy(f8, fc, 3u);
            if (bsp_can_send(UDS_RSP_ID, false, f8, 8u) == 0) {
                break;
            }
            for (volatile int k = 0; k < 200; k++) { }
        }
    }
    s_rx_bs = 0u;                 /* BS=0: 一次放开，不再续发 FC */
    s_rx_fc_cnt = 0u;
}

static void tp_send_frame(const uint8_t *d, uint8_t n)
{
    uint8_t f[8];
    int i;

    memset(f, 0xCC, sizeof(f));
    memcpy(f, d, (n > 8u) ? 8u : n);

    /* ★ TX 邮箱满时 bsp_can_send 失败，之前被忽略 -> 响应/流控丢失 ✗
     *   实测：下载停在第 5~10 块，而写卡全部成功 -> 丢的是响应 */
    for (i = 0; i < 20; i++) {
        if (bsp_can_send(UDS_RSP_ID, false, f, 8u) == 0) {
            return;
        }
        for (volatile int k = 0; k < 100; k++) { }
    }
}

/* 启动一次响应发送（≤7 B 单帧，>7 B 首帧 + 等流控）*/
static void tp_send(const uint8_t *p, uint16_t n)
{
    uint8_t f[8];

    if (n <= 7u) {
        f[0] = (uint8_t) n;
        memcpy(&f[1], p, n);
        tp_send_frame(f, (uint8_t) (n + 1u));
        return;
    }

    /* 首帧：12 bit 长度 */
    f[0] = (uint8_t) (0x10u | ((n >> 8) & 0x0Fu));
    f[1] = (uint8_t) (n & 0xFFu);
    memcpy(&f[2], p, 6u);
    tp_send_frame(f, 8u);

    memcpy(s_tx_buf, p, n);
    s_tx_len     = n;
    s_tx_off     = 6u;
    s_tx_seq     = 1u;
    s_tx_wait_fc = true;                    /* 等测试仪送流控帧 ✓ */
}

/* 收到流控后把连续帧发完 */
static void tp_pump(void)
{
    uint8_t f[8];

    if (s_tx_wait_fc || s_tx_off >= s_tx_len) {
        return;
    }
    while (s_tx_off < s_tx_len) {
        uint16_t n = (uint16_t) (s_tx_len - s_tx_off);

        if (n > 7u) {
            n = 7u;
        }
        f[0] = (uint8_t) (0x20u | (s_tx_seq & 0x0Fu));
        memset(&f[1], 0xCC, 7u);
        memcpy(&f[1], &s_tx_buf[s_tx_off], n);
        tp_send_frame(f, 8u);
        s_tx_off = (uint16_t) (s_tx_off + n);
        s_tx_seq = (uint8_t) ((s_tx_seq + 1u) & 0x0Fu);
    }
}

/* ══════════════════════════ UDS 服务 ══════════════════════════ */

static void neg(uint8_t sid, uint8_t nrc)
{
    uint8_t r[3];

    r[0] = 0x7Fu;
    r[1] = sid;
    r[2] = nrc;
    s_nrc_cnt++;
    tp_send(r, 3u);
}

static void uds_dispatch(const uint8_t *b, uint16_t n)
{
    uint8_t  sid = b[0];
    static uint8_t rsp[UDS_MAX_MSG];   /* ★ 520 B 放栈上会把 CAN 任务(1KB)撑爆 -> HardFault */
    uint16_t rlen = 0u;

    s_req_cnt++;

    switch (sid) {

    /* ── 10 会话控制 ── */
    case 0x10u:
        if (n < 2u) { neg(sid, 0x13u); return; }
        if (b[1] != 0x01u && b[1] != 0x02u) { neg(sid, 0x12u); return; }
        s_session = b[1];
        if (s_session == 1u) { s_unlocked = false; }
        rsp[0] = 0x50u; rsp[1] = b[1];
        rlen = 2u;
        break;

    /* ── 11 ECU 复位 ── */
    case 0x11u:
        if (n < 2u) { neg(sid, 0x13u); return; }
        rsp[0] = 0x51u; rsp[1] = b[1];
        rlen = 2u;
        s_reset_at = s_reset_at;            /* 见下：发完再置 */
        break;

    /* ── 22 读 DID ── */
    case 0x22u: {
        uint16_t did;
        uint8_t  d[64], dl = 0u;

        if (n < 3u) { neg(sid, 0x13u); return; }
        did = (uint16_t) ((b[1] << 8) | b[2]);
        if (diag_read_did(did, d, &dl) != 0) { neg(sid, 0x31u); return; }
        rsp[0] = 0x62u; rsp[1] = b[1]; rsp[2] = b[2];
        memcpy(&rsp[3], d, dl);
        rlen = (uint16_t) (3u + dl);
        break;
    }

    /* ── 27 安全访问 ── */
    case 0x27u:
        if (n < 2u) { neg(sid, 0x13u); return; }
        if (b[1] == 0x01u) {
            if (diag_security_locked()) { neg(sid, 0x37u); return; }  /* G5 延时锁 */
            s_seed = diag_security_seed();
            rsp[0] = 0x67u; rsp[1] = 0x01u;
            rsp[2] = (uint8_t) (s_seed >> 24); rsp[3] = (uint8_t) (s_seed >> 16);
            rsp[4] = (uint8_t) (s_seed >> 8);  rsp[5] = (uint8_t) (s_seed);
            rlen = 6u;
        } else if (b[1] == 0x02u) {
            {
                int sr = diag_security_check(&b[2], (uint8_t) (n - 2u));
                if (sr == -2) { neg(sid, 0x37u); return; }   /* 延时未到 */
                if (sr != 0)  { neg(sid, 0x35u); return; }   /* key 无效 */
            }
            s_unlocked = true;
            rsp[0] = 0x67u; rsp[1] = 0x02u;
            rlen = 2u;
        } else {
            neg(sid, 0x12u); return;
        }
        break;

    /* ── 31 例程控制 ── */
    case 0x31u: {
        uint16_t rid;
        uint8_t  o[32], ol = 0u;

        if (n < 4u) { neg(sid, 0x13u); return; }
        if (!s_unlocked && s_session == 2u) { neg(sid, 0x33u); return; }
        rid = (uint16_t) ((b[2] << 8) | b[3]);
        if (diag_routine_start(rid, &b[4], (uint8_t) (n - 4u), o, &ol) != 0) {
            neg(sid, 0x31u); return;
        }
        rsp[0] = 0x71u; rsp[1] = b[1]; rsp[2] = b[2]; rsp[3] = b[3];
        memcpy(&rsp[4], o, ol);
        rlen = (uint16_t) (4u + ol);
        break;
    }

    /* ── 34 请求下载 ── */
    case 0x34u:
        if (n < 11u) { neg(sid, 0x13u); return; }
        if (s_session != 2u || !s_unlocked) { neg(sid, 0x33u); return; }
        if (b[1] != 0x00u || b[2] != 0x44u) { neg(sid, 0x31u); return; }
        {
            uint32_t len = ((uint32_t) b[7] << 24) | ((uint32_t) b[8] << 16)
                         | ((uint32_t) b[9] << 8) | (uint32_t) b[10];
            {
                int rc34 = diag_dl_begin(len);
                if (rc34 == -3) { neg(sid, 0x22u); return; }  /* 条件不满足：已有下载在跑 */
                if (rc34 != 0)  { neg(sid, 0x72u); return; }  /* 其他：SD/长度问题 */
            }
            rsp[0] = 0x74u; rsp[1] = 0x20u;
            rsp[2] = (uint8_t) (UDS_BLOCK_SIZE >> 8);
            rsp[3] = (uint8_t) (UDS_BLOCK_SIZE & 0xFFu);
            rlen = 4u;
        }
        break;

    /* ── 36 数据传输 ── */
    case 0x36u:
        if (n < 2u) { neg(sid, 0x13u); return; }
        if (s_session != 2u || !s_unlocked) { neg(sid, 0x33u); return; }
        if (diag_dl_block(b[1], &b[2], (uint16_t) (n - 2u)) != 0) { neg(sid, 0x72u); return; }
        rsp[0] = 0x76u; rsp[1] = b[1];
        rlen = 2u;
        break;

    /* ── 37 传输结束 ── */
    case 0x37u: {
        int rc = diag_dl_finish();

        if (rc != 0) { neg(sid, 0x72u); return; }
        rsp[0] = 0x77u;
        rlen = 1u;
        break;
    }

    /* ── 3E 保持会话 ── */
    case 0x3Eu:
        rsp[0] = 0x7Eu; rsp[1] = (n > 1u) ? b[1] : 0x00u;
        rlen = 2u;
        break;

    default:
        neg(sid, 0x11u);                    /* serviceNotSupported */
        return;
    }

    tp_send(rsp, rlen);

    /* 11 01：响应发完再复位（否则响应被切断）*/
    if (sid == 0x11u) {
        s_reset_at = 1u;                    /* ∈ poll 里延时复位 */
    }
}

/* ══════════════════════════ ISO-TP 接收 ══════════════════════════ */

void app_uds_rx(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    uint8_t pci;

    if (dlc < 1u) {
        return;
    }
    pci = (uint8_t) (data[0] >> 4);


    /* 流控帧（测试仪对我们响应的确认）*/
    if (pci == 0x3u) {
        if ((data[0] & 0x0Fu) == 0x0u) {    /* CTS */
            s_tx_wait_fc = false;
            tp_pump();
        }
        return;
    }

    if (pci == 0x0u) {                       /* 单帧 */
        uint8_t n = (uint8_t) (data[0] & 0x0Fu);

        if (n == 0u || n > 7u || dlc < (uint8_t) (n + 1u)) {
            return;
        }
        uds_dispatch(&data[1], n);
        return;
    }

    if (pci == 0x1u) {                       /* 首帧 */
        uint16_t total;

        if (dlc < 8u) {
            return;
        }
        total = (uint16_t) (((uint16_t) (data[0] & 0x0Fu) << 8) | data[1]);
        if (total < 8u || total > UDS_MAX_MSG) {
            return;
        }
        memcpy(s_rx_buf, &data[2], 6u);
        s_rx_len    = total;
        s_rx_got    = 6u;
        s_rx_seq    = 1u;
        s_rx_active = true;

        tp_send_fc();
        return;
    }

    if (pci == 0x2u) {                       /* 连续帧 */
        uint8_t  sn = (uint8_t) (data[0] & 0x0Fu);
        uint16_t n;

        if (!s_rx_active) {
            return;
        }
        if (sn != s_rx_seq) {                /* 序号错：丢弃整条消息 */
            g_uds_seq_err++;
            s_rx_active = false;
            return;
        }
        n = (uint16_t) (s_rx_len - s_rx_got);
        if (n > 7u) {
            n = 7u;
        }
        if ((uint16_t) (dlc - 1u) < n) {
            n = (uint16_t) (dlc - 1u);
        }
        memcpy(&s_rx_buf[s_rx_got], &data[1], n);
        s_rx_got = (uint16_t) (s_rx_got + n);
        s_rx_seq = (uint8_t) ((s_rx_seq + 1u) & 0x0Fu);

        s_rx_fc_cnt++;
        g_uds_cf_cnt++;

        if (s_rx_got >= s_rx_len) {
            s_rx_active = false;
            g_uds_msg_cnt++;
            uds_dispatch(s_rx_buf, s_rx_len);
        } else if (s_rx_bs != 0u && s_rx_fc_cnt >= s_rx_bs) {
            tp_send_fc();                    /* ★ 每块都续流控，否则双方死等 */
        }
    }
}

void app_uds_poll(void)
{
    if (s_reset_at != 0u) {
        s_reset_at++;
        if (s_reset_at > 30u) {              /* ~30 ms 后复位，留出发送时间 */
            s_reset_at = 0u;
            bsp_uart_puts("[uds] ECU 复位（11 01）\r\n");
            NVIC_SystemReset();
        }
    }
    /* 超时保护：接收半截消息超过 ~2 s 就丢弃 */
    if (s_rx_active) {
        static uint32_t idle;
        idle++;
        if (idle > 2000u) { s_rx_active = false; idle = 0u; }
    }
}
