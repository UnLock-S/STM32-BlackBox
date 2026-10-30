/* ─────────────────────────────────────────────────────────────────────────────
 *  app_adc.c —— C0832C 周期采样：取值 → 记录到 SD → 可选经 CAN 广播
 * ───────────────────────────────────────────────────────────────────────────── */

#include "app_adc.h"

#include <stdbool.h>
#include <string.h>

#include "app_log.h"
#include "bsp_adc0832.h"
#include "bsp_can.h"
#include "bsp_uart.h"
#include "sched.h"

#define ADC_TASK_STACK     256u
#define ADC_TASK_PRIO      2u
#define ADC_PERIOD_MS      100u     /* 10 Hz 采样 */
#define ADC_AVG_N          4u
#define ADC_CAN_ID         0x100u   /* 广播采样结果的 CAN ID（自定） */
#define ADC_CAN_ENABLE     1

static volatile uint8_t  s_last[2];
static volatile uint32_t s_count;
static volatile int      s_err;

SCHED_TASK_DEF(g_app_adc, "adc", ADC_TASK_STACK, ADC_TASK_PRIO, app_adc_task, 0);

void app_adc_task(void *arg)
{
    (void) arg;

    if (bsp_adc0832_init() != 0) {
        bsp_uart_puts("[adc] ✗ C0832C 初始化失败\r\n");
        s_err = -1;
    }

    for (;;) {
        sched_alive(1000u);        /* G2: 喂软看门狗（预算 1000 ms）*/
        for (uint8_t ch = 0u; ch < 2u; ch++) {
            uint16_t avg = 0u;

            if (bsp_adc0832_read_avg(ch, ADC_AVG_N, &avg) == 0) {
                s_last[ch] = (uint8_t) avg;
                s_count++;
                s_err = 0;
                (void) app_log_adc(ch, (uint8_t) avg);
            } else {
                s_err = -1;
            }
        }

#if ADC_CAN_ENABLE
        /* 把 CH0 采样值广播出去（黑匣子也顺手当个小数据源） */
        if (bsp_can_started()) {
            uint8_t d[2];
            d[0] = 0u;                       /* 通道 */
            d[1] = s_last[0];
            if (bsp_can_send(ADC_CAN_ID, false, d, 2u) == 0) {
                (void) app_log_can(true, ADC_CAN_ID, false, false, d, 2u);
            }
        }
#endif

        sched_sleep_ms(ADC_PERIOD_MS);
    }
}

int app_adc_start(void)
{
    s_last[0] = 0u;
    s_last[1] = 0u;
    s_count = 0u;
    s_err = 0;
    return sched_task_add(&g_app_adc);
}

uint8_t  app_adc_last(uint8_t ch) { return (ch < 2u) ? s_last[ch] : 0u; }
uint32_t app_adc_samples(void)    { return s_count; }
int      app_adc_ok(void)         { return s_err; }
