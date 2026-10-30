/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_i2c.c —— I2C1 板级封装（HAL，线程安全）
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include "bsp_i2c.h"

#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"

extern I2C_HandleTypeDef hi2c1;

/* ★ I2C 单次传输超时（ms）。
 *
 * 为什么是 500 而不是 100（真机踩坑，代价很大）：
 *   显示任务优先级最低（1），传输中途会被 ADC/CAN/console/心跳 抢占；
 *   而 HAL 的超时是**从传输开始**按 tick 计的 → 一旦"传输时间 + 被抢占时间"超过超时值，
 *   HAL 会**中止传输**，并把 I2C 外设留在坏状态（之后每次操作都失败 → 自愈复位 → 反复）。
 *   实测：100ms 超时 + 25kHz 下 129 字节(46ms) → 写失败累计 1299 次、自愈 3615 次，
 *   面板只收到部分数据（表现为"上半屏白、下半屏雪花"）。
 *   取 500ms：25~50kHz 下单次最长约 46ms，留出 10 倍被抢占余量；真故障时最坏多等 0.5s。
 *   ⚠ 配套认知：**降速 / 加大单次事务都会吃掉这个余量**，改速率时必须一起复核本值。 */
#define BSP_I2C_TIMEOUT_MS   500u

/* ★ 主机发送用的短超时（配合下面的寄存器级发送函数）。
 *   START/地址阶段：正常几 µs 内应完成，20ms 足够；失败说明从机没应答或总线坏了。
 *   单字节 TXE：一个字节在 25kHz 下约 0.36ms，50ms 是极宽松的上限。
 *   —— 这两个值只用于"卡死时快速退出"，不承担被抢占余量（那是 BSP_I2C_TIMEOUT_MS 的职责）*/
#define BSP_I2C_START_TIMEOUT_MS   20u
#define BSP_I2C_BYTE_TIMEOUT_MS    50u

static StaticSemaphore_t s_mutex_cb;
static SemaphoreHandle_t s_mutex;

/* ★★ 取锁超时与持有标志（2026-10-16 修复死锁）
 * 原实现 xSemaphoreTake(s_mutex, portMAX_DELAY) 是【无限等待】✗：
 * 显示任务一旦卡死（手里握着锁），控制台执行任何 oled 命令都会
 * 被永久拖死 —— 实测表现：控制台只剩心跳任务在打 [mon]，命令全部无响应。
 * => 改为限时取锁；拿不到就【不拿】，绝不无限等。 */
#define BSP_I2C_LOCK_TIMEOUT_MS  300u
static volatile uint32_t s_lock_timeout_cnt;   /* 取锁超时次数（可观测）*/
static bool              s_lock_mine;          /* 本次调用是否真的拿到了锁 */

/* 前置声明：下面的恢复函数要用（定义在本文件后半部分） */
static void lock(void);
static void unlock(void);

/* 可观测性计数 */
static volatile uint32_t s_fail_cnt;
static volatile uint32_t s_recover_cnt;

/* ★ 连续失败计数：连续这么多次【真实传输失败】就主动打 9 个时钟解总线。
 *
 * 为什么需要它（真机场景）：烧写/复位 MCU 会打断一次正在进行的传输，
 * 从机停在中途等剩下的时钟，此后不再应答任何命令 ——
 * 而 SCL/SDA 却全是高电平，所以"看线路是否为低"的判据在这里判不出来 ✗
 * （这正是每次烧写后都需要手动给 OLED 断电的根因）。
 *
 * 取 3：偶发单次失败（抢占/毛刺）不会触发；真失步必然连续失败。
 * ★★ 与 Bug #12 的本质区别：触发源是【真实传输失败的连续次数】，
 *    不是软件标志或自愈计数 —— 后者会把"标志脏了"升级成"硬件真坏了"。 */
static volatile uint32_t s_consec_fail;
#define BSP_I2C_BUSCLEAR_AFTER_FAILS   3u

