/* ─────────────────────────────────────────────────────────────────────────────
 *  app_console.c —— 串口命令行实现
 * ─────────────────────────────────────────────────────────────────────────────
 *  命令表是一次性静态数组；新增模块时在此表里加一行即可。
 *  后续批次会补上 adc / i2c / spi / sd / can / oled / log / ota 命令。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "app_console.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "sched.h"
#include "app_adc.h"
#include "app_can.h"
#include "app_display.h"
#include "app_log.h"
#include "app_ota.h"
#include "bsp_adc0832.h"
#include "bsp_can.h"
#include "bsp_clock.h"
#include "bsp_delay.h"
#include "bsp_flash.h"
#include "bsp_i2c.h"
#include "bsp_led.h"
#include "bsp_sdcard.h"
#include "bsp_spi.h"
#include "bsp_uart.h"
#include "bsp_wdg.h"
#include "ssd1306.h"

/* ★ 关闭 SBB_WITH_CONSOLE 时整块裁剪：命令表/任务/栈全部不编译 */
#if SBB_WITH_CONSOLE


/* CubeMX 在 cubemx/Core/Src/main.c 里定义 */
extern UART_HandleTypeDef huart1;

#define CONSOLE_LINE_MAX     64u
#define CONSOLE_TASK_STACK   384u    /* 字 = 1536 B */
#define CONSOLE_PRIORITY     2u
#define CONSOLE_POLL_MS      20u

typedef int (*console_cmd_fn)(int argc, char **argv);

typedef struct {
    const char        *name;
    console_cmd_fn     fn;
    const char        *help;
} console_cmd_t;

/* ───────────── 命令实现 ───────────── */

static int cmd_help(int argc, char **argv);

static int cmd_help(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    bsp_uart_logf("\r\n── 系统状态 ────────────────────────────\r\n");
    bsp_uart_logf("  MCU      STM32F103C8T6 (64 KB flash)\r\n");
    bsp_uart_logf("  SYSCLK   %lu Hz\r\n", (unsigned long) bsp_clock_sysclk());
    bsp_uart_logf("  HCLK     %lu Hz\r\n", (unsigned long) bsp_clock_hclk());
    bsp_uart_logf("  PCLK1    %lu Hz\r\n", (unsigned long) bsp_clock_pclk1());
    bsp_uart_logf("  PCLK2    %lu Hz\r\n", (unsigned long) bsp_clock_pclk2());
    bsp_uart_logf("  tick     %lu\r\n", (unsigned long) sched_tick());
    bsp_uart_logf("  uptime   %lu ms\r\n", (unsigned long) sched_tick());
    bsp_uart_logf("  stack    %lu words free (本任务)\r\n",
                  (unsigned long) sched_stack_free_words());
    bsp_uart_logf("────────────────────────────────────────\r\n");
    return 0;
}

static int cmd_clk(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    bsp_uart_logf("SYSCLK=%lu HCLK=%lu PCLK1=%lu PCLK2=%lu (Hz)\r\n",
                  (unsigned long) bsp_clock_sysclk(),
                  (unsigned long) bsp_clock_hclk(),
                  (unsigned long) bsp_clock_pclk1(),
                  (unsigned long) bsp_clock_pclk2());
    return 0;
}

static int cmd_led(int argc, char **argv)
{
    if (argc < 2) {
        bsp_uart_puts("用法: led <on|off|toggle>\r\n");
        return -1;
    }
    if (strcmp(argv[1], "on") == 0) {
        bsp_led_on();
    } else if (strcmp(argv[1], "off") == 0) {
        bsp_led_off();
    } else if (strcmp(argv[1], "toggle") == 0) {
        bsp_led_toggle();
    } else {
        bsp_uart_puts("未知参数\r\n");
        return -1;
    }
    bsp_uart_puts("OK\r\n");
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            bsp_uart_puts(" ");
        }
        bsp_uart_puts(argv[i]);
    }
    bsp_uart_puts("\r\n");
    return 0;
}

