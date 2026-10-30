/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_delay.h —— 微秒级延时与单调计时（DWT 周期计数器）
 * ─────────────────────────────────────────────────────────────────────────────
 *  用途：C0832C/ADC0832 位翻转的时序、SD 卡命令间的小延时。
 *  为什么用 DWT 而不是 SysTick：SysTick 已被 FreeRTOS 占用（configTICK_RATE_HZ），
 *  且 DWT 无需中断、ISR 安全、精度 1 个 CPU 周期。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_DELAY_H
#define SBB_BSP_DELAY_H

#include <stdint.h>

/* 使能 DWT 周期计数器。**必须在 SystemCoreClock 已知之后调用**（app_start 里）。 */
void bsp_delay_init(void);

/* 忙等 us 微秒。ISR 安全。调用前先 bsp_delay_init()。 */
void bsp_delay_us(uint32_t us);

/* 开机以来的微秒计数（单调递增）。⚠ 约 59.6 s 回绕一次（2^32 / 72 MHz）。 */
uint32_t bsp_micros(void);

#endif /* SBB_BSP_DELAY_H */
