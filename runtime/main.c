/* DQIX PC runtime entry point: load the ROM, set up the machine the way the
 * DS firmware leaves it, and run the recompiled game. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "dispatch.h"

uint8_t* g_rom;
size_t g_rom_size;
uint64_t g_frame_limit;
extern unsigned long long g_calls_traced;

static void load_rom(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open ROM %s\n", path);
    fseek(f, 0, SEEK_END);
    g_rom_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_rom = malloc(g_rom_size);
    if (fread(g_rom, 1, g_rom_size, f) != g_rom_size) rt_fatal("short read on ROM\n");
    fclose(f);
}

static uint32_t rom32(uint32_t off) { uint32_t v; memcpy(&v, g_rom + off, 4); return v; }
static void ram32(uint32_t a, uint32_t v) { memcpy(&g_main_ram[a & (MAIN_RAM_SIZE - 1)], &v, 4); }
static void ram16(uint32_t a, uint16_t v) { memcpy(&g_main_ram[a & (MAIN_RAM_SIZE - 1)], &v, 2); }

/* What the BIOS + firmware leave behind when they start a game card. */
static void boot_firmware(void) {
    uint32_t a9_rom = rom32(0x20), a9_entry = rom32(0x24), a9_load = rom32(0x28), a9_size = rom32(0x2C);
    uint32_t a7_rom = rom32(0x30), a7_load = rom32(0x38), a7_size = rom32(0x3C);
    memcpy(&g_main_ram[a9_load & (MAIN_RAM_SIZE - 1)], g_rom + a9_rom, a9_size);
    if ((a7_load >> 24) == 2) memcpy(&g_main_ram[a7_load & (MAIN_RAM_SIZE - 1)], g_rom + a7_rom, a7_size);
    memcpy(&g_main_ram[0x027FFE00 & (MAIN_RAM_SIZE - 1)], g_rom, 0x170);   /* cartridge header */
    uint32_t chip = 0x00003FC2;
    ram32(0x027FF800, chip); ram32(0x027FF804, chip); ram32(0x027FFC00, chip); ram32(0x027FFC04, chip);
    ram16(0x027FF808, (uint16_t)rom32(0x15E)); ram16(0x027FFC08, (uint16_t)rom32(0x15E));
    ram16(0x027FF80A, (uint16_t)rom32(0x06C)); ram16(0x027FFC0A, (uint16_t)rom32(0x06C));
    ram16(0x027FFC10, 0x5835);
    ram16(0x027FFC40, 1);   /* booted from a card */
    /* firmware user settings (0x027FFC80): English, nickname "Player" */
    uint32_t us = 0x027FFC80;
    ram16(us + 0x00, 5);                    /* version */
    g_main_ram[(us + 0x02) & (MAIN_RAM_SIZE - 1)] = 4;    /* favourite colour */
    g_main_ram[(us + 0x03) & (MAIN_RAM_SIZE - 1)] = 1;    /* birthday month */
    g_main_ram[(us + 0x04) & (MAIN_RAM_SIZE - 1)] = 1;    /* birthday day */
    const char* nick = "Player";
    for (int i = 0; nick[i]; i++) ram16(us + 0x06 + 2 * i, (uint16_t)nick[i]);
    ram16(us + 0x1A, (uint16_t)strlen(nick));
    ram16(us + 0x64, 0x0001 | 0x0400);       /* language English, backlight */
    g_arm9.r[13] = 0x023FFE00;
    g_arm9.cpsr_ctl = 0xDF;                  /* system mode, IRQ+FIQ masked */
    LOG_INFO("ARM9 binary: %u bytes at %08x, entry %08x\n", a9_size, a9_load, a9_entry);
}

static void report(void) {
    fprintf(stderr, "\n--- stopped after %llu frames, %llu function calls traced ---\n",
            (unsigned long long)g_frame, g_calls_traced);
    trace_dump(stderr, 40);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <rom.nds> <extract/usa dir> [frames] [log level]\n", argv[0]);
        return 2;
    }
    load_rom(argv[1]);
    if (argc > 3) g_frame_limit = strtoull(argv[3], NULL, 0);
    if (argc > 4) g_log_level = atoi(argv[4]);
    atexit(report);
    dispatch_init();
    overlays_init(argv[2]);
    boot_firmware();
    vram_remap();
    arm7_hle_init();
    uint32_t entry = rom32(0x24);
    dispatch_call(&g_arm9, entry, 0);
    LOG_WARN("the game's entry function returned\n");
    return 0;
}