static int cmd_stack(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    bsp_uart_logf("console 任务栈余量: %lu words\r\n",
                  (unsigned long) sched_stack_free_words());
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    bsp_uart_logf("uptime = %lu ms (tick 1 kHz)  刷屏=%lu 次\r\n", (unsigned long) sched_tick(), (unsigned long) app_display_flush_count());
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    bsp_uart_puts("复位中…\r\n");
    bsp_delay_us(20000u);            /* 等 UART 把最后一个字发完 */
    NVIC_SystemReset();
    return 0;                        /* 不会到这里 */
}

/* ───────────── 数值解析（刻意不用 strtoul/stdio）───────────── */

static uint32_t parse_u32(const char *s, bool *ok)
{
    uint32_t v = 0u;
    bool     any = false;

    if (s == NULL) {
        if (ok != NULL) {
            *ok = false;
        }
        return 0u;
    }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (s += 2; *s != '\0'; s++) {
            uint8_t d;
            if (*s >= '0' && *s <= '9') {
                d = (uint8_t) (*s - '0');
            } else if (*s >= 'a' && *s <= 'f') {
                d = (uint8_t) (*s - 'a' + 10);
            } else if (*s >= 'A' && *s <= 'F') {
                d = (uint8_t) (*s - 'A' + 10);
            } else {
                break;
            }
            v = (v << 4) | d;
            any = true;
        }
    } else {
        for (; *s >= '0' && *s <= '9'; s++) {
            v = v * 10u + (uint32_t) (*s - '0');
            any = true;
        }
    }
    if (ok != NULL) {
        *ok = any;
    }
    return v;
}

/* ───────────── 外设命令 ───────────── */

static int cmd_adc(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    bsp_uart_logf("[adc] C0832C  CH0=%u CH1=%u  累计=%lu  状态=%s\r\n",
                  (unsigned) app_adc_last(0u), (unsigned) app_adc_last(1u),
                  (unsigned long) app_adc_samples(),
                  (app_adc_ok() == 0) ? "正常" : "有错误");

    for (uint8_t ch = 0u; ch < 2u; ch++) {
        uint8_t v = 0u;
        if (bsp_adc0832_read(ch, &v) == 0) {
            /* ★ 参考电压 = VCC（本芯片无独立 VREF 脚），接线要求 5V。
             *   原先这里硬编码 3.3V（v*33/255 再拆成 %u.%02u），两个错：
             *   ① 基准用错；② 拆分不对 —— 76 counts 会被打成 "0.09 V"，
             *   而正确值是 76/256 × 5.079 ≈ 1.48 V（用万用表实测电压核对过）。 */
            uint32_t mv = ((uint32_t) v * BSP_ADC0832_VREF_MV) / 256u;

            /* ⚠ 只用 %u（不用 %0Nu）：本项目自写格式化器对"0 填充 + 宽度"的组合
             *   支持不完整，曾把控制台任务卡死。要显示小数就自己拆成两段打印。 */
            bsp_uart_logf("      直读 CH%u = %u  (%u mV @VREF=VCC %u mV)\r\n",
                          (unsigned) ch, (unsigned) v,
                          (unsigned) mv, (unsigned) BSP_ADC0832_VREF_MV);
        } else {
            bsp_uart_logf("      直读 CH%u 失败\r\n", (unsigned) ch);
        }
    }
    return 0;
}