/* ★★ 强制总线解锁的触发与限流（修订版 —— 撤掉"开机 30 秒窗口"）
 *
 * 触发条件：**连续 >= 3 次【真实传输失败】**（不是软件标志、不是自愈计数噪声）
 * 限流：距上次强制解锁至少 BSP_I2C_FORCECLEAR_MIN_GAP_MS，避免连续注入。
 *
 * 真实场景（用户反复遇到的那个"卡死"）：**任何一次调试器 attach/halt、或 MCU 复位，
 * 都会在传输途中打断面板** ✗ → 面板停在中途等剩余时钟 → 此后所有传输都失败 ✗
 * → 画面定格，**只能靠按 reset / 断电才能恢复** ✗。
 * 制解药就是这 9 个时钟 + STOP：把它拉回空闲 ✓
 *
 * ⚠ 教训：本项目曾把强制解锁限制在"开机 30 秒窗口"内 ✗ ——
 *   那个决定是基于【被 J-Link 污染的耗时数据】得出的错误结论（见 DEBUG-LOG Bug #22）✗。
 *   只要触发条件锚定"真实传输失败"+ 有限流，运行期用它就是正确的 ✓ */
#define BSP_I2C_FORCECLEAR_MIN_GAP_MS  2000u

/* 强制解锁次数（可观测性） */
static volatile uint32_t s_forceclear_cnt;
static volatile uint32_t s_last_forceclear;
uint32_t bsp_i2c_forceclear_count(void) { return s_forceclear_cnt; }

/* ★★ 把 SCL/SDA 释放为浮空输入并保持 ms 毫秒（靠模块上拉把两条线持续拉高）。
 *
 * 为什么需要（用户实测的关键差异）：
 *   - J-Link 的 RESETPIN 只是**毫秒级脉冲** → 对定格的面板**无效** ✗
 *   - 而**按住板子 reset 键 2~3 秒**再松开 → 面板**立即恢复** ✓
 *   差别就在"总线持续空闲的时长"：面板的 I2C 状态机需要一段足够长的空闲才肯复位 ✓
 *
 * 用法：开机初始化前调用它（不驱动任何线，让面板自己复位），
 *       之后再由 bsp_i2c_clear_force() 打时钟 + 复位外设恢复引脚为 AF 开漏 ✓ */
void bsp_i2c_bus_idle_ms(uint32_t ms)
{
    lock();
    GPIOB->CRL = (GPIOB->CRL & ~0xFF000000u) | 0x44000000u;   /* PB6/PB7 = 浮空输入 */
    unlock();

    /* 用任务睡眠而不是忙等：这段时间让出 CPU，其余任务照常跑 */
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        for (volatile uint32_t d = 0u; d < (ms * 20000u); d++) {
            /* 调度器未启动时的兜底忙等 */
        }
    }
}

/* 记一次真实传输结果：成功清零连续计数，失败累加 */
static void note_result(int rc)
{
    if (rc == 0) {
        s_consec_fail = 0u;
    } else {
        s_fail_cnt++;
        s_consec_fail++;
    }
}

/* ★ 取锁超时次数：稳居 0 表示互斥量健康；持续增长说明有任务卡死握着锁 */
uint32_t bsp_i2c_lock_timeout_count(void) { return s_lock_timeout_cnt; }

uint32_t bsp_i2c_fail_count(void)    { return s_fail_cnt; }
uint32_t bsp_i2c_recover_count(void) { return s_recover_cnt; }
/* 标准 I2C 总线解锁：从机在传输中途被打断（例如我们在刷屏途中复位了 MCU）
 * 可能咬住 SDA 不放，此时 MCU 侧复位外设没用。做法是把 SCL 临时切成 GPIO 输出
 * 打 9 个时钟，再补一个 STOP，让从机把数据位移完并释放总线。
 *
 * ★ force 参数决定何时允许动手：

 *   force=0（常规自愈）：**只有 SCL/SDA 真被拉低时才打时钟**。
 *   force=1（连续传输失败≥3）：从机内部失步但线路全高 → 必须打时钟。
 * 这个区分是 Bug #12 的教训：动作必须由真实故障证据触发。 */
