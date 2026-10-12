/* Shared timing, per-CPU interrupts / timers / DMA, and the ARM9's I/O registers. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"

uint8_t g_io[0x2000];
uint64_t g_frame;
uint32_t g_vcount;
uint64_t g_frame_limit;
static uint32_t g_subline;   /* extra cycles within the current line */

#define CYCLES_PER_LINE 2130u
#define LINES_PER_FRAME 263u

uint64_t time_cycles(void) {
    return (g_frame * LINES_PER_FRAME + g_vcount) * (uint64_t)CYCLES_PER_LINE + g_subline;
}

void irq_raise(Core* k, int bit) { k->iflags |= 1u << bit; }

/* ---------------------------------------------------------------- timers */
static const uint32_t prescale[4] = { 1, 64, 256, 1024 };

static uint64_t timer_overflows(Core* k, int n, uint64_t now, uint16_t* counter) {
    Timer* t = &k->timer[n];
    if (!(t->ctl & 0x80)) { if (counter) *counter = t->latched; return 0; }
    uint64_t period = 0x10000u - t->reload;
    uint64_t ticks;
    if ((t->ctl & 4) && n > 0) ticks = timer_overflows(k, n - 1, now, NULL) - t->start;  /* count-up */
    else ticks = (now - t->start) / prescale[t->ctl & 3];
    if (counter) *counter = (uint16_t)(t->reload + ticks % period);
    return ticks / period;
}

uint16_t timer_read(Core* k, int n) { uint16_t v; timer_overflows(k, n, time_cycles(), &v); return v; }

void timer_write(Core* k, int n, uint32_t off, uint32_t val, int size) {
    Timer* t = &k->timer[n];
    if (off == 0) t->reload = (uint16_t)val;
    uint16_t ctl;
    if (off == 2) ctl = (uint16_t)val;
    else if (size == 4) ctl = (uint16_t)(val >> 16);
    else return;
    uint64_t now = time_cycles();
    if (!(t->ctl & 0x80) && (ctl & 0x80)) {
        t->ctl = ctl;
        t->start = ((ctl & 4) && n > 0) ? timer_overflows(k, n - 1, now, NULL) : now;
        t->overflows_seen = 0;
    } else if ((t->ctl & 0x80) && !(ctl & 0x80)) {
        uint16_t c; timer_overflows(k, n, now, &c); t->latched = c; t->ctl = ctl;
    } else {
        t->ctl = ctl;
    }
}

static void timers_update(Core* k) {
    uint64_t now = time_cycles();
    for (int n = 0; n < 4; n++) {
        if (!(k->timer[n].ctl & 0x80)) continue;
        uint64_t o = timer_overflows(k, n, now, NULL);
        if (o > k->timer[n].overflows_seen) {
            k->timer[n].overflows_seen = o;
            if (k->timer[n].ctl & 0x40) irq_raise(k, IRQ_TIMER0 + n);
        }
    }
}

/* ---------------------------------------------------------------- DMA */
static int dma_timing(Core* k, uint32_t ctl) {
    if (k->id == 0) return (ctl >> 27) & 7;
    switch ((ctl >> 28) & 3) { case 0: return DMA_IMMEDIATE; case 1: return DMA_VBLANK; case 2: return DMA_CARD; default: return 6; }
}

static void dma_run(Core* k, int ch) {
    Dma* d = &k->dma[ch];
    const Bus* b = k->bus;
    uint32_t ctl = d->ctl;
    uint32_t count = ctl & (k->id == 0 ? 0x1FFFFF : 0x3FFF);
    if (!count) count = k->id == 0 ? 0x200000 : 0x4000;
    int word = (ctl >> 26) & 1, step = word ? 4 : 2;
    int sctl = (ctl >> 23) & 3, dctl = (ctl >> 21) & 3;
    uint32_t s = d->src, t = d->dst;
    for (uint32_t i = 0; i < count; i++) {
        if (word) b->w32(t, b->r32(s)); else b->w16(t, b->r16(s));
        if (sctl == 0) s += step; else if (sctl == 1) s -= step;
        if (dctl == 0 || dctl == 3) t += step; else if (dctl == 1) t -= step;
    }
    d->src = s;
    uint32_t base = 0xB0 + 12 * ch;
    if (dctl == 3) memcpy(&d->dst, &k->io[base + 4], 4); else d->dst = t;
    if (ctl & 0x40000000) irq_raise(k, IRQ_DMA0 + ch);
    if (!(ctl & 0x02000000) || d->pending == DMA_IMMEDIATE) {
        ctl &= ~0x80000000u;
        d->ctl = ctl;
        memcpy(&k->io[base + 8], &ctl, 4);
        d->pending = DMA_NONE;
    }
}

