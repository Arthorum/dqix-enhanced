/* ARM9 I/O registers: timing, IRQs, timers, DMA, cartridge, math unit. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"

uint8_t g_io[0x2000];
uint32_t g_ime, g_ie, g_if;
uint64_t g_frame;
uint32_t g_vcount;
static uint32_t g_subline;   /* extra cycles within the current line */

#define CYCLES_PER_LINE 2130u
#define LINES_PER_FRAME 263u

uint64_t time_cycles(void) {
    return (g_frame * LINES_PER_FRAME + g_vcount) * (uint64_t)CYCLES_PER_LINE + g_subline;
}

/* ---------------------------------------------------------------- timers */
typedef struct { uint16_t reload, ctl; uint64_t start; uint64_t overflows_seen; uint16_t latched; } Timer;
static Timer g_timer[4];
static const uint32_t prescale[4] = { 1, 64, 256, 1024 };

static uint64_t timer_overflows(int n, uint64_t now, uint16_t* counter) {
    Timer* t = &g_timer[n];
    if (!(t->ctl & 0x80)) { if (counter) *counter = t->latched; return 0; }
    uint64_t period = 0x10000u - t->reload;
    uint64_t ticks;
    if ((t->ctl & 4) && n > 0) {
        ticks = timer_overflows(n - 1, now, NULL);  /* count-up: one tick per overflow below */
        ticks -= t->start;                           /* start holds the lower timer's count at enable */
    } else {
        ticks = (now - t->start) / prescale[t->ctl & 3];
    }
    if (counter) *counter = (uint16_t)(t->reload + ticks % period);
    return ticks / period;
}

uint16_t timer_read(int n) { uint16_t v; timer_overflows(n, time_cycles(), &v); return v; }
void timer_write_reload(int n, uint16_t v) { g_timer[n].reload = v; }
void timer_write_ctl(int n, uint16_t v) {
    Timer* t = &g_timer[n];
    uint64_t now = time_cycles();
    if (!(t->ctl & 0x80) && (v & 0x80)) {  /* starting */
        t->ctl = v;
        t->start = ((v & 4) && n > 0) ? timer_overflows(n - 1, now, NULL) : now;
        t->overflows_seen = 0;
    } else if ((t->ctl & 0x80) && !(v & 0x80)) {  /* stopping */
        uint16_t c; timer_overflows(n, now, &c); t->latched = c; t->ctl = v;
    } else {
        t->ctl = v;
    }
}
static void timers_update(void) {
    uint64_t now = time_cycles();
    for (int n = 0; n < 4; n++) {
        if (!(g_timer[n].ctl & 0x80)) continue;
        uint64_t o = timer_overflows(n, now, NULL);
        if (o > g_timer[n].overflows_seen) {
            g_timer[n].overflows_seen = o;
            if (g_timer[n].ctl & 0x40) irq_raise(IRQ_TIMER0 + n);
        }
    }
}

/* ---------------------------------------------------------------- display timing */
static void line_events(void) {
    uint16_t stat = IO16(0x004);
    uint32_t lyc = (stat >> 8) | ((stat & 0x80) << 1);
    if (g_vcount == 192) {
        if (stat & 0x08) irq_raise(IRQ_VBLANK);
        dma_on_event(1);
    }
    if (g_vcount < 192) dma_on_event(2);
    if ((stat & 0x20) && g_vcount == lyc) irq_raise(IRQ_VCOUNT);
}

void time_advance_lines(Cpu* c, uint32_t lines) {
    while (lines--) {
        g_subline = 0;
        if (++g_vcount >= LINES_PER_FRAME) {
            g_vcount = 0; g_frame++;
            if (g_frame_limit && g_frame >= g_frame_limit) exit(0);
        }
        line_events();
    }
    timers_update();
    arm7_hle_step();
    if (c) irq_service(c);
}

void time_run_to_vblank(Cpu* c) {
    uint32_t lines = (192 + LINES_PER_FRAME - g_vcount) % LINES_PER_FRAME;
    time_advance_lines(c, lines ? lines : LINES_PER_FRAME);
}

/* ---------------------------------------------------------------- IRQs */
void irq_raise(int bit) { g_if |= 1u << bit; }

/* ---------------------------------------------------------------- DMA */
typedef struct { uint32_t src, dst, cnt; int pending_timing; } Dma;
static Dma g_dma[4];

