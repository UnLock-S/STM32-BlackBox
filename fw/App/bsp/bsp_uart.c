/* ─────────────────────────────────────────────────────────────────────────────
 *  bsp_uart.c —— 调试串口（HAL 实现，线程安全，零 newlib stdio）
 * ─────────────────────────────────────────────────────────────────────────────
 *  ★ 为什么不直接用 vsnprintf/snprintf
 *    实测（arm-none-eabi-nm + linker map）：用 vsnprintf 会把 newlib 的
 *    FILE 机制（findfp.o）拖进镜像，它内部引用 _malloc_r → sbrk_r → _sbrk，
 *    于是 ELF 里出现 malloc 族符号，**ADR-10（禁动态内存）被绕过**（且多耗 ~460 B flash）。
 *    → 本文件自带极简格式化器，只支持日志真正用到的转换，不碰 stdio。
 *
 *  支持：%% %c %s %p %d %i %u %x %X，可带宽度与 '0' 填充、**'-' 左对齐（仅 %s）**、
 *        'l' 长度修饰（32 位下等价）。
 *  ⚠ 限制：**数值转换（%d/%u/%x/%X）不支持 '-' 左对齐** —— 写了会走 default 分支
 *        原样打印成 "%-5d"（刻意如此，宁可看得见也不静默出错）。
 *        需要数值左对齐时，先用 %s 或自行补空格。
 *  不支持：浮点（%f 会原样打印，便于发现）。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "bsp_uart.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

/* CubeMX 在 cubemx/Core/Src/main.c 里定义，此处仅声明 */
extern UART_HandleTypeDef huart1;

#define BSP_UART_TX_TIMEOUT_MS   100u
#define BSP_UART_FMT_BUF         192u
#define BSP_UART_NUM_MAX         12u

/* ── 调试计数（真机可观测性）──
 * 用途：串口"没反应"时，用 J-Link 读这两个数就能立刻分清责任：
 *   g_dbg_uart_tx_bytes 在涨、PC 却收不到 → 问题在**物理链路**（接线/转接板/驱动）
 *   g_dbg_uart_tx_fail 在涨              → 问题在**固件/UART 状态机**（HAL 超时或 BUSY）
 * 不涨                                  → 根本没走到发送（上游逻辑问题） */
volatile uint32_t g_dbg_uart_tx_bytes;
volatile uint32_t g_dbg_uart_tx_fail;

#if BSP_WITH_RTOS
static StaticSemaphore_t s_tx_mutex_cb;
static SemaphoreHandle_t s_tx_mutex;
#endif

void bsp_uart_init(void)
{
#if BSP_WITH_RTOS
    if (s_tx_mutex == NULL) {
        s_tx_mutex = xSemaphoreCreateMutexStatic(&s_tx_mutex_cb);
    }
#endif
}

/* ───────────────────────── 极简格式化器 ───────────────────────── */

static size_t fmt_char(char *b, size_t cap, size_t pos, char c)
{
    if (pos + 1u < cap) {
        b[pos] = c;
    }
    return pos + 1u;
}

static size_t fmt_str(char *b, size_t cap, size_t pos, const char *s, int width,
                      char pad, bool left)
{
    const char *p = (s != NULL) ? s : "(null)";
    size_t      n = strlen(p);
    int         w = width;

    /* 左对齐（%-Ns）：先出内容再补空格；否则先补再出内容 */
    if (!left) {
        while ((size_t) w > n) {
            pos = fmt_char(b, cap, pos, pad);
            w--;
        }
    }
    for (size_t i = 0u; i < n; i++) {
        pos = fmt_char(b, cap, pos, p[i]);
    }
    if (left) {
        while ((size_t) w > n) {
            pos = fmt_char(b, cap, pos, pad);
            w--;
        }
    }
    return pos;
}

static size_t fmt_uint(char *b, size_t cap, size_t pos, uint32_t v, unsigned base,
                       bool neg, bool upper, int width, char pad)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[BSP_UART_NUM_MAX];
    int  n = 0;
    int  w = width;

    if (v == 0u) {
        tmp[n++] = '0';
    }
    while (v != 0u && n < (int) BSP_UART_NUM_MAX) {
        tmp[n++] = dig[v % base];
        v /= base;
    }
    if (neg) {
        tmp[n++] = '-';
    }
    while (n < w) {
        tmp[n++] = pad;
    }
    while (n > 0) {
        pos = fmt_char(b, cap, pos, tmp[--n]);
    }
    return pos;
}

