/* CPU modes, interrupts, CP15 and the DS BIOS (high-level implementation). */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "dispatch.h"

Cpu g_arm9;

/* ---------------------------------------------------------------- modes */
int cpu_mode_index(uint32_t mode) {
    switch (mode & 0x1F) {
    case 0x11: return 1;
    case 0x12: return 2;
    case 0x13: return 3;
    case 0x17: return 4;
    case 0x1B: return 5;
    default: return 0;  /* user / system */
    }
}

void cpu_set_mode(Cpu* c, uint32_t mode) {
    int from = cpu_mode_index(c->cpsr_ctl), to = cpu_mode_index(mode);
    if (from != to) {
        c->bank_r13[from] = c->r[13]; c->bank_r14[from] = c->r[14];
        c->r[13] = c->bank_r13[to];   c->r[14] = c->bank_r14[to];
    }
    c->cpsr_ctl = (c->cpsr_ctl & ~0x1Fu) | (mode & 0x1F);
}

void cpu_write_cpsr_ctl(Cpu* c, uint32_t v, uint32_t mask) {
    uint32_t nv = (c->cpsr_ctl & ~mask) | (v & mask);
    if ((nv & 0x1F) != (c->cpsr_ctl & 0x1F)) cpu_set_mode(c, nv & 0x1F);
    int enabling = (c->cpsr_ctl & 0x80) && !(nv & 0x80);
    c->cpsr_ctl = nv;
    if (enabling) irq_service(c);
}

uint32_t cpu_read_spsr(Cpu* c) { return c->spsr[cpu_mode_index(c->cpsr_ctl)]; }
void cpu_write_spsr(Cpu* c, uint32_t v, uint32_t m) {
    uint32_t* s = &c->spsr[cpu_mode_index(c->cpsr_ctl)];
    *s = (*s & ~m) | (v & m);
}
void cpu_exception_return(Cpu* c, uint32_t addr) {
    (void)addr;
    uint32_t s = cpu_read_spsr(c);
    c->n = s >> 31; c->z = (s >> 30) & 1; c->cf = (s >> 29) & 1; c->v = (s >> 28) & 1; c->q = (s >> 27) & 1;
    cpu_write_cpsr_ctl(c, s, 0x07FFFFFF);
}

/* Call a game function with up to 4 arguments; returns r0. Other registers preserved. */
static uint32_t call_game(Cpu* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t save[16]; memcpy(save, c->r, sizeof save);
    c->r[0] = a0; c->r[1] = a1; c->r[2] = a2; c->r[3] = a3;
    c->r[14] = 0xFFFF0000u;
    dispatch_call(c, fn, 0xFFFF0000u);
    uint32_t r = c->r[0];
    memcpy(c->r, save, sizeof save);
    return r;
}

/* ---------------------------------------------------------------- IRQs */
static int g_irq_depth;

void irq_service(Cpu* c) {
    if (!c || c != &g_arm9) return;
    if (!g_ime || (c->cpsr_ctl & 0x80) || !(g_ie & g_if)) return;
    if (g_irq_depth > 8) return;
    g_irq_depth++;
    Cpu save = *c;
    uint32_t old_cpsr = CPSR_GET(c);
    /* enter IRQ mode with IRQs masked, as the CPU does */
    cpu_set_mode(c, 0x12);
    c->cpsr_ctl |= 0x80;
    c->spsr[2] = old_cpsr;
    /* BIOS IRQ vector: push r0-r3, r12, lr; r0 = DTCM end; call [DTCM+0x3FFC] */
    uint32_t sp = c->r[13] - 24;
    mem_write32(sp, save.r[0]); mem_write32(sp + 4, save.r[1]); mem_write32(sp + 8, save.r[2]);
    mem_write32(sp + 12, save.r[3]); mem_write32(sp + 16, save.r[12]); mem_write32(sp + 20, save.r[14]);
    c->r[13] = sp;
    c->r[0] = g_dtcm_base + DTCM_SIZE;
    c->r[14] = 0xFFFF0290u;
    uint32_t handler = mem_read32(g_dtcm_base + 0x3FFC);
    dispatch_call(c, handler, 0xFFFF0018u);
    /* return: restore the interrupted state (IRQ stack is balanced again) */
    c->r[13] += 24;
    cpu_set_mode(c, save.cpsr_ctl & 0x1F);
    uint32_t b13[6], b14[6], sp6[6];
    memcpy(b13, c->bank_r13, sizeof b13); memcpy(b14, c->bank_r14, sizeof b14); memcpy(sp6, c->spsr, sizeof sp6);
    *c = save;
    memcpy(c->bank_r13, b13, sizeof b13); memcpy(c->bank_r14, b14, sizeof b14); memcpy(c->spsr, sp6, sizeof sp6);
    g_irq_depth--;
    threads_after_irq(c);
}