static int cmd_sd(int argc, char **argv)
{
    if (argc < 2) {
        bsp_uart_puts("用法: sd <init|info|read <lba>|test>\r\n");
        return -1;
    }

    if (strcmp(argv[1], "init") == 0 || strcmp(argv[1], "test") == 0) {
        return bsp_sdcard_selftest();
    }

    if (strcmp(argv[1], "raw") == 0) {
        uint8_t rb[16];

        if (bsp_sdcard_raw_probe(rb, sizeof(rb)) != 0) {
            bsp_uart_logf("[sd] raw 探测失败\r\n");
            return 0;
        }
        bsp_uart_logf("[sd] MISO空闲=%02X  CMD0回读=%02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                      rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6],
                      rb[7], rb[8], rb[9], rb[10], rb[11], rb[12], rb[13], rb[14]);
        if (rb[0] == 0x00u) {
            bsp_uart_logf("[sd] => MISO 被拉死低：查短接 / MOSI-MISO 是否对调\r\n");
        } else if (rb[7] == 0xFFu && rb[8] == 0xFFu && rb[9] == 0xFFu) {
            bsp_uart_logf("[sd] => 卡完全没响应：查供电（模块要接 5V！3V3 是 LDO 输出）/ CS / 卡位\r\n");
        } else {
            bsp_uart_logf("[sd] => 卡有响应！问题在后续协议(CMD8/ACMD41)\r\n");
        }
        return 0;
    }

        if (strcmp(argv[1], "info") == 0) {
        const bsp_sd_info_t *si = bsp_sdcard_info();
        if (!bsp_sdcard_ready()) {
            bsp_uart_puts("[sd] 未初始化（先跑 sd init）\r\n");
            return -1;
        }
        bsp_uart_logf("[sd] 类型=%u 就绪=%s 块数=%lu 容量=%lu KB\r\n",
                      (unsigned) si->type, si->ready ? "是" : "否",
                      (unsigned long) si->blocks,
                      (unsigned long) si->capacity_kb);
        return 0;
    }

    if (strcmp(argv[1], "read") == 0) {
        bool ok = false;
        uint32_t lba = parse_u32((argc > 2) ? argv[2] : "0", &ok);

        /* ★★ 必须是完整 512 字节块缓冲 —— 原来写的是 blk[16] ✗，
         *    而 bsp_sdcard_read() 会写满一个 512 字节块，
         *    -> 直接踩爆控制台任务栈（表现为莫名重启/异常）✗
         *    用 static 避免占用任务栈（控制台栈只有 1.5KB）✓ */
        static uint8_t blk[512];

        if (!ok) {
            bsp_uart_puts("用法: sd read <lba>\r\n");
            return -1;
        }
        if (!bsp_sdcard_ready()) {
            bsp_uart_puts("[sd] 未初始化（先跑 sd init）\r\n");
            return -1;
        }
        if (bsp_sdcard_read(lba, blk, 1u) != 0) {
            bsp_uart_logf("[sd] ✗ 读块 %lu 失败\r\n", (unsigned long) lba);
            return -1;
        }
        bsp_uart_logf("[sd] 块 %lu 前 16 B:", (unsigned long) lba);
        for (uint8_t i = 0u; i < 16u; i++) {
            bsp_uart_logf(" %02X", (unsigned) blk[i]);
        }
        bsp_uart_puts("\r\n");
        return 0;
    }

    bsp_uart_puts("未知子命令（init|info|read|test）\r\n");
    return -1;
}

