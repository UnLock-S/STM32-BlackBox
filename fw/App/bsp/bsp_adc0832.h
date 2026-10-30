/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_adc0832.h —— C0832C / ADC0832 双通道 8 位 ADC（位翻转驱动）
 * ─────────────────────────────────────────────────────────────────────────────
 *  接线（.ioc 里已配好，标签见 main.h）：
 *    ① CS  → PB12  (C0832_CS)
 *    ② CH0 → 变阻器滑臂（分压：一端 5V、另一端 GND、滑臂→CH0）
 *    ③ CH1 → GND
 *    ⑤ DI  → PB14  (C0832_DI)      ⑥ DO → PB15 (C0832_DO)
 *    ⑦ CLK → PB13  (C0832_CLK)     ⑧ VCC → **5V**（4.5–6.3V；**接 3V3 不工作**）
 *
 *  时序（已在真机上用已知电压校准通过）：
 *    ADC0832 是类 SPI 但非标准协议：起始位 + SGL/DIFF + ODD/SIGN 三位命令，
 *    随后 MSB-first 输出 8 位。**关键在采样沿** —— CLK 下降沿改变数据，
 *    必须在**下降沿之后**采样（上升沿后采样会得到真值的一半，见 bsp_adc0832.c 注释）。
 *    DO 是三态输出，命令阶段为高阻，故 PB15 保持 NOPULL 亦可（驱动只在数据窗口采样）。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_ADC0832_H
#define SBB_BSP_ADC0832_H

#include <stdint.h>

#define BSP_ADC0832_CH0   0u
#define BSP_ADC0832_CH1   1u

/* ★ 参考电压（mV）。**本芯片没有独立 VREF 引脚，基准就是 VCC**，
 *   而 C0832C/ADC0832 要求 5V 供电（4.5–6.3V）→ 基准即 5000 mV。
 *   若换用别的供电，这里必须同步改，否则算出的电压会整体偏。
 *   换算：V = counts × VREF_MV / 256 */
#define BSP_ADC0832_VREF_MV   5000u

/* 半周期（微秒）。ADC0832 手册标称 fCLK = 250 kHz（上限约 400–600 kHz）
 * → 2 µs 半周期 = 250 kHz，留足裕量；一次读约 44 µs，10 Hz 采样毫无压力。 */
#define BSP_ADC0832_HALF_US   2u

int bsp_adc0832_init(void);

/* 读单端通道。ch = BSP_ADC0832_CH0 / CH1。成功返回 0，*value 为 0..255。 */
int bsp_adc0832_read(uint8_t ch, uint8_t *value);

/* 连读 n 次取平均（降噪）。返回 0 成功 */
int bsp_adc0832_read_avg(uint8_t ch, uint8_t n, uint16_t *avg);

#endif /* SBB_BSP_ADC0832_H */
