/* Game (Nitro SDK) threads on native coroutines.
 *
 * The SDK switches threads by saving every register into the thread's
 * OSContext and jumping into the middle of another thread's code. Recompiled
 * code can't jump into the middle of a C function, so these three SDK
 * routines are replaced:
 *   OS_SaveContext   0x020C7D34  write the context, remember the thread
 *   OS_LoadContext   0x020C7D80  switch to that thread's native coroutine
 *   OS_IrqHandler_ThreadSwitch (ITCM 0x01FF8068)  interrupt-time switch,
 *                    performed after the interrupt handler has returned
 * The rest of the SDK scheduler (priorities, queues, sleep) runs recompiled.
 *
 * OSContext layout (from OS_SaveContext): +0 cpsr, +4..+0x3C r0-r14,
 * +0x40 resume pc+4, +0x44 SVC-mode sp, +0x48 divider/sqrt state.
 */
#include <string.h>
#include "nds.h"
#include "coro.h"

#define THREADINFO          0x02111304u   /* OSi_ThreadInfo (+0 isNeedRescheduling, +4 current, +8 list, +0xC switch callback) */
#define IRQ_THREAD_QUEUE    0x027E0060u   /* OSThreadQueue of threads woken by interrupts (in DTCM) */
#define CP_SAVE_CONTEXT     0x020CD594u
#define CP_RESTORE_CONTEXT  0x020CD5D4u
#define RESUME_SAVED        0x020C7D7Cu   /* pc+4 that OS_SaveContext stores */
#define RESUME_IRQ          0xFFFF0004u   /* our marker: switched out at an interrupt */
#define THREAD_STACK        (2u << 20)

typedef struct Thread {
    uint32_t ctx;          /* OSContext address (== OSThread address) */
    Coro co;
    Cpu cpu;               /* register state while switched out */
    uint32_t start_pc;
    int alive;
} Thread;

#define MAX_THREADS 64
static Thread g_threads[MAX_THREADS];
static Thread* g_cur;
static uint32_t g_irq_switch_to, g_irq_switch_from;   /* set by the interrupt-time switch */
unsigned g_thread_switches;

static Thread* find(uint32_t ctx) {
    for (int i = 0; i < MAX_THREADS; i++) if (g_threads[i].alive && g_threads[i].ctx == ctx) return &g_threads[i];
    return NULL;
}
static Thread* alloc_thread(uint32_t ctx) {
    for (int i = 0; i < MAX_THREADS; i++) if (!g_threads[i].alive) {
        Thread* t = &g_threads[i];
        memset(t, 0, sizeof *t);
        t->ctx = ctx; t->alive = 1;
        return t;
    }
    rt_fatal("too many game threads\n");
}

/* The thread that was running before any switch: the host's main stack. */
static void ensure_current(uint32_t ctx) {
    if (g_cur) { if (!g_cur->ctx) g_cur->ctx = ctx; return; }
    g_cur = alloc_thread(ctx);
    coro_init_main(&g_cur->co);
}

static void call_fn(Cpu* c, uint32_t fn, uint32_t r0) {
    uint32_t save[16]; memcpy(save, c->r, sizeof save);
    c->r[0] = r0;
    dispatch_call(c, fn, 0);
    memcpy(c->r, save, sizeof save);
}

static void write_context(Cpu* c, uint32_t ctx, uint32_t resume) {
    uint32_t cpsr = CPSR_GET(c);
    mem_write32(ctx + 0, cpsr);
    for (int i = 0; i < 15; i++) mem_write32(ctx + 4 + 4 * i, c->r[i]);
    mem_write32(ctx + 4, 1);              /* resumes with r0 = 1 */
    mem_write32(ctx + 0x40, resume);
    mem_write32(ctx + 0x44, (c->cpsr_ctl & 0x1F) == 0x13 ? c->r[13] : c->bank_r13[3]);
}

static void thread_entry(void) {
    Thread* t = g_cur;
    Cpu* c = &g_arm9;
    uint32_t lr = c->r[14];
    LOG_INFO("thread %08x starts at %08x\n", t->ctx, t->start_pc);
    dispatch_call(c, t->start_pc, 0);
    /* the thread function returned: continue at its link register (OS_ExitThread) */
    dispatch_call(c, lr, t->start_pc);
    rt_fatal("game thread %08x returned from its exit routine\n", t->ctx);
}

