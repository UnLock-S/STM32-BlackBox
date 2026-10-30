/* ─────────────────────────────────────────────────────────────────────────────
 *  app_adc.h —— C0832C 采样任务
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_APP_ADC_H
#define SBB_APP_ADC_H

#include <stdint.h>

int  app_adc_start(void);
void app_adc_task(void *arg);

/* 最近一次采样值（供显示任务读） */
uint8_t  app_adc_last(uint8_t ch);
uint32_t app_adc_samples(void);
int      app_adc_ok(void);          /* 0 = 采样正常 */

#endif /* SBB_APP_ADC_H */
