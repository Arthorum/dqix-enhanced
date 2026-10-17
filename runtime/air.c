/* The air between DS consoles (see air.h): shared memory between the
 * processes of one PC, one ring of frames per sender (single writer, so no
 * locks), and each console's current tick. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air.h"
#include "nds.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#define RING 128

typedef struct {
    volatile uint64_t tick;       /* next tick this console processes */
    volatile uint32_t gone;       /* stopped for good */
    volatile uint32_t attached;
    uint8_t pad[48];
} AirConsole;

typedef struct {
    uint32_t magic, count;
    uint8_t pad0[56];
    AirConsole con[AIR_MAX];
    volatile uint64_t wr[AIR_MAX];                /* frames written by each sender */
    uint8_t pad1[64 - 8 * AIR_MAX % 64];
    volatile uint64_t rd[AIR_MAX][AIR_MAX];       /* rd[receiver][sender]: frames it has taken */
    AirFrame ring[AIR_MAX][RING];
} AirShared;

#define AIR_MAGIC 0x52494151u                    /* "QAIR" */

static AirShared* g_air;
static int g_me = -1, g_n;
void (*air_idle_hook)(void);

/* frames taken from the rings with rx_tick <= now, not yet delivered */
#define LOCAL 64
static AirFrame g_q[LOCAL];
static uint64_t g_qseq[LOCAL];
static int g_qn;
static uint64_t g_pulled_seq;

static uint64_t ld(volatile uint64_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void st(volatile uint64_t* p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#endif
}
static void relax(unsigned* spins) {
    if (++*spins < 200) { __builtin_ia32_pause(); return; }
#ifdef _WIN32
    if (*spins < 4000) SwitchToThread(); else Sleep(0);
#else
    sched_yield();
#endif
}
/* wait helper: calls the idle hook every 15 ms or so */
static void waiting(double* last) {
    double t = now_ms();
    if (*last == 0) *last = t;
    else if (t - *last > 15) { *last = t; if (air_idle_hook) air_idle_hook(); }
}

/* this console is switched off: nobody waits for it any more */
static void air_leave(void) { if (g_air && g_me >= 0) g_air->con[g_me].gone = 1; }

int air_init(const char* name, int index, int count) {
    if (count < 1 || count > AIR_MAX || index < 0 || index >= count) return 0;
    size_t size = sizeof(AirShared);
    char nm[160];
#ifdef _WIN32
    snprintf(nm, sizeof nm, "Local\\dqix-air-%s", name);
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)size, nm);
    if (!h) { LOG_WARN("air: cannot create shared memory %s\n", nm); return 0; }
    g_air = (AirShared*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    snprintf(nm, sizeof nm, "/dqix-air-%s", name);
    int fd = shm_open(nm, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { LOG_WARN("air: cannot open shared memory %s\n", nm); return 0; }
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return 0; }
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    g_air = p == MAP_FAILED ? NULL : (AirShared*)p;
#endif
    if (!g_air) return 0;
    if (g_air->magic != AIR_MAGIC) { g_air->count = (uint32_t)count; g_air->magic = AIR_MAGIC; }
    g_me = index; g_n = count;
    g_air->con[g_me].attached = 1;
    atexit(air_leave);
    LOG_INFO("air: console %d of %d (%s)\n", index + 1, count, name);
    return 1;
}

int air_active(void) { return g_air != NULL; }
int air_index(void) { return g_me; }
int air_count(void) { return g_n; }
void air_set_gone(int index) { if (g_air && index >= 0 && index < g_n) g_air->con[index].gone = 1; }

void air_reach(uint64_t tick, int listening) {
    if (!g_air) return;
    st(&g_air->con[g_me].tick, tick);
    if (!listening || tick <= AIR_LOOKAHEAD) return;
    uint64_t need = tick - AIR_LOOKAHEAD;
    for (int k = 0; k < g_n; k++) {
        if (k == g_me) continue;
        unsigned spins = 0; double last = 0;
        while (!g_air->con[k].gone && ld(&g_air->con[k].tick) < need) { relax(&spins); waiting(&last); }
    }
}