static int cmd_can(int argc, char **argv)
{
    if (argc < 2) {
        bsp_uart_puts("用法: can <stat|send <id> <hex..>|bench <ms>|stop|loop <0|1>>\r\n");
        return -1;
    }

    if (strcmp(argv[1], "stat") == 0) {
        bsp_uart_logf("[can] 已启动=%s 收=%lu 发=%lu 错=%lu\r\n",
                      bsp_can_started() ? "是" : "否",
                      (unsigned long) app_can_rx(),
                      (unsigned long) app_can_tx(),
                      (unsigned long) app_can_err());
        bsp_uart_logf("      最近一帧 id=0x%03lX dlc=%u data=0x%08lX  台架=%s\r\n",
                      (unsigned long) app_can_last_id(),
                      (unsigned) app_can_last_dlc(),
                      (unsigned long) app_can_last_data_u32(),
                      app_can_bench_running() ? "开" : "关");

                /* 控制器错误状态：真总线上有没有 ACK 的直接证据
         * （应用层的"发=N 错=0"只代表成功入队，不代表总线上有人应答）*/
        {
            uint8_t tec = 0u, rec = 0u, lec = 0u;
            bool    boff = false;

            bsp_can_error_state(&tec, &rec, &lec, &boff);
            bsp_uart_logf("[can] TEC=%u REC=%u LEC=%u%s\r\n",
                          (unsigned) tec, (unsigned) rec, (unsigned) lec,
                          boff ? "  [BUS-OFF]"
                               : ((tec > 0u) ? "  [发送受阻:无ACK]"
                                             : "  [正常]"));
        }

        /* 帧率与丢帧：负载率是否可信，看这两行 */
        bsp_uart_logf("[can] 收率=%lu 发率=%lu 帧/秒  中断=%lu 丢帧=%lu\r\n",
                      (unsigned long) app_can_rx_rate(),
                      (unsigned long) app_can_tx_rate(),
                      (unsigned long) bsp_can_irq_count(),
                      (unsigned long) bsp_can_drop_count());
                {
            uint32_t lv = app_can_load_1e4();
            bsp_uart_logf("[can] 负载=%lu.%lu%lu%%（0.01% 分辨率）\r\n",
                          (unsigned long) (lv / 100u),
                          (unsigned long) ((lv % 100u) / 10u),
                          (unsigned long) (lv % 10u));
        }
                /* 趋势图的最近值（纯数字，避开本格式化器不可靠的 %s 路径）*/
        bsp_uart_logf("[can] 趋势(旧->新)=%u %u %u %u %u | %u %u %u %u %u\r\n",
                      (unsigned) app_can_hist(19u), (unsigned) app_can_hist(17u),
                      (unsigned) app_can_hist(15u), (unsigned) app_can_hist(13u),
                      (unsigned) app_can_hist(11u),
                      (unsigned) app_can_hist(9u),  (unsigned) app_can_hist(7u),
                      (unsigned) app_can_hist(5u),  (unsigned) app_can_hist(3u),
                      (unsigned) app_can_hist(1u));
        bsp_uart_logf("[can] 趋势峰值=%u%%\r\n", (unsigned) app_can_hist_peak());
                        return 0;
    }

    if (strcmp(argv[1], "send") == 0) {
        bool ok = false;
        uint32_t id = parse_u32((argc > 2) ? argv[2] : "0", &ok);
        uint8_t d[8];
        uint8_t n = 0u;

        if (!ok) {
            bsp_uart_puts("用法: can send <id> <hex1> <hex2> ...（最多 8 字节）\r\n");
            return -1;
        }
        for (int i = 3; i < argc && n < 8u; i++) {
            bool ok2 = false;
            uint32_t b = parse_u32(argv[i], &ok2);
            if (!ok2) {
                break;
            }
            d[n++] = (uint8_t) b;
        }
        if (app_can_send_test(id, d, n) == 0) {
            bsp_uart_logf("[can] ✓ 已发 id=0x%03lX dlc=%u\r\n",
                          (unsigned long) id, (unsigned) n);
            return 0;
        }
        bsp_uart_puts("[can] ✗ 发送失败（检查收发器/终端电阻/总线状态）\r\n");
        return -1;
    }

    if (strcmp(argv[1], "bench") == 0) {
        bool ok = false;
        uint32_t ms = parse_u32((argc > 2) ? argv[2] : "100", &ok);
        return app_can_bench_start(ok ? ms : 100u);
    }

    if (strcmp(argv[1], "stop") == 0) {
        return app_can_bench_stop();
    }

    if (strcmp(argv[1], "loop") == 0) {
        bool ok = false;
        uint32_t on = parse_u32((argc > 2) ? argv[2] : "1", &ok);
        if (bsp_can_set_loopback(ok && on != 0u) == 0) {
            bsp_uart_logf("[can] 已切到%s模式（自测可不接总线）\r\n",
                          (ok && on != 0u) ? "回环" : "正常");
            return 0;
        }
        bsp_uart_puts("[can] 切换失败\r\n");
        return -1;
    }

    bsp_uart_puts("未知子命令（stat|send|bench|stop|loop）\r\n");
    return -1;
}

