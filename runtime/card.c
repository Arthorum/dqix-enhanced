/* Game card: ROM reads (both CPUs) and the save chip on the backup SPI bus.
 * Registers 0x040001A0-0x040001AF live in g_io and are shared by the CPUs. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"

/* ---------------------------------------------------------------- ROM */
static uint8_t g_card_buf[0x4000];
static uint32_t g_card_len, g_card_pos;
static Core* g_card_owner;

static void rom_start(Core* k, uint32_t ctl) {
    const uint8_t* cmd = &g_io[0x1A8];
    uint32_t bs = (ctl >> 24) & 7;
    uint32_t len = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    g_card_len = len; g_card_pos = 0; g_card_owner = k;
    if (cmd[0] == 0xB7) {
        uint32_t addr = ((uint32_t)cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
        for (uint32_t i = 0; i < len; i++) {
            uint32_t a = addr + i;
            if (a < 0x8000) a = 0x8000 + (a & 0x1FF);
            g_card_buf[i] = g_rom[a % g_rom_size];
        }
        LOG_DEBUG("card: %s reads %x bytes at %08x\n", k->name, len, addr);
    } else if (cmd[0] == 0xB8) {
        for (uint32_t i = 0; i < len; i += 4) { uint32_t id = 0x00003FC2; memcpy(&g_card_buf[i], &id, 4); }
    } else {
        memset(g_card_buf, 0xFF, len);
        LOG_WARN("card: command %02x not emulated\n", cmd[0]);
    }
    if (len == 0) { ctl &= ~0x80000000u; memcpy(&g_io[0x1A4], &ctl, 4); return; }
    ctl |= 0x00800000u;  /* data word ready */
    memcpy(&g_io[0x1A4], &ctl, 4);
    dma_on_event(k, DMA_CARD);
}

uint32_t card_read_data(Core* k) {
    (void)k;
    uint32_t v = 0xFFFFFFFF;
    if (g_card_pos < g_card_len) {
        memcpy(&v, &g_card_buf[g_card_pos], 4);
        g_card_pos += 4;
        if (g_card_pos >= g_card_len) {
            uint32_t ctl = IO32(0x1A4) & ~0x80800000u;
            memcpy(&g_io[0x1A4], &ctl, 4);
            if (IO16(0x1A0) & 0x4000) irq_raise(g_card_owner ? g_card_owner : k, IRQ_CARD_DONE);
        }
    }
    return v;
}

/* ---------------------------------------------------------------- save chip
 * SPI EEPROM / FLASH. Up to 64 KB it behaves as an EEPROM with 2-byte
 * addresses (what DQIX uses); larger sizes act as FLASH with 3-byte
 * addresses. The size follows an existing save file, else 64 KB. */
#define BACKUP_MAX (1u << 20)
static uint8_t g_backup[BACKUP_MAX];
static uint32_t g_backup_size = 64 * 1024;   /* DQIX: 64 KB EEPROM (2-byte addresses) */
static char g_backup_path[1024];
static int g_backup_dirty;
static uint8_t g_bk_cmd, g_bk_status;
static uint32_t g_bk_addr, g_bk_count;

void backup_init(const char* path) {
    memset(g_backup, 0xFF, sizeof g_backup);
    snprintf(g_backup_path, sizeof g_backup_path, "%s", path);
    FILE* f = fopen(path, "rb");
    if (f) {
        size_t n = fread(g_backup, 1, BACKUP_MAX, f);
        fclose(f);
        if (n) g_backup_size = (uint32_t)n;
        LOG_INFO("save: loaded %zu bytes from %s\n", n, path);
    }
}

void backup_flush(void) {
    if (!g_backup_dirty || !g_backup_path[0]) return;
    FILE* f = fopen(g_backup_path, "wb");
    if (!f) { LOG_WARN("save: cannot write %s\n", g_backup_path); return; }
    fwrite(g_backup, 1, g_backup_size, f);
    fclose(f);
    g_backup_dirty = 0;
}

static uint8_t backup_xfer(uint8_t in, int hold) {
    uint8_t out = 0xFF;
    if (g_bk_cmd == 0) {
        g_bk_cmd = in; g_bk_addr = 0; g_bk_count = 0;
        switch (in) {
        case 0x06: g_bk_status |= 2; g_bk_cmd = 0; break;      /* write enable */
        case 0x04: g_bk_status &= ~2; g_bk_cmd = 0; break;     /* write disable */
        default: break;
        }
    } else {
        uint32_t n = g_bk_count++;
        uint32_t abytes = g_backup_size > 0x10000 ? 3 : 2;
        switch (g_bk_cmd) {
        case 0x05: out = g_bk_status; break;                   /* read status */
        case 0x9F: out = n == 0 ? 0x20 : n == 1 ? 0x40 : 0x13; break;   /* JEDEC id */
        case 0x03: case 0x0B: case 0x02: case 0x0A: case 0xDB: case 0xD8:
            if (n < abytes) { g_bk_addr = (g_bk_addr << 8) | in; break; }
            if (g_bk_cmd == 0x0B && n == abytes) break;            /* dummy byte */
            if (g_bk_cmd == 0x03 || g_bk_cmd == 0x0B) { out = g_backup[g_bk_addr++ % g_backup_size]; break; }
            if (g_bk_cmd == 0x02 || g_bk_cmd == 0x0A) {          /* program / write */
                uint32_t a = g_bk_addr++ % g_backup_size;
                /* flash "page program" can only clear bits; EEPROM writes replace */
                g_backup[a] = (g_bk_cmd == 0x02 && g_backup_size > 0x10000) ? (g_backup[a] & in) : in;
                g_backup_dirty = 1;
            }
            break;
        default:
            if (n == 0) LOG_WARN("save: command %02x not emulated\n", g_bk_cmd);
            break;
        }
    }
    if (!hold) {
        if (g_bk_cmd) LOG_DEBUG("save: cmd %02x addr %06x, %u bytes\n", g_bk_cmd, g_bk_addr, g_bk_count);
        if (g_bk_cmd == 0xDB || g_bk_cmd == 0xD8) {               /* page / sector erase */
            uint32_t sz = g_bk_cmd == 0xDB ? 0x100 : 0x10000;
            uint32_t a = (g_bk_addr % g_backup_size) & ~(sz - 1);
            memset(&g_backup[a], 0xFF, sz);
            g_backup_dirty = 1;
        }
        if (g_bk_cmd == 0x02 || g_bk_cmd == 0x0A || g_bk_cmd == 0xDB || g_bk_cmd == 0xD8) g_bk_status &= ~2;
        g_bk_cmd = 0;
        backup_flush();
    }
    return out;
}

/* ---------------------------------------------------------------- registers */
uint32_t card_read(Core* k, uint32_t off, int size) {
    (void)k;
    uint32_t v; memcpy(&v, &g_io[off & ~3u], 4);
    (void)size;
    return v;
}

void card_write(Core* k, uint32_t off, uint32_t v, int size) {
    memcpy(&g_io[off], &v, size);
    uint32_t end = off + size;
    if (off <= 0x1A2 && end > 0x1A2) {                            /* AUXSPIDATA: one byte over SPI */
        uint16_t cnt = IO16(0x1A0);
        if ((cnt & 0x8000) && (cnt & 0x2000)) {
            uint8_t in = (uint8_t)(size == 4 ? (v >> 16) : v);
            uint8_t out = backup_xfer(in, cnt & 0x40);
            g_io[0x1A2] = out; g_io[0x1A3] = 0;
        }
    }
    if (off <= 0x1A7 && end > 0x1A7 && (g_io[0x1A7] & 0x80)) rom_start(k, IO32(0x1A4));
}
