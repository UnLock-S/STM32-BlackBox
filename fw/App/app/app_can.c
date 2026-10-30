/* ─────────────────────────────────────────────────────────────────────────────
 *  app_can.c —— CAN 轮询收发任务 + 台架自测
 * ─────────────────────────────────────────────────────────────────────────────
 *  任务每 CAN_POLL_MS 轮询硬件 FIFO（当前未开 CAN NVIC，见 bsp_can.h 说明），
 *  取到的每帧都写进 SD 日志；台架模式再按周期发一帧递增计数器。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "app_can.h"
#include "app_uds.h"

#include <string.h>

#include "app_log.h"
#include "bsp_can.h"
#include "bsp_uart.h"
#include "sched.h"

#define CAN_TASK_STACK     256u
#define CAN_TASK_PRIO      3u    /* ★ 2 -> 3: 显示任务每 900ms 占 400ms 做 I2C，同优先级下 CAN 被饿死 -> UDS 连续帧丢失 */
#define CAN_POLL_MS  1u    /* ★ 原 5ms -> 1ms：轮询取帧上限由 3帧/5ms=600 帧/秒
                            *   提升到 3帧/1ms=3000 帧/秒，覆盖 40% 总线负载 ✓
                            *   （本板 CAN 接收中断不可用，见 bsp_can.c 说明）*/

#define CAN_BENCH_ID       0x200u

static volatile bool     s_bench;
static volatile uint32_t s_bench_period;
static volatile uint32_t s_bench_last;
static volatile uint32_t s_bench_seq;

static volatile uint32_t s_rx;
static volatile uint32_t s_err;
static volatile uint32_t s_last_id;
static volatile uint8_t  s_last_dlc;
static volatile uint32_t s_last_data;

SCHED_TASK_DEF(g_app_can, "can", CAN_TASK_STACK, CAN_TASK_PRIO, app_can_task, 0);

/* ★★ 总线负载估算（用户需求：把 CAN 那行做成负载率显示）
 *
 * bxCAN **没有"总线占用时间"计数器** ✗，所以只能用帧率近似：
 *     负载 ≈ (每秒收发帧数) × (每帧位数) / 波特率
 * 这里按**标准帧 + 8 字节**取 111 位（11 位 ID + 控制+数据+CRC+ACK+帧间隔，
 * 含位填充的保守估计）✓ —— 帧长不同会有偏差，属**近似值**，用途是"看趋势" ✓
 */
static volatile uint32_t s_rx_mfps;      /* 收：毫帧/秒（fps × 1000，给负载率保留精度）*/
static volatile uint32_t s_tx_mfps;      /* 发：毫帧/秒 */
static uint32_t s_rate_tick;
static uint32_t s_rx_mark;
static uint32_t s_tx_mark;
static uint32_t s_bits_mark;          /* ★ 上一秒的累计位长（收发合计）*/
static volatile uint32_t s_load_1e4;  /* ★ 负载，单位 0.01% */
#define CAN_HIST_N  20u               /* ★ 趋势图格数（每秒一格）*/
static volatile uint8_t  s_hist[CAN_HIST_N];   /* 最近 20 秒的负载（0..100 %）*/
static volatile uint8_t  s_hist_head;          /* 最新一格的位置 */
static volatile uint8_t  s_hist_peak;          /* 窗口内峰值（0..100 %）*/

#define CAN_BITRATE              1000000u

static void update_rates(void)
{
    uint32_t now = sched_tick();
    uint32_t d   = (uint32_t) (now - s_rate_tick);

    if (d < 1000u) {
        return;                                  /* 每秒统计一次 */
    }
    {
        uint32_t rx = bsp_can_rx_count();
        uint32_t tx = bsp_can_tx_count();

        s_rx_mfps  = (uint32_t) (((uint64_t) (rx - s_rx_mark) * 1000000u) / d);
        s_tx_mfps  = (uint32_t) (((uint64_t) (tx - s_tx_mark) * 1000000u) / d);
        s_rx_mark  = rx;
        s_tx_mark  = tx;
        /* ★ 负载：这一秒里总线实际被占用的位数 ÷ 1Mbps
         *   1e6 位/秒 = 100%  →  负载(0.01%) = 位数 × 10000 / 1e6 = 位数 / 100 */
        {
            uint32_t bits = (bsp_can_bits_rx() + bsp_can_bits_tx()) - s_bits_mark;

            s_bits_mark = bsp_can_bits_rx() + bsp_can_bits_tx();
            s_load_1e4  = bits / 100u;
            if (s_load_1e4 > 10000u) { s_load_1e4 = 10000u; }
        }

        /* ★ 每秒往趋势图里压一格，并重算窗口峰值（峰值用于自动定标 ✓）*/
        {
            s_hist_head = (uint8_t) ((s_hist_head + 1u) % CAN_HIST_N);
            s_hist[s_hist_head] = (uint8_t) (s_load_1e4 / 100u);
            uint8_t pk = 0u;
            for (uint8_t i = 0u; i < CAN_HIST_N; i++) {
                if (s_hist[i] > pk) { pk = s_hist[i]; }
            }
            s_hist_peak = pk;
        }
        s_rate_tick = now;
    }
}

uint32_t app_can_rx_rate(void) { return s_rx_mfps / 1000u; }
uint32_t app_can_tx_rate(void) { return s_tx_mfps / 1000u; }