static void i2c_bus_clear(int force)
{
    uint32_t crl;
    volatile int i;

    /* ★★ 先给总线一段【真正的空闲】—— 这一步才是恢复面板的关键。
     *
     * 用户实测（决定性）：**按 MCU 的 reset 键能让定格的面板立即恢复** ✓
     * 原因：复位期间 MCU **完全不驱动** SCL/SDA，两条线被模块上拉持续拉高，
     *       面板的 I2C 状态机得到一段"持续空闲"而自行复位 ✓
     *
     * 只打 9 个时钟达不到同样效果 ✗（时序太短、且我们在驱动线）——
     * 所以这里先复现"复位期"：把引脚切成【浮空输入】不驱动，空闲约 50 ms，
     * 之后再打时钟 + STOP 双保险 ✓ */
    if (force != 0) {
        GPIOB->CRL = (GPIOB->CRL & ~0xFF000000u) | 0x44000000u;   /* PB6/PB7 = 浮空输入 */
        for (volatile uint32_t d = 0u; d < 1000000u; d++) {
            /* ≈40~50 ms @72MHz：不依赖 DWT/tick，复位早期也安全 */
        }
    }

    /* ★★ 关键前置判据：**只在总线真的被咬住时才打时钟**。
     *
     * 血泪教训（本函数曾经每 2.7 次/秒被调用，直接把 OLED 打死）：
     *   本函数原先无条件执行 → 自愈触发时（约 1 次/刷屏）就往【正常的总线】上
     *   注入 9 个时钟 + 一个 STOP。SSD1306 会把这些当成半截命令吃掉，
     *   解析器被逐步打乱 → 20~30 分钟后必然"面板仍 ACK 但画面定格"。
     *   而自愈触发的原因往往只是 HAL 的 Lock 标志被早退路径钉住（软件状态问题），
     *   总线本身完全正常 —— 此时动总线是纯粹的破坏。
     *
     * 判据：SCL/SDA 两条线都被释放（高）= 总线空闲 = 没有任何从机咬住 → 立即返回。
     * （force=1 时跳过这个判据 —— 那种情况下从机已内部失步，线路是全高的。） */
    if ((force == 0) &&
        ((GPIOB->IDR & (GPIO_PIN_6 | GPIO_PIN_7)) == (GPIO_PIN_6 | GPIO_PIN_7))) {
        return;
    }

    /* PB6(SCL) 在 CRL bits[27:24]，PB7(SDA) 在 bits[31:28]
     * 0x7 = CNF 01(开漏输出) + MODE 11(50MHz) */
    crl = GPIOB->CRL;
    GPIOB->CRL = (crl & ~0xFF000000u) | 0x77000000u;
    GPIOB->BSRR = (uint32_t) (GPIO_PIN_6 | GPIO_PIN_7);      /* 两条线都释放（高） */

    for (i = 0; i < 9; i++) {
        GPIOB->BRR  = GPIO_PIN_6;                            /* SCL 低 */
        for (volatile int d = 0; d < 60; d++) { }
        GPIOB->BSRR = GPIO_PIN_6;                            /* SCL 高 */
        for (volatile int d = 0; d < 60; d++) { }
    }
    /* STOP：SCL 高时把 SDA 由低拉高 */
    GPIOB->BRR  = GPIO_PIN_7;
    for (volatile int d = 0; d < 60; d++) { }
    GPIOB->BSRR = GPIO_PIN_6;
    for (volatile int d = 0; d < 60; d++) { }
    GPIOB->BSRR = GPIO_PIN_7;
    for (volatile int d = 0; d < 60; d++) { }
    /* PB6/PB7 的引脚配置由随后的 HAL_I2C_Init()→MspInit() 恢复成 AF 开漏 */
}