void dma_write_ctl(Core* k, int ch) {
    uint32_t base = 0xB0 + 12 * ch, ctl;
    memcpy(&ctl, &k->io[base + 8], 4);
    Dma* d = &k->dma[ch];
    int was = d->ctl & 0x80000000u;
    d->ctl = ctl;
    if (!(ctl & 0x80000000u)) { d->pending = DMA_NONE; return; }
    if (!was) { memcpy(&d->src, &k->io[base], 4); memcpy(&d->dst, &k->io[base + 4], 4); }
    d->pending = dma_timing(k, ctl);
    if (d->pending == DMA_IMMEDIATE || d->pending == DMA_GXFIFO) dma_run(k, ch);
}

void dma_on_event(Core* k, int timing) {
    for (int ch = 0; ch < 4; ch++)
        if (k->dma[ch].pending == timing && (k->dma[ch].ctl & 0x80000000u)) dma_run(k, ch);
}

/* ---------------------------------------------------------------- display timing */
static void line_events(Core* k) {
    uint16_t stat; memcpy(&stat, &k->io[0x004], 2);
    uint32_t lyc = (stat >> 8) | ((stat & 0x80) << 1);
    if (g_vcount == 192) {
        if (stat & 0x08) irq_raise(k, IRQ_VBLANK);
        dma_on_event(k, DMA_VBLANK);
    }
    if (k->id == 0 && g_vcount < 192) dma_on_event(k, DMA_HBLANK);
    if ((stat & 0x20) && g_vcount == lyc) irq_raise(k, IRQ_VCOUNT);
}

void time_advance_lines(uint32_t lines) {
    while (lines--) {
        g_subline = 0;
        if (++g_vcount >= LINES_PER_FRAME) {
            g_vcount = 0; g_frame++;
            if (g_frame_limit && g_frame >= g_frame_limit) exit(0);
        }
        if (g_vcount < 192) gpu_render_line((int)g_vcount);
        else if (g_vcount == 192) { gpu_frame_done(); input_update(); }
        line_events(&g_c9);
        line_events(&g_c7);
        timers_update(&g_c9);
        timers_update(&g_c7);
        arm7_run();
    }
}

/* The ARM9 burned `cycles` (e.g. a BIOS delay loop). */
void time_spend(uint32_t cycles) {
    g_subline += cycles;
    if (g_subline >= CYCLES_PER_LINE) {
        uint32_t lines = g_subline / CYCLES_PER_LINE, rem = g_subline % CYCLES_PER_LINE;
        time_advance_lines(lines);
        g_subline = rem;
    }
}

