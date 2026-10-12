/* ARM7 sound hardware: 16 channels (PCM8, PCM16, IMA-ADPCM, PSG, noise),
 * mixed at the DS rate (33.51 MHz / 1024 = ~32728 Hz) into a ring buffer
 * that the frontend plays. Channel state also drives the busy flags the
 * ARM7 sound driver polls. Sound capture is not emulated. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"

#define IO7_32_(o) (*(uint32_t*)&g_io7[o])

typedef struct {
    int active;
    uint32_t cnt;            /* latched control */
    uint32_t sad, len, pnt;  /* source (bytes), length and loop start (words) */
    uint16_t tmr;
    int fmt, repeat;
    uint32_t pos;            /* sample index */
    uint32_t total;          /* samples in the whole sound (loop start + length) */
    uint32_t loop_start;     /* sample index of the loop start */
    int32_t counter;         /* cycles accumulated toward the next sample */
    int16_t cur;             /* current output sample */
    /* ADPCM */
    int32_t adpcm_val, adpcm_idx;
    int32_t loop_val, loop_idx; int loop_saved;
    /* PSG / noise */
    uint32_t psg_step;
    uint16_t lfsr;
} Chan;

static Chan g_ch[16];
static uint32_t g_mix_cycles;          /* cycles toward the next output sample */

#define OUT_N 16384                    /* stereo frames in the ring */
static int16_t g_out[OUT_N * 2];
static volatile uint32_t g_out_w, g_out_r;

static const int16_t adpcm_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767 };
static const int8_t adpcm_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static uint8_t rd8(uint32_t a) { return (uint8_t)g_c7.bus->r8(a); }
static uint16_t rd16(uint32_t a) { return (uint16_t)g_c7.bus->r16(a); }
static uint32_t rd32(uint32_t a) { return g_c7.bus->r32(a); }

static void chan_stop(int n) {
    Chan* c = &g_ch[n];
    c->active = 0;
    c->cur = 0;
    uint32_t v = IO7_32_(0x400 + n * 16) & ~0x80000000u;
    memcpy(&g_io7[0x400 + n * 16], &v, 4);
}

static void adpcm_decode(Chan* c, uint32_t i) {
    /* sample i (counting from the first data nibble) */
    uint8_t b = rd8(c->sad + 4 + i / 2);
    int nib = (i & 1) ? (b >> 4) : (b & 15);
    int step = adpcm_step[c->adpcm_idx];
    int diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) { c->adpcm_val -= diff; if (c->adpcm_val < -0x7FFF) c->adpcm_val = -0x7FFF; }
    else { c->adpcm_val += diff; if (c->adpcm_val > 0x7FFF) c->adpcm_val = 0x7FFF; }
    c->adpcm_idx += adpcm_index[nib & 7];
    if (c->adpcm_idx < 0) c->adpcm_idx = 0;
    if (c->adpcm_idx > 88) c->adpcm_idx = 88;
}

static void chan_start(int n) {
    Chan* c = &g_ch[n];
    uint32_t base = 0x400 + n * 16;
    c->cnt = IO7_32_(base);
    c->sad = IO7_32_(base + 4) & 0x07FFFFFC;
    c->tmr = *(uint16_t*)&g_io7[base + 8];
    c->pnt = *(uint16_t*)&g_io7[base + 10];
    c->len = IO7_32_(base + 12) & 0x3FFFFF;
    c->fmt = (c->cnt >> 29) & 3;
    c->repeat = (c->cnt >> 27) & 3;
    c->pos = 0; c->counter = 0; c->cur = 0;
    c->active = 1;
    switch (c->fmt) {
    case 0: c->loop_start = c->pnt * 4; c->total = (c->pnt + c->len) * 4; break;
    case 1: c->loop_start = c->pnt * 2; c->total = (c->pnt + c->len) * 2; break;
    case 2: {
        uint32_t h = rd32(c->sad);
        c->adpcm_val = (int16_t)(h & 0xFFFF);
        c->adpcm_idx = (h >> 16) & 0x7F; if (c->adpcm_idx > 88) c->adpcm_idx = 88;
        c->cur = (int16_t)c->adpcm_val;
        /* the header word is part of the loop-start area */
        c->loop_start = c->pnt ? (c->pnt - 1) * 8 : 0;
        c->total = (c->pnt + c->len) ? (c->pnt + c->len - 1) * 8 : 0;
        c->loop_saved = 0;
        break;
    }
    default:
        c->psg_step = 0; c->lfsr = 0x7FFF;
        c->total = 0xFFFFFFFF;
        break;
    }
    if (c->fmt != 3 && c->total == 0) chan_stop(n);
    static int slog = -1; if (slog < 0) slog = getenv("DQIX_SNDLOG") != NULL;
    if (slog) fprintf(stderr, "[f%llu] ch%d start fmt %d rep %d sad %08x tmr %04x pnt %x len %x cnt %08x master %08x\n", (unsigned long long)g_frame, n,
                      c->fmt, c->repeat, c->sad, c->tmr, c->pnt, c->len, c->cnt, IO7_32_(0x500));
}

