/* ─────────────────────────────────────────────────────────────────────────────
 *  ssd1306.c —— SSD1306 OLED 驱动（I2C，128x64，page 寻址）
 * ─────────────────────────────────────────────────────────────────────────────
 *  显存为 8 页 × 128 列的字节阵列，每字节 1 列 8 像素（LSB = 上一行）。
 *  文字用 5x7 点阵（列字节，bit0 = 顶行），渲染时把 5 列字节按位铺进显存。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "ssd1306.h"

#include <stdarg.h>
#include <string.h>

#include "bsp_delay.h"
#include "bsp_i2c.h"
#include "main.h"          /* HAL 声明（HAL_GetTick 用于刷屏耗时仪表） */
#include "bsp_uart.h"

#define SSD_CTRL_CMD       0x00u
#define SSD_CTRL_DATA      0x40u

static uint8_t s_buf[SSD1306_WIDTH * SSD1306_HEIGHT / 8u];   /* 1024 B */
static int     s_addr;
static bool    s_ready;

/* 可观测性：写失败次数。**必须计数** —— 之前 flush 忽略返回码，
 * 结果 I2C 明明全失败还打印"已写入测试画面"（真机踩过）。 */
static volatile uint32_t s_err_cnt;

uint32_t ssd1306_errors(void) { return s_err_cnt; }

/* 面板保健：重发关键配置位。见头文件里的说明（掉配置仍会 ACK，是很容易漏掉的一类故障）。 */
static volatile uint32_t s_repair_cnt;
static volatile uint32_t s_heal_cnt;

uint32_t ssd1306_repairs(void) { return s_repair_cnt; }
uint32_t ssd1306_heals(void)   { return s_heal_cnt; }

/* ★★ 最小保健：只补发【电荷泵开 0x8D,0x14 + 显示开 0xAF】——
 *    一条命令事务 4 字节、**没有显示关**，所以绝不会白闪。
 *
 * 为什么必须有它（本项目实测的失败模式）：
 *   面板会"静默丢掉显示使能"—— 此时**ACK 正常、写入成功、写失败计数恒为 0**，
 *   但画面停止更新（停在旧帧），过一会儿又可能被某次写入"碰醒"。
 *   于是「只在写失败时触发保健」这条判据**永远等不到** ✗
 *   （本项目就因此让屏幕定格了 10 分钟以上，而所有计数器都是"正常"的）。
 *
 * ★★ 最小保健：重发【所有影响显示的配置】，但**不含 0xAE（显示关）** → 绝不白闪。
 *
 * 为什么扩展到这么多条（真机实测教训）：
 *   原先只补 `0x2E + 0x8D,0x14 + 0xAF`（5 字节）✗ ——
 *   实测面板定格时，保健已经发了 **201 次** 依然救不回来 ✗
 *   → 说明丢掉的不是"显示使能/电荷泵"，而是**对比度、多路复用比、扫描方向、
 *     起始行、显示偏移**这类其他显示配置 ✓
 *
 * 这里把它们全部重发一遍 ✓（去掉 0xAE 显示关 → 不会闪屏 ✓）；
 * 代价：一条事务约 17 字节（25kHz 下 <4ms），可忽略 ✓
 *
 * ⚠ 完整序列 ssd1306_repair()（含 0xAE）仍保留给"真的写失败"的情形。 */
int ssd1306_heal_min(void)
{
    static const uint8_t seq[] = {
        0x2Eu,        /* 关滚动 */
        0xA6u,        /* 正常显示（非反白） */
        0xA4u,        /* 显示跟随 RAM */
        0xA8u, 0x3Fu, /* 多路复用比 = 64 */
        0xD3u, 0x00u, /* 显示偏移 = 0 */
        0x40u,        /* 起始行 = 0 */
        0xC8u,        /* COM 扫描方向：反向 */
        0xA1u,        /* 段重映射 */
        0x81u, 0xCFu, /* 对比度 */
        0x8Du, 0x14u, /* 电荷泵开 */
        0xAFu,        /* 显示开 */
    };
    uint8_t buf[1u + sizeof(seq)];

    if (!s_ready) {
        return -1;
    }
    buf[0] = SSD_CTRL_CMD;
    memcpy(&buf[1], seq, sizeof(seq));
    if (bsp_i2c_write((uint8_t) s_addr, buf, sizeof(buf)) != 0) {
        s_err_cnt++;
        return -1;
    }
    s_heal_cnt++;
    return 0;
}

