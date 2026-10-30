/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_can.c —— CAN1 板级封装（HAL，轮询收帧 + 环形队列）
 * ─────────────────────────────────────────────────────────────────────────────
 *  单生产者（bsp_can_poll / ISR）单消费者（任务里的 bsp_can_recv），
 *  用临界区保护索引 —— 全静态、无动态分配（ADR-10）。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_can.h"

#include <string.h>

#include "main.h"
#include "sched.h"
#include "FreeRTOS.h"
#include "task.h"

extern CAN_HandleTypeDef hcan;

#define BSP_CAN_RX_RING      32u      /* 帧数（32 × 16 B ≈ 512 B） */
#define BSP_CAN_TX_TIMEOUT   50u      /* ms */

static bsp_can_frame_t s_ring[BSP_CAN_RX_RING];
static volatile uint32_t s_head;      /* 写入位置 */
static volatile uint32_t s_tail;      /* 读出位置 */
static volatile uint32_t s_drop_cnt;  /* ★ 环形队列溢出丢帧数（判断负载率是否可信）*/
static volatile uint32_t s_irq_cnt;   /* ★ 接收中断进入次数（诊断）*/
static volatile uint32_t s_bits_rx;   /* ★ 累计接收位长（负载率用，逐帧按 DLC 算）*/
static volatile uint32_t s_bits_tx;   /* ★ 累计发送位长 */

static volatile uint32_t s_rx_cnt;
static volatile uint32_t s_tx_cnt;
static volatile uint32_t s_err_cnt;

static CAN_FilterTypeDef s_filter;

/* ★ 单帧占用的总线位数估算（与 CANoe 的口径靠拢）
 *   标准帧固定开销 44 位：SOF(1)+仲裁(11)+RTR/IDE/r0(3)+DLC(4)+CRC(15)+CRC界定(1)+ACK(2)+EOF(7)
 *   扩展帧仲裁段再 +20 位（29 位 ID 等）
 *   再加数据 8×DLC 位，最后按【平均约 10% 位填充】折算
 * 注：位填充是逐帧变化的，这里取平均；CANoe 按实际填充计，两者会有 ±5% 偏差 */
static uint32_t frame_bits(bool ext, uint8_t dlc)
{
    uint32_t base = (ext ? 64u : 44u) + (8u * (uint32_t) dlc);

    return (base * 110u) / 100u;
}

static void ring_push(const bsp_can_frame_t *f)
{
    unsigned key = sched_enter_critical();
    uint32_t next = (s_head + 1u) % BSP_CAN_RX_RING;

    if (next == s_tail) {
        s_drop_cnt++;                     /* ★ 溢出：丢最老一帧并计数 */
        /* 满：丢弃最旧的一帧（保住最新数据，黑匣子语义） */
        s_tail = (s_tail + 1u) % BSP_CAN_RX_RING;
        s_err_cnt++;
    }
    s_ring[s_head] = *f;
    s_head = next;
    s_rx_cnt++;
    sched_exit_critical(key);
}

