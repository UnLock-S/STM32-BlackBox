/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_led.h —— 板载 LED（蓝板 PC13，低电平点亮）
 * ─────────────────────────────────────────────────────────────────────────────
 *  实现基于 HAL，引脚宏来自 CubeMX 生成的 cubemx/Core/Inc/main.h
 *  （LED_Pin / LED_GPIO_Port，由 .ioc 里的标签 LED 自动生成）。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_LED_H
#define SBB_BSP_LED_H

#include <stdbool.h>

void bsp_led_init(void);
void bsp_led_on(void);
void bsp_led_off(void);
void bsp_led_toggle(void);
void bsp_led_set(bool on);

#endif /* SBB_BSP_LED_H */
