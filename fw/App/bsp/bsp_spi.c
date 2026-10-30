/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_spi.c —— SPI1 板级封装（HAL，线程安全，可切换速率）
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_spi.h"

#include "main.h"

#ifndef BSP_WITH_RTOS
/* ★ 本 BSP 同时被 App（有 RTOS）和 bootloader（裸机）编译。
 *   bootloader 用 -DBSP_WITH_RTOS=0 关掉互斥量代码，
 *   避免为了一把锁把整个 FreeRTOS 内核拉进 8 KB 的 bootloader。 */
#define BSP_WITH_RTOS 1
#endif

#if BSP_WITH_RTOS
#include "FreeRTOS.h"
#include "semphr.h"
#endif

/* CubeMX 在 cubemx/Core/Src/main.c 里定义 */
extern SPI_HandleTypeDef hspi1;

#define BSP_SPI_TIMEOUT_MS   500u
/* ★ 锁超时：原来用 portMAX_DELAY 无限等 ✗
 *   日志刷块(ADC任务) 与 OTA 写卡(CAN任务) 抢同一把锁，
 *   一旦持有者卡在 SD 忙等里，另一侧永远拿不到 -> 整个 CAN 任务死锁 ✗
 *   实测：370 个 CF 全收、零序号错，但第 6 条消息后板子完全不响应 */
#define BSP_SPI_LOCK_TIMEOUT_MS  200u

#if BSP_WITH_RTOS
static StaticSemaphore_t s_mutex_cb;
static SemaphoreHandle_t s_mutex;
#endif
static unsigned          s_speed = BSP_SPI_SPEED_FAST;

/* APB2 = 72 MHz 下的分频选择 */
static uint32_t prescaler_of(unsigned which)
{
    return (which == BSP_SPI_SPEED_SLOW) ? SPI_BAUDRATEPRESCALER_256
                                         : SPI_BAUDRATEPRESCALER_8;
}

static bool s_lock_mine;

static void lock(void)
{
#if BSP_WITH_RTOS
    s_lock_mine = false;
    if (s_mutex != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(BSP_SPI_LOCK_TIMEOUT_MS)) == pdTRUE) {
            s_lock_mine = true;
        }
    } else {
        s_lock_mine = true;      /* 调度器未启动：单执行流，不需要锁 */
    }
#else
    s_lock_mine = true;
#endif
}

static void unlock(void)
{
#if BSP_WITH_RTOS
    if (s_lock_mine && s_mutex != NULL
        && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void) xSemaphoreGive(s_mutex);
    }
#endif
    s_lock_mine = false;
}

int bsp_spi_init(void)
{
#if BSP_WITH_RTOS
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_cb);
    }
#endif
    /* 片选默认释放（拉高） */
    bsp_spi_cs(false);
    /* 默认慢速：SD 卡上电后必须先低速发 74+ 个 CLK */
    return bsp_spi_set_speed(BSP_SPI_SPEED_SLOW);
}

int bsp_spi_set_speed(unsigned which)
{
    uint32_t psc = prescaler_of(which);

    lock();
    if (hspi1.Init.BaudRatePrescaler != psc) {
        hspi1.Init.BaudRatePrescaler = psc;
        if (HAL_SPI_Init(&hspi1) != HAL_OK) {
            unlock();
            return -1;
        }
    }
    s_speed = which;
    unlock();
    return 0;
}

unsigned bsp_spi_speed(void)
{
    return s_speed;
}

void bsp_spi_cs(bool assert)
{
    /* 低有效：assert → 拉低 */
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin,
                      assert ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

int bsp_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (len == 0u) {
        return 0;
    }

    int rc = 0;
    lock();
    if (tx != NULL && rx != NULL) {
        rc = (HAL_SPI_TransmitReceive(&hspi1, (uint8_t *) tx, rx, (uint16_t) len,
                                      BSP_SPI_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
    } else if (tx != NULL) {
        rc = (HAL_SPI_Transmit(&hspi1, (uint8_t *) tx, (uint16_t) len,
                               BSP_SPI_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
    } else {
        rc = (HAL_SPI_Receive(&hspi1, rx, (uint16_t) len,
                              BSP_SPI_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
    }
    unlock();
    return rc;
}

int bsp_spi_send(const uint8_t *tx, size_t len)
{
    /* ★★ 必须用【全双工】方式发送（带一个假接收缓冲）—— 真机踩坑：
     *
     * 原来直接调 bsp_spi_xfer(tx, NULL, len) -> HAL_SPI_Transmit（只发不收）✗
     * 只收不发时 MISO 上收到的字节没人取走，硬件会置 OVR（溢出）标志 ✗
     * -> 之后所有全双工读（spi_rw）全部失败，读回恒为 0xFF
     * -> SD 卡驱动的 wait_ready() 永远超时 -> bsp_sdcard_init() 恒返回 -1 ✗
     *
     * 现象很迷惑: 单独用 spi_rw() 的原始探测能收到卡的 0x01 响应，
     *            而走 bsp_spi_send 的完整初始化却一直"无卡"。
     *
     * 修法: 每次发送都带一个丢弃用的接收缓冲，保证 MISO 被读走 ✓ */
    uint8_t  tmp[32];
    size_t   off = 0u;

    if (tx == NULL) {
        return -1;
    }
    while (off < len) {
        size_t n = len - off;

        if (n > sizeof(tmp)) {
            n = sizeof(tmp);
        }
        if (bsp_spi_xfer(tx + off, tmp, n) != 0) {
            return -1;
        }
        off += n;
    }
    return 0;
}

int bsp_spi_recv(uint8_t *rx, size_t len)
{
    return bsp_spi_xfer(NULL, rx, len);
}
