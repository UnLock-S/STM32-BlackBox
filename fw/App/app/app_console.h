/* ─────────────────────────────────────────────────────────────────────────────
 *  app_console.h —— 串口命令行（USART1，115200 8N1）
 * ─────────────────────────────────────────────────────────────────────────────
 *  为什么要有它：明天接线后每改一次外设都要能**不重新烧写**就验证 ——
 *  控制台让用户直接敲命令查状态、单测某个外设。
 *
 *  实现：轮询 USART1 的 RXNE（不用中断），避免动 CubeMX 生成的
 *  stm32f1xx_it.c / NVIC 配置。人类输入的速率下轮询完全够。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_APP_CONSOLE_H
#define SBB_APP_CONSOLE_H

/* 建控制台任务（全静态）。在 app_start() 里调用。 */
int app_console_start(void);

/* 任务体（由 sched 调用） */
void app_console_task(void *arg);

/* 供其他模块借用：打印一行提示（线程安全，走 bsp_uart） */
void app_console_hint(const char *s);

#endif /* SBB_APP_CONSOLE_H */