static void switch_to(uint32_t ctx) {
    Thread* cur = g_cur;
    Thread* t = find(ctx);
    uint32_t resume = mem_read32(ctx + 0x40);
    int resumable = t && (resume == RESUME_SAVED || resume == RESUME_IRQ) && t != cur;
    if (t == cur) {
        LOG_WARN("thread %08x switching to itself\n", ctx);
        return;
    }
    cur->cpu = g_arm9;
    g_thread_switches++;
    if (resumable) {
        g_cur = t;
        g_arm9 = t->cpu;
    } else {
        /* first run of this context (or a context re-initialised for reuse) */
        if (t) t->alive = 0;
        t = alloc_thread(ctx);
        Cpu n = g_arm9;
        uint32_t cpsr = mem_read32(ctx);
        for (int i = 0; i < 15; i++) n.r[i] = mem_read32(ctx + 4 + 4 * i);
        n.r[15] = 0;
        n.n = cpsr >> 31; n.z = (cpsr >> 30) & 1; n.cf = (cpsr >> 29) & 1; n.v = (cpsr >> 28) & 1; n.q = (cpsr >> 27) & 1;
        n.cpsr_ctl = cpsr & 0x07FFFFFF & ~0x20u;
        n.bank_r13[3] = mem_read32(ctx + 0x44);
        t->start_pc = (mem_read32(ctx + 0x40) - 4) | ((cpsr >> 5) & 1);
        coro_create(&t->co, thread_entry, THREAD_STACK);
        g_cur = t;
        g_arm9 = n;
    }
    LOG_DEBUG("switch %08x -> %08x%s\n", cur->ctx, t->ctx, resumable ? "" : " (new)");
    coro_switch(&cur->co, &t->co);
    /* back on this thread: whoever switched here loaded our registers */
}

/* ---- replaced SDK functions ------------------------------------------ */

/* BOOL OS_SaveContext(OSContext* ctx): returns 0 now, "1" when resumed. */
void fn_main_020c7d34(Cpu* c) {
    uint32_t ctx = c->r[0];
    ensure_current(ctx);
    call_fn(c, CP_SAVE_CONTEXT, ctx + 0x48);
    write_context(c, ctx, RESUME_SAVED);
    c->r[0] = 0;
}

/* void OS_LoadContext(OSContext* ctx): switch threads. Returns when this
 * thread is resumed; the SDK caller then returns, exactly as it does when
 * OS_SaveContext reports a resume. */
void fn_main_020c7d80(Cpu* c) {
    uint32_t ctx = c->r[0];
    ensure_current(mem_read32(THREADINFO + 4));
    call_fn(c, CP_RESTORE_CONTEXT, ctx + 0x48);
    switch_to(ctx);
    g_arm9.r[0] = 1;
}

/* OS_IrqHandler_ThreadSwitch: wake threads queued by interrupts, and if a
 * reschedule is needed pick the next thread. The switch itself happens in
 * threads_after_irq(), once the interrupted state has been restored. */
static void irq_thread_switch(Cpu* c);
void fn_itcm_01ff8068(Cpu* c) {
    irq_thread_switch(c);
    /* every exit of the original pops the lr that OS_IrqHandler pushed */
    c->r[13] += 4;
}

static void irq_thread_switch(Cpu* c) {
    uint32_t q = IRQ_THREAD_QUEUE;           /* OSThreadQueue {head, tail} */
    uint32_t t = mem_read32(q);
    if (t) {
        while (t) {
            mem_write32(t + 0x64, 1);    /* state = ready */
            mem_write32(t + 0x78, 0);
            mem_write32(t + 0x7C, 0);
            uint32_t next = mem_read32(t + 0x80);
            mem_write32(t + 0x80, 0);
            t = next;
        }
        mem_write32(q, 0); mem_write32(q + 4, 0);
        mem_write16(THREADINFO, 1);
    }
    if (!mem_read16(THREADINFO)) return;
    mem_write16(THREADINFO, 0);
    uint32_t cur = mem_read32(THREADINFO + 4);
    uint32_t next = mem_read32(THREADINFO + 8);
    while (next && mem_read32(next + 0x64) != 1) next = mem_read32(next + 0x68);
    if (!next || next == cur) return;
    uint32_t cb = mem_read32(THREADINFO + 0xC);
    if (cb) {
        uint32_t save[16]; memcpy(save, c->r, sizeof save);
        c->r[0] = cur; c->r[1] = next;
        dispatch_call(c, cb, 0x01FF8148);
        memcpy(c->r, save, sizeof save);
    }
    mem_write32(THREADINFO + 4, next);
    g_irq_switch_from = cur;
    g_irq_switch_to = next;
}

/* Called by irq_service after the interrupted registers are back. */
void threads_after_irq(Cpu* c) {
    uint32_t next = g_irq_switch_to;
    if (!next) return;
    g_irq_switch_to = 0;
    uint32_t prev = g_irq_switch_from;
    ensure_current(prev);
    call_fn(c, CP_SAVE_CONTEXT, prev + 0x48);
    write_context(c, prev, RESUME_IRQ);
    call_fn(c, CP_RESTORE_CONTEXT, next + 0x48);
    switch_to(next);
}
