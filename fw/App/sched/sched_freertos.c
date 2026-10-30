/* ─────────────────────────────────────────────────────────────────────────────
 *  sched_freertos.c —— 调度层抽象在 FreeRTOS 上的实现
 * ─────────────────────────────────────────────────────────────────────────────
 *  该 FreeRTOS 实例由 CubeMX 管理与生成（cubemx/Middlewares/...），
 *  配置为 configSUPPORT_STATIC_ALLOCATION=1 / configSUPPORT_DYNAMIC_ALLOCATION=0。
 *  换 RTOS 时只重写本文件。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "sched.h"

int sched_task_add(const sched_task_def_t *def)
{
    if (def == NULL || def->fn == NULL || def->store.tcb == NULL ||
        def->store.stack == NULL || def->store.stack_words == 0u) {
        return -1;
    }

    TaskHandle_t h = xTaskCreateStatic((TaskFunction_t) def->fn,
                                       def->name,
                                       (uint32_t) def->store.stack_words,
                                       def->arg,
                                       (UBaseType_t) def->priority,
                                       def->store.stack,
                                       def->store.tcb);
    return (h == NULL) ? -1 : 0;
}

void sched_sleep_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    if (t == 0u) {
        t = 1u;                 /* 至少让出一次 CPU，避免忙等 */
    }
    vTaskDelay(t);
}

uint32_t sched_tick(void)
{
    return (uint32_t) xTaskGetTickCount();
}

uint32_t sched_stack_free_words(void)
{
    return (uint32_t) uxTaskGetStackHighWaterMark(NULL);
}

unsigned sched_enter_critical(void)
{
    taskENTER_CRITICAL();
    return 0u;                  /* FreeRTOS 的临界区无需保存 key */
}

void sched_exit_critical(unsigned key)
{
    (void) key;
    taskEXIT_CRITICAL();
}

/* ── G2 多任务软看门狗 ───────────────────────────────────────────────── */
#define SCHED_ALIVE_MAX  6u

static struct {
    TaskHandle_t      h;
    volatile uint32_t t;
    uint32_t          budget;
    const char       *name;
} s_alive[SCHED_ALIVE_MAX];

static const char *s_wd_bad;

void sched_alive(uint32_t budget_ms)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    uint32_t now = sched_tick();
    uint8_t i;

    for (i = 0u; i < SCHED_ALIVE_MAX; i++) {
        if (s_alive[i].h == me) {
            s_alive[i].t      = now;
            s_alive[i].budget = budget_ms;
            return;
        }
    }
    for (i = 0u; i < SCHED_ALIVE_MAX; i++) {
        if (s_alive[i].h == NULL) {
            s_alive[i].h      = me;
            s_alive[i].t      = now;
            s_alive[i].budget = budget_ms;
            s_alive[i].name   = pcTaskGetName(me);
            return;
        }
    }
}

int sched_watchdog_ok(void)
{
    uint32_t now = sched_tick();
    uint8_t i;

    s_wd_bad = NULL;
    for (i = 0u; i < SCHED_ALIVE_MAX; i++) {
        if (s_alive[i].h == NULL) {
            continue;
        }
        if ((uint32_t) (now - s_alive[i].t) > s_alive[i].budget) {
            s_wd_bad = s_alive[i].name;
            return -1;
        }
    }
    return 0;
}

const char *sched_watchdog_name(void)
{
    return s_wd_bad;
}
