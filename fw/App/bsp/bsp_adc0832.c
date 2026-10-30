/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_adc0832.c —— C0832C/ADC0832 位翻转驱动
 * ─────────────────────────────────────────────────────────────────────────────
 *  用直接寄存器写（BSRR/ODR/IDR）而不是 HAL_GPIO_WritePin —— 位翻转对时序敏感，
 *  HAL 调用的开销会让半周期不稳定。GPIO 方向已由 MX_GPIO_Init() 配好：
 *    CS/CLK/DI = 推挽输出，DO = 输入（无上拉）
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_adc0832.h"

#include "main.h"
#include "bsp_delay.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* ── 并发保护 ──
 * ★ 为什么必须加：C0832C 是**位翻转**协议，读一次要用 PB12(CS)/PB13(CLK)/PB14(DI)/
 *   PB15(DO) 走 28 个半周期（~30 µs）。而 ADC 任务（100 ms 周期）和控制台任务
 *   （敲 `adc` 命令时）**同优先级 2**，FreeRTOS 会按 tick 时间片轮转 ——
 *   完全可能在一次读的中途被切走，另一个任务随即开始自己的读，把 CS/CLK/DI 搅乱，
 *   两边都读到乱值（甚至 DO 采样错位）。
 *   → 用静态互斥量把整次读做成临界资源（与 bsp_spi/bsp_i2c 同一套模式）。 */
static StaticSemaphore_t s_mutex_cb;
static SemaphoreHandle_t s_mutex;

static void lock(void)
{
    if (s_mutex != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void) xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mutex != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void) xSemaphoreGive(s_mutex);
    }
}

#define CS_PORT   C0832_CS_GPIO_Port
#define CS_PIN    C0832_CS_Pin
#define CLK_PORT  C0832_CLK_GPIO_Port
#define CLK_PIN   C0832_CLK_Pin
#define DI_PORT   C0832_DI_GPIO_Port
#define DI_PIN    C0832_DI_Pin
#define DO_PORT   C0832_DO_GPIO_Port
#define DO_PIN    C0832_DO_Pin

#define SET(p, m)     ((p)->BSRR = (m))
#define CLR(p, m)     ((p)->BRR  = (m))
#define DO_READ()     (((DO_PORT->IDR & DO_PIN) != 0u) ? 1u : 0u)

static inline void clk_low(void)  { CLR(CLK_PORT, CLK_PIN); }
static inline void clk_high(void) { SET(CLK_PORT, CLK_PIN); }
static inline void di_high(void)  { SET(DI_PORT, DI_PIN); }
static inline void di_low(void)   { CLR(DI_PORT, DI_PIN); }
static inline void cs_low(void)   { CLR(CS_PORT, CS_PIN); }
static inline void cs_high(void)  { SET(CS_PORT, CS_PIN); }

int bsp_adc0832_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_cb);
    }
    cs_high();          /* 片选释放 */
    clk_low();
    di_low();
    return (s_mutex != NULL) ? 0 : -1;
}

/* 一个时钟脉冲：低 → 高 → 低。命令阶段在低电平期间摆好 DI。 */
static void pulse_with_di(unsigned bit)
{
    clk_low();
    bsp_delay_us(BSP_ADC0832_HALF_US);
    if (bit != 0u) {
        di_high();
    } else {
        di_low();
    }
    bsp_delay_us(BSP_ADC0832_HALF_US);
    clk_high();
    bsp_delay_us(BSP_ADC0832_HALF_US);
    clk_low();
    bsp_delay_us(BSP_ADC0832_HALF_US);
}

int bsp_adc0832_read(uint8_t ch, uint8_t *value)
{
    uint8_t v = 0u;
    int     rc = 0;

    if (value == NULL || ch > BSP_ADC0832_CH1) {
        return -1;
    }

    lock();                          /* 见文件上方并发保护说明 */

    cs_high();
    clk_low();
    di_low();
    bsp_delay_us(BSP_ADC0832_HALF_US);

    cs_low();                       /* 选中 */
    bsp_delay_us(BSP_ADC0832_HALF_US);

    /* 3 位命令：起始位 1、SGL/DIFF=1（单端）、ODD/SIGN=通道号 */
    pulse_with_di(1u);
    pulse_with_di(1u);
    pulse_with_di((ch == BSP_ADC0832_CH0) ? 0u : 1u);

    /* ── 命令后读取 8 位数据 ──
     * ★★ 采样沿是这里的关键（真机踩过，值会**正好差一半**）：
     *   ADC0832 在 **CLK 下降沿**改变/输出数据，且命令阶段结束后 DO 要到
     *   **下一个下降沿**才输出 MSB。若在**上升沿之后**采样，第一位会采到 MSB
     *   之前的那一位（DO 尚处高阻 → 读 0），于是整个字节左移一格、丢掉 LSB
     *   → 读到的值 = 真值 >> 1（正好一半）。
     *   实测证据：滑动端 1.461 V / VREF 5.079 V → 期望 256×0.2877 ≈ **73.6**，
     *             旧代码读到 **36**；36×2 = 72 ≈ 73.6 ✓ 一半的关系。
     *   （CH1 接 GND 时读 0 —— 0 左右移都是 0，**根本暴露不出这个错误**，
     *     所以必须用非零的已知输入才能验出来。）
     * → 正确做法：**在下降沿之后采样**（与器件手册/通用实现一致）。 */
    for (int i = 0; i < 8; i++) {
        clk_high();
        bsp_delay_us(BSP_ADC0832_HALF_US);
        clk_low();
        bsp_delay_us(BSP_ADC0832_HALF_US);
        v = (uint8_t) ((v << 1) | DO_READ());   /* ★ 下降沿之后采样 */
    }

    cs_high();                      /* 释放 */
    di_low();
    clk_low();

    *value = v;
    unlock();
    return rc;
}

int bsp_adc0832_read_avg(uint8_t ch, uint8_t n, uint16_t *avg)
{
    uint32_t sum = 0u;

    if (avg == NULL || n == 0u) {
        return -1;
    }

    for (uint8_t i = 0u; i < n; i++) {
        uint8_t v;
        if (bsp_adc0832_read(ch, &v) != 0) {
            return -1;
        }
        sum += v;
    }
    *avg = (uint16_t) (sum / n);
    return 0;
}
