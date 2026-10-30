/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_can.h —— CAN1 板级封装（bxCAN，1 Mbit/s）
 * ─────────────────────────────────────────────────────────────────────────────
 *  CAN1 参数由 CubeMX 生成：PB8(RX)/PB9(TX) 重映射，Prescaler=4 / BS1=6TQ /
 *  BS2=2TQ → 1.000 Mbps，采样点 77.8%（符合 CiA 301 的 75~87.5%）。
 *
 *  ★ 收发模型（当前实现：轮询）
 *    CubeMX 目前**没有**勾 CAN 的 NVIC 中断（.ioc 的 NVIC 页只有核心异常），
 *    生成的 stm32f1xx_it.c 里没有 CAN1_RX0_IRQHandler。因此本层用
 *    **轮询 HAL_CAN_GetRxMessage()** 收帧，由任务以 1~5 ms 周期调 bsp_can_poll()。
 *    → 已预留中断路径：bsp_can_rx_isr()。明天在 CubeMX 里勾上
 *      "CAN1 RX0 interrupt" 后，把 bsp_can_rx_isr() 填进生成的
 *      CAN1_RX0_IRQHandler 的 USER CODE 段即可，无需改本文件逻辑。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_CAN_H
#define SBB_BSP_CAN_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    bool     ext;
    bool     rtr;
    uint8_t  data[8];
    uint32_t ts_ms;
} bsp_can_frame_t;

/* 起 CAN 并配滤波器（收全部标准帧）。在 app_start() 里调用。 */
int  bsp_can_init(void);

/* 发一帧。ext=true 走扩展帧。返回 0 成功。
   ⚠ 会在邮箱满时等待（阻塞至多 BSP_CAN_TX_TIMEOUT_MS） */
int  bsp_can_send(uint32_t id, bool ext, const uint8_t *data, uint8_t dlc);

/* 从接收环形队列取一帧（非阻塞）。true=取到 */
bool bsp_can_recv(bsp_can_frame_t *out);

/* 轮询硬件 FIFO 收帧并推入环形队列。建议由任务以 1~5 ms 调用。 */
int  bsp_can_poll(void);

/* 预留：中断上下文收帧。在 CubeMX 打开 CAN1 RX0 NVIC 后接进 it.c 的 USER CODE */
void bsp_can_rx_isr(void);

/* 统计与状态 */
uint32_t bsp_can_rx_count(void);
uint32_t bsp_can_tx_count(void);
uint32_t bsp_can_err_count(void);

/* ★ 控制器错误状态：直读 bxCAN ESR（判断真总线上有没有 ACK 的唯一证据）*/
void bsp_can_error_state(uint8_t *tec, uint8_t *rec, uint8_t *lec, bool *bus_off);
/* ★ 诊断：接收中断次数 / 环形队列溢出丢帧数（丢帧>0 说明负载率偏低）*/
uint32_t bsp_can_irq_count(void);
uint32_t bsp_can_drop_count(void);
/* ★ 累计位长（负载率用）：按秒取差值 → 位/秒 */
uint32_t bsp_can_bits_rx(void);
uint32_t bsp_can_bits_tx(void);
bool     bsp_can_started(void);

/* 设置回环/正常模式（自测用；会停/起 CAN） */
int  bsp_can_set_loopback(bool enable);

#endif /* SBB_BSP_CAN_H */
