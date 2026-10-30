/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_led.c —— 板载 LED（HAL 实现）
 * ─────────────────────────────────────────────────────────────────────────────
 *  GPIO 的时钟/模式已由 CubeMX 的 MX_GPIO_Init() 配好（PC13 推挽输出，
 *  初值 GPIO_PIN_SET = 灭）。本文件只做电平操作，不重复初始化。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_led.h"
#include "main.h"          /* LED_Pin / LED_GPIO_Port —— CubeMX 生成 */

/* 蓝板 PC13：阳极接 3V3、阴极接 PC13 → **低电平点亮** */
#define BSP_LED_ACTIVE_LEVEL   GPIO_PIN_RESET

void bsp_led_init(void)
{
    /* GPIO 已由 MX_GPIO_Init() 初始化，这里只把 LED 置于熄灭态 */
    bsp_led_off();
}

void bsp_led_on(void)
{
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, BSP_LED_ACTIVE_LEVEL);
}

void bsp_led_off(void)
{
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin,
                      (BSP_LED_ACTIVE_LEVEL == GPIO_PIN_SET) ? GPIO_PIN_RESET
                                                             : GPIO_PIN_SET);
}

void bsp_led_toggle(void)
{
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
}

void bsp_led_set(bool on)
{
    if (on) {
        bsp_led_on();
    } else {
        bsp_led_off();
    }
}