void air_send(uint64_t rx_tick, int kind, int aid, uint32_t cmd_id, const uint8_t* data, int len) {
    if (!g_air || len <= 0) return;
    if (len > AIR_FRAME_MAX) len = AIR_FRAME_MAX;
    uint64_t w = g_air->wr[g_me];
    /* ring full: wait until every receiver that is still around has taken the oldest */
    for (int k = 0; k < g_n; k++) {
        if (k == g_me) continue;
        unsigned spins = 0; double last = 0;
        while (!g_air->con[k].gone && w - ld(&g_air->rd[k][g_me]) >= RING) { relax(&spins); waiting(&last); }
    }
    AirFrame* f = &g_air->ring[g_me][w % RING];
    f->rx_tick = rx_tick; f->cmd_id = cmd_id; f->kind = (uint8_t)kind; f->aid = (uint8_t)aid; f->from = (uint8_t)g_me;
    f->len = (uint16_t)len;
    memcpy(f->data, data, (size_t)len);
    st(&g_air->wr[g_me], w + 1);
}

/* move every frame that starts by `now` from the rings into the local queue, in (tick, sender) order */
static void pull(uint64_t now) {
    for (;;) {
        int best = -1; uint64_t bt = 0;
        for (int k = 0; k < g_n; k++) {
            if (k == g_me) continue;
            uint64_t r = g_air->rd[g_me][k];
            if (r >= ld(&g_air->wr[k])) continue;
            const AirFrame* f = &g_air->ring[k][r % RING];
            if (f->rx_tick > now) continue;
            if (best < 0 || f->rx_tick < bt) { best = k; bt = f->rx_tick; }
        }
        if (best < 0) return;
        uint64_t r = g_air->rd[g_me][best];
        const AirFrame* f = &g_air->ring[best][r % RING];
        if (f->kind == AIR_REPLY && (int)(f->cmd_id >> 28) != g_me) { st(&g_air->rd[g_me][best], r + 1); continue; }   /* a reply to another host */
        if (g_qn == LOCAL) { memmove(&g_q[0], &g_q[1], sizeof(AirFrame) * (LOCAL - 1)); memmove(&g_qseq[0], &g_qseq[1], 8 * (LOCAL - 1)); g_qn--; }
        g_q[g_qn] = g_air->ring[best][r % RING];
        g_qseq[g_qn++] = g_pulled_seq++;
        st(&g_air->rd[g_me][best], r + 1);
    }
}

static void drop(int i) {
    memmove(&g_q[i], &g_q[i + 1], sizeof(AirFrame) * (size_t)(g_qn - i - 1));
    memmove(&g_qseq[i], &g_qseq[i + 1], 8 * (size_t)(g_qn - i - 1));
    g_qn--;
}

int air_recv(uint64_t now, AirFrame* out) {
    if (!g_air) return 0;
    pull(now);
    for (int i = 0; i < g_qn; i++) {
        if (g_q[i].kind != AIR_FRAME) continue;
        *out = g_q[i];
        drop(i);
        return 1;
    }
    return 0;
}

void air_skip(uint64_t now) {
    if (!g_air) return;
    pull(now);
    g_qn = 0;
}

int air_take_reply(uint64_t now, uint32_t cmd_id, int aid, AirFrame* out) {
    if (!g_air) return 0;
    pull(now);
    for (int i = 0; i < g_qn; i++) {
        if (g_q[i].kind != AIR_REPLY || g_q[i].cmd_id != cmd_id || g_q[i].aid != aid) continue;
        *out = g_q[i];
        drop(i);
        return 1;
    }
    /* replies to older CMDs are of no use any more */
    for (int i = 0; i < g_qn;) { if (g_q[i].kind == AIR_REPLY && g_q[i].cmd_id != cmd_id) drop(i); else i++; }
    return 0;
}

/* DQIX_AIR="name,index,count" (tests: several copies on one PC). */
void air_init_from_env(void) {
    static int done; if (done) return; done = 1;
    const char* e = getenv("DQIX_AIR");
    if (!e) return;
    char name[64]; int idx, cnt;
    if (sscanf(e, "%63[^,],%d,%d", name, &idx, &cnt) == 3) air_init(name, idx, cnt);
}