/* ---------------------------------------------------------------- CP15 */
static uint32_t g_cp15_ctl = 0x00052078, g_cp15_dtcm = 0x0080000A, g_cp15_itcm = 0x0000000C;

static void wait_for_interrupt(Cpu* c) {
    uint32_t saved_ime = g_ime;
    for (int i = 0; i < 263 * 60 && !(g_ie & g_if); i++) time_advance_lines(NULL, 1);
    g_ime = saved_ime;
    irq_service(c);
}

void cp15_write(Cpu* c, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v) {
    (void)op1;
    if (crn == 1 && crm == 0 && op2 == 0) {
        g_cp15_ctl = v;
        g_dtcm_enabled = (v >> 16) & 1;
        g_itcm_enabled = (v >> 18) & 1;
    } else if (crn == 9 && crm == 1 && op2 == 0) {
        g_cp15_dtcm = v; g_dtcm_base = v & 0xFFFFF000u;
        LOG_INFO("DTCM at %08x\n", g_dtcm_base);
    } else if (crn == 9 && crm == 1 && op2 == 1) {
        g_cp15_itcm = v;
    } else if (crn == 7 && ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2))) {
        wait_for_interrupt(c);
    }
    /* caches, write buffer, protection regions: nothing to do */
}

uint32_t cp15_read(Cpu* c, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
    (void)c; (void)op1;
    if (crn == 0 && crm == 0 && op2 == 0) return 0x41059461;   /* ARM946E-S */
    if (crn == 0 && crm == 0 && op2 == 1) return 0x0F0D2112;   /* cache type */
    if (crn == 1) return g_cp15_ctl;
    if (crn == 9 && op2 == 0) return g_cp15_dtcm;
    if (crn == 9 && op2 == 1) return g_cp15_itcm;
    return 0;
}

/* ---------------------------------------------------------------- BIOS */
static void intr_wait(Cpu* c, int discard, uint32_t mask) {
    uint32_t flags_addr = g_dtcm_base + 0x3FF8;
    if (discard) mem_write32(flags_addr, mem_read32(flags_addr) & ~mask);
    uint32_t saved_ime = g_ime;
    g_ime = 1;   /* the BIOS enables IME while waiting */
    for (int i = 0; i < 263 * 120; i++) {
        irq_service(c);
        uint32_t f = mem_read32(flags_addr);
        if (f & mask) { mem_write32(flags_addr, f & ~mask); g_ime = saved_ime; return; }
        time_advance_lines(c, 1);
    }
    g_ime = saved_ime;
    LOG_WARN("IntrWait(mask %08x): nothing after 2 seconds (IE %08x IF %08x IME %u)\n", mask, g_ie, g_if, g_ime);
}

static uint16_t crc16(uint16_t crc, uint32_t p, uint32_t len) {
    static const uint16_t val[8] = { 0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001 };
    for (uint32_t i = 0; i < len; i++) {
        crc ^= mem_read8(p + i);
        for (int j = 0; j < 8; j++) { int carry = crc & 1; crc >>= 1; if (carry) crc ^= (uint16_t)(val[j] << (7 - j)); }
    }
    return crc;
}

