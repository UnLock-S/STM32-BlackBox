/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_uart.h —— 调试串口（USART1，PA9/PA10 → CH347 或 J-Link VCOM）
 * ─────────────────────────────────────────────────────────────────────────────
 *  底层 USART1 由 CubeMX 生成的 MX_USART1_UART_Init() 初始化（115200 8N1）。
 *  本层补两件事：
 *    · 线程安全（静态互斥量，多任务可并发打日志）
 *    · 格式化输出 bsp_uart_logf()
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_UART_H
#define SBB_BSP_UART_H

#include <stddef.h>
#include <stdint.h>

/* 建互斥量。**必须在调度器启动前或首个任务里调用一次。** */
void bsp_uart_init(void);

int bsp_uart_write(const void *buf, size_t len);
int bsp_uart_puts(const char *s);

/* 线程安全的格式化输出（等价于 printf，但不需要 newlib 的 FILE） */
int bsp_uart_logf(const char *fmt, ...);

#endif /* SBB_BSP_UART_H */
