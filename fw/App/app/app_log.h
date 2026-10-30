/* ─────────────────────────────────────────────────────────────────────────────
 *  app_log.h —— SD 卡原始块日志（无文件系统）
 * ─────────────────────────────────────────────────────────────────────────────
 *  ★ 为什么不用 FatFs
 *    CubeMX 当前没导入 FatFs 中间件（.ioc 里没有），FatFs 需要它自己的一堆源文件
 *    与配置。为「明天就能跑」选择了**自带格式的原始块记录**：
 *      · 自包含、无外部依赖、占 RAM 极小（1 个 512 B 块缓冲）
 *      · 主机侧用 tools/sbb_log2csv.py 转成 CSV（该脚本本地已可验证）
 *    → 明天若在 CubeMX 里导入 FatFs，可只替换 app_log 的实现，接口不变。
 *
 *  ── 卡上布局（从 LBA 0 起）──
 *    块 0        : 头（magic "SBBLOG01" + 版本 + 记录尺寸 + 记录数 + CRC）
 *    块 1..N     : 记录区，每块 25 条 20 B 记录（500/512 B，余 12 B 不用）
 *
 *  ── 单条记录 20 B（小端）──
 *    +0  u32  ts_ms      相对开机毫秒
 *    +4  u8   type       1=ADC 2=CAN_RX 3=CAN_TX 4=EVENT 5=OTA
 *    +5  u8   ch         通道/索引
 *    +6  u8   flags      type 相关（CAN: bit0=ext, bit1=rtr）
 *    +7  u8   len        payload 有效字节数
 *    +8  u8[12] payload
 *
 *    payload 布局（按 type）：
 *      ADC   : [0]        值 0..255
 *      CAN   : [0..3]     id（小端）  [4..4+dlc-1] 数据
 *      EVENT : [0..3] v0  [4..7] v1
 *      OTA   : [0..3] v0  [4..7] v1
 *    ★ payload 取 12 B 是因为 CAN 需要 id(4)+data(8) —— 16 B 记录装不下（原来
 *      的 8 B payload 会让 id 覆盖数据，已修正）。

 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_APP_LOG_H
#define SBB_APP_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define APP_LOG_TYPE_ADC      0x01u
#define APP_LOG_TYPE_CAN_RX   0x02u
#define APP_LOG_TYPE_CAN_TX   0x03u
#define APP_LOG_TYPE_EVENT    0x04u
#define APP_LOG_TYPE_OTA      0x05u

#define APP_LOG_RECORD_SIZE   20u
#define APP_LOG_MAGIC         "SBBLOG01"
#define APP_LOG_HEADER_LBA    15999999u  /* ★ 原 0u → 把“SBBLOG01”头写在第 0 扇区 ✗
                                          *   直接覆掉 MBR/BPB -> 卡一插进板子就不是 FAT32 了 ✗
                                          *   （真机: 格式化后 sd read 0 读回 "SBBLOG01"）
                                          *   现放在日志区起点前一块 ✓ */
#define APP_LOG_FIRST_LBA     16000000u   /* ★ 原 1u 会把卡的第 0/1 扇区（MBR/BPB）写掉 ✗
                                          *   真机证据: 格式化后插卡，sd read 0 读回
                                          *   "SBBLOG01"（我们的日志头）-> 文件系统头被覆盖
                                          *   -> bootloader fat_mount rc=-2（不是 FAT32）
                                          *   16000000 块 ≈ 8 GB 偏移，落在 FAT 数据区内部，
                                          *   不再碰 MBR/FAT/根目录 ✓（卡容量 30 GB）*/

/* 探测 SD 卡并写头。返回 0 成功（SD 不在位会失败，但系统继续运行） */
int  app_log_init(void);
bool app_log_ready(void);

/* 追加一条记录（内部缓冲满则自动落盘） */
int  app_log_record(uint8_t type, uint8_t ch, uint8_t flags,
                    const uint8_t *payload, uint8_t len);

/* 便捷：事件型 */
int  app_log_event(uint8_t ch, uint32_t v0, uint32_t v1);
int  app_log_adc(uint8_t ch, uint8_t value);
int  app_log_can(bool tx, uint32_t id, bool ext, bool rtr,
                 const uint8_t *data, uint8_t dlc);

/* 把缓冲写进卡 */
int  app_log_flush(void);

/* ★ OTA 期间暂停日志落盘：SD 卡同时被两路写会交错
 *   -> 卡的内部状态机混乱 -> 写块卡死（实测现象）*/
void app_log_pause(void);
void app_log_resume(void);

uint32_t app_log_count(void);      /* 已写记录总数 */
uint32_t app_log_drop(void);       /* 因 SD 故障丢弃的记录数 */
uint32_t app_log_next_lba(void);   /* 下一条记录要落的块号 */

/* 控制台用：打印统计 */
void app_log_info(void);

#endif /* SBB_APP_LOG_H */
