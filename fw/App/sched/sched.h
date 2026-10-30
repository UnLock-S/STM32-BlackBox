/* ─────────────────────────────────────────────────────────────────────────────
 *  sched.h —— 调度层抽象（App 与 RTOS 之间的隔离缝）
 * ─────────────────────────────────────────────────────────────────────────────
 *  分层约定（SDD §3.3.1）：
 *    · App 层与 BSP 层**只调用本头文件**的接口，不直接碰 RTOS API
 *    · 换 RTOS 只改这一层（本文件 + sched_freertos.c）
 *    · 本文件是唯一允许知道具体 RTOS 的头文件
 *
 *  ⚠ 与旧版（vendored 内核）的差异：
 *    · RTOS 内核改由 CubeMX 管理（cubemx/Middlewares/Third_Party/FreeRTOS）
 *    · **本层不再提供 sched_start()** —— 调度器由 CubeMX 生成的
 *      cubemx/Core/Src/main.c 里的 osKernelStart() 启动；提供二次启动入口只会
 *      造成误用。
 *    · 任务仍**全静态分配**（SCHED_TASK_DEF 编译期展开，ADR-10）
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_SCHED_H
#define SBB_SCHED_H

#include <stdint.h>
#include <stddef.h>

#include "FreeRTOS.h"   /* ← 仅本头文件可见 */
#include "task.h"

typedef void (*sched_task_fn)(void *arg);

/* 单个任务的静态存储（TCB + 栈） */
typedef struct {
    StaticTask_t *tcb;
    StackType_t  *stack;
    uint16_t      stack_words;
} sched_store_t;

/* 任务定义（配置 + 静态存储） */
typedef struct {
    const char   *name;
    sched_task_fn fn;
    void         *arg;
    uint8_t       priority;
    sched_store_t store;
} sched_task_def_t;

/* ── 在模块 .c 里展开，一步得到「静态存储 + 任务定义」 ──────────────────────
 *  用法：  SCHED_TASK_DEF(g_app_led, "led", 128, 3, app_led_task, 0);
 *  代替 malloc：tcb 与 stack 都是 static 数组（ADR-10 禁动态分配）
 * ────────────────────────────────────────────────────────────────────────── */
#define SCHED_TASK_DEF(var, task_name, stack_words_, prio_, entry, arg_)     \
    static StaticTask_t  var##_tcb;                                          \
    static StackType_t   var##_stack[(stack_words_)];                        \
    sched_task_def_t     var = {                                             \
        (task_name), (entry), (arg_), (prio_),                               \
        { &var##_tcb, var##_stack, (stack_words_) } }

/* ── 任务 ── */
int sched_task_add(const sched_task_def_t *def);

/* ── 时间 ── */
void     sched_sleep_ms(uint32_t ms);
uint32_t sched_tick(void);
uint32_t sched_stack_free_words(void);

/* ★ G2 多任务软看门狗
 *   每个任务在自己的循环里调用 sched_alive()，内部按任务句柄自动登记；
 *   心跳任务定期调 sched_watchdog_ok()，任一任务超过窗口没动就返回 -1，
 *   心跳据此【停止喂狗】让 IWDG 复位。
 *   为什么需要：IWDG 只有心跳在喂，任何别的任务卡死都不会被发现。 */
void sched_alive(uint32_t budget_ms);   /* budget=本任务两次调用间的最大间隔 */
int  sched_watchdog_ok(void);           /* 按各任务自己的预算判断 */
const char *sched_watchdog_name(void);   /* 超时任务名，便于打日志 */

/* ── 临界区（key 在 FreeRTOS 上是空操作，换 OS 时才有意义）── */
unsigned sched_enter_critical(void);
void     sched_exit_critical(unsigned key);

#endif /* SBB_SCHED_H */
