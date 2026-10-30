/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_clock.h —— 系统时钟（读取实际值，不做配置）
 * ─────────────────────────────────────────────────────────────────────────────
 *  时钟树由 CubeMX 生成的 SystemClock_Config() 配置
 *  （HSE 8 MHz × PLL9 → SYSCLK 72 MHz / HCLK 72 / PCLK1 36 / PCLK2 72）。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_CLOCK_H
#define SBB_BSP_CLOCK_H

#include <stdint.h>

void     bsp_clock_init(void);   /* 用实际寄存器校正 SystemCoreClock */

uint32_t bsp_clock_sysclk(void);
uint32_t bsp_clock_hclk(void);
uint32_t bsp_clock_pclk1(void);
uint32_t bsp_clock_pclk2(void);

#endif /* SBB_BSP_CLOCK_H */