static void dma_run(int ch) {
    uint32_t base = 0xB0 + 12 * ch;
    uint32_t ctl = IO32(base + 8);
    Dma* d = &g_dma[ch];
    uint32_t count = ctl & 0x1FFFFF;
    if (!count) count = 0x200000;
    int word = (ctl >> 26) & 1;
    int step = word ? 4 : 2;
    int sctl = (ctl >> 23) & 3, dctl = (ctl >> 21) & 3;
    int timing = (ctl >> 27) & 7;
    uint32_t s = d->src, t = d->dst;
    for (uint32_t i = 0; i < count; i++) {
        if (word) mem_write32(t, mem_read32(s)); else mem_write16(t, mem_read16(s));
        if (sctl == 0) s += step; else if (sctl == 1) s -= step;
        if (dctl == 0 || dctl == 3) t += step; else if (dctl == 1) t -= step;
    }
    d->src = s;
    d->dst = (dctl == 3) ? IO32(base + 4) : t;
    if (ctl & 0x40000000) irq_raise(IRQ_DMA0 + ch);
    if (!(ctl & 0x02000000) || timing == 0) {   /* not repeating: finished */
        ctl &= ~0x80000000u;
        memcpy(&g_io[base + 8], &ctl, 4);
        d->pending_timing = -1;
    }
}

void dma_write_ctl(int ch) {
    uint32_t base = 0xB0 + 12 * ch;
    uint32_t ctl = IO32(base + 8);
    Dma* d = &g_dma[ch];
    if (!(ctl & 0x80000000u)) { d->pending_timing = -1; return; }
    d->src = IO32(base);
    d->dst = IO32(base + 4);
    int timing = (ctl >> 27) & 7;
    d->pending_timing = timing;
    if (timing == 0 || timing == 7) dma_run(ch);   /* immediate; GX FIFO DMA also drains at once */
}

void dma_on_event(int timing) {
    for (int ch = 0; ch < 4; ch++)
        if (g_dma[ch].pending_timing == timing && (IO32(0xB8 + 12 * ch) & 0x80000000u)) dma_run(ch);
}

/* ---------------------------------------------------------------- cartridge */
static uint8_t g_card_buf[0x4000];
static uint32_t g_card_len, g_card_pos;

