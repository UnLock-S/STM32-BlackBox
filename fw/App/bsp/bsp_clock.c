/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_clock.c —— 系统时钟读取（HAL 实现）
 * ─────────────────────────────────────────────────────────────────────────────
 *  目标时钟树（与 .ioc 一致，已由 CubeMX 验证）：
 *      HSE 8 MHz × PLL9 = SYSCLK 72 MHz
 *      AHB  /1 → HCLK  72 MHz
 *      APB1 /2 → PCLK1 36 MHz   ← CAN 位时序的前提
 *      APB2 /1 → PCLK2 72 MHz
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_clock.h"
#include "main.h"

void bsp_clock_init(void)
{
    /* 用实际寄存器重算 SystemCoreClock。若 HSE 起振失败，CubeMX 生成的
       SystemClock_Config() 会进入 Error_Handler()，不会静默回落，因此这里
       主要作用是给调试器/日志提供一致的读数。 */
    SystemCoreClockUpdate();
}

uint32_t bsp_clock_sysclk(void)
{
    return SystemCoreClock;
}

uint32_t bsp_clock_hclk(void)
{
    return HAL_RCC_GetHCLKFreq();
}

uint32_t bsp_clock_pclk1(void)
{
    return HAL_RCC_GetPCLK1Freq();
}

uint32_t bsp_clock_pclk2(void)
{
    return HAL_RCC_GetPCLK2Freq();
}
