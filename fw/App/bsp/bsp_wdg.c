/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_wdg.c —— IWDG 实现
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_wdg.h"

#include "main.h"
#include "bsp_uart.h"

/* ─────────────────────────────────────────────────────────────────────────────
 *  ★ 为什么直接操作寄存器而不是用 HAL_IWDG_*
 *    CubeMX 当前没在 .ioc 里使能 IWDG，因此生成的 stm32f1xx_hal_conf.h 里没有
 *    #define HAL_IWDG_MODULE_ENABLED —— IWDG_HandleTypeDef / HAL_IWDG_Init() 都不存在。
 *    若去改 stm32f1xx_hal_conf.h（CubeMX 拥有），下次生成会被覆盖。
 *    → 直接写 IWDG 寄存器：零依赖、不会被 CubeMX 回归、也不需要 HAL 模块。
 *      （明天在 CubeMX 里加 IWDG 后，这段可与生成的 MX_IWDG_Init 共存）
 *
 *  IWDG 关键寄存器（RM0008）：
 *    KR  0x5555 = 放开 PR/RLR 写保护 ; 0xCCCC = 启动 ; 0xAAAA = 喂狗
 *    PR  分频 = 4 × 2^PR
 *    RLR 重装值
 *    超时 T = (4 × 2^PR × (RLR + 1)) / f_LSI      (f_LSI ≈ 40 kHz，数据手册 30~60)
 *    PR=4 → /64；RLR=1249 → 64 × 1250 / 40000 = 2.00 s
 * ───────────────────────────────────────────────────────────────────────────── */

#define IWDG_KR_UNLOCK   0x5555u
#define IWDG_KR_START    0xCCCCu
#define IWDG_KR_FEED     0xAAAAu

#define IWDG_PR_DIV64    4u
#define IWDG_RLR_2S      1249u

static bool s_on;

/* 使能 LSI（IWDG 的时钟源；CubeMX 没配 IWDG 所以 LSI 也没开）
 * ★ 刻意**不用 HAL_GetTick() 做超时**：HAL 节拍（TIM3）可能被 BASEPRI 屏蔽
 *   （见 app_start 里关于 BASEPRI 的说明）—— 那种情况下基于 tick 的超时永远不成立，
 *   看门狗初始化会把启动永久卡死。改用受限次数的忙等：LSI 典型 1~2 ms 起振，
 *   20 万次空循环在 72 MHz 下已达秒级，足够宽裕又绝不会死循环。 */
static int lsi_enable(void)
{
    RCC->CSR |= RCC_CSR_LSION;

    for (uint32_t i = 0u; i < 200000u; i++) {
        if ((RCC->CSR & RCC_CSR_LSIRDY) != 0u) {
            return 0;
        }
    }
    return -1;                          /* LSI 起不来 */
}

int bsp_wdg_init(void)
{
#if BSP_WDG_ENABLE
    if (lsi_enable() != 0) {
        bsp_uart_puts("[wdg] ✗ LSI 未能启动，IWDG 放弃\r\n");
        s_on = false;
        return -1;
    }

    IWDG->KR = IWDG_KR_UNLOCK;          /* 放开 PR/RLR */
    IWDG->PR = IWDG_PR_DIV64;
    IWDG->RLR = IWDG_RLR_2S;

    /* 等 PR/RLR 同步完成（同样用受限次数忙等，不依赖 tick） */
    for (uint32_t i = 0u; i < 20000u; i++) {
        if (IWDG->SR == 0u) {
            break;
        }
    }

    IWDG->KR = IWDG_KR_START;           /* 启动（此后无法关闭） */
    IWDG->KR = IWDG_KR_FEED;            /* 立刻喂一次，给出完整的 2 s 窗口 */

    s_on = true;
    bsp_uart_puts("[wdg] ✓ IWDG 已启动（≈2.0 s，此后无法关闭）\r\n");
    return 0;
#else
    s_on = false;
    bsp_uart_puts("[wdg] 已按编译开关禁用（BSP_WDG_ENABLE=0）\r\n");
    return 0;
#endif
}

void bsp_wdg_feed(void)
{
#if BSP_WDG_ENABLE
    if (s_on) {
        IWDG->KR = IWDG_KR_FEED;
    }
#endif
}

bool bsp_wdg_enabled(void)
{
    return s_on;
}
