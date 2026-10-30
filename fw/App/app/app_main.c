/* ─────────────────────────────────────────────────────────────────────────────
 *  app_main.c —— 应用层入口与系统监控任务
 * ─────────────────────────────────────────────────────────────────────────────
 *  由 CubeMX 生成的 Core/Src/main.c 通过 USER CODE 段调用：
 *    · app_start()         —— USER CODE BEGIN RTOS_THREADS（建任务，全静态）
 *    · app_monitor_task()  —— CubeMX 的 defaultTask 任务体
 *
 *  只调用 sched_* 与 bsp_* / app_* —— 不直接碰 RTOS API（分层约定）
 * ───────────────────────────────────────────────────────────────────────────── */

#include "app_main.h"

/* CMSIS device header：提供 NVIC_SystemReset()（G3 栈溢出钩子用） */
#include "stm32f1xx.h"

#include "app_adc.h"
#include "app_can.h"
#include "app_console.h"
#include "app_display.h"
#include "app_log.h"
#include "bsp_clock.h"
#include "bsp_delay.h"
#include "bsp_i2c.h"
#include "bsp_led.h"
#include "bsp_spi.h"
#include "bsp_uart.h"
#include "bsp_wdg.h"
#include "sched.h"

#define APP_VERSION_STR        "0.2.0"

#define HEARTBEAT_STACK_WORDS  384u   /* ★ 256 字时实测余量已降到 26 字 ✗ -> 扩到 384 (1536 B) */   /* ★ 原 192 字实测只剩 26 字（≈86% 已用）✗
                                       *   它内部有 sprintf 风格的 monitor 打印（[mon] tick...）
                                       *   → 余量太紧，扩到 256 字（=1024 B）✓ */
#define HEARTBEAT_PRIORITY     3u
#define HEARTBEAT_PERIOD_MS    500u

#define MONITOR_PERIOD_MS      5000u

static void app_heartbeat_task(void *arg);

/* 清除 BASEPRI（内联汇编，不依赖具体的 CMSIS 头是否导出 __set_BASEPRI）。
 * 等价于 CMSIS 的 __set_BASEPRI(0)：BASE = 0 → 不再屏蔽任何优先级的中断。 */
static inline void app_clear_basepri(void)
{
    __asm volatile ("msr basepri, %0" :: "r" (0u) : "memory");
}

/* 静态存储 + 任务定义（禁止 malloc —— ADR-10，构建期门禁强制） */
SCHED_TASK_DEF(g_app_heartbeat, "hbeat", HEARTBEAT_STACK_WORDS, HEARTBEAT_PRIORITY,
               app_heartbeat_task, 0);

static volatile uint32_t s_heartbeat_count;

static void app_heartbeat_task(void *arg)
{
    (void) arg;

    for (;;) {
        bsp_led_toggle();
        s_heartbeat_count++;
        /* ★ 喂狗放在这里而不是 mon 任务：
         *   本任务周期 500 ms << IWDG 超时 2 s，余量充足；
         *   而 mon 任务周期是 5 s —— 用它喂狗会导致看门狗先复位（踩过这个坑）。 */
        /* ★ G2：任一登记任务超过 2 s 没动 -> 停止喂狗，让 IWDG 复位。
         *   否则只有心跳自己在喂，别的任务卡死永远不会被发现。 */
        if (sched_watchdog_ok() != 0) {
            bsp_uart_logf("[fatal] task '%s' hung -> stop feeding WDT\n",
                          sched_watchdog_name());
        } else {
            bsp_wdg_feed();
        }
        sched_sleep_ms(HEARTBEAT_PERIOD_MS);
    }
}

void app_monitor_task(void const *arg)
{
    (void) arg;

    bsp_uart_logf("\r\n");
    bsp_uart_logf("==================================================\r\n");
    bsp_uart_logf("  SBB · STM32F103C8T6 单板黑匣子   fw %s\r\n", APP_VERSION_STR);
    bsp_uart_logf("  build %s %s\r\n", __DATE__, __TIME__);
    bsp_uart_logf("--------------------------------------------------\r\n");
    bsp_uart_logf("  SYSCLK %lu Hz   HCLK %lu Hz\r\n",
                  (unsigned long) bsp_clock_sysclk(),
                  (unsigned long) bsp_clock_hclk());
    bsp_uart_logf("  PCLK1  %lu Hz   PCLK2 %lu Hz\r\n",
                  (unsigned long) bsp_clock_pclk1(),
                  (unsigned long) bsp_clock_pclk2());
    bsp_uart_logf("  FreeRTOS tick=1000 Hz，全静态分配（无堆、无 malloc）\r\n");
    bsp_uart_logf("==================================================\r\n");

    for (;;) {
        sched_sleep_ms(MONITOR_PERIOD_MS);
        bsp_uart_logf("[mon] tick=%lu hbeat=%lu stack_free=%lu words wdg=%s\r\n",
                      (unsigned long) sched_tick(),
                      (unsigned long) s_heartbeat_count,
                      (unsigned long) sched_stack_free_words(),
                      bsp_wdg_enabled() ? "on" : "off");
    }
}

