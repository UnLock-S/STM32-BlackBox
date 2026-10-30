/* ─────────────────────────────────────────────────────────────────────────────
 *  app_main.h —— 应用层入口
 * ─────────────────────────────────────────────────────────────────────────────
 *  main() 由 CubeMX 生成（cubemx/Core/Src/main.c）拥有。
 *  本工程的应用代码**不定义 main()**，而是：
 *    · app_start()          —— 在 main.c 的 USER CODE BEGIN RTOS_THREADS 里调用
 *    · app_monitor_task()   —— 实现 CubeMX 生成的 StartDefaultTask 的任务体
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_APP_MAIN_H
#define SBB_APP_MAIN_H

/* 建应用任务（全静态）。在调度器启动前由 main.c 调用。 */
void app_start(void);

/* CubeMX 的 defaultTask 任务体：周期打印系统状态（心跳 + 栈余量）。
   arg 由 CubeMX 传入（未使用）。 */
void app_monitor_task(void const *arg);

#endif /* SBB_APP_MAIN_H */