/* ★★ 总线负载（单位 0.01%，即 ×10000；上限 10000 = 100.00%）
 *
 *   负载 = (收帧率 + 发帧率) × 111 位 / 1 Mbps
 *   换成 0.01% 单位:
 *     fps × 111 / 1e6 × 10000 = fps × 1.11
 *   用毫帧/秒(mfps)表达:  mfps/1000 × 1.11 = mfps × 111 / 100000
 *
 * 为什么不用整数 fps：1 帧/秒 ≈ 0.011%，用整数 fps 时第二位的分辨率全丢，
 * 小数第二位永远是 0。毫帧/秒保留了 3 位，够撑起 0.01% ✓ */
uint32_t app_can_load_1e4(void)
{
    return s_load_1e4;
}

/* ★ 趋势图取值：i=0 是最新一格，i=n-1 是最早一格 */
uint8_t app_can_hist(uint8_t i)
{
    return (i < CAN_HIST_N) ? s_hist[(s_hist_head + CAN_HIST_N - i) % CAN_HIST_N] : 0u;
}

/* ★ 窗口内峰值（%）：显示端据此自动定标，低负载也能看清波动 */
uint8_t app_can_hist_peak(void) { return s_hist_peak; }

uint8_t app_can_hist_n(void) { return (uint8_t) CAN_HIST_N; }

void app_can_task(void *arg)
{
    (void) arg;

    if (bsp_can_init() != 0) {
        bsp_uart_puts("[can] ✗ CAN 启动失败（检查 1 Mbps 波特率 / PB8-PB9 重映射 / 收发器供电）\r\n");
    } else {
        bsp_uart_puts("[can] ✓ CAN 已启动（1 Mbps，采样点 77.8%）\r\n");
    }

    for (;;) {
        sched_alive(1000u);        /* G2: 喂软看门狗（预算 1000 ms）*/
        bsp_can_frame_t f;

        /* 收：把硬件 FIFO 搬到环形队列，再逐帧取出处理 */
        (void) bsp_can_poll();

        while (bsp_can_recv(&f)) {
            s_rx++;
            s_last_id  = f.id;
            s_last_dlc = f.dlc;
            s_last_data = 0u;
            for (uint8_t i = 0u; i < f.dlc && i < 4u; i++) {
                s_last_data |= (uint32_t) f.data[i] << (8u * i);
            }
            /* ★ UDS 诊断帧不写日志（1 Mbps 刷写时会把 SD 刷爆 ✗）*/
            if (!f.ext && f.id == UDS_REQ_ID) {
                app_uds_rx(f.id, f.data, f.dlc);
                continue;
            }
            (void) app_log_can(false, f.id, f.ext, f.rtr, f.data, f.dlc);
        }

        /* 台架发送 */
        if (s_bench) {
            uint32_t now = sched_tick();
            if ((uint32_t) (now - s_bench_last) >= s_bench_period) {
                uint8_t d[8];
                s_bench_last = now;
                s_bench_seq++;
                memset(d, 0, sizeof(d));
                d[0] = (uint8_t) (s_bench_seq);
                d[1] = (uint8_t) (s_bench_seq >> 8);
                d[2] = (uint8_t) (s_bench_seq >> 16);
                d[3] = (uint8_t) (s_bench_seq >> 24);
                d[4] = 0xA5u;
                if (bsp_can_send(CAN_BENCH_ID, false, d, 8u) == 0) {
                    (void) app_log_can(true, CAN_BENCH_ID, false, false, d, 8u);
                }
            }
        }

        s_err = bsp_can_err_count();
        update_rates();                     /* ★ 每秒刷新收发速率 → 供负载率显示 */
        app_uds_poll();
        sched_sleep_ms(CAN_POLL_MS);
    }
}

int app_can_start(void)
{
    s_bench = false;
    s_bench_period = 100u;
    s_bench_last = 0u;
    s_bench_seq = 0u;
    s_rx = 0u;
    s_err = 0u;
    return sched_task_add(&g_app_can);
}

int app_can_bench_start(uint32_t period_ms)
{
    if (period_ms < 10u) {
        period_ms = 10u;
    }
    s_bench_period = period_ms;
    s_bench_last = sched_tick();
    s_bench = true;
    bsp_uart_logf("[can] 台架发送已启动：每 %lu ms 发一帧 ID=0x%03lX\r\n",
                  (unsigned long) period_ms, (unsigned long) CAN_BENCH_ID);
    return 0;
}

int app_can_bench_stop(void)
{
    s_bench = false;
    bsp_uart_puts("[can] 台架发送已停止\r\n");
    return 0;
}

bool app_can_bench_running(void) { return s_bench; }

int app_can_send_test(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    int rc = bsp_can_send(id, false, data, dlc);

    if (rc == 0) {
        (void) app_log_can(true, id, false, false, data, dlc);
    }
    return rc;
}

uint32_t app_can_rx(void)          { return s_rx; }
uint32_t app_can_tx(void)          { return bsp_can_tx_count(); }
uint32_t app_can_err(void)         { return s_err; }
uint32_t app_can_last_id(void)     { return s_last_id; }
uint8_t  app_can_last_dlc(void)    { return s_last_dlc; }
uint32_t app_can_last_data_u32(void) { return s_last_data; }
