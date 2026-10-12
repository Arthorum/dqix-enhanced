/* DQIX PC runtime: shared state for the emulated DS hardware (ARM9 side). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "cpu.h"

/* ---- logging ---- */
extern int g_log_level;
void rt_log(int level, const char* fmt, ...);
#define LOG_WARN(...)  rt_log(1, __VA_ARGS__)
#define LOG_INFO(...)  rt_log(2, __VA_ARGS__)
#define LOG_DEBUG(...) rt_log(3, __VA_ARGS__)
_Noreturn void rt_fatal(const char* fmt, ...);

/* ---- cartridge ---- */
extern uint8_t* g_rom;
extern size_t g_rom_size;

/* ---- memory ---- */
#define MAIN_RAM_SIZE (4u << 20)
#define ITCM_SIZE     0x8000u
#define DTCM_SIZE     0x4000u
#define SWRAM_SIZE    0x8000u
extern uint8_t g_main_ram[MAIN_RAM_SIZE];
extern uint8_t g_itcm[ITCM_SIZE];
extern uint8_t g_dtcm[DTCM_SIZE];
extern uint8_t g_swram[SWRAM_SIZE];
extern uint8_t g_palette[0x800];
extern uint8_t g_oam[0x800];
extern uint8_t g_vram[0xA4000];          /* banks A..I back to back */
extern uint32_t g_dtcm_base;
extern int g_itcm_enabled, g_dtcm_enabled;
void vram_remap(void);
uint8_t* vram_ptr(uint32_t addr);        /* NULL when unmapped */
/* Writes into the overlay area bump this so overlay detection re-checks. */
extern uint32_t g_overlay_area_start, g_overlay_area_end, g_overlay_gen;

/* ---- I/O ---- */
uint32_t io9_read(uint32_t addr, int size);
void     io9_write(uint32_t addr, uint32_t val, int size);
extern uint8_t g_io[0x2000];             /* backing store for 0x04000000-0x04001FFF */
static inline uint32_t IO32(uint32_t off) { uint32_t v; memcpy(&v, &g_io[off], 4); return v; }
static inline uint16_t IO16(uint32_t off) { uint16_t v; memcpy(&v, &g_io[off], 2); return v; }

/* ---- interrupts / timing ---- */
enum {
    IRQ_VBLANK = 0, IRQ_HBLANK = 1, IRQ_VCOUNT = 2, IRQ_TIMER0 = 3, IRQ_DMA0 = 8, IRQ_KEYPAD = 12,
    IRQ_IPCSYNC = 16, IRQ_IPC_SEND_EMPTY = 17, IRQ_IPC_RECV_NOT_EMPTY = 18,
    IRQ_CARD_DONE = 19, IRQ_GXFIFO = 21,
};
extern uint32_t g_ime, g_ie, g_if;
extern uint64_t g_frame;                 /* frames since boot */
extern uint32_t g_vcount;                /* current scanline 0..262 */
extern uint64_t g_frame_limit;           /* stop after this many frames (0 = never) */
void irq_raise(int bit);
void irq_service(Cpu* c);                /* deliver pending IRQs if enabled */
void time_advance_lines(Cpu* c, uint32_t lines);
void time_run_to_vblank(Cpu* c);
uint64_t time_cycles(void);              /* ARM9 bus cycles (33.5 MHz) since boot */
uint16_t timer_read(int n);
void timer_write_ctl(int n, uint16_t v);
void timer_write_reload(int n, uint16_t v);

/* ---- DMA, card, math ---- */
void dma_write_ctl(int ch);
void dma_on_event(int timing);           /* 1=vblank 2=hblank 5=card */
void card_write_ctl(uint32_t v);
uint32_t card_read_data(void);
void math_div(void);
void math_sqrt(void);

/* ---- IPC with the ARM7 ---- */
void     ipc_sync_write(uint16_t v);
uint16_t ipc_sync_read(void);
void     ipc_fifocnt_write(uint16_t v);
uint16_t ipc_fifocnt_read(void);
void     ipc_fifo_send(uint32_t v);
uint32_t ipc_fifo_recv(void);
void     arm7_hle_init(void);
void     arm7_hle_step(void);            /* let the ARM7 side react */

/* ---- CPU helpers ---- */
extern Cpu g_arm9;
int  cpu_mode_index(uint32_t mode);
void cpu_set_mode(Cpu* c, uint32_t mode);

/* ---- dispatch / overlays / tracing ---- */
void dispatch_init(void);
const char* recomp_func_name(uint32_t addr);
void overlays_init(const char* extract_dir);
void trace_dump(FILE* f, int n);

/* ---- game threads ---- */
void threads_after_irq(Cpu* c);
extern unsigned g_thread_switches;