void app_start(void)
{
    /* ★★ 关键修复：清除 BASEPRI ★★
     *  走查（真机实测）：CubeMX 的 main() 里 osKernelInitialize()/osThreadCreate()
     *  会在 **调度器启动之前** 执行 FreeRTOS 的临界区
     *  (taskENTER_CRITICAL → vPortEnterCritical → portDISABLE_INTERRUPTS)。
     *  而 ARM_CM3 端口的 uxCriticalNesting 初值是 0xAAAAAAAA（port.c:133），
     *  只有 xPortStartScheduler 才把它清 0（port.c:334）。
     *  → 调度器启动前的这一对 enter/exit 里，vPortExitCritical 的
     *    `if (uxCriticalNesting == 0) portENABLE_INTERRUPTS();` 分支**永不成立**，
     *    BASEPRI 就永久留在 configMAX_SYSCALL_INTERRUPT_PRIORITY(=0x50)。
     *  后果：HAL 时基 TIM3 的 NVIC 优先级是 15(0xF0) → 被 0x50 永久屏蔽
     *        → HAL_GetTick() 冻结 → 所有「用 tick 做超时」的循环永远不退出
     *        （实测卡在 bsp_wdg_init 的 LSI 等待里，应用从未启动）。
     *  真机证据：RCC/TIM3 寄存器显示 TIM3 在计数、UIE 已使能、NVIC 中断已挂起(ISPR
     *            bit29=1)但从不被取走；BASEPRI=0x50；uxCriticalNesting=0xAAAAAAAA；
     *            把 TIM3 优先级从 15 改 0 后应用立刻跑起来（心跳/两个 tick 都活了）。
     *
     *  调度器启动后 uxCriticalNesting 归 0，FreeRTOS 自己的临界区配对就正常了，
     *  所以这里清一次即可，之后无需再干预。 */
    app_clear_basepri();

    /* ── 板级初始化（外设本身已由 CubeMX 的 MX_*_Init() 配好）── */
    bsp_clock_init();
    bsp_led_init();
    bsp_uart_init();
    bsp_delay_init();        /* DWT 微秒计时：位翻转时序要用 */
    (void) bsp_spi_init();   /* 默认慢速，SD 卡上电时序要求 */
    (void) bsp_i2c_init();

    /* ── 建应用任务（全静态分配）── */
    /* 顺序：先建能出日志的（SD→CAN→ADC→显示），最后控制台 + 心跳 */
    (void) app_log_init();          /* 探测 SD；不在位也继续（其余功能照跑） */
    if (app_can_start() != 0) {
        bsp_led_on();               /* 常亮 = 报警 */
    }
    if (app_adc_start() != 0) {
        bsp_led_on();
    }
    if (app_display_start() != 0) {
        bsp_led_on();
    }
    if (sched_task_add(&g_app_heartbeat) != 0) {
        bsp_led_on();
    }
    if (app_console_start() != 0) {
        bsp_led_on();
    }

    bsp_uart_puts("[sbb] app_start 完成\r\n");

    /* ── 看门狗最后启动：此时所有任务已建好，喂狗才有意义 ──
     * ⚠ IWDG 一旦启动无法关闭；系统卡死 → 2 s 后自动复位（黑匣子应当如此）。
     *   调试期若不希望被复位，把 App/bsp/bsp_wdg.h 的 BSP_WDG_ENABLE 改 0 重编。
     * 这里再清一次 BASEPRI：任务创建过程中 FreeRTOS 又会进出临界区（见函数开头说明）。 */
    app_clear_basepri();
    (void) bsp_wdg_init();
}

/* ★ G3 栈溢出钩子
 *   configCHECK_FOR_STACK_OVERFLOW = 2 时 FreeRTOS 会在切换任务时检查栈边界，
 *   但 CMSIS 提供的实现是 __weak 空函数 —— 溢出后静默继续，表现为随机跑飞。
 *   这里用强符号覆盖它：先打印任务名，再复位（复位后仍溢出就会形成可见的
 *   日志连环，比随机死机好定位得多）。
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void) xTask;
    bsp_uart_logf("\r\n[fatal] STACK OVERFLOW in task: %s\r\n", pcTaskName);
    for (volatile uint32_t i = 0u; i < 200000u; i++) { }   /* 等日志发完 */
    NVIC_SystemReset();
}