void card_write_ctl(uint32_t v) {
    memcpy(&g_io[0x1A4], &v, 4);
    if (!(v & 0x80000000u)) return;
    const uint8_t* cmd = &g_io[0x1A8];
    uint32_t bs = (v >> 24) & 7;
    uint32_t len = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    g_card_len = len; g_card_pos = 0;
    if (cmd[0] == 0xB7) {
        uint32_t addr = ((uint32_t)cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
        for (uint32_t i = 0; i < len; i++) {
            uint32_t a = addr + i;
            if (a < 0x8000) a = 0x8000 + (a & 0x1FF);
            g_card_buf[i] = g_rom[a % g_rom_size];
        }
    } else if (cmd[0] == 0xB8) {
        for (uint32_t i = 0; i < len; i += 4) { uint32_t id = 0x00003FC2; memcpy(&g_card_buf[i], &id, 4); }
    } else {
        memset(g_card_buf, 0xFF, len);
        LOG_WARN("card: command %02x not emulated\n", cmd[0]);
    }
    if (len == 0) { v &= ~0x80000000u; memcpy(&g_io[0x1A4], &v, 4); return; }
    v |= 0x00800000u;  /* data ready */
    memcpy(&g_io[0x1A4], &v, 4);
    dma_on_event(5);   /* a card DMA pulls the whole block */
}

uint32_t card_read_data(void) {
    uint32_t v = 0xFFFFFFFF;
    if (g_card_pos < g_card_len) {
        memcpy(&v, &g_card_buf[g_card_pos], 4);
        g_card_pos += 4;
        if (g_card_pos >= g_card_len) {
            uint32_t ctl = IO32(0x1A4) & ~0x80800000u;
            memcpy(&g_io[0x1A4], &ctl, 4);
            if (IO16(0x1A0) & 0x4000) irq_raise(IRQ_CARD_DONE);
        }
    }
    return v;
}

/* ---------------------------------------------------------------- divider / sqrt */
void math_div(void) {
    uint16_t cnt = IO16(0x280);
    int64_t num, den;
    memcpy(&num, &g_io[0x290], 8);
    memcpy(&den, &g_io[0x298], 8);
    int mode = cnt & 3;
    if (mode == 0) { num = (int32_t)num; den = (int32_t)den; }
    else if (mode == 1 || mode == 3) { den = (int32_t)den; }
    int64_t den_raw;
    memcpy(&den_raw, &g_io[0x298], 8);
    if (den_raw == 0) cnt |= 0x4000; else cnt &= ~0x4000;   /* flag looks at the full 64-bit denominator */
    int64_t q, r;
    if (den == 0) {
        q = num < 0 ? 1 : -1; r = num;
        if (mode == 0) q ^= (int64_t)0xFFFFFFFF00000000ULL;
    } else if (num == INT64_MIN && den == -1) {
        q = num; r = 0;
    } else if (mode == 0 && num == INT32_MIN && den == -1) {
        q = 0x80000000LL; r = 0;
    } else {
        q = num / den; r = num % den;
    }
    memcpy(&g_io[0x2A0], &q, 8);
    memcpy(&g_io[0x2A8], &r, 8);
    memcpy(&g_io[0x280], &cnt, 2);
}

void math_sqrt(void) {
    uint16_t cnt = IO16(0x2B0);
    uint64_t p;
    memcpy(&p, &g_io[0x2B8], 8);
    if (!(cnt & 1)) p &= 0xFFFFFFFFu;
    uint64_t r = (uint64_t)sqrtl((long double)p);
    while (r * r > p) r--;
    while ((r + 1) * (r + 1) <= p) r++;
    uint32_t r32 = (uint32_t)r;
    memcpy(&g_io[0x2B4], &r32, 4);
}

/* ---------------------------------------------------------------- register access */
static uint32_t g_poll_reads;

static void poll_tick(void) {
    /* A read of a status register is often a busy-wait: let time move. */
    if (++g_poll_reads % 8 == 0) time_advance_lines(&g_arm9, 1);
    else g_subline += 16;
}

static uint32_t reg_read32(uint32_t off) {
    switch (off & ~3u) {
    case 0x004: { /* DISPSTAT + VCOUNT */
        poll_tick();
        uint16_t stat = IO16(0x004) & 0xFFB8;
        uint16_t lyc = (stat >> 8) | ((stat & 0x80) << 1);
        if (g_vcount >= 192 && g_vcount < 262) stat |= 1;
        if (g_subline > 1600) stat |= 2;
        if (g_vcount == lyc) stat |= 4;
        return stat | (g_vcount << 16);
    }
    case 0x100: case 0x104: case 0x108: case 0x10C: {
        int n = (off - 0x100) / 4;
        g_subline += 4;
        return timer_read(n) | ((uint32_t)g_timer[n].ctl << 16);
    }
    case 0x130: return 0x03FF | ((uint32_t)IO16(0x132) << 16);   /* no keys pressed */
    case 0x180: return ipc_sync_read();
    case 0x184: return ipc_fifocnt_read();
    case 0x1A4: return IO32(0x1A4);
    case 0x208: return g_ime;
    case 0x210: return g_ie;
    case 0x214: return g_if;
    case 0x280: case 0x2B0: return IO32(off & ~3u);
    case 0x600: return 0x04000002;  /* GXSTAT: FIFO empty, idle */
    default: return IO32(off & ~3u);
    }
}

uint32_t io9_read(uint32_t a, int size) {
    if (a >= 0x04100000) {
        if ((a & ~3u) == 0x04100000) return ipc_fifo_recv();
        if ((a & ~3u) == 0x04100010) return card_read_data();
        LOG_WARN("io read %08x\n", a);
        return 0;
    }
    uint32_t off = a - 0x04000000u;
    if (off >= sizeof g_io) { LOG_WARN("io read %08x\n", a); return 0; }
    uint32_t v = reg_read32(off);
    v >>= (off & 3) * 8;
    return size == 1 ? (v & 0xFF) : size == 2 ? (v & 0xFFFF) : v;
}

static void reg_written(uint32_t off, int size, Cpu* c) {
    (void)size;
    uint32_t base = off & ~3u;
    switch (base) {
    case 0x0B8: case 0x0C4: case 0x0D0: case 0x0DC: dma_write_ctl((base - 0xB8) / 12); break;
    case 0x100: case 0x104: case 0x108: case 0x10C: {
        int n = (base - 0x100) / 4;
        if (off - base < 2) timer_write_reload(n, IO16(base));
        if (off - base >= 2 || size == 4) timer_write_ctl(n, IO16(base + 2));
        break;
    }
    case 0x180: ipc_sync_write(IO16(0x180)); break;
    case 0x184: ipc_fifocnt_write(IO16(0x184)); break;
    case 0x188: ipc_fifo_send(IO32(0x188)); break;
    case 0x1A4: card_write_ctl(IO32(0x1A4)); break;
    case 0x208: g_ime = IO32(0x208) & 1; irq_service(c); break;
    case 0x210: g_ie = IO32(0x210); irq_service(c); break;
    case 0x214: break;
    case 0x240: case 0x244: case 0x248: vram_remap(); break;
    case 0x280: case 0x290: case 0x294: case 0x298: case 0x29C: math_div(); break;
    case 0x2B0: case 0x2B8: case 0x2BC: math_sqrt(); break;
    default: break;
    }
}

void io9_write(uint32_t a, uint32_t v, int size) {
    if (a >= 0x04100000) { LOG_WARN("io write %08x\n", a); return; }
    uint32_t off = a - 0x04000000u;
    if (off >= sizeof g_io) {
        if (a >= 0x04000400 && a < 0x04000700) return;  /* geometry engine: later */
        LOG_WARN("io write %08x = %x\n", a, v); return;
    }
    if ((off & ~3u) == 0x214) {   /* IF: write 1 to acknowledge */
        uint32_t m = size == 4 ? v : size == 2 ? (v & 0xFFFF) << ((off & 2) * 8) : (v & 0xFF) << ((off & 3) * 8);
        g_if &= ~m;
        return;
    }
    memcpy(&g_io[off], &v, size);
    reg_written(off, size, &g_arm9);
}