typedef struct { Cpu* c; uint32_t src; uint32_t cb; int has_cb; } Reader;
static uint8_t rd8(Reader* r) {
    if (r->has_cb) return (uint8_t)call_game(r->c, mem_read32(r->cb + 8), r->src++, 0, 0, 0);
    return mem_read8(r->src++);
}
static uint32_t rd_header(Reader* r, uint32_t dst, uint32_t param) {
    if (r->has_cb) {
        uint32_t h = call_game(r->c, mem_read32(r->cb), r->src, dst, param, 0);
        r->src += 4;
        return h;
    }
    uint32_t h = mem_read32(r->src); r->src += 4; return h;
}

static void lz77(Cpu* c, int cb, int write16) {
    Reader r = { c, c->r[0], c->r[3], cb };
    uint32_t dst = c->r[1];
    uint32_t hdr = rd_header(&r, dst, c->r[2]);
    uint32_t len = hdr >> 8, out = 0;
    uint16_t pending = 0;
    #define PUT(b) do { uint8_t _b = (b); \
        if (write16) { if (out & 1) mem_write16(dst + out - 1, pending | (_b << 8)); else pending = _b; } \
        else mem_write8(dst + out, _b); out++; } while (0)
    static uint8_t window[0x10000];
    while (out < len) {
        uint8_t flags = rd8(&r);
        for (int i = 0; i < 8 && out < len; i++, flags <<= 1) {
            if (flags & 0x80) {
                uint8_t b1 = rd8(&r), b2 = rd8(&r);
                uint32_t n = (b1 >> 4) + 3, disp = (((b1 & 15) << 8) | b2) + 1;
                for (uint32_t k = 0; k < n && out < len; k++) {
                    uint8_t v = window[(out - disp) & 0xFFFF];
                    window[out & 0xFFFF] = v; PUT(v);
                }
            } else {
                uint8_t v = rd8(&r); window[out & 0xFFFF] = v; PUT(v);
            }
        }
    }
    if (write16 && (out & 1)) mem_write16(dst + out - 1, pending);
    if (cb && mem_read32(c->r[3] + 4)) call_game(c, mem_read32(c->r[3] + 4), 0, 0, 0, 0);
    #undef PUT
}

static void rle(Cpu* c, int cb, int write16) {
    Reader r = { c, c->r[0], c->r[3], cb };
    uint32_t dst = c->r[1];
    uint32_t len = rd_header(&r, dst, c->r[2]) >> 8, out = 0;
    uint8_t pending = 0;
    while (out < len) {
        uint8_t f = rd8(&r);
        uint32_t n = (f & 0x80) ? (f & 0x7F) + 3 : (f & 0x7F) + 1;
        uint8_t v = (f & 0x80) ? rd8(&r) : 0;
        for (uint32_t k = 0; k < n && out < len; k++) {
            uint8_t b = (f & 0x80) ? v : rd8(&r);
            if (write16) { if (out & 1) mem_write16(dst + out - 1, pending | (b << 8)); else pending = b; }
            else mem_write8(dst + out, b);
            out++;
        }
    }
    if (write16 && (out & 1)) mem_write16(dst + out - 1, pending);
}

static void bit_unpack(Cpu* c) {
    uint32_t src = c->r[0], dst = c->r[1], info = c->r[2];
    uint32_t srclen = mem_read16(info), sw = mem_read8(info + 2), dw = mem_read8(info + 3), off = mem_read32(info + 4);
    int zero_flag = off >> 31; off &= 0x7FFFFFFF;
    uint32_t acc = 0, accbits = 0;
    for (uint32_t i = 0; i < srclen; i++) {
        uint8_t b = mem_read8(src + i);
        for (uint32_t bit = 0; bit < 8; bit += sw) {
            uint32_t v = (b >> bit) & ((1u << sw) - 1);
            if (v || zero_flag) v += off;
            acc |= v << accbits; accbits += dw;
            if (accbits >= 32) { mem_write32(dst, acc); dst += 4; acc = 0; accbits = 0; }
        }
    }
}