/* 复位 I2C 外设。调用者需已持有锁（内部不重复加锁）。
 *
 * ★ 为什么必须这么"重"（真机实测 4/4 次恢复后探测依旧失败，逐层定位出来的）：
 *   1) **HAL 的早退不解锁**：HAL_I2C_IsDeviceReady() 里
 *        __HAL_LOCK(hi2c);
 *        if (State == READY) { if (BUSY==SET) return HAL_BUSY; }   ← 直接 return
 *      这个分支**不调用 __HAL_UNLOCK**，于是 hi2c->Lock 永久停在 HAL_LOCKED，
 *      之后**所有** HAL I2C 调用一进门就被 __HAL_LOCK 挡回 HAL_BUSY。
 *      → 必须显式把 hi2c1.Lock 复位。
 *   2) **单纯 PE 0→1 清不掉 F1 卡住的 BUSY 位**（BUSY 要等 STOP 才由硬件清，
 *      超时路径不发 STOP）→ 必须用 RCC 强制复位整个 I2C 外设。
 *   3) 从机侧咬住总线（BERR/STOPF 等）→ 先打 9 个时钟解锁总线。 */
static int i2c_reset_peripheral(void)
{
    __HAL_I2C_DISABLE(&hi2c1);

    i2c_bus_clear(0);                      /* ★ 先尝试解开被从机咬住的总线（仅在真被咬住时动手） */

    /* 用 RCC 强制复位 I2C1（这是唯一可靠清 BUSY 的手段） */
    __HAL_RCC_I2C1_FORCE_RESET();
    for (volatile uint32_t i = 0u; i < 4000u; i++) {
        /* 等复位生效（~数十 µs） */
    }
    __HAL_RCC_I2C1_RELEASE_RESET();

    /* 清掉 HAL 句柄里被"早退不解锁"污染的锁与状态 */
    hi2c1.Lock      = HAL_UNLOCKED;
    hi2c1.State     = HAL_I2C_STATE_RESET;
    hi2c1.ErrorCode = HAL_I2C_ERROR_NONE;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        return -1;
    }
    s_recover_cnt++;
    return 0;
}

int bsp_i2c_recover(void)
{
    int rc;
    lock();
    rc = i2c_reset_peripheral();
    unlock();
    return rc;
}

/* ★ 供上层做"软件硬复位"：先让总线彻底空闲（引脚切输入不驱动 ≈50ms），
 *   再打 9 个时钟 + STOP。**这是软件能做到的、最接近"按 MCU reset 键"的复位** ✓
 *   仅用于开机 / 器件失联后的恢复流程。 */
void bsp_i2c_clear_force(void)
{
    lock();
    /* ★ 限流：init 在面板失联时会被每 500ms 重试，
     *   若每次都打时钟就变成"持续干扰" ✗。只在距上次 >= 2s 时真正动手。 */
    if ((HAL_GetTick() - s_last_forceclear) >= BSP_I2C_FORCECLEAR_MIN_GAP_MS) {
        s_last_forceclear = HAL_GetTick();
        i2c_bus_clear(1);
        s_forceclear_cnt++;
    }
    /* ★★ 必须紧跟一次外设恢复：i2c_bus_clear 会把 PB6/PB7 切成输入/输出并改动 GPIO 配置，
     *   只有 HAL_I2C_Init（在 i2c_reset_peripheral 里）才会把引脚恢复成 AF 开漏 ✗
     *   —— 不配对的话，之后 I2C 外设根本驱动不了总线，所有传输永久失败 ✗
     *   （本项目踩过：面板失联时 init 每 500ms 重试，而恢复只在首次做 → 引脚一直是输入）*/
    (void) i2c_reset_peripheral();
    unlock();
}

/* 每次传输前的自愈检查（调用者需已持有锁）。
 * 三种"已经坏了"的表征都要触发复位：
 *   总线忙(BUSY=1) / HAL 句柄被锁(Lock=LOCKED) / 外设处于 RESET 态 */