/* ★ 仪表：上一次整屏刷新的耗时（ms）。见 ssd1306_flush() 里的说明。 */
static volatile uint32_t s_flush_ms;

uint32_t ssd1306_last_flush_ms(void) { return s_flush_ms; }

/* ★ 初始化与"面板保健"共用的命令序列。
 *   注意这里**没有任何清显存命令**（0xAE/0xAF 只是关/开显示，不动 RAM），
 *   所以可以随时重发而不会丢画面 —— 这是保健函数能周期调用的前提。
 *   ⚠ 只重发几个关键字（对比度/电荷泵/显示开）是**不够的**：真机踩过，
 *     面板"内部寻址/扫描方向被抖动打乱"时救不回来（表现：屏幕定格、右下角缺块）。 */
static const uint8_t s_oled_seq[] = {
    0xAE,              /* 显示关（同一事务内马上又开，肉眼看不到） */
    0x2E,              /* ★ 关闭滚动！—— 真机踩过：烧写打断命令传输时，面板可能收到
                        *   一条残缺的滚动设置命令(0x26/0x27/0x29)而进入滚动模式，
                        *   表现为"白屏一直滚动"。没有这条就永远关不掉滚动 ✗，
                        *   当时只能靠按 MCU 复位键（碰巧让面板状态机复位）才恢复。 */
    0x20, 0x02,        /* 寻址模式 = 页寻址 */
    0xB0,              /* 起始页 */
    0xC8,              /* COM 扫描方向：反向 */
    0x00, 0x10,        /* 低/高列地址 = 0 */
    0x40,              /* 起始行 = 0 */
    0x81, 0xCF,        /* 对比度 */
    0xA1,              /* 段重映射 */
    0xA6,              /* 正常显示（非反白） */
    0xA8, 0x3F,        /* 多路复用比 = 64 */
    0xA4,              /* 显示跟随 RAM */
    0xD3, 0x00,        /* 显示偏移 = 0 */
    0xD5, 0x80,        /* 时钟分频 */
    0xD9, 0xF1,        /* 预充电周期 */
    0xDA, 0x12,        /* COM 引脚配置（128x64） */
    0xDB, 0x40,        /* VCOMH 反压 */
    0x8D, 0x14,        /* ★ 电荷泵开 —— 不开屏幕不亮 */
    0xAF,              /* 显示开 */
};

int ssd1306_repair(void)
{
    /* ★ 重发**完整**配置序列（而不是只重发几个关键字）。见 s_oled_seq 的说明。 */
    uint8_t buf[1u + sizeof(s_oled_seq)];

    if (!s_ready) {
        return -1;
    }

    buf[0] = SSD_CTRL_CMD;
    memcpy(&buf[1], s_oled_seq, sizeof(s_oled_seq));
    if (bsp_i2c_write((uint8_t) s_addr, buf, sizeof(buf)) != 0) {
        s_err_cnt++;
        return -1;
    }
    s_repair_cnt++;
    return 0;
}

/* ── 经典 5x7 点阵（公有领域常用表；bit0=顶行）。仅收数字/大写/常用符号 ── */
typedef struct {
    char    ch;
    uint8_t col[5];
} glyph_t;