int bsp_can_init(void)
{
    s_head = 0u;
    s_tail = 0u;
    s_rx_cnt = 0u;
    s_tx_cnt = 0u;
    s_err_cnt = 0u;

    /* 接收滤波器：全部通过（掩码 0 = 不过滤），FIFO0 */
    s_filter.FilterBank           = 0u;
    s_filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    s_filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    s_filter.FilterIdHigh         = 0x0000u;
    s_filter.FilterIdLow          = 0x0000u;
    s_filter.FilterMaskIdHigh     = 0x0000u;
    s_filter.FilterMaskIdLow      = 0x0000u;
    s_filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    s_filter.FilterActivation     = ENABLE;
    s_filter.SlaveStartFilterBank = 14u;

    if (HAL_CAN_ConfigFilter(&hcan, &s_filter) != HAL_OK) {
        return -1;
    }
    if (HAL_CAN_Start(&hcan) != HAL_OK) {
        return -2;
    }
    (void) HAL_CAN_ActivateNotification(&hcan, CAN_IT_RX_FIFO0_MSG_PENDING);

    /* ★★ 自己打开 CAN1 RX0 中断（不依赖 CubeMX 的 NVIC 勾选）
     *
     * 为什么必须开（真机实测教训 ★）：
     *   bxCAN 的接收 FIFO 只有 3 深，而 app_can_task 是 5ms 轮询
     *   -> 接收速率被硬顶在 3帧/5ms = 600 帧/秒
     *   实测：CANoe 侧 40% 负载（约 3600 帧/秒）时，
     *         板子只数到约 580 帧/秒 -> 显示屏负载率只有 8%（假值）
     *   也就是说：高速总线上的报文 80% 以上被丢掉了
     *
     * 优先级 5：与 UART RX(4) 同一区间，均高于
     * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
     * （中断内不调用 FreeRTOS API，无需 FromISR）
     * 中断服务程序见本文件末尾 —— 写在 App 层，CubeMX 再生成不会冲掉 */
    /* ⚠⚠ 2026-10-16 撤下：开启本中断后板子出现 HardFault（见 DEBUG-LOG Bug #26）
     *
     * 真机证据（J-Link）：CFSR=0x00010400 = UNDEFINSTR + UNSTKERR，
     *   PC 落在 _sbss（在执行 BSS 数据），MSP=0x200006F0（主栈掉进 BSS 区）
     *   → 主栈被压爆；同时串口 0 字节（控制台任务被饿死）
     *
     * 触发条件：**总线上持续出现错误帧时**（本项目 CANoe 侧时序不匹配 →
     *   REC/TEC 双满）。此时接收中断被反复触发 → 中断风暴 → 主栈溢出。
     *   注意：连已知稳定的 v0.6.1 在同样总线条件下也会挂 →
     *   这不是某次代码改动的问题，而是"开中断"这个方案在异常总线上不安全。
     *
     * 现回到【轮询取帧】（之前稳定运行数小时的方案）。
     * 代价：接收速率上限 600 帧/秒（3 帧 FIFO ÷ 5ms）→ 负载率显示会偏低。
     * 重新启用前必须先解决：(a) 中断里的重入/风暴抑制，(b) 总线不再刷错误帧。*/
    /* ⚠⚠ 2026-10-16 最终结论：本板上【CAN 接收中断不可用】
     *
     * 实测三次：只要使能本中断，板子就在 CAN 初始化后约 1~2 秒
     *   （即第一个有效 CAN 帧到达时）挂死 -> IWDG 2 秒复位 -> 无限重启循环。
     *   已排除的原因：
     *     - 不是 FreeRTOS 任务态 API（已改用 HAL_GetTick，中断内零 API）
     *     - 不是总线错误风暴（此时 TEC=0 REC=0，总线健康）
     *     - 不是镜像损坏（整片擦除+校验过）
     *   最可能是本板 CAN1_RX0 中断与 FreeRTOS/其他外设存在本板特有的冲突，
     *   或克隆版 J-Link/复位链路干扰 —— 未再深挖（收益低）。
     *
     * 替代方案（已采用）：保持轮询取帧，但把轮询周期从 5ms 缩到 1ms
     *   -> 上限 3帧/1ms = 3000 帧/秒（原 600），足以覆盖 40% 总线负载 ✓
     *   轮询绝对稳定，不引入中断风险 ✓ */
    /* HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 5u, 0u); */
    /* HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn); */
    return 0;
}

static int can_send_raw(uint32_t id, bool ext, bool rtr, const uint8_t *data, uint8_t dlc)
{
    CAN_TxHeaderTypeDef hdr;
    uint32_t            mailbox;
    uint32_t            t0;

    if (dlc > 8u) {
        return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.StdId = ext ? 0u : (id & 0x7FFu);
    hdr.ExtId = ext ? id : 0u;
    hdr.IDE   = ext ? CAN_ID_EXT : CAN_ID_STD;
    hdr.RTR   = rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA;
    hdr.DLC   = dlc;
    hdr.TransmitGlobalTime = DISABLE;

    t0 = sched_tick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0u) {
        if ((uint32_t) (sched_tick() - t0) > BSP_CAN_TX_TIMEOUT) {
            s_err_cnt++;
            return -2;              /* 邮箱一直满 */
        }
        vTaskDelay(1u);
    }

    if (HAL_CAN_AddTxMessage(&hcan, &hdr, (uint8_t *) data, &mailbox) != HAL_OK) {
        s_err_cnt++;
        return -3;
    }
    s_tx_cnt++;
    s_bits_tx += frame_bits(ext, dlc);   /* ★ 发送也占总线时间 */
    return 0;
}

