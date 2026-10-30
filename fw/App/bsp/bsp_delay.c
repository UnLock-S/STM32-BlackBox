/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_delay.c —— DWT 周期计数器实现
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_delay.h"
#include "main.h"

#define BSP_DELAY_CYCLES_PER_US   (SystemCoreClock / 1000000u)

void bsp_delay_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* 使能跟踪与调试模块 */
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;              /* 使能周期计数器 */
}

void bsp_delay_us(uint32_t us)
{
    if (us == 0u) {
        return;
    }

    const uint32_t ticks = us * BSP_DELAY_CYCLES_PER_US;
    const uint32_t start = DWT->CYCCNT;

    /* 用无符号相减处理回绕 */
    while ((uint32_t) (DWT->CYCCNT - start) < ticks) {
        /* busy wait */
    }
}

uint32_t bsp_micros(void)
{
    return DWT->CYCCNT / BSP_DELAY_CYCLES_PER_US;
}
