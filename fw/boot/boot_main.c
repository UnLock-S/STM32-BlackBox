/* ─────────────────────────────────────────────────────────────────────────────
 *  boot_main.c —— SBB Bootloader（裸机，无 RTOS）
 * ─────────────────────────────────────────────────────────────────────────────
 *  职责（按顺序）:
 *    ① 时钟 / 串口 / SPI / SD 最小初始化
 *    ② 读标志页（0x0800FC00），判断是否需要升级
 *    ③ 需要升级  -> 从 SD 读镜像、擦写 App 区、CRC 校验（boot_update.c）
 *    ④ 校验 App 向量表 -> 设置 VTOR -> 跳转
 *
 *  设计原则:
 *    - **永不擦写自己所在的 0~8 KB**：任何时刻掉电，下次上电都还能进来重试 ✓
 *    - 标志页独立（最后一页）：擦 App 区不会碰到标志 -> 掉电后标志仍在 ✓
 *    - 一切失败都回落到"停在 bootloader 并提示"，绝不跳进半成品 App ✗
 * ───────────────────────────────────────────────────────────────────────────── */
#include <string.h>

#include "boot_cfg.h"

#include "main.h"           /* HAL + 外设句柄类型 */
#include "bsp_uart.h"
#include "bsp_spi.h"
#include "bsp_sdcard.h"
#include "boot_flash.h"
#include "boot_update.h"

/* ── 本工程自己持有的句柄（与 App 的 main.c 里同名，但属于各自的可执行文件）── */
SPI_HandleTypeDef  hspi1;
UART_HandleTypeDef huart1;

/* ── HAL 时基：本工程不编译 stm32f1xx_hal_timebase_tim.c，用 SysTick 默认时基 ── */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
    }
}

/* ── 时钟：72 MHz（HSE 8 MHz 无源晶振 × 9），与 App 完全一致 ──────────────── */
static void boot_clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;          /* ★ 蓝板是无源晶振，必须 ON 不是 BYPASS */
    osc.HSEPredivValue      = RCC_HSE_PREDIV_DIV1;
    osc.HSIState            = RCC_HSI_ON;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL          = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                       | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        Error_Handler();
    }
}

/* ── CS 引脚（PA4）───────────────────────────────────────────────────────────────
 *  ★ bootloader 不编译 CubeMX 的 main.c，因此 MX_GPIO_Init() 不会执行 ✗
 *    SD 的 CS（PA4）就一直没被配置成输出 -> 卡永远不应答 CMD0 ✗
 *    （真机实测：不配时 10 次 CMD0 全 rc=-1 R1=0xFF；配上后一次成功）*/
static void boot_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);   /* 先拉高（释放）*/
    g.Pin   = SD_CS_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SD_CS_GPIO_Port, &g);
}

/* ── 串口 + SPI（GPIO/时钟由 CubeMX 生成的 HAL_*_MspInit 完成）─────────────── */
static void boot_periph_init(void)
{
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        Error_Handler();
    }

    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 115200;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        Error_Handler();
    }

    bsp_uart_init();
    (void) bsp_spi_init();
}

/* ── 标志页读取（Stage A 只读并打印，写入在 Stage B 实现）────────────────── ── */
static int boot_meta_read(boot_meta_t *m)
{
    const boot_meta_t *p = (const boot_meta_t *) BOOT_META_ADDR;

    if (m == NULL) {
        return -1;
    }
    memcpy(m, p, sizeof(*m));
    if (m->magic != BOOT_META_MAGIC) {
        return -2;                       /* 空白页 = 0xFF/0x00，视为"无标志" */
    }
    return 0;
}

/* ── App 向量表校验 + 跳转 ────────────────────────────────────────────────── */
static int boot_app_vector_ok(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t *) (addr + 0u);
    uint32_t pc = *(volatile uint32_t *) (addr + 4u);

    /* 栈顶必须落在 SRAM（0x20000000..0x20005000），复位入口必须在 Flash 且为 Thumb */
    if ((sp & 0x2FFE0000u) != 0x20000000u) {
        return -1;
    }
    if ((pc & 0xFFFF0000u) != 0x08000000u) {
        return -2;
    }
    if ((pc & 1u) == 0u) {
        return -3;
    }
    return 0;
}

static void boot_jump(uint32_t addr) __attribute__((noreturn));
static void boot_jump(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t *) (addr + 0u);
    uint32_t pc = *(volatile uint32_t *) (addr + 4u);

    bsp_uart_logf("[boot] \u2192 \u8df3\u8f6c\u5e94\u7528 0x%08lX (sp=0x%08lX pc=0x%08lX)\r\n",
                  (unsigned long) addr, (unsigned long) sp, (unsigned long) pc);

    __disable_irq();

    SysTick->CTRL = 0u;                  /* 关掉 boot 的时基，交给 App 重新配 */
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;
    SCB->ICSR    |= SCB_ICSR_PENDSTCLR_Msk;

    /* ★ VTOR 必须在跳转前指向 App 的向量表，否则第一个中断会跑回 bootloader ✗ */
    SCB->VTOR = addr;

    __set_MSP(sp);                       /* ★ 先换栈，再跳 */
    __DSB();
    __ISB();

    __enable_irq();
    ((void (*)(void)) pc)();

    for (;;) {                           /* 不该到这里 */
    }
}