/* 返回写入的字符数（不含结尾 NUL）。不支持的转换按字面输出。 */
int bsp_uart_vlogf(char *out, size_t cap, const char *fmt, va_list ap)
{
    size_t pos = 0u;

    if (out == NULL || cap == 0u || fmt == NULL) {
        return 0;
    }

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            pos = fmt_char(out, cap, pos, *p);
            continue;
        }

        p++;
        if (*p == '\0') {
            break;
        }

        char pad     = ' ';
        int  width   = 0;
        bool left    = false;            /* '-' 左对齐标志 */

        if (*p == '-') {                 /* ★ 必须解析，否则 %-8s 会被当字面量打出
                                          *   （2026-10-14 实测：help 输出成了 "%-8s help"） */
            left = true;
            p++;
        }
        if (*p == '0') {                 /* 零填充 */
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') { /* 宽度 */
            width = (width * 10) + (*p - '0');
            p++;
        }
        if (*p == 'l') {                 /* 长度修饰（32 位下等价）*/
            p++;
            if (*p == 'l') {
                p++;
            }
        }

        switch (*p) {
        case '%':
            pos = fmt_char(out, cap, pos, '%');
            break;
        case 'c':
            pos = fmt_char(out, cap, pos, (char) va_arg(ap, int));
            break;
        case 's':
            pos = fmt_str(out, cap, pos, va_arg(ap, const char *), width, pad, left);
            break;
        case 'd':
        case 'i': {
            int32_t  v   = va_arg(ap, int32_t);
            bool     neg = (v < 0);
            uint32_t u   = neg ? (uint32_t) (-(int64_t) v) : (uint32_t) v;
            pos = fmt_uint(out, cap, pos, u, 10u, neg, false, width, pad);
            break;
        }
        case 'u':
            pos = fmt_uint(out, cap, pos, va_arg(ap, uint32_t), 10u, false, false, width, pad);
            break;
        case 'x':
            pos = fmt_uint(out, cap, pos, va_arg(ap, uint32_t), 16u, false, false, width, pad);
            break;
        case 'X':
            pos = fmt_uint(out, cap, pos, va_arg(ap, uint32_t), 16u, false, true, width, pad);
            break;
        case 'p':
            pos = fmt_str(out, cap, pos, "0x", 0, ' ', false);
            pos = fmt_uint(out, cap, pos, (uint32_t) (uintptr_t) va_arg(ap, void *),
                           16u, false, false, 8, '0');
            break;
        default:                        /* 不支持的转换：原样输出，便于发现 */
            pos = fmt_char(out, cap, pos, '%');
            pos = fmt_char(out, cap, pos, *p);
            break;
        }
    }

    if (pos < cap) {
        out[pos] = '\0';
    } else {
        out[cap - 1u] = '\0';
    }
    return (int) ((pos < cap) ? pos : (cap - 1u));
}

/* ───────────────────────── 收发 ───────────────────────── */

static void bsp_uart_lock(void)
{
#if BSP_WITH_RTOS
    /* 调度器未启动时不能 take；此时只有一个执行流，无需互斥 */
    if (s_tx_mutex != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void) xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    }
#endif
}

static void bsp_uart_unlock(void)
{
#if BSP_WITH_RTOS
    if (s_tx_mutex != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void) xSemaphoreGive(s_tx_mutex);
    }
#endif
}

int bsp_uart_write(const void *buf, size_t len)
{
    if (buf == NULL || len == 0u) {
        return 0;
    }

    int rc = 0;
    bsp_uart_lock();
    if (HAL_UART_Transmit(&huart1, (uint8_t *) buf, (uint16_t) len,
                          BSP_UART_TX_TIMEOUT_MS) != HAL_OK) {
        rc = -1;
    }
    bsp_uart_unlock();

    if (rc == 0) {
        g_dbg_uart_tx_bytes += (uint32_t) len;   /* 见文件上方调试计数说明 */
    } else {
        g_dbg_uart_tx_fail++;
    }
    return rc;
}

int bsp_uart_puts(const char *s)
{
    size_t n = 0u;
    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return bsp_uart_write(s, n);
}

int bsp_uart_logf(const char *fmt, ...)
{
    char    buf[BSP_UART_FMT_BUF];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = bsp_uart_vlogf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        (void) bsp_uart_write(buf, (size_t) n);
    }
    return n;
}
