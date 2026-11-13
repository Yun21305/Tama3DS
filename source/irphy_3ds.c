/*
 * 3DS physical layer for the PspIrDA stack (replaces irphy.c).
 *
 * The 3DS ir:u service is frame/DMA based, not a byte UART API, so:
 *  - irphy_send() buffers bytes and transmits the whole SIR frame when
 *    the EOF byte (0xC1) arrives. A data 0xC1 is always escaped by IrLAP,
 *    so 0xC1 only ever appears as the real end of frame.
 *  - Reception runs continuously in variable-length mode (flag=1) into a
 *    ring buffer in the shared memory. It is stopped while sending,
 *    because the IR transceiver is half duplex.
 */
#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include "irphy.h"
#include "pspirda.h"

#define IR_MEM_SIZE  0x1000
#define RX_RING      IR_MEM_SIZE
#define TX_MAX       0x800        /* StartSendTransfer limit */

static u32 *ir_mem = NULL;
static u8   tx_buf[TX_MAX];
static u32  tx_len = 0;

static Handle send_event = 0;
static bool rx_running = false;
static u32  rx_consumed = 0;      /* bytes already handed to IrLAP */

u32 irphy_rx_restarts = 0;        /* diagnostics shown by main.c */
u32 irphy_led_hits = 0;           /* GPIO 偵測到紅外光的次數 */
int irphy_rx_flag = 1;            /* 1 = 可變長度環形接收, 0 = 固定長度接收 */
int irphy_sleep_off = 0;          /* 1 = 送出睡眠相關指令（v0.2 新增，懷疑造成發射失效） */
int irphy_led_poll = 0;           /* 1 = 重試時輪詢 GPIO 紅外光感測（診斷） */
u32 irphy_tx_frames = 0;
volatile int irphy_abort = 0;     /* 按 START 中止傳送 */
uint8_t irphy_baud_mask = 0x02;   /* IrLAP 速率位元：0x02=9600 ... 0x3E=最高 115200 */
u32 irphy_cur_bps = 9600;
u32 irphy_hw_resets = 0;          /* 接收端硬體重置次數（診斷用） */
static u8 cur_rate_val = 11;
extern void (*irphy_idle_cb)(void); /* 等待時重繪畫面（main.c） */
u32 irphy_rx_bytes = 0;

static Result IRU_GetRecvTransferCount(u32 *count)
{
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x7, 0, 0);
    Result r = svcSendSyncRequest(iruGetServHandle());
    if (R_FAILED(r)) return r;
    *count = cmdbuf[2];
    return (Result)cmdbuf[1];
}

static Result IRU_GetErrorStatus(u32 *st)
{
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x10, 0, 0);
    Result r = svcSendSyncRequest(iruGetServHandle());
    if (R_FAILED(r)) return r;
    *st = cmdbuf[2] & 0xFF;
    return (Result)cmdbuf[1];
}

static Result IRU_SetSleepModeActive(u8 v)
{
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x11, 1, 0);
    cmdbuf[1] = v;
    Result r = svcSendSyncRequest(iruGetServHandle());
    if (R_FAILED(r)) return r;
    return (Result)cmdbuf[1];
}

static Result IRU_SetSleepModeState(u8 v)
{
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x12, 1, 0);
    cmdbuf[1] = v;
    Result r = svcSendSyncRequest(iruGetServHandle());
    if (R_FAILED(r)) return r;
    return (Result)cmdbuf[1];
}

static Result IRU_GetTransferState(u8 *out)
{
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0xF, 0, 0);
    Result r = svcSendSyncRequest(iruGetServHandle());
    if (R_FAILED(r)) return r;
    *out = cmdbuf[2] & 0xFF;
    return (Result)cmdbuf[1];
}

u32 irphy_send_stuck = 0;

/*
 * 正確的送出方式（參考 Doridian/3dsir 的 send_chunk）：
 * libctru 的 iruSendData(..., true) 會呼叫 IRU_WaitSendTransfer，
 * 這個指令會「取消」正在進行的傳送，大約 10 ms 後就把訊號切斷。
 * 9600 bps 下 10 ms 只夠送出約 10 byte，34 byte 的 IrDA 封包只送出前導 FF。
 * 改成等 SendFinishedEvent，並輪詢 TransferState 直到變回 1（ready）。
 */
