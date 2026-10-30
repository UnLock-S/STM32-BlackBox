/* ─────────────────────────────────────────────────────────────────────────────
 *  app_log.c —— SD 原始块日志实现
 * ─────────────────────────────────────────────────────────────────────────────
 *  全静态：一个 512 B 块缓冲 + 少量计数。ADR-10 门禁在构建期保证无 malloc。
 *  缓冲满才落盘（32 条/块），SD 故障时不阻塞系统，只累加 drop 计数。
 * ───────────────────────────────────────────────────────────────────────────── */

#include "app_log.h"

#include <string.h>

#include "bsp_sdcard.h"
#include "bsp_uart.h"
#include "sched.h"

#include "FreeRTOS.h"
#include "semphr.h"

#define LOG_RECORDS_PER_BLOCK   (512u / APP_LOG_RECORD_SIZE)   /* 32 */
#define LOG_TAIL_RESERVE_S      ((uint32_t) (6u * 1024u * 1024u))  /* 预留尾部 6 MB */

static uint8_t  s_blk[512];
static uint32_t s_fill;          /* 当前块里已用记录数 */
static uint32_t s_count;         /* 累计已落盘记录数 */
static uint32_t s_drop;
static uint32_t s_next_lba;      /* 下次落盘目标块号 */
static uint32_t s_last_lba;
static bool     s_ready;
static bool     s_dirty;

/* ★ 互斥：app_log_record 会被 ADC 任务和 CAN 任务同时调用
 *   （CAN 高帧率时每秒 200 条）。没有锁时两个任务会同时改写
 *   s_fill/s_blk 并在 SPI 事务上交错，导致 SD 写块失败
 *   -> 整块记录被丢弃（实测: 丢弃=38867 且疯涨）。
 *   锁只在【记录 + 满块落盘】这一段持有，落盘约 1 ms。 */
#define APP_LOG_LOCK_TIMEOUT_MS  50u
static StaticSemaphore_t  s_mutex_cb;
static SemaphoreHandle_t  s_mutex;
static volatile uint32_t  s_lock_fail;

static bool lock(void)
{
    if (s_mutex == NULL || xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return true;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_LOG_LOCK_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    s_lock_fail++;      /* 拿不到锁就放弃本次记录，绝不动共享状态 */
    return false;
}

static void unlock(void)
{
    if (s_mutex != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void) xSemaphoreGive(s_mutex);
    }
}

int app_log_init(void)
{
    const bsp_sd_info_t *si;

    s_fill = 0u;
    s_count = 0u;
    s_drop = 0u;
    s_ready = false;
    s_dirty = false;
    memset(s_blk, 0, sizeof(s_blk));

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_cb);
    }
    s_lock_fail = 0u;

    {   /* SD 开机重试：模块 LDO + 卡上电需要几十 ms，立刻发 CMD0 会没响应 */
        int rc = -1;
        int i;

        for (i = 0; i < 3; i++) {
            rc = bsp_sdcard_init();
            if (rc == 0) {
                break;
            }
            bsp_uart_logf("[log] SD 初始化失败 rc=%d，100ms 后重试\r\n", rc);
            sched_sleep_ms(100u);
        }
        if (rc != 0) {
            bsp_uart_puts("[log] ✗ SD 卡不可用 —— 日志功能关闭（系统其余部分继续运行）\r\n");
            return -1;
        }
        bsp_uart_puts("[log] ✓ SD 就绪\r\n");
    }

    si = bsp_sdcard_info();
    if (si == NULL || si->blocks == 0u) {
        return -2;
    }

    /* 可用块 = 总块数 - 预留尾部 - 头部 1 块 */
    /* ★ 日志改写在【卡的后半区】，不再从 LBA 1 开始 ✗
     *   原来从 LBA 1 写会把 FAT32 的 BPB/FAT/根目录全部覆掉 ✗
     *   → Windows 里就看不到卡，也就无法把 SBB_APP.BIN 拖进去 ✗
     *   分区:   前半区 = FAT32（放固件）  后半区 = 日志
     *   ★ 注意: 日志区仍会覆盖后半区的数据集簇，但 BPB/FAT/根目录在前半区，
     *     所以文件系统本身可用 ✓ */
    s_next_lba = (si->blocks > 0u) ? (si->blocks / 2u) : APP_LOG_FIRST_LBA;
    s_last_lba = (si->blocks > (LOG_TAIL_RESERVE_S / 512u))
               ? (si->blocks - (LOG_TAIL_RESERVE_S / 512u))
               : si->blocks;
    if (s_last_lba <= s_next_lba) {
        s_last_lba = si->blocks;
    }

    /* 写头块 */
    {
        uint8_t hdr[512];
        memset(hdr, 0, sizeof(hdr));
        memcpy(hdr, APP_LOG_MAGIC, 8u);
        hdr[8]  = 0x01u;                          /* 版本主 */
        hdr[9]  = 0x00u;                          /* 版本次 */
        hdr[10] = (uint8_t) (APP_LOG_RECORD_SIZE & 0xFFu);
        hdr[11] = (uint8_t) (APP_LOG_RECORD_SIZE >> 8);
        hdr[12] = (uint8_t) (APP_LOG_FIRST_LBA);
        hdr[13] = (uint8_t) (APP_LOG_FIRST_LBA >> 8);
        hdr[14] = (uint8_t) (APP_LOG_FIRST_LBA >> 16);
        hdr[15] = (uint8_t) (APP_LOG_FIRST_LBA >> 24);

        if (bsp_sdcard_write(APP_LOG_HEADER_LBA, hdr, 1u) != 0) {
            bsp_uart_puts("[log] ✗ 写头块失败\r\n");
            return -3;
        }
    }

    s_ready = true;
    bsp_uart_logf("[log] ✓ 日志已就绪：块 %lu .. %lu（共 %lu KB 分区）\r\n",
                  (unsigned long) APP_LOG_FIRST_LBA,
                  (unsigned long) s_last_lba,
                  (unsigned long) (((s_last_lba - APP_LOG_FIRST_LBA) * 512u) / 1024u));
    return 0;
}

