/* ─────────────────────────────────────────────────────────────────────────────
 *  app_can.h —— CAN 收发任务 + 台架自测（bench）
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_APP_CAN_H
#define SBB_APP_CAN_H

#include <stdbool.h>
#include <stdint.h>

int  app_can_start(void);
void app_can_task(void *arg);

/* 台架：周期发一帧递增计数器 */
int  app_can_bench_start(uint32_t period_ms);
int  app_can_bench_stop(void);
bool app_can_bench_running(void);

/* 单次发送一帧测试报文 */
int  app_can_send_test(uint32_t id, const uint8_t *data, uint8_t dlc);

/* 统计（含最近一帧，供显示） */
uint32_t app_can_rx(void);
uint32_t app_can_tx(void);
/* ★ 总线负载（用户需求）：收发帧率与负载千分比（近似值，见 app_can.c 说明） */
uint32_t app_can_rx_rate(void);          /* 收：帧/秒 */
uint32_t app_can_tx_rate(void);          /* 发：帧/秒 */
uint32_t app_can_load_1e4(void);         /* 负载 0~10000，单位 0.01%（两位小数）*/
/* ★ 负载趋势（每秒一格，供屏幕画折线/柱状）*/
uint8_t  app_can_hist(uint8_t i);       /* i=0 最新，i=19 最早；值 0..100 % */
uint8_t  app_can_hist_peak(void);       /* 窗口内峰值 0..100 %（自动定标用）*/
uint8_t  app_can_hist_n(void);          /* 格数 */
uint32_t app_can_err(void);
uint32_t app_can_last_id(void);
uint8_t  app_can_last_dlc(void);
uint32_t app_can_last_data_u32(void);

#endif /* SBB_APP_CAN_H */