void bios_swi(Cpu* c, uint32_t n) {
    switch (n) {
    case 0x03: break;                                               /* WaitByLoop */
    case 0x04: intr_wait(c, c->r[0] != 0, c->r[1]); break;          /* IntrWait */
    case 0x05: intr_wait(c, 1, 1); break;                           /* VBlankIntrWait */
    case 0x06: wait_for_interrupt(c); break;                        /* Halt */
    case 0x09: {                                                    /* Div */
        int32_t a = (int32_t)c->r[0], b = (int32_t)c->r[1];
        if (b == 0) { LOG_WARN("BIOS Div by zero\n"); break; }
        int32_t q = (a == INT32_MIN && b == -1) ? a : a / b;
        int32_t r = (a == INT32_MIN && b == -1) ? 0 : a % b;
        c->r[0] = (uint32_t)q; c->r[1] = (uint32_t)r; c->r[3] = (uint32_t)(q < 0 ? -q : q);
        break;
    }
    case 0x0B: {                                                    /* CpuSet */
        uint32_t s = c->r[0], d = c->r[1], ctl = c->r[2], cnt = ctl & 0x1FFFFF;
        int fill = (ctl >> 24) & 1, w32 = (ctl >> 26) & 1;
        for (uint32_t i = 0; i < cnt; i++) {
            if (w32) { mem_write32(d, mem_read32(s)); d += 4; if (!fill) s += 4; }
            else { mem_write16(d, mem_read16(s)); d += 2; if (!fill) s += 2; }
        }
        break;
    }
    case 0x0C: {                                                    /* CpuFastSet */
        uint32_t s = c->r[0], d = c->r[1], ctl = c->r[2];
        uint32_t cnt = ((ctl & 0x1FFFFF) + 7) & ~7u;
        int fill = (ctl >> 24) & 1;
        for (uint32_t i = 0; i < cnt; i++) { mem_write32(d, mem_read32(s)); d += 4; if (!fill) s += 4; }
        break;
    }
    case 0x0D: {                                                    /* Sqrt */
        uint32_t v = c->r[0], r = 0;
        for (uint32_t bit = 1u << 30; bit; bit >>= 2) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; }
        c->r[0] = r;
        break;
    }
    case 0x0E: c->r[0] = crc16((uint16_t)c->r[0], c->r[1], c->r[2]); break;   /* GetCRC16 */
    case 0x0F: c->r[0] = 0; break;                                  /* IsDebugger */
    case 0x10: bit_unpack(c); break;
    case 0x11: lz77(c, 0, 0); break;                                /* LZ77 -> 8-bit writes */
    case 0x12: lz77(c, 1, 1); break;                                /* LZ77 via callbacks -> 16-bit writes */
    case 0x14: rle(c, 0, 0); break;
    case 0x15: rle(c, 1, 1); break;
    default:
        rt_fatal("BIOS call %02x not implemented (r0=%08x r1=%08x r2=%08x r3=%08x)\n", n, c->r[0], c->r[1], c->r[2], c->r[3]);
    }
}

void cpu_breakpoint(Cpu* c, uint32_t a, uint32_t imm) { (void)c; LOG_WARN("bkpt #%u at %08x\n", imm, a); }
void cpu_unhandled(Cpu* c, uint32_t a, uint32_t w) {
    c->fault_addr = a;
    trace_dump(stderr, 24);
    rt_fatal("untranslatable instruction %08x at %08x\n", w, a);
}

/* ---------------------------------------------------------------- logging */
int g_log_level = 2;
void rt_log(int level, const char* fmt, ...) {
    if (level > g_log_level) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[f%llu l%u] ", (unsigned long long)g_frame, g_vcount);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
_Noreturn void rt_fatal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "FATAL [frame %llu]: ", (unsigned long long)g_frame);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    trace_dump(stderr, 32);
    exit(1);
}
