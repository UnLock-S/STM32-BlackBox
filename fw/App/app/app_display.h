/* ─────────────────────────────────────────────────────────────────────────────
 *  app_display.h —— OLED 状态屏刷新任务
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_APP_DISPLAY_H
#define SBB_APP_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

int  app_display_start(void);
void app_display_task(void *arg);
bool app_display_ready(void);

/* 刷屏次数（诊断）*/
uint32_t app_display_flush_count(void);
uint32_t app_display_heal_count(void);   /* G4: OLED 自愈次数 */

#endif /* SBB_APP_DISPLAY_H */
