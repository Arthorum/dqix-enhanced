/* Function lookup for indirect calls (both CPUs), overlay detection, the call
 * budget, and tracing. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "dispatch.h"

extern const RecompFunc g_recomp7_funcs[];
extern const unsigned g_recomp7_func_count;

#define HBITS 16
typedef struct { int32_t head[1 << HBITS]; int32_t* next; const RecompFunc* funcs; unsigned count; } Table;
static Table g_t9, g_t7;

static inline uint32_t hkey(uint32_t a) { return ((a >> 1) * 2654435761u) >> (32 - HBITS); }

static void table_init(Table* t, const RecompFunc* f, unsigned n) {
    memset(t->head, -1, sizeof t->head);
    t->next = malloc(sizeof(int32_t) * (n ? n : 1));
    t->funcs = f; t->count = n;
    for (unsigned i = 0; i < n; i++) {
        uint32_t k = hkey(f[i].addr & ~1u);
        t->next[i] = t->head[k];
        t->head[k] = (int32_t)i;
    }
}

void dispatch_init(void) {
    table_init(&g_t9, g_recomp_funcs, g_recomp_func_count);
    table_init(&g_t7, g_recomp7_funcs, g_recomp7_func_count);
}

/* ---------------------------------------------------------------- overlays */
typedef struct { uint32_t base, size, code_start, code_end; uint8_t* img; uint32_t gen; int loaded; } Ovl;
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
    /* compare only code: data in an overlay changes once it runs */
    for (unsigned i = 0; i < g_t9.count; i++) {
        const RecompFunc* fn = &g_t9.funcs[i];
        if (fn->ovl_id >= 0 && fn->ovl_id < g_ovl_count && fn->size) {   /* size 0 = interpreted (code changes at run time) */
            Ovl* o = &g_ovl[fn->ovl_id];
            uint32_t a = fn->addr & ~1u, e = a + fn->size;
            if (e > o->code_end) o->code_end = e;
            if (!o->code_start || a < o->code_start) o->code_start = a;
        }
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
    /* compare code only (some overlays start with data their initialisers fill in) */
    uint32_t start = o->code_start ? o->code_start - o->base : 0;
    uint32_t end = o->code_end > o->base ? o->code_end - o->base : o->size;
    uint32_t code = end - start;
    uint32_t n = code < 256 ? code : 256;
    const uint8_t* ram = &g_main_ram[o->base & (MAIN_RAM_SIZE - 1)];
    int was = o->loaded;
    o->loaded = memcmp(ram + start, o->img + start, n) == 0;
    if (o->loaded && code > 1024) {
        uint32_t mid = (start + code / 2) & ~3u;
        o->loaded = memcmp(ram + mid, o->img + mid, 64) == 0;
    }
    if (o->loaded != was) {
        LOG_INFO("overlay %d %s\n", id, o->loaded ? "loaded" : "unloaded");
        if (!o->loaded) {
            for (uint32_t i = start; i < end; i++)
                if (ram[i] != o->img[i]) { LOG_INFO("  first difference at %08x: %02x, expected %02x (code %08x-%08x)\n",
                                                    o->base + i, ram[i], o->img[i], o->base + start, o->base + end); break; }
        }
    }
    return o->loaded;
}

/* ---------------------------------------------------------------- call budget */
#define BUDGET (1u << 20)
uint32_t g_call_budget = BUDGET, g_call_budget7 = BUDGET;

void recomp_budget_expired(Cpu* c) {
    if (c == &g_arm7) {
        g_call_budget7 = BUDGET;
        arm7_yield();
    } else {
        g_call_budget = BUDGET;
        if (!arm7_is_running()) { time_advance_lines(1); irq_service(&g_arm9); }
    }
}

#define LOOP_BUDGET 1024u
uint32_t g_loop_budget = LOOP_BUDGET, g_loop_budget7 = LOOP_BUDGET;

