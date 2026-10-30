/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_wdg.h —— 独立看门狗 IWDG
 * ─────────────────────────────────────────────────────────────────────────────
 *  ⚠ 当前 .ioc 里**没有** IWDG（CubeMX 未使能），所以本层直接调用 HAL_IWDG_Init()
 *    自己初始化（用户已授权：CubeMX 管不到的部分临时用代码实现并留档）。
 *    → 明天请在 CubeMX 里加上 IWDG（约 2 s 超时），生成后本文件会自动与
 *      生成的 MX_IWDG_Init() 共存（同参数时无副作用）。见 docs/CUBEMX-TODO.md
 *
 *  LSI ≈ 40 kHz（数据手册 30~60 kHz，不准）。超时 ≈ (4 × 2^PR) × RLR / 40kHz。
 *  取 PR=4（64 分频）、RLR=1249 → 4×64×1249/40000 ≈ 8.0 s？→ 实际 2^4=16
 *  公式：T = (4 × 2^PR × RLR) / f_LSI。PR=4 → 4×16×1249/40000 = 1.999 s ✓
 *
 *  ★ 策略：看门狗一旦启动**无法关闭**（除非复位）。因此
 *    bsp_wdg_init() 只在 app_start() 末尾调用，且喂狗由 app_monitor 任务做。
 *    调试时若不希望被复位，改 BSP_WDG_ENABLE=0 重新编译。
 * ───────────────────────────────────────────────────────────────────────────── */

#ifndef SBB_BSP_WDG_H
#define SBB_BSP_WDG_H

#include <stdbool.h>

/* 1 = 启用看门狗。调试期可置 0 编译掉 */
#ifndef BSP_WDG_ENABLE
#define BSP_WDG_ENABLE   1
#endif

int  bsp_wdg_init(void);
void bsp_wdg_feed(void);
bool bsp_wdg_enabled(void);

#endif /* SBB_BSP_WDG_H */