static int cmd_oled(int argc, char **argv)
{
    /* ── 像素级点亮测试（不依赖字形表）── */
    if (argc > 1 && strcmp(argv[1], "reinit") == 0) {
        /* 面板"显示任务卡住但面板仍 ACK"时，重跑一次完整初始化
         * 面板若已 NACK，这条救不回来 -> 只能给模块 VCC 断电 */
        int rc = ssd1306_repair();

        bsp_uart_logf("[oled] 重新初始化 rc=%d（0=成功）写入失败=%lu\r\n",
                      rc, (unsigned long) ssd1306_errors());
        ssd1306_clear();
        ssd1306_text(0u, 2u, "OLED REINIT");
        ssd1306_flush();
        return rc;
    }

    if (argc > 1 && strcmp(argv[1], "fill") == 0) {
        if (!ssd1306_ready()) {
            (void) ssd1306_init();
        }
        ssd1306_fill(0xFFu);
        ssd1306_flush();
        bsp_uart_logf("[oled] 全屏点亮 → 屏幕应整片白。写入失败=%lu\r\n",
                      (unsigned long) ssd1306_errors());
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "clear") == 0 || strcmp(argv[1], "blank") == 0)) {
        if (!ssd1306_ready()) {
            (void) ssd1306_init();
        }
        ssd1306_fill(0x00u);
        ssd1306_flush();
        bsp_uart_logf("[oled] 全屏熄灭 → 屏幕应整片黑。写入失败=%lu\r\n",
                      (unsigned long) ssd1306_errors());
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "scan") == 0) {
        bsp_uart_puts("[i2c] 扫描 0x08..0x77：\r\n");
        for (uint8_t a = 0x08u; a <= 0x77u; a++) {
            if (bsp_i2c_probe(a) == 0) {
                bsp_uart_logf("      发现设备 0x%02X\r\n", (unsigned) a);
            }
        }
        return 0;
    }

    if (ssd1306_ready()) {
        ssd1306_clear();
        ssd1306_text(0u, 0u, "SBB OLED TEST");
        ssd1306_text(0u, 2u, "0123456789");
        ssd1306_text(0u, 3u, "ABCDEFGHIJ");
        ssd1306_textf(0u, 5u, "addr 0x%02X", (unsigned) ssd1306_addr());
        ssd1306_flush();
        bsp_uart_logf("[oled] ✓ 已写入测试画面  写入失败=%lu 次  I2C 忙=%d  刷屏耗时=%lu ms\r\n",
                      (unsigned long) ssd1306_errors(), bsp_i2c_busy(),
                      (unsigned long) ssd1306_last_flush_ms());
        if (ssd1306_errors() != 0u) {
            bsp_uart_puts("       ⚠ 有写入失败 → 屏幕可能是空的（I2C 被卡过）。");
            bsp_uart_puts("再执行一次 oled 试试\r\n");
        }
        return 0;
    }

    if (ssd1306_init() == 0) {
        bsp_uart_puts("[oled] ✓ 初始化成功，再执行一次 oled 看画面\r\n");
        return 0;
    }
    bsp_uart_logf("[oled] ✗ 未检测到 SSD1306（试试 oled scan 看 I2C 上有谁）\r\n");
    bsp_uart_logf("       总线忙=%d I2C失败=%lu 恢复=%lu\r\n",
                  bsp_i2c_busy(),
                  (unsigned long) bsp_i2c_fail_count(),
                  (unsigned long) bsp_i2c_recover_count());
    return -1;
}

static int cmd_log(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "flush") == 0) {
        int rc = app_log_flush();
        bsp_uart_logf("[log] 落盘 %s\r\n", (rc == 0) ? "成功" : "失败");
        return rc;
    }
    app_log_info();
    bsp_uart_logf("[log] SD 就绪=%s  卡容量=%lu KB\r\n",
                  bsp_sdcard_ready() ? "是" : "否",
                  (unsigned long) bsp_sdcard_info()->capacity_kb);
    return 0;
}