static const glyph_t s_font[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
    { '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
    { ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
    { '/', { 0x20, 0x10, 0x08, 0x04, 0x02 } },
    { '%', { 0x62, 0x64, 0x08, 0x13, 0x23 } },
    { '>', { 0x41, 0x22, 0x14, 0x08, 0x00 } },
    { '*', { 0x14, 0x08, 0x3E, 0x08, 0x14 } },
    { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
    { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
    { '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
    { '3', { 0x21, 0x41, 0x45, 0x4B, 0x31 } },
    { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
    { '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
    { '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } },
    { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
    { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
    { '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
    { 'A', { 0x7E, 0x11, 0x11, 0x11, 0x7E } },
    { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } },
    { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
    { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } },
    { 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
    { 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } },
    { 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } },
    { 'H', { 0x7F, 0x08, 0x08, 0x08, 0x7F } },
    { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } },
    { 'J', { 0x20, 0x40, 0x41, 0x3F, 0x01 } },
    { 'K', { 0x7F, 0x08, 0x14, 0x22, 0x41 } },
    { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
    { 'M', { 0x7F, 0x02, 0x04, 0x02, 0x7F } },
    { 'N', { 0x7F, 0x04, 0x08, 0x10, 0x7F } },
    { 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } },
    { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
    { 'Q', { 0x3E, 0x41, 0x51, 0x21, 0x5E } },
    { 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
    { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
    { 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } },
    { 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } },
    { 'V', { 0x1F, 0x20, 0x40, 0x20, 0x1F } },
    { 'W', { 0x7F, 0x20, 0x18, 0x20, 0x7F } },
    { 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
    { 'Y', { 0x03, 0x04, 0x78, 0x04, 0x03 } },
    { 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },
};

static const glyph_t *find_glyph(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char) (c - 'a' + 'A');
    }
    for (size_t i = 0u; i < (sizeof(s_font) / sizeof(s_font[0])); i++) {
        if (s_font[i].ch == c) {
            return &s_font[i];
        }
    }
    return &s_font[0];                  /* 未收录 → 空格 */
}

/* ── 低层 I2C 写 ──
 * 注：初始化序列已改为**一次事务**发送（见 ssd1306_init），
 * flush 也自己拼 3/65 字节缓冲，所以不再需要通用的"逐命令"封装。 */

int ssd1306_init(void)
{
    /* ★ 全部初始化命令放**一次 I2C 事务**发（控制字节 0x00 + N 个命令字节）。
     *   原因（真机教训）：STM32F1 的 I2C 每次传输超时都可能把外设打成 BUSY 卡死，
     *   27 次独立事务 = 27 次风险；一次事务既快又稳。
     * ★ 寻址模式设为 **页寻址 (0x20, 0x02)** —— 与 ssd1306_flush() 使用的
     *   "0xB0|page + 列地址 + 连续 128 字节" 完全一致。
     *   （原代码设成水平模式 0x00 却用页寻址指令写数据，属于自相矛盾。）
     * ★ 若模块是 128x64 的 **SH1106**（1.3" 常见）而非 SSD1306，
     *   把 SSD1306_COL_OFFSET 改成 2（它的 RAM 有 2 列偏移）。 */
    /* 初始化序列与保健共用一份（见文件上方 s_oled_seq） */

    s_ready = false;
    s_addr  = SSD1306_I2C_ADDR;
    s_err_cnt = 0u;

    (void) bsp_i2c_init();

    /* ★★ 软件"硬复位"—— 开机（= 每次烧写后）自动执行一次"能做到的最大程度复位"。
     *
     * 背景：SSD1306 **没有软件复位命令** ✗、4 脚模块**没有 RES 引脚** ✗，
     * 所以软件无法真正断面板的电；这里做能做到的全部：
     *   ① 强制总线解锁（9 个时钟 + STOP）→ 让面板的 I2C 状态机回到空闲，
     *      释放它在传输途中被打断时可能咬住的 SCL/SDA；
     *   ② RCC 强制复位 I2C 外设 → 清掉 MCU 侧残留（含 HAL 早退留下的 Lock）；
     *   之后紧接着发**完整 init 序列**（含 0x2E 关滚动、0x20,0x02 页寻址）
     *   → 面板的显示寄存器全部回到已知状态 ✓
     *   （面板 RAM 的覆盖由 app_display 的首帧 flush 完成 —— 8 页全覆盖 ✓）
     *
     * ⚠ 这一步只在 init 流程里做（开机/器件复位），**运行期绝不做** ✗
     *   （运行期往正常总线打时钟会逐步打乱面板，见 Bug #12/#16）。
     * ⚠⚠ 且**只在首次 init 时做一次**：app_display 在面板未就绪时会每 500ms 重试 init，
     *     若每次都打时钟，就退化成"运行期反复注入垃圾"（正是 Bug #12 的翻版）✗ */
    static bool s_hard_reset_done;
    if (!s_hard_reset_done) {
        s_hard_reset_done = true;
        /* ★★ 开机先让总线【持续空闲 2 秒】再初始化 —— 这是让定格面板恢复的关键。
         *   实测差异（用户）：
         *     J-Link 的 RESETPIN 只是毫秒级脉冲 → 面板纹丝不动 ✗
         *     按住板子 reset 键 2~3 秒 → 面板立即恢复 ✓
         *   差别就在"总线持续空闲的时长"：面板 I2C 状态机需要足够长的空闲才复位 ✓
         *   这一段把 SCL/SDA 释放为输入（不驱动），两条线被模块上拉持续拉高 ✓
         *   之后再由 clear_force 打时钟 + 复位外设，把引脚恢复成 AF 开漏 ✓ */
        bsp_i2c_bus_idle_ms(2000u);
        bsp_i2c_clear_force();
        (void) bsp_i2c_recover();
    }

    /* ★ 探测重试：真机实测——开机时第一次探测失败、手动再跑就成功
     *   （F1 的 I2C 首次使用偶发失败）→ 重试 3 次再下结论。
     *   另一道保险是 app_display 任务里的周期重试（支持热插拔）。
     *   ⚠ 这里【不再】每次重试都做强制总线解锁：那是运行期动作，
     *     而且会把 PB6/PB7 切成输入、需要配对恢复外设（本项目踩过：忘了配对
     *     导致引脚一直是输入、面板永久失联）✗
     *     —— 面板失联的恢复现在由"烧写/复位时的真硬件复位(RESETPIN)"负责 ✓ */
    {
        int found = 0;

        for (int attempt = 0; attempt < 3 && !found; attempt++) {
            if (bsp_i2c_probe((uint8_t) s_addr) == 0) {
                found = 1;
                break;
            }
            if (bsp_i2c_probe(0x3Du) == 0) {
                s_addr = 0x3D;
                found = 1;
                break;
            }
            /* ★ 每次探测失败都主动复位一次外设 + 解锁总线。
             *   不这么做就会形成"死循环依赖"：面板被打乱 → 探测失败 →
             *   s_ready 保持 0 → flush() 不执行 → 保健函数永远不被调用 → 永远救不回。 */
            (void) bsp_i2c_recover();
            bsp_delay_us(20000u);
        }

        if (!found) {
            bsp_uart_logf("[oled] ✗ 未找到 SSD1306（试过 0x3C / 0x3D，各 3 次）\r\n");
            bsp_uart_logf("       总线忙=%d 失败次数=%lu 恢复次数=%lu\r\n",
                          bsp_i2c_busy(),
                          (unsigned long) bsp_i2c_fail_count(),
                          (unsigned long) bsp_i2c_recover_count());
            return -1;
        }
    }

    bsp_delay_us(10000u);

    /* 一次性把初始化序列发出去 */
    {
        uint8_t buf[1u + sizeof(s_oled_seq)];
        buf[0] = SSD_CTRL_CMD;
        memcpy(&buf[1], s_oled_seq, sizeof(s_oled_seq));
        if (bsp_i2c_write((uint8_t) s_addr, buf, sizeof(buf)) != 0) {
            s_err_cnt++;
            bsp_uart_logf("[oled] ✗ 初始化序列发送失败（I2C 错误）\r\n");
            bsp_uart_logf("       总线忙=%d 失败次数=%lu 恢复次数=%lu\r\n",
                          bsp_i2c_busy(),
                          (unsigned long) bsp_i2c_fail_count(),
                          (unsigned long) bsp_i2c_recover_count());
            return -2;
        }
    }

    ssd1306_clear();
    ssd1306_flush();

    if (s_err_cnt != 0u) {
        bsp_uart_logf("[oled] ⚠ 初始化后有 %lu 次写入失败（屏幕可能是空的）\r\n",
                      (unsigned long) s_err_cnt);
        return -3;
    }

    s_ready = true;
    bsp_uart_logf("[oled] ✓ SSD1306 就绪 @0x%02X\r\n", (unsigned) s_addr);
    return 0;
}

bool ssd1306_ready(void) { return s_ready; }
int  ssd1306_addr(void)  { return s_addr; }

void ssd1306_clear(void)
{
    memset(s_buf, 0x00, sizeof(s_buf));
}

/* 用单个字节模式填满显存：0xFF = 全亮（整片白）、0x00 = 全灭（整片黑）。
 * 用途：像素级点亮测试 —— 不依赖字形表与寻址细节，
 * 能在 2 秒内判断"面板是否受控、寻址方向对不对"。 */
void ssd1306_fill(uint8_t pattern)
{
    memset(s_buf, pattern, sizeof(s_buf));
}

void ssd1306_flush(void)
{
    /* ★ 面板保健：**只在确实发生写失败时才重发配置**。
     *
     * 历史教训（真机可见现象：屏幕每 20~25 秒白闪一下）：
     *   早期版本按"每 10 次刷屏"或"I2C 自愈计数有变化"就重发初始化序列 ✗ ——
     *   总线健康、写失败为 0 时也照发，电荷泵/显示开关瞬变 → 肉眼看到周期性白闪。
     *   判据必须锚定【面板是否真的没接住数据】= 写失败次数，
     *   而不是【刷屏次数/自愈次数】这类"软件侧噪声"。
     *   （与 Bug #12 同一条教训：动作要由真实故障证据触发，不能由软件计数触发。） */
    static uint32_t s_err_at_last_repair;

    if (!s_ready) {
        return;
    }

    /* ★ 写失败累计增长 >= 3 次 → 面板确实丢了配置/被打断 → 补发【完整】配置 */
    if ((s_err_cnt - s_err_at_last_repair) >= 3u) {
        s_err_at_last_repair = s_err_cnt;
        (void) ssd1306_repair();
    }

    /* ★★ 周期性【最小保健】兜底（每 10 次刷屏，约 5~9 秒一次）。
     *   实测面板会"静默丢掉显示使能"：写失败恒为 0、所有计数器都正常，
     *   但画面定格在旧帧 → 上面那条"写失败触发"的保健永远等不到 ✗。
     *   这里只补发 0x8D,0x14 + 0xAF（4 字节、无显示关）→ 不白闪、开销可忽略。
     *   ⚠ 与 Bug #12/#16 的区别：这是**向器件重发配置**（器件自己能处理的合法命令），
     *     不是往总线打时钟/复位外设那种物理层干涉 —— 前者安全，后者必须严加把关。
     *   ⚠ 间隔取 3 次刷屏而不是 10：实测显示任务的整轮周期约 3 秒（刷屏 0.4s + 任务延时），
     *     按 10 次算要 30 秒才兜底一次，用户会明确感到"卡了很久"。取 3 ≈ 10 秒内必自愈。 */
    static uint32_t s_heal_tick;
    if ((++s_heal_tick % 3u) == 0u) {
        (void) ssd1306_heal_min();
    }

    /* ★ 仪表：记录本次刷屏耗时（ms）。用于一眼判断 I2C 总线是否被 RC 拖慢：
     *   1032 字节在 25 kHz 下理论约 372 ms、50 kHz 下约 186 ms。
     *   若实测远大于理论值 → 总线上升沿太慢（上拉弱/线太长），先治硬件。 */
    uint32_t t0 = HAL_GetTick();

    for (uint8_t page = 0u; page < 8u; page++) {
        /* ★ 每页只发 **2 次** I2C 事务（原先 4 次）：
         *   ① 1 条命令事务：控制字节 0x00 + 页地址 + 列地址低/高 → 一条事务里连发合法；
         *   ② 1 条数据事务：控制字节 0x40 + 整页 128 字节 → 页寻址模式下写完会自动绕回本页列 0。
         *   为什么在意：本项目实测 I2C 走线 RC 偏大（长杜邦线），事务越多越容易出 BERR 毛刺，
         *   而毛刺会把面板的 I2C 解析器打乱（表现为面板仍 ACK 但画面定格）。
         *   32 次 → 16 次，等于把被打乱的机会砍半。 */
        uint8_t cmd[6];
        uint8_t data[1u + SSD1306_WIDTH];

        /* ★★ 每页都【重新声明寻址模式】再设页码/列地址（6 字节，原来 4 字节）。
         *
         * 为什么必须重发 0x20,0x02：面板是**有状态**的，而它的寻址模式可能被
         * 【残缺命令 / 上电时序 / 总线毛刺】改成水平模式(0x20,0x00)。
         * 水平模式下写 128 字节会**自动跨页推进** → 本页数据溢出到后面各页
         * → 表现为"白条 + 文字残留 + 画面一直在变"（用户实测），最后定格 ✗
         *
         * 只发"页码+列地址"是【依赖面板上一状态保持正确】的写法 ✗；
         * 每页自带模式声明后，无论面板之前处于什么状态，本页都一定写对 ✓✓✓
         * 代价：每页多 3 字节 × 8 页 = 24 字节（约 1ms），可忽略。 */
        cmd[0] = SSD_CTRL_CMD;
        cmd[1] = 0x20u; cmd[2] = 0x02u;                 /* 寻址模式 = 页寻址（关键） */
        cmd[3] = (uint8_t) (0xB0u | page);              /* 页地址 */
        cmd[4] = (uint8_t) (0x00u + (SSD1306_COL_OFFSET & 0x0Fu));   /* 列地址低 4 位 */
        cmd[5] = (uint8_t) (0x10u + ((SSD1306_COL_OFFSET >> 4) & 0x0Fu)); /* 列地址高 4 位 */
        if (bsp_i2c_write((uint8_t) s_addr, cmd, sizeof(cmd)) != 0) {
            s_err_cnt++;                     /* ★ 必须计数，别再"静默成功" */
        }

        data[0] = SSD_CTRL_DATA;
        memcpy(&data[1], &s_buf[page * SSD1306_WIDTH], SSD1306_WIDTH);
        if (bsp_i2c_write((uint8_t) s_addr, data, sizeof(data)) != 0) {
            s_err_cnt++;
        }
    }

    s_flush_ms = HAL_GetTick() - t0;
}

/* ★★ 以下两个函数【专为诊断】而加，见 app_display 的开机自检 */

static void draw_col(uint8_t x, uint8_t line, uint8_t bits);   /* 前置声明 */

/* 静态棋盘格：相邻列交替 0xAA/0x55 → 均匀的 1 像素棋盘。
 * 用途：判断"画面一直在滚动"到底是
 *   ① 面板真的处于硬件滚动模式 ✗，还是
 *   ② 逐页写入（每页几十~几百 ms、8 页写完要几秒）造成的"画面在动"的错觉 ✓
 * 判读：棋盘静止且均匀 → 是 ②（面板正常）；持续滚动 → 是 ①。 */
void ssd1306_fill_checker(void)
{
    for (uint16_t i = 0u; i < (uint16_t) (SSD1306_WIDTH * 8u); i++) {
        s_buf[i] = ((i & 1u) != 0u) ? 0xAAu : 0x55u;
    }
}

/* 1 像素边框：四条边贴边画满。
 * 用途：判断寻址模式 / 列偏移是否正确 ——
 *   边框完整贴边           → 寻址正确 ✓
 *   缺边 / 错位 / 整体斜移 → 寻址模式或列偏移(COL_OFFSET)不对 ✗ */
void ssd1306_border(void)
{
    uint8_t x;
    uint8_t page;

    for (x = 0u; x < SSD1306_WIDTH; x++) {
        draw_col(x, 0u, 0x01u);                 /* 顶边：page0 的 bit0 */
        draw_col(x, 7u, 0x80u);                 /* 底边：page7 的 bit7 */
    }
    for (page = 0u; page < 8u; page++) {
        draw_col(0u, page, 0xFFu);                                       /* 左边 */
        draw_col((uint8_t) (SSD1306_WIDTH - 1u), page, 0xFFu);           /* 右边 */
    }
}

static void draw_col(uint8_t x, uint8_t line, uint8_t bits)
{
    uint8_t  page = (uint8_t) (line & 0x07u);
    uint16_t idx;

    if (x >= SSD1306_WIDTH) {
        return;
    }
    idx = (uint16_t) (page * SSD1306_WIDTH) + x;
    s_buf[idx] = bits;                  /* 直接覆盖该列该页 */
}

void ssd1306_text(uint8_t x, uint8_t line, const char *s)
{
    if (s == NULL) {
        return;
    }

    for (; *s != '\0'; s++) {
        const glyph_t *g = find_glyph(*s);

        for (uint8_t c = 0u; c < 5u; c++) {
            draw_col((uint8_t) (x + c), line, g->col[c]);
        }
        draw_col((uint8_t) (x + 5u), line, 0x00u);   /* 字间空列 */
        x = (uint8_t) (x + 6u);
        if ((x + 5u) >= SSD1306_WIDTH) {
            break;
        }
    }
}

/* 极简格式化（复用与 bsp_uart 相同的原则：不引 newlib stdio） */
void ssd1306_textf(uint8_t x, uint8_t line, const char *fmt, ...)
{
    char    out[24];
    size_t  pos = 0u;
    va_list ap;

    va_start(ap, fmt);
    for (const char *p = fmt; *p != '\0' && pos < (sizeof(out) - 1u); p++) {
        if (*p != '%') {
            out[pos++] = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 's': {
            const char *t = va_arg(ap, const char *);
            while (t != NULL && *t != '\0' && pos < (sizeof(out) - 1u)) {
                out[pos++] = *t++;
            }
        } break;
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            char    tmp[12];
            int     n = 0;
            bool    neg = (v < 0);
            uint32_t u = neg ? (uint32_t) (-(int64_t) v) : (uint32_t) v;
            if (u == 0u) {
                tmp[n++] = '0';
            }
            while (u != 0u && n < 12) {
                tmp[n++] = (char) ('0' + (u % 10u));
                u /= 10u;
            }
            if (neg) {
                out[pos++] = '-';
            }
            while (n > 0 && pos < (sizeof(out) - 1u)) {
                out[pos++] = tmp[--n];
            }
        } break;
        case 'u': {
            uint32_t u = va_arg(ap, uint32_t);
            char     tmp[12];
            int      n = 0;
            if (u == 0u) {
                tmp[n++] = '0';
            }
            while (u != 0u && n < 12) {
                tmp[n++] = (char) ('0' + (u % 10u));
                u /= 10u;
            }
            while (n > 0 && pos < (sizeof(out) - 1u)) {
                out[pos++] = tmp[--n];
            }
        } break;
        case '%':
            out[pos++] = '%';
            break;
        default:
            out[pos++] = '%';
            if (pos < (sizeof(out) - 1u)) {
                out[pos++] = *p;
            }
            break;
        }
    }
    va_end(ap);

    out[pos] = '\0';
    ssd1306_text(x, line, out);
}

/* ★★ 柱状趋势图：n 个柱子，每柱高度 = value / vmax × 8（该页 8 像素）
 *   柱子自下往上长；值非零时至少亮 1 行（否则小值会完全看不见 ✗）
 *   定标交给调用方（传窗口峰值）—— 这样低负载也能看出波动 ✓ */
void ssd1306_chart(uint8_t x, uint8_t line, const uint8_t *v, uint8_t n, uint8_t vmax)
{
    if (vmax == 0u) {
        vmax = 1u;
    }
    for (uint8_t i = 0u; i < n; i++) {
        uint32_t h = ((uint32_t) v[i] * 8u) / vmax;

        if (h == 0u && v[i] != 0u) {
            h = 1u;                          /* 非零至少 1 行 */
        }
        if (h > 8u) {
            h = 8u;
        }
        /* SSD1306 里 bit0 是该页最【上】一行 → 自下往上长需左移 */
        draw_col((uint8_t) (x + i), line,
                 (uint8_t) ((h == 0u) ? 0x00u : (0xFFu << (8u - h))));
    }
}

void ssd1306_bar(uint8_t x, uint8_t line, uint8_t pct)
{
    uint8_t cols = (uint8_t) ((pct > 100u) ? 100u : pct);
    uint8_t filled = (uint8_t) ((cols * 20u) / 100u);   /* 20 列宽 */

    for (uint8_t i = 0u; i < 20u; i++) {
        draw_col((uint8_t) (x + i), line, (i < filled) ? 0x7Fu : 0x41u);
    }
}