static Result send_full(const u8 *data, u32 len)
{
    u8 state = 0;
    IRU_GetTransferState(&state);
    if (state == 2) IRU_WaitSendTransfer();      /* 上一次沒結束 */

    if (send_event) svcClearEvent(send_event);
    u64 t0 = osGetTime();
    Result r = IRU_StartSendTransfer((u8 *)data, len);
    if (R_FAILED(r)) return r;

    u32 limit = len * 10 * 1000 / irphy_cur_bps + 200;   /* 傳輸時間 + 200 ms */
    if (send_event)
        svcWaitSynchronization(send_event, (s64)limit * 1000000LL);

    while (R_SUCCEEDED(IRU_GetTransferState(&state)) && state == 2) {
        if (osGetTime() - t0 > limit) {
            irphy_send_stuck++;
            IRU_WaitSendTransfer();
            break;
        }
        svcSleepThread(1000000ULL);
    }
    return 0;
}

void irphy_apply_config(void)
{
    if (!irphy_sleep_off) {
        DEBUG(1, "config: no sleep cmds (v0.1 behaviour)\n");
        return;
    }
    Result a = IRU_SetSleepModeActive(0);
    Result b = IRU_SetSleepModeState(0);
    DEBUG(1, "config: sleep cmds sent (r=%08lX %08lX)\n",
          (unsigned long)a, (unsigned long)b);
}

static void rx_start(void)
{
    if (rx_running) return;
    rx_consumed = 0;
    if (R_SUCCEEDED(IRU_StartRecvTransfer(RX_RING, (u8)irphy_rx_flag)))
        rx_running = true;
}

static void rx_stop(void)
{
    u32 cnt = 0;
    if (!rx_running) return;
    IRU_WaitRecvTransfer(&cnt);
    rx_running = false;
}

/* returns available unread bytes */
static u32 rx_available(void)
{
    u32 cnt = 0;
    if (!rx_running) rx_start();
    if (R_FAILED(IRU_GetRecvTransferCount(&cnt))) return 0;
    if (cnt < rx_consumed) rx_consumed = cnt;
    if (cnt - rx_consumed > RX_RING)          /* overrun: skip ahead */
        rx_consumed = cnt - RX_RING;
    if (!irphy_rx_flag && cnt >= RX_RING && rx_consumed >= cnt) {
        rx_stop();                            /* 固定長度模式用完，重開 */
        rx_start();
        return 0;
    }
    return cnt - rx_consumed;
}

Result irphy_init(void)
{
    ir_mem = (u32 *)memalign(0x1000, IR_MEM_SIZE);
    if (!ir_mem) return -1;
    memset(ir_mem, 0, IR_MEM_SIZE);
    Result r = iruInit(ir_mem, IR_MEM_SIZE);
    if (R_FAILED(r)) { free(ir_mem); ir_mem = NULL; return r; }
    IRU_SetBitRate(11);                      /* 9600 bps, IrDA discovery speed */
    IRU_GetSendFinishedEvent(&send_event);
    return 0;
}

void irphy_exit(void)
{
    rx_stop();
    if (send_event) svcCloseHandle(send_event);
    send_event = 0;
    iruExit();
    if (ir_mem) free(ir_mem);
    ir_mem = NULL;
}

static void set_rate(u8 val, u32 bps)
{
    bool was = rx_running;
    rx_stop();
    IRU_SetBitRate(val);
    cur_rate_val = val;
    irphy_cur_bps = bps;
    if (was) rx_start();
}

/* 連線協商（SNRM/UA）後切換到雙方都支援的最高速率 */
void irphy_set_baud(uint8_t mask)
{
    if (mask & 0x20)      set_rate(3, 115200);
    else if (mask & 0x10) set_rate(14, 57600);
    else if (mask & 0x08) set_rate(15, 38400);
    else if (mask & 0x04) set_rate(16, 19200);
    else                  set_rate(11, 9600);
    DEBUG(1, "baud -> %lu (mask %02X)\n", (unsigned long)irphy_cur_bps, mask);
}

/* Escalating recovery, called by IrLAP when the same frame failed several
   times in a row. Each level is logged so we can see which one helps. */
void irphy_recover(int level)
{
    irphy_hw_resets++;
    rx_stop();
    if (level == 1) {
        /* let a busy peer finish (e.g. flash write), then listen fresh */
        DEBUG(1, "recover L1: pause 400 ms\n");
        svcSleepThread(400 * 1000000ULL);
    } else if (level == 2) {
        /* re-apply the UART bit rate (re-programs the divisor/FIFO) */
        DEBUG(1, "recover L2: re-set bitrate %u\n", cur_rate_val);
        IRU_SetBitRate(cur_rate_val == 11 ? 6 : 11);
        svcSleepThread(20 * 1000000ULL);
        IRU_SetBitRate(cur_rate_val);
        svcSleepThread(100 * 1000000ULL);
    } else {
        /* toggle the receive mode for the next replies */
        irphy_rx_flag = !irphy_rx_flag;
        DEBUG(1, "recover L3: rx mode -> %s\n", irphy_rx_flag ? "ring" : "fixed");
        svcSleepThread(100 * 1000000ULL);
    }
    rx_start();
}