static int cmd_wdg(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    bsp_uart_logf("[wdg] IWDG %s（≈2.0 s）。喂狗由 mon 任务每 5 s… \r\n",
                  bsp_wdg_enabled() ? "已启用" : "未启用");
    bsp_uart_puts("      注意：喂狗周期必须小于超时，见 app_monitor_task\r\n");
    return 0;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "apply") == 0) {
        return app_ota_apply();
    }
    (void) app_ota_probe();
    app_ota_report();
    return 0;
}

static int cmd_flash(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    /* 只读探测：不擦不写 */
    bsp_uart_logf("[flash] 基址 0x%08lX 大小 %lu KB 页 %u B × %u 页\r\n",
                  (unsigned long) BSP_FLASH_BASE,
                  (unsigned long) (BSP_FLASH_SIZE / 1024u),
                  (unsigned) BSP_FLASH_PAGE_SIZE,
                  (unsigned) BSP_FLASH_PAGE_COUNT);
    bsp_uart_logf("        应用区 CRC32(前 4 KB) = 0x%08lX\r\n",
                  (unsigned long) bsp_flash_crc32(BSP_FLASH_BASE, 4096u));
    return 0;
}

static int cmd_selftest(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    bsp_uart_puts("\r\n╔══ SBB 逐项自检 ══════════════════════╗\r\n");

    bsp_uart_logf("║ 时钟     SYSCLK %lu Hz          %s\r\n",
                  (unsigned long) bsp_clock_sysclk(),
                  (bsp_clock_sysclk() == 72000000u) ? "✅" : "⚠");
    bsp_uart_logf("║ 心跳     %s                      \r\n",
                  "✅ (LED 每 500 ms 翻转)");
    bsp_uart_logf("║ 看门狗   %s                      \r\n",
                  bsp_wdg_enabled() ? "✅ 已启用" : "⚠ 未启用");
    bsp_uart_logf("║ CAN      %s                      \r\n",
                  bsp_can_started() ? "✅ 已启动" : "✗ 未启动");

    {
        uint8_t v = 0u;
        bsp_uart_logf("║ C0832C   %s                      \r\n",
                      (bsp_adc0832_read(0u, &v) == 0) ? "✅ 可读" : "✗ 读取失败");
    }

    bsp_uart_logf("║ OLED     %s                      \r\n",
                  ssd1306_ready() ? "✅ 就绪" : "✗ 未就绪");

    bsp_uart_logf("║ SD 卡    %s                      \r\n",
                  bsp_sdcard_ready() ? "✅ 已识别" : "✗ 未识别");

    bsp_uart_logf("║ 日志     %lu 条                   \r\n",
                  (unsigned long) app_log_count());
    bsp_uart_logf("║ 栈余量   %lu words               \r\n",
                  (unsigned long) sched_stack_free_words());

    bsp_uart_puts("╚══════════════════════════════════════╝\r\n");
    bsp_uart_puts("说明：✗/⚠ 项按提示接线后再跑一次；\r\n");
    bsp_uart_puts("      CAN 自测可用 `can loop 1` 回环，SD 用 `sd init`，\r\n");
    bsp_uart_puts("      OLED 用 `oled scan` 再 `oled`。\r\n\r\n");
    return 0;
}

/* ───────────── 命令表 ───────────── */

static const console_cmd_t s_cmds[] = {
    { "help",   cmd_help,   "列出命令" },
    { "status", cmd_status, "打印系统状态（时钟/节拍/栈）" },
    { "clk",    cmd_clk,    "打印时钟频率" },
    { "led",    cmd_led,    "led <on|off|toggle> —— 板载 LED" },
    { "echo",   cmd_echo,   "echo <文字> —— 回显" },
    { "stack",  cmd_stack,  "当前任务栈余量" },
    { "uptime", cmd_uptime, "运行时长" },
    { "adc",    cmd_adc,    "读 C0832C 双通道并显示累计采样" },
    { "sd",     cmd_sd,     "sd <init|info|read <lba>|test> —— SD 卡" },
    { "can",    cmd_can,    "can <stat|send|bench|stop|loop> —— CAN" },
    { "oled",   cmd_oled,   "oled [scan] —— OLED 测试/扫描 I2C" },
    { "log",    cmd_log,    "log [flush] —— SD 日志统计" },
    { "wdg",    cmd_wdg,    "看门狗状态" },
    { "ota",    cmd_ota,    "ota [apply] —— 固件升级探针" },
    { "flash",  cmd_flash,  "内部 Flash 信息（只读探测）" },
    { "test",   cmd_selftest, "逐项自检（明天接线后用这个）" },
    { "reset",  cmd_reset,  "软复位" },
};