bool app_log_ready(void) { return s_ready; }

static int flush_block(void)
{
    if (!s_ready) {
        return -1;
    }
    if (s_fill == 0u || !s_dirty) {
        return 0;
    }

    if (s_next_lba >= s_last_lba) {
        bsp_uart_puts("[log] ⚠ 日志分区已写满，停止记录（老数据保留）\r\n");
        s_ready = false;
        return -2;
    }

    if (bsp_sdcard_write(s_next_lba, s_blk, 1u) != 0) {
        s_drop += s_fill;
        s_fill = 0u;
        s_dirty = false;
        return -3;
    }

    s_count += s_fill;
    s_next_lba++;
    s_fill = 0u;
    s_dirty = false;
    memset(s_blk, 0, sizeof(s_blk));
    return 0;
}

int app_log_flush(void)
{
    int rc;

    if (!lock()) {
        return -1;
    }
    rc = flush_block();
    unlock();
    return rc;
}

uint32_t app_log_lock_fail(void) { return s_lock_fail; }

void app_log_pause(void)  { s_ready = false; }
void app_log_resume(void) { s_ready = true;  }

static int record_unlocked(uint8_t type, uint8_t ch, uint8_t flags,
                           const uint8_t *payload, uint8_t len)
{
    uint8_t *rec;

    if (!s_ready) {
        s_drop++;
        return -1;
    }
    if (len > 12u) {
        len = 12u;
    }

    if (s_fill >= LOG_RECORDS_PER_BLOCK) {
        if (flush_block() != 0) {
            return -2;
        }
    }

    rec = &s_blk[s_fill * APP_LOG_RECORD_SIZE];
    memset(rec, 0, APP_LOG_RECORD_SIZE);

    {
        uint32_t ts = sched_tick();
        rec[0] = (uint8_t) (ts);
        rec[1] = (uint8_t) (ts >> 8);
        rec[2] = (uint8_t) (ts >> 16);
        rec[3] = (uint8_t) (ts >> 24);
    }
    rec[4] = type;
    rec[5] = ch;
    rec[6] = flags;
    rec[7] = len;
    if (payload != NULL && len > 0u) {
        memcpy(&rec[8], payload, len);
    }

    s_fill++;
    s_dirty = true;
    return 0;
}

/* 对外接口：加锁后调内部实现（所有 return 路径都不会忘记释放） */
int app_log_record(uint8_t type, uint8_t ch, uint8_t flags,
                   const uint8_t *payload, uint8_t len)
{
    int rc;

    if (!lock()) {
        return -1;
    }
    rc = record_unlocked(type, ch, flags, payload, len);
    unlock();
    return rc;
}

int app_log_event(uint8_t ch, uint32_t v0, uint32_t v1)
{
    uint8_t p[8];
    p[0] = (uint8_t) (v0);        p[1] = (uint8_t) (v0 >> 8);
    p[2] = (uint8_t) (v0 >> 16);  p[3] = (uint8_t) (v0 >> 24);
    p[4] = (uint8_t) (v1);        p[5] = (uint8_t) (v1 >> 8);
    p[6] = (uint8_t) (v1 >> 16);  p[7] = (uint8_t) (v1 >> 24);
    return app_log_record(APP_LOG_TYPE_EVENT, ch, 0u, p, 8u);
}

int app_log_adc(uint8_t ch, uint8_t value)
{
    return app_log_record(APP_LOG_TYPE_ADC, ch, 0u, &value, 1u);
}

int app_log_can(bool tx, uint32_t id, bool ext, bool rtr,
                const uint8_t *data, uint8_t dlc)
{
    uint8_t p[12];
    uint8_t flags = 0u;

    if (ext) {
        flags |= 0x01u;
    }
    if (rtr) {
        flags |= 0x02u;
    }
    if (dlc > 8u) {
        dlc = 8u;
    }

    /* payload = id(4 LE) + data(8)。20 B 记录里 payload 有 12 B，正好装下。 */
    memset(p, 0, sizeof(p));
    p[0] = (uint8_t) (id);        p[1] = (uint8_t) (id >> 8);
    p[2] = (uint8_t) (id >> 16);  p[3] = (uint8_t) (id >> 24);
    if (data != NULL && dlc > 0u) {
        memcpy(&p[4], data, dlc);
    }

    return app_log_record(tx ? APP_LOG_TYPE_CAN_TX : APP_LOG_TYPE_CAN_RX,
                          0u, flags, p, (uint8_t) (4u + dlc));
}

uint32_t app_log_count(void)    { return s_count + s_fill; }
uint32_t app_log_drop(void)     { return s_drop; }
uint32_t app_log_next_lba(void) { return s_next_lba; }

void app_log_info(void)
{
    bsp_uart_logf("[log] ready=%s 已写记录=%lu 缓冲中=%lu 丢弃=%lu 下一块=%lu\r\n",
                  s_ready ? "是" : "否",
                  (unsigned long) s_count,
                  (unsigned long) s_fill,
                  (unsigned long) s_drop,
                  (unsigned long) s_next_lba);
}