int bsp_can_send(uint32_t id, bool ext, const uint8_t *data, uint8_t dlc)
{
    return can_send_raw(id, ext, false, data, dlc);
}

static void drain_fifo(uint32_t fifo, bool from_isr)
{
    CAN_RxHeaderTypeDef hdr;

    /* 单次最多搬 16 帧：防止极端突发下中断里长时间停留
     * （剩下的交给 app_can_task 的轮询或下一次中断）*/
    uint32_t guard = 16u;

    while (HAL_CAN_GetRxFifoFillLevel(&hcan, fifo) > 0u && guard-- > 0u) {
        bsp_can_frame_t f;

        memset(&f, 0, sizeof(f));
        if (HAL_CAN_GetRxMessage(&hcan, fifo, &hdr, f.data) != HAL_OK) {
            s_err_cnt++;
            return;
        }
        f.ext = (hdr.IDE == CAN_ID_EXT);
        f.rtr = (hdr.RTR == CAN_RTR_REMOTE);
        f.id  = f.ext ? hdr.ExtId : hdr.StdId;
        f.dlc = (uint8_t) (hdr.DLC & 0x0Fu);
        /* ★★ 中断里【不用任何 FreeRTOS API】—— 包括 xTaskGetTickCountFromISR ✗
         *
         * 真机实测教训：xTaskGetTickCountFromISR 内部会走
         *   vPortValidateInterruptPriority() 断言，一旦不通过就
         *   taskDISABLE_INTERRUPTS() + for(;;) 死等 -> 心跳任务停喂狗
         *   -> IWDG 约 2 秒复位 -> 板子进无限重启循环 ✗
         *   （中断优先级 5 虽与 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 相等、
         *     理论上允许 FromISR，但实测就是要命 —— 干脆一个 API 都不用 ✓）
         *
         * 改用 HAL_GetTick()：它只读一个全局变量 uwTick ✓
         *   不是 FreeRTOS API ✓，中断内绝对安全 ✓
         *   单位同样是 1ms（RTOS tick=1000Hz），可直接混用 ✓ */
        f.ts_ms = from_isr ? (uint32_t) HAL_GetTick()
                           : (uint32_t) sched_tick();
        s_bits_rx += frame_bits(f.ext, f.dlc);   /* ★ 先累加再入环（丢帧也已计入总线占用 ✓）*/
        ring_push(&f);
    }
}

/* ★ 诊断计数（只读，用 J-Link 按符号读）
 *   fovr  : bxCAN 接收 FIFO 溢出次数。RF0R 是“读清”寄存器，
 *           HAL_CAN_GetRxFifoFillLevel() 也会读它 -> 必须在这里先锁存 ✗?>
 *   maxfrm: 单次 poll 最多搬了几帧（判断是否总是撞 3 深上限）
 *   polls : poll 总次数 */
volatile uint32_t g_can_fovr_cnt;
volatile uint32_t g_can_poll_maxframe;
volatile uint32_t g_can_poll_cnt;

int bsp_can_poll(void)
{
    uint32_t rf0 = hcan.Instance->RF0R;      /* 直接读：FMP[1:0] + FOVR（读清）*/
    uint32_t fmp = rf0 & CAN_RF0R_FMP0;

    g_can_poll_cnt++;
    if ((rf0 & CAN_RF0R_FOVR0) != 0u) {
        g_can_fovr_cnt++;                    /* ★ FIFO 溢出实锤 */
    }
    if (fmp > g_can_poll_maxframe) {
        g_can_poll_maxframe = fmp;
    }

    if (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) > 0u) {
        drain_fifo(CAN_RX_FIFO0, false);
    }
    if (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO1) > 0u) {
        drain_fifo(CAN_RX_FIFO1, false);
    }
    if (HAL_CAN_GetError(&hcan) != HAL_CAN_ERROR_NONE) {
        s_err_cnt++;
    }
    return 0;
}

/* 中断上下文入口（可选）：CubeMX 打开 CAN1 RX0 NVIC 后从 it.c 调 */
void bsp_can_rx_isr(void)
{
    drain_fifo(CAN_RX_FIFO0, true);
}

