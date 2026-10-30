/* ─────────────────────────────────────────────────────────────────────────────
 *  ssd1306.h —— 0.96" OLED（SSD1306，I2C 128x64）
 * ─────────────────────────────────────────────────────────────────────────────
 *  接线：SCL=PB6、SDA=PB7（I2C1 **100 kHz**，.ioc 已配）。默认从机地址 0x3C。
 *  ⚠ PB6/PB7 是开漏且无内部上拉 → 上拉靠 OLED 模块板载电阻。
 *  提供 page-based 帧缓冲 + 5x7 点阵文字（缓冲区 128*64/8 = 1024 B）。
 *
 *  ⚠ 字形表是经典 5x7 公有领域点阵，仅覆盖数字/大写字母/常用符号；
 *    小写字母按大写映射。明天上电后**请目视核对字形**，如需扩充再加表项。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_SSD1306_H
#define SBB_SSD1306_H

#include <stdbool.h>
#include <stdint.h>

#define SSD1306_WIDTH      128u
#define SSD1306_HEIGHT     64u
#define SSD1306_I2C_ADDR   0x3Cu

/* 显式声明为 0 时适配 SSD1306；模块若是 128x64 的 SH1106（1.3" 常见）
 * 其 RAM 有 2 列偏移，把这里改成 2 即可 */
#ifndef SSD1306_COL_OFFSET
#define SSD1306_COL_OFFSET   0u
#endif

/* 初始化（会做探测）。返回 0 成功 */
int  ssd1306_init(void);
bool ssd1306_ready(void);
int  ssd1306_addr(void);

/* ★ 面板"保健"：只重发关键配置字节（页寻址、对比度、电荷泵 0x8D,0x14、显示开 0xAF），
 *   **不清显存、不闪烁**。返回 0 成功。
 *
 * 为什么必须有这个函数（真机踩坑）：
 *   SSD1306 在电源/总线抖动后会丢掉"显示开 / 电荷泵"等配置，但它**仍然能在 I2C 上应答**，
 *   于是驱动侧看到的是"写入失败=0、I2C 无错误、总线电平正常"，而屏幕**定格不动**。
 *   原先 ssd1306_init() 只在**首次**成功时调用，之后永不重发配置 → 面板一旦掉配置就永久定格。
 *   现在 ssd1306_flush() 会自动周期性调用本函数自愈，无需上层干预。 */
int  ssd1306_repair(void);
uint32_t ssd1306_repairs(void);   /* 已执行的面板保健次数（可观测性） */

/* ★ 最小保健：只补发【电荷泵开 + 显示开】（4 字节、无显示关 → 不白闪）。
 *   针对"面板静默丢失显示使能"——ACK 正常、写失败恒为 0、画面却停在旧帧。
 *   由 ssd1306_flush() 周期性调用（每 10 次刷屏）兜底。 */
int ssd1306_heal_min(void);
uint32_t ssd1306_heals(void);     /* 已执行的最小保健次数（可观测性） */

/* 上一次整屏刷新的耗时（ms）。25 kHz 下 1032 字节理论 ≈372 ms；
 * 实测远大于此 → I2C 上升沿太慢（上拉弱/走线长），先治硬件。 */
uint32_t ssd1306_last_flush_ms(void);

/* 累计的 I2C 写失败次数（0 = 一切正常；非 0 说明屏幕数据可能没写进去） */
uint32_t ssd1306_errors(void);

/* 显存操作（改完需 ssd1306_flush() 才上屏） */
void ssd1306_clear(void);
void ssd1306_fill(uint8_t pattern);   /* 0xFF=全亮(整片白) 0x00=全灭(整片黑) */
void ssd1306_flush(void);

/* 5x7 文字；x 单位像素（按 6 px 步进），line 单位 8 像素行（0..7） */
void ssd1306_text(uint8_t x, uint8_t line, const char *s);
void ssd1306_textf(uint8_t x, uint8_t line, const char *fmt, ...);

/* 画一条水平进度条（0..100%） */
void ssd1306_bar(uint8_t x, uint8_t line, uint8_t pct);
/* ★ 柱状趋势图：v 有 n 个值，按 vmax 定标（vmax=0 视作 1）*/
void ssd1306_chart(uint8_t x, uint8_t line, const uint8_t *v, uint8_t n, uint8_t vmax);

/* ★ 诊断用（开机自检调用）：
 *   fill_checker —— 静态棋盘格，用于判断"画面滚动"是硬件滚动还是逐页写入的错觉
 *   border       —— 1 像素贴边边框，用于判断寻址模式/列偏移是否正确 */
void ssd1306_fill_checker(void);
void ssd1306_border(void);

#endif /* SBB_SSD1306_H */