static void i2c_prepare(void)
{
    /* ★ 运行期**不做**强制总线解锁。
     *
     * 为什么撤掉（今晚的教训链条）：面板"传输被打断后失联"的场景，
     * 现在已由【烧写/复位时的真硬件复位】从根上解决 ✓ ——
     * tools/flash-jlink.sh 走 RESETPIN（把 RST 脚拉低），
     * 面板得到一段"总线持续空闲"而自行复位 ✓（用户接的 nRESET 线）
     * → 运行期再注入 9 个时钟属于多余动作 ✗，且它本身要切引脚/打拍，
     *   在显示任务（最低优先级）里做会带来新的时序风险 ✗。
     *
     * 因此这里只保留"确有其事"的两条：外设状态异常 → 复位外设（不动总线电平）。 */

    /* 判据：总线忙 / HAL 句柄被锁 / 外设处于复位态 → 复位外设（不动总线电平） */
    if (((hi2c1.Instance->SR2 & I2C_SR2_BUSY) != 0u) ||
        (hi2c1.Lock != HAL_UNLOCKED) ||
        (hi2c1.State == HAL_I2C_STATE_RESET)) {
        (void) i2c_reset_peripheral();
    }
}

/* 供控制台/诊断用：当前是否处于"已坏"状态 */
int bsp_i2c_busy(void)
{
    return (((hi2c1.Instance->SR2 & I2C_SR2_BUSY) != 0u) ||
            (hi2c1.Lock != HAL_UNLOCKED)) ? 1 : 0;
}

int bsp_i2c_locked(void)
{
    return (hi2c1.Lock != HAL_UNLOCKED) ? 1 : 0;
}

static void lock(void)
{
    s_lock_mine = false;
    if (s_mutex != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(BSP_I2C_LOCK_TIMEOUT_MS)) == pdTRUE) {
            s_lock_mine = true;
        } else {
            s_lock_timeout_cnt++;      /* ★ 超时：不拿锁继续走，绝不无限等 ✓ */
        }
    } else {
        s_lock_mine = true;            /* 调度器未启动：不需要锁 ✓ */
    }
}

static void unlock(void)
{
    /* ★ 只有真的拿到了锁才归还 —— 否则会破坏互斥量计数 ✗ */
    if (s_lock_mine && s_mutex != NULL
        && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void) xSemaphoreGive(s_mutex);
    }
    s_lock_mine = false;
}

int bsp_i2c_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_cb);
    }
    return (s_mutex != NULL) ? 0 : -1;
}

/* ★★ 主机发送：寄存器级实现，刻意【不等 BTF】。
 *
 * 为什么要绕开 HAL（真机踩坑，Bug #14）：
 *   F1 的 HAL 在每个 Master_Transmit 末尾会等 BTF（Byte Transfer Finished）标志，
 *   而本板 OLED 面板在"ACK 时序退化"状态下 BTF 迟迟不来 →
 *   每个事务白等满 BSP_I2C_TIMEOUT_MS(500ms)，实测刷屏 5.3~12.6 秒，
 *   而数据其实是发出去的（HAL 靠 TXE 豁免仍返回成功，所以写失败计数很低）。
 *
 *   主机**发送**方向并不需要 BTF（BTF 只对"连续传输/读操作"有意义），
 *   所以改成：等 TXE → 写 DR → 最后一字节后只等 2ms 兜底 → 发 STOP。
 *   收益：不再依赖面板的 ACK 时序；烧写复位打断传输后也能正常工作。
 *
 * 返回值：0 成功；-1 超时/总线异常；-2 从机 NACK（AF）
 * 注意：调用者必须已持有 bsp 互斥锁，且已执行 i2c_prepare()。 */
