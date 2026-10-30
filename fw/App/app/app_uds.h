/* ─────────────────────────────────────────────────────────────────────────────
 *  app_uds.h —— UDS on CAN 服务端（ISO-TP 连续帧 + ISO 14229 服务子集）
 * ─────────────────────────────────────────────────────────────────────────────
 *  结构分三层，互不耦合:
 *    ① ISO-TP 层: SF/FF/CF/FC 拆包与打包（本文件）
 *    ② 服务层   : 10/11/22/27/31/34/36/37/3E 分发（本文件）
 *    ③ 数据层   : DID 内容、例程实现、暂存区写入（diag_handlers.c 手写）
 *
 *  ★ 为什么放在 App 而不是 Bootloader
 *    Bootloader 只有 14 KB，装不下 ISO-TP + 服务机 + CAN 驱动。
 *    这里的职责只是"把镜像收下来写进暂存区"，搬运仍由 Bootloader 做 ✓
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef SBB_APP_UDS_H
#define SBB_APP_UDS_H

#include <stdbool.h>
#include <stdint.h>

#define UDS_REQ_ID        0x7E0u        /* 测试仪 -> ECU */
#define UDS_RSP_ID        0x7E8u        /* ECU -> 测试仪 */

#define UDS_MAX_MSG       520u          /* 512 B 块 + 8 B 余量 */
#define UDS_BLOCK_SIZE    512u          /* 每次 0x36 的数据量 = 一个扇区 ✓ */

/* 接收一帧（由 app_can.c 在收到 0x7E0 时调用） */
void app_uds_rx(uint32_t id, const uint8_t *data, uint8_t dlc);

/* 周期调用：处理超时、发送待发帧（在 CAN 任务里每 1 ms 调一次）*/
void app_uds_poll(void);

/* 统计（可观测性）*/
uint32_t app_uds_requests(void);
uint32_t app_uds_nrc(void);

/* 数据层（diag_handlers.c 实现）*/
int      diag_read_did(uint16_t did, uint8_t *out, uint8_t *len);
int      diag_routine_start(uint16_t rid, const uint8_t *in, uint8_t in_len,
                            uint8_t *out, uint8_t *out_len);
uint32_t diag_security_seed(void);
int      diag_security_locked(void);   /* 1=锁定中 */
int      diag_security_check(const uint8_t *key, uint8_t len);  /* 0=ok -1=key错 -2=锁定 */

/* 刷写暂存区 */
int      diag_dl_begin(uint32_t total);
void     diag_dl_abort(void);   /* 31 01 FF00 / 中断恢复：放弃当前会话 */
int      diag_dl_block(uint8_t seq, const uint8_t *buf, uint16_t len);
int      diag_dl_finish(void);

#endif /* SBB_APP_UDS_H */
