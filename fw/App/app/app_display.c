/* ─────────────────────────────────────────────────────────────────────────────
 *  app_display.c —— OLED 状态屏：固件版本 / 运行时长 / ADC / CAN / SD / 日志
 * ─────────────────────────────────────────────────────────────────────────────
 *  每 DISPLAY_PERIOD_MS 刷一屏。OLED 不在位时自动降级（只打一次日志后静默）。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "app_display.h"

#include <stdbool.h>

#include "app_adc.h"
#include "app_can.h"
#include "app_log.h"
#include "bsp_sdcard.h"
#include "bsp_uart.h"
#include "ssd1306.h"
#include "sbb_version.h"
#include "sched.h"

#define DISPLAY_TASK_STACK   320u
#define DISPLAY_TASK_PRIO    3u        /* ★ 1 -> 2：与 CAN 同级时间片轮转
                                              *   原来 1 时被 CAN(1ms 轮询) 饿死，
                                              *   实测屏几秒才刷一次 ✗ */
#define DISPLAY_PERIOD_MS    500u

/* ★ G4 面板自愈
 *   SSD1306 会"假死"：I2C 照样 ACK、写入也不报错，但显示卡住不动。
 *   这类故障读不到任何错误标志，只能靠【周期重初始化】把面板状态机拉回来。
 *   另外：连续出现写入错误时立即重初始化（真·总线故障也能救）。 */
#define OLED_HEAL_PERIOD_MS  (120u * 1000u)   /* 2 分钟一次，重初始化只发 17 字节 */
#define OLED_HEAL_ERR_DELTA  16u              /* 错误数涨了这么多就立刻救 */

static bool s_ready;
static bool s_selftest_done;

/* ★ 刷屏次数（文件作用域，供 uptime 等命令读取，用来量化实际刷新率）*/
static volatile uint32_t s_flush_cnt;
static uint32_t s_last_heal;          /* G4: 上次重初始化的 tick */
static uint32_t s_heal_count;         /* G4: 累计自愈次数（uptime 可查） */
static uint32_t s_err_snapshot;       /* G4: 上次比对时的错误计数 */

uint32_t app_display_heal_count(void)
{
    return s_heal_count;
}

uint32_t app_display_flush_count(void)
{
    return s_flush_cnt;
}

SCHED_TASK_DEF(g_app_display, "disp", DISPLAY_TASK_STACK, DISPLAY_TASK_PRIO,
               app_display_task, 0);

