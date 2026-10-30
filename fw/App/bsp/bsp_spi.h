/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_spi.h —— SPI1 板级封装（SD 卡用，软件片选）
 * ─────────────────────────────────────────────────────────────────────────────
 *  SPI1 的底层参数由 CubeMX 生成：
 *    · 主模式全双工、8 bit、MSB first、CPOL=Low、CPHA=1Edge、NSS=Software
 *    · 时钟分频在 .ioc 里定的是 /8 = 9 MBits/s
 *    · 片选 PA4 是**普通 GPIO 输出**（标签 SD_CS），不是硬件 NSS
 *
 *  ★ 为什么需要切换速率：
 *    SD 卡规范要求**初始化阶段 ≤ 400 kHz**，识别完成后才能升到高速。
 *    APB2 = 72 MHz 下可选：/256 = 0.281 MHz（唯一满足 ≤400 kHz 的档）、/8 = 9 MHz。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_SPI_H
#define SBB_BSP_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 速率档位 */
#define BSP_SPI_SPEED_SLOW   0u   /* /256 ≈ 0.281 MHz —— SD 初始化阶段 */
#define BSP_SPI_SPEED_FAST   1u   /* /8   = 9.0 MHz   —— 数据传输 */

/* 只做互斥量与初始速率设置；SPI1 外设本身已由 MX_SPI1_Init() 初始化 */
int bsp_spi_init(void);

/* 切换速率（会重新配置 SPI1） */
int bsp_spi_set_speed(unsigned which);

/* 全双工收发；tx 或 rx 可为 NULL 表示只发/只收 */
int bsp_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len);
int bsp_spi_send(const uint8_t *tx, size_t len);
int bsp_spi_recv(uint8_t *rx, size_t len);

/* SD 卡软件片选（PA4，低有效）。assert=true 表示选中（拉低）。 */
void bsp_spi_cs(bool assert);

/* 当前速率档位 */
unsigned bsp_spi_speed(void);

#endif /* SBB_BSP_SPI_H */