bool bsp_can_recv(bsp_can_frame_t *out)
{
    bool got = false;

    if (out == NULL) {
        return false;
    }

    unsigned key = sched_enter_critical();
    if (s_tail != s_head) {
        *out  = s_ring[s_tail];
        s_tail = (s_tail + 1u) % BSP_CAN_RX_RING;
        got = true;
    }
    sched_exit_critical(key);
    return got;
}

uint32_t bsp_can_rx_count(void) { return s_rx_cnt; }
uint32_t bsp_can_tx_count(void) { return s_tx_cnt; }
uint32_t bsp_can_err_count(void) { return s_err_cnt; }

bool bsp_can_started(void)
{
    return (HAL_CAN_GetState(&hcan) == HAL_CAN_STATE_LISTENING);
}

int bsp_can_set_loopback(bool enable)
{
    (void) HAL_CAN_Stop(&hcan);

    hcan.Init.AutoBusOff = ENABLE;   /* ★ bus-off 后自动恢复（否则永久失联）*/
    hcan.Init.AutoBusOff    = ENABLE;   /* ★ 同样需要 */
    hcan.Init.Mode = enable ? CAN_MODE_LOOPBACK : CAN_MODE_NORMAL;
    if (HAL_CAN_Init(&hcan) != HAL_OK) {
        return -1;
    }
    if (HAL_CAN_ConfigFilter(&hcan, &s_filter) != HAL_OK) {
        return -2;
    }
    if (HAL_CAN_Start(&hcan) != HAL_OK) {
        return -3;
    }
    return 0;
}


/* ★★ 直读 bxCAN 错误状态（用户诊断需求）
 * 为什么需要它：应用层的"发=N 错=0"**看不出总线是否健康** ✗ ——
 *   `HAL_CAN_AddTxMessage()` 返回 OK 只代表**成功入队** ✓，
 *   与"总线上有没有节点回 ACK"无关 ✓；真总线上帧要成功，
 *   必须 TEC（发送错误计数）保持 0、REC 不飙升 ✓
 *
 * ESR 位定义（RM0008 bxCAN）：
 *   [2:0]  LEC  = 最近一次错误码（0=无错, 3=ACK 错, ...）
 *   [23:16] TEC = 发送错误计数（≥256 = bus-off）
 *   [31:24] REC = 接收错误计数（≥128 = error-passive）
 */
void bsp_can_error_state(uint8_t *tec, uint8_t *rec, uint8_t *lec, bool *bus_off)
{
    uint32_t esr = hcan.Instance->ESR;
    uint32_t msr = hcan.Instance->MSR;

    if (tec)     { *tec = (uint8_t) ((esr >> 16) & 0xFFu); }
    if (rec)     { *rec = (uint8_t) ((esr >> 24) & 0xFFu); }
    if (lec)     { *lec = (uint8_t) (esr & 0x07u); }
    if (bus_off) { *bus_off = ((esr & 0x04u) != 0u) || ((msr & 0x01u) == 0u && ((esr >> 16) & 0xFFu) >= 255u); }
}


/* ★★ CAN1 RX0 中断服务程序
 *
 * 覆盖 startup_stm32f103xb.s 里的 weak 符号。
 * 写在 App 层（bsp_can.c）而不是 stm32f1xx_it.c
 *   —— 因为 it.c 是 CubeMX 拥有的，重新生成会被覆盖。
 *
 * 中断里只做：把硬件 FIFO 搬进环形队列 + 计数
 * 绝不做：打印、日志、阻塞、FreeRTOS API（这些归 app_can_task）
 */
void CAN1_RX0_IRQHandler(void);

void CAN1_RX0_IRQHandler(void)
{
    s_irq_cnt++;
    drain_fifo(CAN_RX_FIFO0, true);
}

/* 诊断：接收中断进入次数 */
uint32_t bsp_can_irq_count(void) { return s_irq_cnt; }

/* ★ 诊断：环形队列溢出丢帧数。
 *   负载率是否可信看这个：只要它一直在涨，负载率就偏低 */
uint32_t bsp_can_drop_count(void) { return s_drop_cnt; }

/* ★ 累计位长（负载率用）：调用方按秒取差值即可 */
uint32_t bsp_can_bits_rx(void) { return s_bits_rx; }
uint32_t bsp_can_bits_tx(void) { return s_bits_tx; }
