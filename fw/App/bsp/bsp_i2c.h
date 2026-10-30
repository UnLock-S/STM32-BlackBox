/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_i2c.h —— I2C1 板级封装（OLED 等 I2C 从设备）
 * ─────────────────────────────────────────────────────────────────────────────
 *  I2C1 参数由 CubeMX 生成：**100 kHz**（标准模式；`I2C1.ClockSpeed=100000`，
 *  PCLK1=36 MHz → CCR=180 / TRISE=37），引脚 PB6(SCL)/PB7(SDA)。
 *  ⚠ 引脚在 HAL_I2C_MspInit 里配成 GPIO_MODE_AF_OD（开漏）且**未开内部上拉**，
 *    所以 SCL/SDA 的上拉**依赖从设备侧**（常见 OLED 模块板载 4.7k 上拉）。
 *    SSD1306 支持 100 kHz，直接用即可；要提速到 400 kHz 就在 CubeMX 里改 ClockSpeed。
 *  本层提供线程安全的阻塞式收发（带互斥量）。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_I2C_H
#define SBB_BSP_I2C_H

#include <stddef.h>
#include <stdint.h>

int bsp_i2c_init(void);

/* 7 位从机地址（不含读写位） */
int bsp_i2c_write(uint8_t addr7, const uint8_t *buf, size_t len);
int bsp_i2c_read(uint8_t addr7, uint8_t *buf, size_t len);

/* 带寄存器/命令字节的读写（多数 I2C 外设用这个形式） */
int bsp_i2c_mem_write(uint8_t addr7, uint8_t mem, const uint8_t *buf, size_t len);
int bsp_i2c_mem_read(uint8_t addr7, uint8_t mem, uint8_t *buf, size_t len);

/* 探测：能从机返回 ACK 则返回 0 */
int bsp_i2c_probe(uint8_t addr7);

/* ── F1 的 I2C 失效模式与恢复 ──
 * STM32F1 的 I2C + HAL 有一个特点：**任何一次传输超时后，外设可能留下 BUSY=1
 * 且 START 位未撤**，此后**所有**操作都会失败（HAL_BUSY / 超时），而 HAL 自己
 * 不会恢复。实测遇到：I2C1_SR2 = MSL|BUSY|TRA 全 1 而总线实际空闲。
 * → 下面两个接口用于检测与恢复；本层的读写函数会在每次传输前自动检查并恢复。 */
int bsp_i2c_busy(void);        /* 1 = 外设报告总线忙或 HAL 句柄被锁住 */
int bsp_i2c_locked(void);      /* 1 = hi2c1.Lock 卡在 HAL_LOCKED（HAL 早退不解锁导致） */
int      bsp_i2c_recover(void);     /* 复位 I2C 外设（RCC 强制复位 + 清 HAL 锁） */
uint32_t bsp_i2c_fail_count(void);
/* ★ 取锁超时次数（>0 且持续增长 = 有任务卡死握着 I2C 锁）*/
uint32_t bsp_i2c_lock_timeout_count(void);  /* 累计传输失败次数 */
uint32_t bsp_i2c_recover_count(void);
/* 强制总线解锁次数（仅在开机窗口内可能增长；稳态运行必须保持不增长） */
uint32_t bsp_i2c_forceclear_count(void);

/* ★ 诊断/复位用：把 SCL/SDA 释放为浮空输入并保持 ms 毫秒。
 *   用于复现"按住板子 reset 键 2~3 秒"那种"总线持续空闲"的条件
 *   —— 实测这才是让定格面板恢复的关键（毫秒级脉冲复位无效）✓ */
void bsp_i2c_bus_idle_ms(uint32_t ms);

/* ★ 供上层做"软件硬复位"用：**无条件**打 9 个时钟 + STOP 解开总线。
 *   与自愈路径里的 bus_clear 不同，这里不看线路电平 —— 调用者明确知道自己在复位器件。
 *   ⚠ 只应在开机 / 器件复位流程里调用；运行期调用就是往正常总线注入垃圾 ✗
 *     （见 DEBUG-LOG 的 Bug #12 / #16）。 */
void bsp_i2c_clear_force(void);

#endif /* SBB_BSP_I2C_H */