void time_poll(Core* k) {
    static uint32_t polls9, polls7;
    if (k->id == 1) {            /* the ARM7 waiting on something: let the ARM9 run */
        if (++polls7 % 4 == 0) arm7_yield();
        return;
    }
    if (++polls9 % 8 == 0) { time_advance_lines(1); irq_service(&g_arm9); }
    else g_subline += 16;
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

/* ---------------------------------------------------------------- ARM9 registers */
uint32_t dispstat_read(Core* k) {
    time_poll(k);
    uint16_t stat; memcpy(&stat, &k->io[0x004], 2);
    stat &= 0xFFB8;
    uint16_t lyc = (stat >> 8) | ((stat & 0x80) << 1);
    if (g_vcount >= 192 && g_vcount < 262) stat |= 1;
    if (g_subline > 1600) stat |= 2;
    if (g_vcount == lyc) stat |= 4;
    return stat | (g_vcount << 16);
}

static uint32_t reg9_read32(uint32_t off) {
    Core* k = &g_c9;
    switch (off) {
    case 0x004: return dispstat_read(k);
    case 0x100: case 0x104: case 0x108: case 0x10C: {
        int n = (off - 0x100) / 4;
        g_subline += 4;
        return timer_read(k, n) | ((uint32_t)k->timer[n].ctl << 16);
    }
    case 0x130: return input_keyinput() | ((uint32_t)IO16(0x132) << 16);
    case 0x180: time_poll(k); return ipc_sync_read(k);
    case 0x184: time_poll(k); return ipc_fifocnt_read(k);
    case 0x1A0: case 0x1A4: case 0x1A8: case 0x1AC: return card_read(k, off, 4);
    case 0x208: return k->ime;
    case 0x210: return k->ie;
    case 0x214: return k->iflags;
    case 0x600: return 0x04000002;  /* GXSTAT: FIFO empty, geometry engine idle */
    default: return IO32(off);
    }
}

uint32_t io9_read(uint32_t a, int size) {
    if (a >= 0x04100000) {
        if ((a & ~3u) == 0x04100000) return ipc_fifo_recv(&g_c9);
        if ((a & ~3u) == 0x04100010) return card_read_data(&g_c9);
        LOG_WARN("io9 read %08x\n", a);
        return 0;
    }
    uint32_t off = a - 0x04000000u;
    if (off >= sizeof g_io) { LOG_WARN("io9 read %08x\n", a); return 0; }
    uint32_t v = reg9_read32(off & ~3u) >> ((off & 3) * 8);
    return size == 1 ? (v & 0xFF) : size == 2 ? (v & 0xFFFF) : v;
}

void io9_write(uint32_t a, uint32_t v, int size) {
    Core* k = &g_c9;
    if (a >= 0x04100000) { LOG_WARN("io9 write %08x\n", a); return; }
    uint32_t off = a - 0x04000000u;
    if (off >= sizeof g_io) { LOG_WARN("io9 write %08x = %x\n", a, v); return; }
    uint32_t base = off & ~3u;
    uint32_t sh = (off & 3) * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu << sh : 0xFFu << sh;
    uint32_t val32 = size == 4 ? v : (v << sh) & mask;
    switch (base) {
    case 0x214: k->iflags &= ~val32; return;                         /* write 1 to acknowledge */
    case 0x208: k->ime = ((k->ime & ~mask) | (val32 & mask)) & 1; irq_service(&g_arm9); return;
    case 0x210: k->ie = (k->ie & ~mask) | (val32 & mask); irq_service(&g_arm9); return;
    case 0x1A0: case 0x1A4: case 0x1A8: case 0x1AC: card_write(k, off, v, size); return;
    }
    memcpy(&g_io[off], &v, size);
    switch (base) {
    case 0x0B8: case 0x0C4: case 0x0D0: case 0x0DC: if ((off & 3) + size > 2) dma_write_ctl(k, (base - 0xB8) / 12); break;
    case 0x100: case 0x104: case 0x108: case 0x10C: timer_write(k, (base - 0x100) / 4, off - base, v, size); break;
    case 0x180: ipc_sync_write(k, IO16(0x180)); break;
    case 0x184: ipc_fifocnt_write(k, IO16(0x184)); break;
    case 0x188: ipc_fifo_send(k, IO32(0x188)); break;
    case 0x240: case 0x244: case 0x248: vram_remap(); break;
    case 0x028: case 0x02C: gpu_affine_written(0, 2); break;
    case 0x038: case 0x03C: gpu_affine_written(0, 3); break;
    case 0x1028: case 0x102C: gpu_affine_written(1, 2); break;
    case 0x1038: case 0x103C: gpu_affine_written(1, 3); break;
    case 0x280: case 0x290: case 0x294: case 0x298: case 0x29C: math_div(); break;
    case 0x2B0: case 0x2B8: case 0x2BC: math_sqrt(); break;
    default: break;
    }
}