void app_display_task(void *arg)
{
    (void) arg;

    if (ssd1306_init() == 0) {
        s_ready = true;
    } else {
        bsp_uart_puts("[disp] OLED 暂不可用（未接或初始化失败）—— 每 2 s 自动重试\r\n");
    }

    for (;;) {
        sched_alive(9000u);        /* G2: 喂软看门狗（预算 9000 ms）*/
        /* ★ OTA 下载中：整屏显示进度条（由 diag_handlers 更新 g_ota_pct）*/
        {
            extern volatile uint8_t g_ota_pct;
            extern volatile uint32_t g_ota_tick;
            /* ★ 超时自愈：刷写被中断（拔线/上位机崩/复位）时 g_ota_pct
             *   不会有人清零，屏幕会永远卡在进度条。这里用"最后一次进度
             *   更新距今的时长"判断，超过 8 s 就退回普通状态屏。 */
            if (g_ota_pct > 0u &&
                (uint32_t) (sched_tick() - g_ota_tick) > 8000u) {
                g_ota_pct = 0u;
                bsp_uart_puts("[disp] OTA timeout, back to status\r\n");
            }
            if (g_ota_pct > 0u) {
                ssd1306_clear();
                ssd1306_text(0u, 0u, "OTA DOWNLOAD");
                ssd1306_textf(0u, 2u, "%u%%", (unsigned) g_ota_pct);
                ssd1306_bar(0u, 4u, g_ota_pct);
                ssd1306_text(0u, 6u, (g_ota_pct >= 100u) ? "RESET -> BOOT" : "DO NOT POWER OFF");
                ssd1306_flush();
                s_flush_cnt++;
                sched_sleep_ms(150u);
                continue;
            }
        }

        /* ★ 支持热插拔：开机时没接 OLED 也不再放弃。
         *   原实现只在任务启动时 init 一次 → 用户后接上 OLED 必须手动敲 `oled`
         *   才会初始化（真机踩过：接了屏但屏幕一直黑）。 */
        if (!s_ready) {
            if (ssd1306_init() == 0) {
                s_ready = true;
                bsp_uart_puts("[disp] ✓ OLED 已连接，开始刷新状态屏\r\n");
            }
        }

        if (s_ready) {
            /* ★ G4 面板自愈：周期性 + 错误突增两种触发 */
            {
                uint32_t now  = sched_tick();
                uint32_t errs = ssd1306_errors();
                int due = 0;

                if ((uint32_t) (now - s_last_heal) > OLED_HEAL_PERIOD_MS) {
                    due = 1;
                }
                if ((errs - s_err_snapshot) >= OLED_HEAL_ERR_DELTA) {
                    due = 2;
                }
                if (due != 0) {
                    s_last_heal    = now;
                    s_err_snapshot = errs;
                    if (ssd1306_init() == 0) {
                        s_heal_count++;
                        bsp_uart_logf("[disp] OLED heal #%u trigger=%d\r\n",
                                      (unsigned) s_heal_count, due);
                    } else {
                        bsp_uart_logf("[disp] OLED heal FAILED trigger=%d\r\n", due);
                    }
                }
            }

            /* ★ 面板自检（只在首次点亮后跑一次）：全亮 0.8 s → 全黑 0.8 s。
             *   为什么放在这里：状态屏每 500 ms 重绘，手动 `oled fill` 会被立刻覆盖，
             *   用户根本来不及看；而开机自检不依赖串口、只要盯着屏幕就行。
             *   判读：全白出现 = 面板受控、电荷泵/对比度/寻址都对；
             *         全白不出现 = 硬件或初始化顺序问题（不是字形表问题）。 */
            if (!s_selftest_done) {
                s_selftest_done = true;
                bsp_uart_puts("[disp] 面板自检：白 -> 黑 -> 棋盘 -> 边框 -> 状态屏\r\n");

                /* ① 全亮 0.8s：证明面板受控、电荷泵/对比度/寻址都对 */
                ssd1306_fill(0xFFu);
                ssd1306_flush();
                sched_sleep_ms(800u);

                /* ② 全黑 0.8s：证明清屏路径通 */
                ssd1306_fill(0x00u);
                ssd1306_flush();
                sched_sleep_ms(800u);

                /* ③ 静态棋盘格 2s —— 判"滚动"的真伪：
                 *     静止且均匀 → 面板没有硬件滚动，之前看到的"滚动"是逐页写入的错觉 ✓
                 *     持续滚动   → 面板真的在滚动模式（0x2E 无效 → 怀疑不是 SSD1306）✗ */
                ssd1306_fill_checker();
                ssd1306_flush();
                sched_sleep_ms(2000u);

                /* ④ 贴边边框 + 固定文字 2.5s —— 判寻址/位置：
                 *     四条边框完整贴边 → 寻址模式正确 ✓
                 *     缺边/错位/斜移   → 寻址模式或列偏移不对 ✗ */
                ssd1306_clear();
                ssd1306_border();
                ssd1306_text(10u, 2u, "SBB TEST");
                ssd1306_text(10u, 4u, "ABCDEF 0123");
                ssd1306_flush();
                sched_sleep_ms(2500u);

                bsp_uart_logf("[disp] 自检完成（写入失败=%lu 刷屏=%lums）\r\n",
                              (unsigned long) ssd1306_errors(),
                              (unsigned long) ssd1306_last_flush_ms());
            }

            const bsp_sd_info_t *sd = bsp_sdcard_info();

            ssd1306_clear();

            ssd1306_text(0u, 0u, "SBB F103");
            ssd1306_textf(64u, 0u, "v%s", sbb_fingerprint_str());   /* ★ 屏幕可见版本号：改代码后必须同步，
                                                       *   否则无法判断板上跑的是哪一版 */

            /* ⚠ ssd1306_textf 的格式化器只支持 %s/%d/%u/%%，没有 'l' 修饰 —— 用 %u + (unsigned) */
            ssd1306_textf(0u, 1u, "UP %us",
                          (unsigned) (sched_tick() / 1000u));

            ssd1306_textf(0u, 2u, "ADC %u %u",
                          (unsigned) app_adc_last(0u),
                          (unsigned) app_adc_last(1u));

            ssd1306_textf(0u, 3u, "CAN R%u T%u",
                          (unsigned) app_can_rx(),
                          (unsigned) app_can_tx());

            ssd1306_text(0u, 4u, "SD  ");
            if (app_log_ready()) {
                ssd1306_textf(24u, 4u, "%u KB", (unsigned) (sd->capacity_kb));
            } else {
                ssd1306_text(24u, 4u, "NONE");
            }

            /* 第 5 行：日志条数 + 负载数字（两位小数）*/
            ssd1306_textf(0u, 5u, "LOG %u L%u.%u%u%%",
                          (unsigned) app_log_count(),
                          (unsigned) (app_can_load_1e4() / 100u),
                          (unsigned) ((app_can_load_1e4() % 100u) / 10u),
                          (unsigned) (app_can_load_1e4() % 10u));

            /* ★ 第 6 行改为【总线负载】显示（用户需求）：
             *   左边数字 = 负载千分比（如 "LD 12.3%"），右边进度条同步填充 ✓
             *   负载 = 每秒收发帧数 × 每帧约 111 位 / 1 Mbps（近似，看趋势用 ✓） */
            {
                /* ★ 第 6 行：最近 20 秒负载趋势图（整行 20 列）
                 *   每列高度 = 那一秒的负载，按【窗口峰值】自动定标
                 *   -> 低负载（如 1.5%）也能看清波动，比"空进度条"有意义得多
                 *   数组按【从左到右 = 从旧到新】排列，一眼看出趋势方向 */
                uint8_t hv[20];
                uint8_t n    = app_can_hist_n();
                uint8_t vmax = app_can_hist_peak();

                if (n > 20u) { n = 20u; }
                for (uint8_t i = 0u; i < n; i++) {
                    hv[i] = app_can_hist((uint8_t) (n - 1u - i));
                }
                if (vmax < 1u) { vmax = 1u; }
                ssd1306_chart(0u, 6u, hv, n, vmax);
            }

            ssd1306_flush();
            s_flush_cnt++;
        }

        sched_sleep_ms(DISPLAY_PERIOD_MS);
    }
}

int app_display_start(void)
{
    s_ready = false;
    return sched_task_add(&g_app_display);
}

bool app_display_ready(void)
{
    return s_ready;
}