void recomp_loop_expired(Cpu* c) {
    if (c == &g_arm7) {
        g_loop_budget7 = LOOP_BUDGET;
        arm7_yield();
    } else {
        g_loop_budget = LOOP_BUDGET;
        if (!arm7_is_running()) { time_spend(LOOP_BUDGET * 4); irq_service(&g_arm9); }
    }
}

/* ---------------------------------------------------------------- tracing */
#define TRACE_N 256
static uint32_t g_trace[TRACE_N];
static uint8_t g_trace_cpu[TRACE_N];
static unsigned g_trace_pos;
unsigned long long g_calls_traced;

static const char* name_in(Table* t, uint32_t addr) {
    for (int32_t i = t->head[hkey(addr & ~1u)]; i >= 0; i = t->next[i])
        if ((t->funcs[i].addr & ~1u) == (addr & ~1u)) return t->funcs[i].name;
    return "?";
}
const char* recomp_func_name(uint32_t addr) { return name_in(&g_t9, addr); }
const char* recomp7_func_name(uint32_t addr) { return name_in(&g_t7, addr); }

void recomp_trace_enter(uint32_t addr) {
    g_trace_cpu[g_trace_pos % TRACE_N] = 9;
    g_trace[g_trace_pos++ % TRACE_N] = addr;
    g_calls_traced++;
}
void recomp7_trace_enter(uint32_t addr) {
    g_trace_cpu[g_trace_pos % TRACE_N] = 7;
    g_trace[g_trace_pos++ % TRACE_N] = addr;
    g_calls_traced++;
}

void trace_dump(FILE* f, int n) {
    if (!g_calls_traced) return;
    fprintf(f, "last %d functions entered (oldest first):\n", n);
    unsigned total = g_trace_pos < TRACE_N ? g_trace_pos : TRACE_N;
    if ((unsigned)n > total) n = (int)total;
    for (int k = n; k > 0; k--) {
        unsigned idx = (g_trace_pos - k) % TRACE_N;
        uint32_t a = g_trace[idx];
        int cpu = g_trace_cpu[idx];
        fprintf(f, "  ARM%d %08x %s\n", cpu, a, cpu == 7 ? recomp7_func_name(a) : recomp_func_name(a));
    }
}

/* ---------------------------------------------------------------- dispatch */
void interp_call(Core* k, uint32_t target);

/* The recompiled function at `addr` that may run now, or NULL. */
const RecompFunc* dispatch_find(Core* k, uint32_t addr) {
    Table* t = k->id == 0 ? &g_t9 : &g_t7;
    uint32_t key = addr & ~1u;
    for (int32_t i = t->head[hkey(key)]; i >= 0; i = t->next[i]) {
        const RecompFunc* f = &t->funcs[i];
        if ((f->addr & ~1u) != key) continue;
        if (!f->size) return NULL;                                 /* interpreted on purpose */
        if (f->ovl_id >= 0 && !overlay_is_loaded(f->ovl_id)) continue;
        return f;
    }
    return NULL;
}

static void dispatch_any(Core* k, Cpu* c, uint32_t target, uint32_t from) {
    const RecompFunc* f = dispatch_find(k, target);
    if (f) { f->fn(c); return; }
    /* not recompiled: code built or decrypted at run time */
    static uint32_t seen[2][64]; static int nseen[2];
    int known = 0;
    for (int i = 0; i < nseen[k->id]; i++) if (seen[k->id][i] == target) known = 1;
    if (!known && nseen[k->id] < 64) {
        seen[k->id][nseen[k->id]++] = target;
        LOG_INFO("%s: interpreting code at %08x (called from %08x)\n", k->name, target, from);
    }
    interp_call(k, target);
}

void dispatch_call(Cpu* c, uint32_t target, uint32_t from) { dispatch_any(&g_c9, c, target, from); }
void dispatch7_call(Cpu* c, uint32_t target, uint32_t from) { dispatch_any(&g_c7, c, target, from); }
void interp_stub(Cpu* c, uint32_t addr) { interp_call(core_of(c), addr); }
