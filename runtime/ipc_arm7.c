/* IPC between ARM9 and ARM7, and a stand-in ARM7.
 * For now the ARM7 only answers the boot handshake and logs messages, so we
 * can see what the game expects from it. */
#include <string.h>
#include "nds.h"

static uint16_t g_sync9;        /* ARM9's IPCSYNC: bits 8-11 output, 14 IRQ enable */
static uint16_t g_sync7_out = 8; /* what the ARM7 shows the ARM9 in bits 0-3 */
static uint16_t g_fifocnt9;     /* ARM9's IPCFIFOCNT enable bits */
static int g_fifo_error;

typedef struct { uint32_t q[16]; int head, count; } Fifo;
static Fifo g_to7, g_to9;
static uint32_t g_last_recv;

static int fifo_push(Fifo* f, uint32_t v) {
    if (f->count == 16) return 0;
    f->q[(f->head + f->count++) % 16] = v;
    return 1;
}
static int fifo_pop(Fifo* f, uint32_t* v) {
    if (!f->count) return 0;
    *v = f->q[f->head]; f->head = (f->head + 1) % 16; f->count--;
    return 1;
}

void ipc_sync_write(uint16_t v) {
    g_sync9 = v & 0x4F00;
    /* PXI boot handshake, ARM7 side (SDK PXI_InitFifo): the ARM7 shows
     * 8, 7, ... 0, stepping down each time the ARM9 echoes its value. */
    if (((v >> 8) & 0xF) == g_sync7_out && g_sync7_out > 0) g_sync7_out--;
    if (v & 0x2000) LOG_DEBUG("ipc: ARM9 -> ARM7 sync IRQ\n");
}
uint16_t ipc_sync_read(void) { return g_sync9 | g_sync7_out; }

void ipc_fifocnt_write(uint16_t v) {
    if (v & 0x0008) { g_to7.count = 0; g_to7.head = 0; }
    if (v & 0x4000) g_fifo_error = 0;
    g_fifocnt9 = v & 0x8404;
}
uint16_t ipc_fifocnt_read(void) {
    uint16_t v = g_fifocnt9;
    if (g_to7.count == 0) v |= 0x0001;
    if (g_to7.count == 16) v |= 0x0002;
    if (g_to9.count == 0) v |= 0x0100;
    if (g_to9.count == 16) v |= 0x0200;
    if (g_fifo_error) v |= 0x4000;
    return v;
}

static const char* const pxi_tags[] = {
    "EX", "USER0", "USER1", "SYSTEM", "NVRAM", "RTC", "TOUCHPANEL", "SOUND", "PM", "MIC",
    "WM", "FS", "OS", "CTRDG", "CARD", "WVR", "CTRDG_EX", "CTRDG_PHI", "?18", "?19",
};

void ipc_fifo_send(uint32_t v) {
    if (!(g_fifocnt9 & 0x8000)) return;
    if (!fifo_push(&g_to7, v)) { g_fifo_error = 1; return; }
    uint32_t tag = v & 0x1F;
    LOG_INFO("pxi: ARM9 -> ARM7 tag %s data %07x%s\n", tag < 20 ? pxi_tags[tag] : "?", v >> 6, (v & 0x20) ? " (err)" : "");
}

uint32_t ipc_fifo_recv(void) {
    uint32_t v;
    if (!(g_fifocnt9 & 0x8000)) return g_last_recv;
    if (!fifo_pop(&g_to9, &v)) { g_fifo_error = 1; return g_last_recv; }
    g_last_recv = v;
    return v;
}

/* PXI services the real ARM7 registers (read from its PXI_SetFifoRecvCallback
 * call sites): NVRAM, TOUCHPANEL, SOUND, PM, MIC, FS, OS, CTRDG, CARD, 16, 17.
 * The ARM9 waits for these "callback ready" bits in shared memory. */
#define ARM7_PXI_READY_MASK (0x00037BD0u | (1u << 5))  /* + RTC, registered from Thumb code */

void arm7_hle_init(void) {
    mem_write32(0x027FFC00 + 0x388 + 4 * 1, ARM7_PXI_READY_MASK);
}

void arm7_hle_step(void) {
    uint32_t v;
    while (fifo_pop(&g_to7, &v)) {
        /* Not answered yet: logged in ipc_fifo_send. */
        (void)v;
    }
    if (g_to7.count == 0 && (g_fifocnt9 & 0x0004)) irq_raise(IRQ_IPC_SEND_EMPTY);
    if (g_to9.count && (g_fifocnt9 & 0x0400)) irq_raise(IRQ_IPC_RECV_NOT_EMPTY);
}