/* ── 主流程 ───────────────────────────────────────────────────────────────── */
int main(void)
{
    boot_meta_t meta;
    int rc;

    HAL_Init();
    boot_clock_init();
    boot_gpio_init();          /* ★ 必须在 SPI/SD 之前：CS 要先配成输出 */
    boot_periph_init();

    bsp_uart_puts("\r\n");
    bsp_uart_logf("[boot] SBB Bootloader %s  build %s %s\r\n", "v0.9.0", __DATE__, __TIME__);
    bsp_uart_logf("[boot] app@0x%08lX  max %lu KB   meta@0x%08lX\r\n",
                  (unsigned long) BOOT_APP_ADDR,
                  (unsigned long) (BOOT_APP_MAX_SIZE / 1024u),
                  (unsigned long) BOOT_META_ADDR);

    /* ① 标志页 */
    rc = boot_meta_read(&meta);
    if (rc != 0) {
        bsp_uart_logf("[boot] \u6807\u5fd7\u9875\u65e0\u6548 (rc=%d) \u2192 \u6b63\u5e38\u542f\u52a8\r\n", rc);
    } else {
        bsp_uart_logf("[boot] \u6807\u5fd7: state=0x%08lX size=%lu crc=0x%08lX count=%lu\r\n",
                      (unsigned long) meta.state,
                      (unsigned long) meta.img_size,
                      (unsigned long) meta.img_crc,
                      (unsigned long) meta.boot_count);
    }

    /* ② 升级请求（Stage B 在此接入 SD 搬运；现在先明确拒绝，避免误判为已完成）*/
    if (rc == 0 && meta.state == BOOT_META_STATE_REQUEST) {
        int urc;

        bsp_uart_puts("[boot] ★ 收到升级请求，开始从 SD 搬运\r\n");
        urc = boot_update_run();

        if (urc == 0) {
            boot_flash_unlock();
            (void) boot_meta_erase();      /* 清标志 */
            boot_flash_lock();
            bsp_uart_puts("[boot] ✓ 标志已清除，跳转新固件\r\n");
        } else if (urc <= -18) {
            /* 已开始擦除（App 区不完整）→ 绝不能跳过去 */
            bsp_uart_logf("[boot] ✗ 升级失败 rc=%d，停在 bootloader 重试\r\n", urc);
            int tries = 0;
            for (;;) {
                if (++tries > 5) {
                    boot_flash_unlock();
                    (void) boot_meta_erase();
                    boot_flash_lock();
                    break;
                }
                HAL_Delay(3000u);
                bsp_uart_puts("[boot] 重试升级...\r\n");
                if (boot_update_run() == 0) {
                    boot_flash_unlock();
                    (void) boot_meta_erase();
                    boot_flash_lock();
                    break;
                }
            }
        } else {
            /* 还没动 App 区（卡没插/文件不对）→ 跳老固件 */
            bsp_uart_logf("[boot] ⚠ 升级未执行 rc=%d，继续启动老固件\r\n", urc);
        }
    }

    /* ③ SD 探测（Stage B 才真正用来读镜像；这里先证明 bootloader 能驱动卡）*/
    {
        const bsp_sd_info_t *si = NULL;
        int i;
        int src = -1;

        /* ★ bootloader 在复位后 ~50 ms 就跑，卡的 LDO + 内部上电
         *   还没完成 -> CMD0 必然无响应 ✗（实测 3 次全失败）。
         *   改成 10 次 x 200 ms（最长 2 s），直到卡就绪。 */
        for (i = 0; i < 10; i++) {
            src = bsp_sdcard_init();
            if (src == 0) {
                break;
            }
            HAL_Delay(200u);
        }
        if (src == 0) {
            si = bsp_sdcard_info();
            bsp_uart_logf("[boot] SD \u5c31\u7eea  %lu KB  %lu \u5757\r\n",
                          (si != NULL) ? (unsigned long) si->capacity_kb : 0ul,
                          (si != NULL) ? (unsigned long) si->blocks : 0ul);
        } else {
            bsp_uart_logf("[boot] \u26a0 SD \u4e0d\u53ef\u7528 (rc=%d)\uff0c\u4ec5\u8df3\u8f6c\u5e94\u7528\r\n", src);
        }
    }

    /* ④ 跳转 */
    rc = boot_app_vector_ok(BOOT_APP_ADDR);
    if (rc != 0) {
        bsp_uart_logf("[boot] \u2717 App \u5411\u91cf\u8868\u65e0\u6548 (rc=%d)\uff0c\u505c\u5728 bootloader\r\n", rc);
        bsp_uart_logf("       0x%08lX = 0x%08lX\r\n0x%08lX = 0x%08lX\r\n",
                      (unsigned long) BOOT_APP_ADDR,
                      (unsigned long) *(volatile uint32_t *) BOOT_APP_ADDR,
                      (unsigned long) (BOOT_APP_ADDR + 4u),
                      (unsigned long) *(volatile uint32_t *) (BOOT_APP_ADDR + 4u));
        for (;;) {
            HAL_Delay(500u);
            bsp_uart_puts("[boot] \u7b49\u5f85\u56fa\u4ef6\uff08App \u533a\u65e0\u6548\uff09\r\n");
        }
    }

    boot_jump(BOOT_APP_ADDR);
}