/* Produce the next sample of channel n. */
static void chan_step(int n) {
    Chan* c = &g_ch[n];
    switch (c->fmt) {
    case 3:
        if (n >= 8 && n <= 13) {                                  /* PSG square wave */
            int duty = (c->cnt >> 24) & 7;
            c->psg_step = (c->psg_step + 1) & 7;
            c->cur = (duty == 7) ? -0x7FFF : (c->psg_step <= (uint32_t)duty ? 0x7FFF : -0x7FFF);
            if (duty != 7) c->cur = ((int)c->psg_step < 7 - duty) ? -0x7FFF : 0x7FFF;
        } else if (n >= 14) {                                     /* noise */
            int out = c->lfsr & 1;
            c->lfsr >>= 1;
            if (out) { c->lfsr ^= 0x6000; c->cur = -0x7FFF; } else c->cur = 0x7FFF;
        } else c->cur = 0;
        return;
    case 2:
        if (c->pos == c->loop_start && !c->loop_saved) { c->loop_val = c->adpcm_val; c->loop_idx = c->adpcm_idx; c->loop_saved = 1; }
        adpcm_decode(c, c->pos);
        c->cur = (int16_t)c->adpcm_val;
        break;
    case 1: c->cur = (int16_t)rd16(c->sad + c->pos * 2); break;
    default: c->cur = (int16_t)((int8_t)rd8(c->sad + c->pos) << 8); break;
    }
    c->pos++;
    if (c->pos >= c->total) {
        if (c->repeat == 1) {
            c->pos = c->loop_start;
            if (c->fmt == 2 && c->loop_saved) { c->adpcm_val = c->loop_val; c->adpcm_idx = c->loop_idx; }
        } else chan_stop(n);
    }
}

static void mix_one(void) {
    int32_t l = 0, r = 0;
    for (int n = 0; n < 16; n++) {
        Chan* c = &g_ch[n];
        if (!c->active) continue;
        uint32_t cnt = IO7_32_(0x400 + n * 16);                   /* volume / pan can change while playing */
        int vol = cnt & 0x7F;
        static const int div_shift[4] = { 0, 1, 2, 4 };
        int pan = (cnt >> 16) & 0x7F;
        int32_t s = ((int32_t)c->cur * vol) >> 7;
        s >>= div_shift[(cnt >> 8) & 3];
        l += (s * (127 - pan)) >> 7;
        r += (s * pan) >> 7;
    }
    uint32_t master = IO7_32_(0x500);
    if (!(master & 0x8000)) { l = r = 0; }
    int mv = master & 0x7F;
    l = (l * mv) >> 7; r = (r * mv) >> 7;
    if (l > 32767) l = 32767; if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; if (r < -32768) r = -32768;
    uint32_t w = g_out_w;
    if (w - g_out_r >= OUT_N) return;                             /* ring full: nobody listening */
    g_out[(w % OUT_N) * 2] = (int16_t)l;
    g_out[(w % OUT_N) * 2 + 1] = (int16_t)r;
    g_out_w = w + 1;
}

/* Advance the sound hardware by `cycles` (33.51 MHz). */
void sound_advance(uint32_t cycles) {
    while (cycles) {
        uint32_t step = 1024 - g_mix_cycles;
        if (step > cycles) step = cycles;
        for (int n = 0; n < 16; n++) {
            Chan* c = &g_ch[n];
            if (!c->active) continue;
            uint32_t tmr = *(uint16_t*)&g_io7[0x400 + n * 16 + 8];
            int32_t period = (int32_t)(0x10000 - tmr) * 2;
            if (period < 2) period = 2;
            c->counter += (int32_t)step;
            while (c->counter >= period && c->active) { c->counter -= period; chan_step(n); }
        }
        g_mix_cycles += step;
        cycles -= step;
        if (g_mix_cycles >= 1024) { g_mix_cycles -= 1024; mix_one(); }
    }
}

/* A write to a channel's control register (offset within 0x400-0x4FF). */
void sound_cnt_written(int n, uint32_t old) {
    uint32_t now = IO7_32_(0x400 + n * 16);
    if ((now & 0x80000000u) && !(old & 0x80000000u)) chan_start(n);
    else if (!(now & 0x80000000u) && g_ch[n].active) { g_ch[n].active = 0; g_ch[n].cur = 0; }
}

int sound_chan_busy(int n) { return g_ch[n].active; }

/* Frontend side: take up to n stereo frames; returns how many were available. */
uint32_t sound_read(int16_t* dst, uint32_t n) {
    uint32_t avail = g_out_w - g_out_r;
    if (n > avail) n = avail;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (g_out_r + i) % OUT_N;
        dst[i * 2] = g_out[k * 2]; dst[i * 2 + 1] = g_out[k * 2 + 1];
    }
    g_out_r += n;
    return n;
}
uint32_t sound_buffered(void) { return g_out_w - g_out_r; }
void sound_discard(void) { g_out_r = g_out_w; }