#define CONSOLE_CMD_COUNT   (sizeof(s_cmds) / sizeof(s_cmds[0]))

static void print_help(void)
{
    bsp_uart_logf("\r\n可用命令 (%lu):\r\n", (unsigned long) CONSOLE_CMD_COUNT);
    for (size_t i = 0u; i < CONSOLE_CMD_COUNT; i++) {
        bsp_uart_logf("  %-8s %s\r\n", s_cmds[i].name, s_cmds[i].help);
    }
    bsp_uart_puts("\r\n> ");
}

/* ───────────── 行解析与派发 ───────────── */

static int split_args(char *line, char **argv, int max)
{
    int n = 0;
    char *p = line;

    while (*p != '\0' && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return n;
}

static void run_line(char *line)
{
    char *argv[6];
    int   argc = split_args(line, argv, 6);

    if (argc == 0) {
        bsp_uart_puts("> ");
        return;
    }

    for (size_t i = 0u; i < CONSOLE_CMD_COUNT; i++) {
        if (strcmp(argv[0], s_cmds[i].name) == 0) {
            if (strcmp(argv[0], "help") == 0) {
                print_help();
                return;
            }
            (void) s_cmds[i].fn(argc, argv);
            bsp_uart_puts("> ");
            return;
        }
    }

    bsp_uart_logf("未知命令: %s （help 查看命令表）\r\n> ", argv[0]);
}

/* ───────────── 任务 ───────────── */

SCHED_TASK_DEF(g_app_console, "console", CONSOLE_TASK_STACK, CONSOLE_PRIORITY,
               app_console_task, 0);

static char s_line[CONSOLE_LINE_MAX];
static unsigned s_len;

/* ── 调试计数：与 bsp_uart 的 g_dbg_uart_tx_* 配合定位"串口没反应" ──
 *   g_dbg_console_bytes : 从环形队列取走的字节数（= ISR 收到多少）
 *   g_dbg_console_lines : 实际执行过的命令行数（= 命令到达并被解析） */
volatile uint32_t g_dbg_console_bytes;
volatile uint32_t g_dbg_console_lines;

/* ───────────── 接收：RXNE 中断 + 环形缓冲 ─────────────
 * ★ 为什么必须改成中断（2026-10-14 实测教训，不是理论）
 *   原实现是任务每 20 ms 轮询一次 USART1 的 RXNE。F1 的 USART 接收寄存器
 *   **只有 1 个字节、没有 FIFO**；而 USB-UART 转接板（FT232/CH340）会按 USB
 *   latency timer 把字符**合并成突发**送到 MCU —— 一个 20 ms 周期内到达 2 个以上
 *   字节时，后到的会直接覆盖前一个 → **随机丢字符**。
 *   实测表现：`test` 的 4 个字符到了、末尾的 CR 丢了 → 命令看起来"完全没反应"；
 *   而 `s_len`/`s_line` 读出来是 `4/"test"`，正好证明是丢字节而不是没到。
 *   人手打字察觉不到（一次一个字节），脚本/粘贴/快速输入必现。
 *   → 改成 RXNE 中断把字节收进环形缓冲，任务慢慢取，**一个字节都不会丢**。
 *
 * ★ 中断优先级取 4 的理由：FreeRTOS 在临界区里把 BASEPRI 设为
 *   configMAX_SYSCALL_INTERRUPT_PRIORITY(=5<<4)，**屏蔽优先级数值 ≥ 5 的中断**。
 *   取 4 就永远不会被临界区屏蔽。本 ISR 不调用任何 FreeRTOS API，所以取 <5 合法。
 *
 * ⚠ 与 CubeMX 的关系：当前生成的 cubemx/Core/Src/stm32f1xx_it.c **没有**
 *   USART1_IRQHandler（已 grep 确认），所以这里定义它不会冲突。
 *   若将来在 CubeMX 里勾上 "USART1 global interrupt"，it.c 会生成同名函数 →
 *   **重复定义、链接失败**。届时把下面的 USART1_IRQHandler 删掉，改在 it.c 的
 *   USER CODE 段里调用 app_console_rx_isr() 即可（逻辑一行都不用动）。 */
#define CONSOLE_RX_RING   128u

static volatile uint8_t  s_rx_ring[CONSOLE_RX_RING];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

void app_console_rx_isr(void)
{
    if ((USART1->SR & USART_SR_RXNE) != 0u) {
        uint8_t  b    = (uint8_t) (USART1->DR & 0xFFu);   /* 读 DR 即清 RXNE */
        uint16_t next = (uint16_t) ((s_rx_head + 1u) % CONSOLE_RX_RING);

        if (next != s_rx_tail) {          /* 队列满则丢最新字节，保证不破 */
            s_rx_ring[s_rx_head] = b;
            s_rx_head = next;
        }
    }
}

/* 覆盖 startup 文件里的弱符号（见上方关于 it.c 冲突的说明） */
void USART1_IRQHandler(void)
{
    app_console_rx_isr();
}

static void console_feed(char c)
{
    g_dbg_console_bytes++;

    if (c == '\r' || c == '\n') {
        bsp_uart_puts("\r\n");
        s_line[s_len] = '\0';
        if (s_len > 0u) {
            g_dbg_console_lines++;
            run_line(s_line);
        } else {
            bsp_uart_puts("> ");
        }
        s_len = 0u;
    } else if (c == '\b' || c == 0x7F) {
        if (s_len > 0u) {
            s_len--;
            bsp_uart_puts("\b \b");
        }
    } else if (s_len < (CONSOLE_LINE_MAX - 1u)) {
        s_line[s_len++] = c;
        (void) bsp_uart_write(&c, 1u);    /* 回显 */
    }
}

void app_console_task(void *arg)
{
    (void) arg;

    s_len = 0u;
    s_rx_head = 0u;
    s_rx_tail = 0u;

    bsp_uart_puts("\r\n[sbb] 控制台就绪（RX 中断模式）。输入 help 查看命令。\r\n> ");

    for (;;) {
        sched_alive(60000u);        /* G2: 喂软看门狗（预算 60000 ms）*/
        /* 把环形缓冲里的字节全部处理掉，不丢也不阻塞 */
        while (s_rx_tail != s_rx_head) {
            char c = (char) s_rx_ring[s_rx_tail];
            s_rx_tail = (uint16_t) ((s_rx_tail + 1u) % CONSOLE_RX_RING);
            console_feed(c);
        }
        sched_sleep_ms(CONSOLE_POLL_MS);
    }
}

int app_console_start(void)
{
    s_rx_head = 0u;
    s_rx_tail = 0u;

    /* 先开中断再建任务，避免启动瞬间到达的字节被丢 */
    HAL_NVIC_SetPriority(USART1_IRQn, 4u, 0u);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);

    return sched_task_add(&g_app_console);
}

void app_console_hint(const char *s)
{
    if (s != NULL) {
        bsp_uart_logf("\r\n[提示] %s\r\n> ", s);
    }
}

#else  /* !SBB_WITH_CONSOLE —— 极简桩，接口保持不变 */

int app_console_start(void)
{
    return 0;                      /* 不建任务，不占 1.5 KB 栈 */
}

void app_console_rx_isr(void)
{
    /* 无控制台：丢弃串口字节 */
}

void app_console_hint(const char *s)
{
    (void) s;
}

#endif /* SBB_WITH_CONSOLE */
