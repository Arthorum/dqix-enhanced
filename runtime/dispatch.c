/* Function lookup for indirect calls, overlay detection, and tracing. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "dispatch.h"

#define HBITS 16
static int32_t g_head[1 << HBITS];
static int32_t* g_next;

static inline uint32_t hkey(uint32_t a) { return ((a >> 1) * 2654435761u) >> (32 - HBITS); }

void dispatch_init(void) {
    memset(g_head, -1, sizeof g_head);
    g_next = malloc(sizeof(int32_t) * g_recomp_func_count);
    for (unsigned i = 0; i < g_recomp_func_count; i++) {
        uint32_t k = hkey(g_recomp_funcs[i].addr & ~1u);
        g_next[i] = g_head[k];
        g_head[k] = (int32_t)i;
    }
}

/* ---------------------------------------------------------------- overlays */
typedef struct { uint32_t base, size; uint8_t* img; uint32_t gen; int loaded; } Ovl;
static Ovl g_ovl[64];
static int g_ovl_count;

void overlays_init(const char* extract) {
    char path[1024];
    snprintf(path, sizeof path, "%s/arm9_overlays/overlays.yaml", extract);
    FILE* f = fopen(path, "r");
    if (!f) rt_fatal("cannot open %s\n", path);
    char line[256];
    int id = -1;
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned v;
        if (sscanf(line, "- id: %u", &v) == 1) id = (int)v;
        else if (id >= 0 && id < 64 && sscanf(line, " base_address: %u", &v) == 1) g_ovl[id].base = v;
        else if (id >= 0 && id < 64 && sscanf(line, " code_size: %u", &v) == 1) g_ovl[id].size = v;
        if (id + 1 > g_ovl_count) g_ovl_count = id + 1;
    }
    fclose(f);
    for (int i = 0; i < g_ovl_count; i++) {
        snprintf(path, sizeof path, "%s/arm9_overlays/ov%03d.bin", extract, i);
        FILE* b = fopen(path, "rb");
        if (!b) rt_fatal("cannot open %s\n", path);
        g_ovl[i].img = malloc(g_ovl[i].size + 1);
        if (fread(g_ovl[i].img, 1, g_ovl[i].size, b) != g_ovl[i].size) rt_fatal("short read %s\n", path);
        fclose(b);
        g_ovl[i].gen = ~0u;
        if (g_ovl[i].base < lo) lo = g_ovl[i].base;
        if (g_ovl[i].base + g_ovl[i].size > hi) hi = g_ovl[i].base + g_ovl[i].size;
    }
    g_overlay_area_start = lo;
    g_overlay_area_end = hi;
    LOG_INFO("%d overlays, area %08x-%08x\n", g_ovl_count, lo, hi);
}

/* An overlay counts as loaded when its code is present in RAM. */
int overlay_is_loaded(int id) {
    if (id < 0 || id >= g_ovl_count) return 0;
    Ovl* o = &g_ovl[id];
    if (o->gen == g_overlay_gen) return o->loaded;
    o->gen = g_overlay_gen;
    uint32_t n = o->size < 256 ? o->size : 256;
    const uint8_t* ram = &g_main_ram[o->base & (MAIN_RAM_SIZE - 1)];
    o->loaded = memcmp(ram, o->img, n) == 0;
    if (o->loaded && o->size > 1024) {
        uint32_t mid = (o->size / 2) & ~3u;
        o->loaded = memcmp(ram + mid, o->img + mid, 64) == 0;
    }
    return o->loaded;
}

/* ---------------------------------------------------------------- tracing */
#define TRACE_N 256
static uint32_t g_trace[TRACE_N];
static unsigned g_trace_pos;
static uint8_t* g_seen;          /* per function index: entered at least once */
unsigned long long g_calls_traced;

const char* recomp_func_name(uint32_t addr) {
    for (int32_t i = g_head[hkey(addr & ~1u)]; i >= 0; i = g_next[i])
        if ((g_recomp_funcs[i].addr & ~1u) == (addr & ~1u)) return g_recomp_funcs[i].name;
    return "?";
}

void recomp_trace_enter(uint32_t addr) {
    g_trace[g_trace_pos++ % TRACE_N] = addr;
    g_calls_traced++;
}

void trace_dump(FILE* f, int n) {
    if (!g_calls_traced) return;
    fprintf(f, "last %d functions entered (oldest first):\n", n);
    unsigned total = g_trace_pos < TRACE_N ? g_trace_pos : TRACE_N;
    if ((unsigned)n > total) n = (int)total;
    for (int k = n; k > 0; k--) {
        uint32_t a = g_trace[(g_trace_pos - k) % TRACE_N];
        fprintf(f, "  %08x %s\n", a, recomp_func_name(a));
    }
}

/* ---------------------------------------------------------------- dispatch */
void dispatch_call(Cpu* c, uint32_t target, uint32_t from) {
    uint32_t key = target & ~1u;
    for (int32_t i = g_head[hkey(key)]; i >= 0; i = g_next[i]) {
        const RecompFunc* f = &g_recomp_funcs[i];
        if ((f->addr & ~1u) != key) continue;
        if (f->ovl_id >= 0 && !overlay_is_loaded(f->ovl_id)) continue;
        f->fn(c);
        return;
    }
    (void)g_seen;
    trace_dump(stderr, 24);
    rt_fatal("no recompiled function at %08x (called from %08x)\n", target, from);
}