static int i2c_master_write_raw(uint8_t addr7, const uint8_t *buf, size_t len)
{
    I2C_TypeDef *I = hi2c1.Instance;
    uint32_t t0;

    /* ① 起始条件 */
    I->CR1 |= I2C_CR1_START;
    t0 = HAL_GetTick();
    while ((I->SR1 & I2C_SR1_SB) == 0u) {
        if ((HAL_GetTick() - t0) > BSP_I2C_START_TIMEOUT_MS) {
            I->CR1 |= I2C_CR1_STOP;
            return -1;
        }
    }

    /* ② 7 位地址 + 写方向 */
    I->DR = (uint16_t) ((uint16_t) addr7 << 1u);
    t0 = HAL_GetTick();
    while ((I->SR1 & I2C_SR1_ADDR) == 0u) {
        if ((I->SR1 & I2C_SR1_AF) != 0u) {
            I->SR1 = (uint16_t) ~I2C_SR1_AF;      /* ST 惯用法：写 0 清 AF */
            I->CR1 |= I2C_CR1_STOP;
            return -2;
        }
        if ((HAL_GetTick() - t0) > BSP_I2C_START_TIMEOUT_MS) {
            I->CR1 |= I2C_CR1_STOP;
            return -1;
        }
    }
    (void) I->SR1;                                /* 读 SR1 再读 SR2 清 ADDR */
    (void) I->SR2;

    /* ③ 逐字节发送 */
    for (size_t i = 0u; i < len; i++) {
        t0 = HAL_GetTick();
        while ((I->SR1 & I2C_SR1_TXE) == 0u) {
            if ((I->SR1 & I2C_SR1_AF) != 0u) {
                I->SR1 = (uint16_t) ~I2C_SR1_AF;
                I->CR1 |= I2C_CR1_STOP;
                return -2;
            }
            if ((HAL_GetTick() - t0) > BSP_I2C_BYTE_TIMEOUT_MS) {
                I->CR1 |= I2C_CR1_STOP;
                return -1;
            }
        }
        I->DR = buf[i];
    }

    /* ④ 等最后一个字节真的出去 —— 只等 2ms（这里就是要绕开的那个 500ms） */
    t0 = HAL_GetTick();
    while (((I->SR1 & I2C_SR1_BTF) == 0u) && ((I->SR1 & I2C_SR1_AF) == 0u)) {
        if ((HAL_GetTick() - t0) > 2u) {
            break;                                /* 面板退化时 BTF 不来，不强等 */
        }
    }
    if ((I->SR1 & I2C_SR1_AF) != 0u) {
        I->SR1 = (uint16_t) ~I2C_SR1_AF;
        I->CR1 |= I2C_CR1_STOP;
        return -2;
    }

    /* ⑤ 停止条件，并等 BUSY 落下（限 2ms）再放行下一次传输 */
    I->CR1 |= I2C_CR1_STOP;
    t0 = HAL_GetTick();
    while (((I->SR2 & I2C_SR2_BUSY) != 0u) && ((HAL_GetTick() - t0) <= 2u)) {
        /* 等 STOP 生效 */
    }
    return 0;
}

int bsp_i2c_write(uint8_t addr7, const uint8_t *buf, size_t len)
{
    int rc;
    lock();
    i2c_prepare();
    rc = i2c_master_write_raw(addr7, buf, len);
    note_result(rc);
    unlock();
    return (rc == 0) ? 0 : -1;
}

int bsp_i2c_read(uint8_t addr7, uint8_t *buf, size_t len)
{
    int rc;
    lock();
    i2c_prepare();
    rc = (HAL_I2C_Master_Receive(&hi2c1, (uint16_t) ((uint16_t) addr7 << 1u),
                                 buf, (uint16_t) len,
                                 BSP_I2C_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
    note_result(rc);
    unlock();
    return rc;
}

int bsp_i2c_mem_write(uint8_t addr7, uint8_t mem, const uint8_t *buf, size_t len)
{
    int rc;
    lock();
    i2c_prepare();
    rc = (HAL_I2C_Mem_Write(&hi2c1, (uint16_t) ((uint16_t) addr7 << 1u), mem,
                            I2C_MEMADD_SIZE_8BIT, (uint8_t *) buf, (uint16_t) len,
                            BSP_I2C_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
    note_result(rc);
    unlock();
    return rc;
}

int bsp_i2c_mem_read(uint8_t addr7, uint8_t mem, uint8_t *buf, size_t len)
{
    int rc;
    lock();
    i2c_prepare();
    rc = (HAL_I2C_Mem_Read(&hi2c1, (uint16_t) ((uint16_t) addr7 << 1u), mem,
                           I2C_MEMADD_SIZE_8BIT, buf, (uint16_t) len,
                           BSP_I2C_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
    note_result(rc);
    unlock();
    return rc;
}

int bsp_i2c_probe(uint8_t addr7)
{
    int rc;
    lock();
    i2c_prepare();
    rc = (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t) ((uint16_t) addr7 << 1u),
                                2u, BSP_I2C_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
    unlock();
    return rc;
}