void irphy_reset(void)
{
    rx_stop();
    IRU_SetBitRate(11);                      /* 每次連線都從 9600 開始 */
    cur_rate_val = 11;
    irphy_cur_bps = 9600;
    tx_len = 0;
    irphy_rx_flag = 1;
    rx_start();
}

#define MIN_XBOFS 10

void irphy_send(uint8_t v)
{
    DUMP(v, 1);
    /*
     * P's 在 UA 回應中要求 0 個額外前導 (85 01 81)，所以 IrLAP 之後的封包
     * 直接以 C0 開頭。但 3DS 每次傳送開頭的第一個 byte 容易遺失，
     * 一旦 C0 掉了整個封包就作廢。所以每個封包前都固定補 10 個 FF；
     * 規格允許接收端忽略 BOF 之前多出來的 FF。
     */
    if (tx_len == 0 && v == 0xC0) {
        for (int i = 0; i < MIN_XBOFS; i++) tx_buf[tx_len++] = 0xFF;
    }
    if (tx_len < TX_MAX) tx_buf[tx_len++] = v;
    if (v != 0xC1 && tx_len < TX_MAX) return;

    rx_stop();
    /* P's 要求的最小轉向時間最多 10 ms (86 01 07)：收到後先等 10 ms 再送 */
    svcSleepThread(10 * 1000000ULL);
    send_full(tx_buf, tx_len);
    irphy_tx_frames++;
    tx_len = 0;
    irphy_rx_flag = 1;
    rx_start();
}

uint8_t irphy_wait(int16_t timeout)
{
    u64 start = osGetTime();
    u64 limit = (timeout < 0) ? 5000 : (u64)timeout;
    u32 err = 0;

    u32 led = 0, hits = 0;
    for (;;) {
        if (irphy_abort) return 0;
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            irphy_abort = 1;
            DEBUG(1, "aborted by user\n");
            return 0;
        }
        if (rx_available()) return 1;
        if (irphy_led_poll && R_SUCCEEDED(IRU_GetIRLEDRecvState(&led)) && led) hits++;

        /* reception stops on a transfer error: restart it */
        if (R_SUCCEEDED(IRU_GetErrorStatus(&err)) && err != 0) {
            DEBUG(5, "rx error status 0x%02lX, restarting\n", (unsigned long)err);
            rx_stop();
            rx_start();
            irphy_rx_restarts++;
        }
        if (osGetTime() - start >= limit) {
            u32 cnt = 0;
            IRU_GetRecvTransferCount(&cnt);
            irphy_led_hits += hits;
            if (timeout >= 100 && irphy_led_poll)
                DEBUG(1, "RETRY WAIT: uart bytes %lu, IR light hits %lu\n",
                      (unsigned long)cnt, (unsigned long)hits);
            if (timeout >= 100)
                DEBUG(5, "timeout: cnt %lu used %lu err %02lX led_hits %lu\n",
                      (unsigned long)cnt, (unsigned long)rx_consumed,
                      (unsigned long)err, (unsigned long)hits);
            /* Stall pattern: 1-3 bytes then silence. v0.15 did a full IR
               re-init here, which broke reception completely. Now only a soft
               restart of the receive transfer, and a detailed state dump. */
            if (timeout >= 100 && cnt >= 1 && cnt <= 3) {
                u8 ts = 0; u32 es = 0;
                IRU_GetErrorStatus(&es);
                {
                    u32 *cb = getThreadCommandBuffer();
                    cb[0] = IPC_MakeHeader(0xF, 0, 0);
                    if (R_SUCCEEDED(svcSendSyncRequest(iruGetServHandle()))) ts = cb[2] & 0xFF;
                }
                const u8 *ring = (const u8 *)ir_mem;
                DEBUG(1, "stall: cnt %lu err %02lX state %u bytes %02X %02X %02X\n",
                      (unsigned long)cnt, (unsigned long)es, ts, ring[0], ring[1], ring[2]);
            }
            return 0;
        }
        /* 只在等待新封包開頭（timeout 500 ms）時重繪，封包中途絕不重繪 */
        if (irphy_idle_cb && timeout >= 100) irphy_idle_cb();
        svcSleepThread(1000000ULL);          /* 1 ms */
    }
}

uint8_t irphy_receive(void)
{
    while (!rx_available()) {
        if (irphy_abort) return 0;
        svcSleepThread(1000000ULL);
    }
    uint8_t v = ((uint8_t *)ir_mem)[rx_consumed % RX_RING];
    rx_consumed++;
    irphy_rx_bytes++;
    DUMP(v, 0);
    return v;
}
