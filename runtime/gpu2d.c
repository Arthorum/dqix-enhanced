/* The DS's two 2D graphics engines (A = main, B = sub), rendered one
 * scanline at a time like the hardware, so mid-frame register changes work.
 *
 * Per engine: 4 backgrounds (text, affine, extended/bitmap; BG0 can be the 3D
 * layer on engine A), 128 sprites, windows, colour effects (alpha blend,
 * brighten, darken) and master brightness. Reference: GBATEK. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"

uint32_t g_screen[2][256 * 192];
static uint32_t g_engine_out[2][256 * 192];

/* 3D layer of engine A for the current line (gpu3d.c fills it later). */
uint16_t g_3d_line[256];
uint8_t  g_3d_alpha[256];      /* 0 = transparent, 31 = opaque */
int      g_3d_present;
void gpu3d_render_line(int line);

typedef struct {
    int base;                  /* register offset in g_io: 0x000 or 0x1000 */
    uint32_t bgvram, objvram;  /* virtual VRAM addresses */
    uint32_t pal;              /* palette RAM offset */
    int32_t refx[2], refy[2];  /* BG2/BG3 affine internal reference points */
} Engine;

static Engine g_eng[2] = {
    { 0x0000, 0x06000000, 0x06400000, 0x000, {0}, {0} },
    { 0x1000, 0x06200000, 0x06600000, 0x400, {0}, {0} },
};

static inline uint16_t R16(Engine* e, uint32_t off) { uint16_t v; memcpy(&v, &g_io[e->base + off], 2); return v; }
static inline uint32_t R32(Engine* e, uint32_t off) { uint32_t v; memcpy(&v, &g_io[e->base + off], 4); return v; }

static inline uint8_t vr8(uint32_t a) { uint8_t* p = vram_ptr(a); return p ? *p : 0; }
static inline uint16_t vr16(uint32_t a) { uint8_t* p = vram_ptr(a & ~1u); uint16_t v = 0; if (p) memcpy(&v, p, 2); return v; }
static inline uint16_t pal16(uint32_t off) { uint16_t v; memcpy(&v, &g_palette[off & 0x7FE], 2); return v; }

/* ---------------------------------------------------------------- extended palettes */
static const uint8_t* ext_bg_pal(int eng, int slot) {
    if (eng == 0) {
        uint8_t cnt = vram_bank_cnt(4);                          /* E: all 4 slots */
        if ((cnt & 0x87) == 0x84) return vram_bank(4) + slot * 0x2000;
        for (int b = 5; b <= 6; b++) {                           /* F, G: 2 slots each */
            cnt = vram_bank_cnt(b);
            if ((cnt & 0x87) == 0x84) {
                int first = ((cnt >> 3) & 1) * 2;
                if (slot == first || slot == first + 1) return vram_bank(b) + (slot - first) * 0x2000;
            }
        }
        return NULL;
    }
    if ((vram_bank_cnt(7) & 0x87) == 0x82) return vram_bank(7) + slot * 0x2000;   /* H */
    return NULL;
}
static const uint8_t* ext_obj_pal(int eng) {
    if (eng == 0) {
        if ((vram_bank_cnt(4) & 0x87) == 0x85) return vram_bank(4);
        for (int b = 5; b <= 6; b++) if ((vram_bank_cnt(b) & 0x87) == 0x85) return vram_bank(b);
        return NULL;
    }
    if ((vram_bank_cnt(8) & 0x87) == 0x83) return vram_bank(8);                   /* I */
    return NULL;
}
static inline uint16_t ext16(const uint8_t* p, uint32_t idx) { uint16_t v = 0; if (p) memcpy(&v, p + idx * 2, 2); return v; }

/* ---------------------------------------------------------------- line buffers */
#define PX_OPAQUE 0x8000u
typedef struct { uint16_t col[256]; } BgLine;                        /* bit 15 = opaque */
typedef struct { uint16_t col[256]; uint8_t prio[256]; uint8_t semi[256]; uint8_t win[256]; } ObjLine;

static void render_text_bg(Engine* e, int eng, int bg, int y, BgLine* out) {
    uint32_t disp = R32(e, 0);
    uint16_t cnt = R16(e, 0x08 + bg * 2);
    uint32_t hofs = R16(e, 0x10 + bg * 4) & 0x1FF, vofs = R16(e, 0x12 + bg * 4) & 0x1FF;
    uint32_t charbase = e->bgvram + ((cnt >> 2) & 15) * 0x4000 + (eng == 0 ? ((disp >> 24) & 7) * 0x10000 : 0);
    uint32_t mapbase  = e->bgvram + ((cnt >> 8) & 31) * 0x800  + (eng == 0 ? ((disp >> 27) & 7) * 0x10000 : 0);
    int size = cnt >> 14;
    uint32_t w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    int c256 = (cnt >> 7) & 1;
    const uint8_t* ext = NULL;
    if (c256 && (disp & 0x40000000)) {
        int slot = bg;
        if (bg < 2 && (cnt & 0x2000)) slot = bg + 2;
        ext = ext_bg_pal(eng, slot);
    }
    uint32_t yy = (y + vofs) & (h - 1);
    uint32_t ty = yy >> 3;
    for (int x = 0; x < 256;) {
        uint32_t xx = (x + hofs) & (w - 1);
        uint32_t tx = xx >> 3;
        uint32_t sb = (tx >> 5) + (ty >> 5) * (w >> 8);
        uint16_t ent = vr16(mapbase + sb * 0x800 + (((ty & 31) * 32 + (tx & 31)) * 2));
        uint32_t py = yy & 7;
        if (ent & 0x800) py = 7 - py;
        uint32_t tile = ent & 0x3FF;
        int flip = (ent & 0x400) != 0;
        /* one tile row: 8 (or 4) bytes, contiguous within a 16 KB VRAM page */
        const uint8_t* row = c256 ? vram_ptr(charbase + tile * 64 + py * 8) : vram_ptr(charbase + tile * 32 + py * 4);
        uint32_t palbase = c256 ? 0 : (ent >> 12) * 16;
        for (uint32_t px0 = xx & 7; px0 < 8 && x < 256; px0++, x++) {
            uint32_t px = flip ? 7 - px0 : px0;
            uint32_t idx;
            if (!row) idx = 0;
            else if (c256) idx = row[px];
            else idx = (row[px >> 1] >> ((px & 1) * 4)) & 15;
            if (!idx) { out->col[x] = 0; continue; }
            uint16_t col;
            if (c256) col = ext ? ext16(ext, (ent >> 12) * 256 + idx) : pal16(e->pal + idx * 2);
            else col = pal16(e->pal + (palbase + idx) * 2);
            out->col[x] = (col & 0x7FFF) | PX_OPAQUE;
        }
    }
}

/* Affine / extended / bitmap backgrounds (BG2, BG3). kind: 1 affine, 2 extended. */
static void render_affine_bg(Engine* e, int eng, int bg, int kind, BgLine* out) {
    uint32_t disp = R32(e, 0);
    uint16_t cnt = R16(e, 0x08 + bg * 2);
    int16_t pa = (int16_t)R16(e, 0x20 + (bg - 2) * 16), pc = (int16_t)R16(e, 0x24 + (bg - 2) * 16);
    int32_t rx = e->refx[bg - 2], ry = e->refy[bg - 2];
    int wrap = (cnt >> 13) & 1;
    int size = cnt >> 14;
    uint32_t charbase = e->bgvram + ((cnt >> 2) & 15) * 0x4000 + (eng == 0 ? ((disp >> 24) & 7) * 0x10000 : 0);
    uint32_t mapbase  = e->bgvram + ((cnt >> 8) & 31) * 0x800  + (eng == 0 ? ((disp >> 27) & 7) * 0x10000 : 0);
    int bitmap = kind == 2 && (cnt & 0x80);
    int direct = bitmap && (cnt & 0x04);
    uint32_t w, h;
    if (bitmap) {
        static const uint32_t bw[4] = { 128, 256, 512, 512 }, bh[4] = { 128, 256, 256, 512 };
        w = bw[size]; h = bh[size];
    } else {
        w = h = 128u << size;
    }
    const uint8_t* ext = NULL;
    if (kind == 2 && !bitmap && (disp & 0x40000000)) ext = ext_bg_pal(eng, bg);
    uint32_t bmbase = e->bgvram + ((cnt >> 8) & 31) * 0x4000;
    for (int x = 0; x < 256; x++, rx += pa, ry += pc) {
        int32_t tx = rx >> 8, ty = ry >> 8;
        if (wrap) { tx &= (int32_t)w - 1; ty &= (int32_t)h - 1; }
        else if (tx < 0 || ty < 0 || tx >= (int32_t)w || ty >= (int32_t)h) { out->col[x] = 0; continue; }
        uint16_t col;
        if (direct) {
            col = vr16(bmbase + (ty * w + tx) * 2);
            if (!(col & 0x8000)) { out->col[x] = 0; continue; }
        } else if (bitmap) {
            uint8_t idx = vr8(bmbase + ty * w + tx);
            if (!idx) { out->col[x] = 0; continue; }
            col = pal16(e->pal + idx * 2);
        } else if (kind == 2) {                                     /* 16-bit map entries, 256 colours */
            uint16_t ent = vr16(mapbase + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2);
            uint32_t px = tx & 7, py = ty & 7;
            if (ent & 0x400) px = 7 - px;
            if (ent & 0x800) py = 7 - py;
            uint8_t idx = vr8(charbase + (ent & 0x3FF) * 64 + py * 8 + px);
            if (!idx) { out->col[x] = 0; continue; }
            col = ext ? ext16(ext, (ent >> 12) * 256 + idx) : pal16(e->pal + idx * 2);
        } else {                                                    /* 8-bit map entries */
            uint8_t tile = vr8(mapbase + (ty >> 3) * (w >> 3) + (tx >> 3));
            uint8_t idx = vr8(charbase + tile * 64 + (ty & 7) * 8 + (tx & 7));
            if (!idx) { out->col[x] = 0; continue; }
            col = pal16(e->pal + idx * 2);
        }
        out->col[x] = (col & 0x7FFF) | PX_OPAQUE;
    }
}

/* ---------------------------------------------------------------- sprites */
static const uint8_t obj_w[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t obj_h[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static void render_objs(Engine* e, int eng, int y, ObjLine* out) {
    uint32_t disp = R32(e, 0);
    const uint8_t* oam = &g_oam[eng * 0x400];
    const uint8_t* ext = (disp & 0x80000000u) ? ext_obj_pal(eng) : NULL;
    memset(out->col, 0, sizeof out->col);
    memset(out->prio, 4, sizeof out->prio);
    memset(out->semi, 0, sizeof out->semi);
    memset(out->win, 0, sizeof out->win);
    for (int i = 0; i < 128; i++) {
        uint16_t a0, a1, a2;
        memcpy(&a0, oam + i * 8, 2); memcpy(&a1, oam + i * 8 + 2, 2); memcpy(&a2, oam + i * 8 + 4, 2);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) continue;                     /* hidden */
        int shape = (a0 >> 14) & 3, sz = (a1 >> 14) & 3;
        if (shape == 3) continue;
        int w = obj_w[shape][sz], h = obj_h[shape][sz];
        int bw = w, bh = h;                                         /* bounding box */
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int oy = a0 & 0xFF;
        int ly = (y - oy) & 0xFF;
        if (ly >= bh) continue;
        int ox = a1 & 0x1FF;
        if (ox >= 256) ox -= 512;
        int mode = (a0 >> 10) & 3;
        int c256 = (a0 >> 13) & 1;
        int prio = (a2 >> 10) & 3;
        uint32_t tile = a2 & 0x3FF;
        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            int p = (a1 >> 9) & 31;
            memcpy(&pa, oam + p * 32 + 6, 2); memcpy(&pb, oam + p * 32 + 14, 2);
            memcpy(&pc, oam + p * 32 + 22, 2); memcpy(&pd, oam + p * 32 + 30, 2);
        }
        for (int lx = 0; lx < bw; lx++) {
            int sx = ox + lx;
            if (sx < 0 || sx >= 256) continue;
            int tx, ty;
            if (affine) {
                int cx = lx - bw / 2, cy = ly - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            }
            uint16_t col;
            int opaque;
            if (mode == 3) {                                        /* bitmap sprite */
                uint32_t addr;
                if (disp & 0x40) addr = e->objvram + tile * (128u << ((disp >> 22) & 1)) + (ty * w + tx) * 2;
                else if (disp & 0x20) addr = e->objvram + ((tile & 0x1F) * 0x10 + (tile & ~0x1Fu) * 0x80) + (ty * 256 + tx) * 2;
                else addr = e->objvram + ((tile & 0x0F) * 0x10 + (tile & ~0x0Fu) * 0x80) + (ty * 128 + tx) * 2;
                col = vr16(addr);
                opaque = (col & 0x8000) && (a2 >> 12);
            } else {
                uint32_t base, rowstride;
                if (disp & 0x10) {                                  /* 1D mapping */
                    base = e->objvram + tile * (32u << ((disp >> 20) & 3));
                    rowstride = (w / 8) * (c256 ? 64 : 32);
                } else {
                    base = e->objvram + tile * 32;
                    rowstride = 32 * 32;
                }
                uint32_t addr = base + (ty / 8) * rowstride + (tx / 8) * (c256 ? 64 : 32);
                uint8_t idx;
                if (c256) idx = vr8(addr + (ty & 7) * 8 + (tx & 7));
                else { uint8_t b = vr8(addr + (ty & 7) * 4 + (tx & 7) / 2); idx = (tx & 1) ? b >> 4 : b & 15; }
                opaque = idx != 0;
                if (c256) col = ext ? ext16(ext, (a2 >> 12) * 256 + idx) : pal16(e->pal + 0x200 + idx * 2);
                else col = pal16(e->pal + 0x200 + ((a2 >> 12) * 16 + idx) * 2);
            }
            if (!opaque) continue;
            if (mode == 2) { out->win[sx] = 1; continue; }
            if (!(out->col[sx] & PX_OPAQUE) || prio < out->prio[sx]) {
                out->col[sx] = (col & 0x7FFF) | PX_OPAQUE;
                out->prio[sx] = (uint8_t)prio;
                out->semi[sx] = mode == 1;
            }
        }
    }
}

/* ---------------------------------------------------------------- composition */
static inline uint16_t blend(uint16_t a, uint16_t b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31; if (g > 31) g = 31; if (bl > 31) bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}
static inline uint16_t brighten(uint16_t c, int evy) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}
static inline uint16_t darken(uint16_t c, int evy) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}
static inline uint32_t to_rgb(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

static void render_engine_line(int eng, int y) {
    Engine* e = &g_eng[eng];
    uint32_t* dst = &g_engine_out[eng][y * 256];
    uint32_t disp = R32(e, 0);
    int dmode = (disp >> 16) & 3;
    if (eng == 1) dmode &= 1;
    if (dmode == 0) { for (int x = 0; x < 256; x++) dst[x] = 0xFFFFFF; return; }
    if (dmode == 2) {                                               /* show a VRAM bank directly */
        int b = (disp >> 18) & 3;
        for (int x = 0; x < 256; x++) { uint16_t c; memcpy(&c, vram_bank(b) + (y * 256 + x) * 2, 2); dst[x] = to_rgb(c); }
        return;
    }
    if (disp & 0x80) { for (int x = 0; x < 256; x++) dst[x] = 0xFFFFFF; return; }   /* forced blank */

    int mode = disp & 7;
    static BgLine bgl[4];
    static ObjLine objl;
    int bg_on[4] = { 0 };
    int bg_3d = eng == 0 && (disp & 8);
    for (int bg = 0; bg < 4; bg++) {
        if (!((disp >> (8 + bg)) & 1)) continue;
        int kind;                                                   /* 0 text, 1 affine, 2 extended, 3 = 3D */
        if (bg < 2) kind = (bg == 0 && bg_3d) ? 3 : 0;
        else if (bg == 2) kind = mode <= 1 || mode == 3 ? 0 : (mode == 2 || mode == 4) ? 1 : 2;
        else kind = mode == 0 ? 0 : (mode <= 2) ? 1 : 2;
        if (bg == 2 && mode == 1) kind = 0;
        if (bg == 3 && mode == 1) kind = 1;
        if (mode == 6 && bg != 0 && bg != 2) continue;
        bg_on[bg] = 1;
        if (kind == 3) {
            for (int x = 0; x < 256; x++)
                bgl[0].col[x] = (g_3d_present && g_3d_alpha[x]) ? (uint16_t)((g_3d_line[x] & 0x7FFF) | PX_OPAQUE) : 0;
        } else if (kind == 0) render_text_bg(e, eng, bg, y, &bgl[bg]);
        else render_affine_bg(e, eng, bg, kind, &bgl[bg]);
    }
    int obj_on = (disp >> 12) & 1;
    if (obj_on) render_objs(e, eng, y, &objl);

    /* windows */
    uint8_t winmask[256];
    int win_any = (disp >> 13) & 7;
    uint16_t winin = R16(e, 0x48), winout = R16(e, 0x4A);
    if (!win_any) memset(winmask, 0x3F, sizeof winmask);
    else {
        for (int x = 0; x < 256; x++) winmask[x] = winout & 0x3F;
        if ((disp & 0x4000) && obj_on) for (int x = 0; x < 256; x++) if (objl.win[x]) winmask[x] = (winout >> 8) & 0x3F;
        for (int wi = 1; wi >= 0; wi--) {
            if (!((disp >> (13 + wi)) & 1)) continue;
            uint16_t wh = R16(e, 0x40 + wi * 2), wv = R16(e, 0x44 + wi * 2);
            int x1 = wh >> 8, x2 = wh & 0xFF, y1 = wv >> 8, y2 = wv & 0xFF;
            int iny = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
            if (!iny) continue;
            for (int x = 0; x < 256; x++) {
                int inx = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
                if (inx) winmask[x] = (winin >> (wi * 8)) & 0x3F;
            }
        }
    }

    uint16_t bldcnt = R16(e, 0x50), bldalpha = R16(e, 0x52), bldy = R16(e, 0x54);
    int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = bldy & 31;
    if (eva > 16) eva = 16; if (evb > 16) evb = 16; if (evy > 16) evy = 16;
    int effect = (bldcnt >> 6) & 3;
    uint16_t backdrop = pal16(e->pal) & 0x7FFF;
    uint16_t mb = R16(e, 0x6C);
    int mb_mode = mb >> 14, mb_f = mb & 31;
    if (mb_f > 16) mb_f = 16;

    /* drawing order for this line: per priority, sprites first, then BGs by number */
    int order[20], norder = 0;                                      /* 0-3 BG; 4+prio = sprites of that priority */
    for (int prio = 0; prio < 4; prio++) {
        if (obj_on) order[norder++] = 4 + prio;
        for (int bg = 0; bg < 4; bg++) if (bg_on[bg] && (R16(e, 0x08 + bg * 2) & 3) == prio) order[norder++] = bg;
    }
    for (int x = 0; x < 256; x++) {
        /* top two layers: layer ids 0-3 BG, 4 OBJ, 5 backdrop */
        int l1 = 5, l2 = 5;
        uint16_t c1 = backdrop, c2 = backdrop;
        uint8_t wm = winmask[x];
        for (int k = 0; k < norder; k++) {
            int L = order[k], id;
            uint16_t c;
            if (L >= 4) {
                if (!(wm & 0x10) || !(objl.col[x] & PX_OPAQUE) || objl.prio[x] != L - 4) continue;
                id = 4; c = objl.col[x];
            } else {
                if (!(wm & (1 << L)) || !(bgl[L].col[x] & PX_OPAQUE)) continue;
                id = L; c = bgl[L].col[x];
            }
            if (l1 == 5) { l1 = id; c1 = c; } else { l2 = id; c2 = c; break; }
        }
        uint16_t out = c1 & 0x7FFF;
        int fx = (wm & 0x20) != 0;
        int top_t1 = (bldcnt >> l1) & 1, sec_t2 = (bldcnt >> (8 + l2)) & 1;
        if (l1 == 4 && objl.semi[x] && sec_t2) out = blend(c1, c2, eva, evb);
        else if (l1 == 0 && bg_3d && g_3d_present && sec_t2 && g_3d_alpha[x] < 31) {
            int a = g_3d_alpha[x] + 1;                              /* 3D layer's own alpha */
            out = blend(c1, c2, a / 2, 16 - a / 2);
        } else if (fx && top_t1) {
            if (effect == 1 && sec_t2) out = blend(c1, c2, eva, evb);
            else if (effect == 2) out = brighten(c1 & 0x7FFF, evy);
            else if (effect == 3) out = darken(c1 & 0x7FFF, evy);
        }
        if (mb_mode == 1) out = brighten(out, mb_f);
        else if (mb_mode == 2) out = darken(out, mb_f);
        dst[x] = to_rgb(out);
    }
}

/* ---------------------------------------------------------------- frame flow */
static void latch_affine(int eng, int bg) {
    Engine* e = &g_eng[eng];
    uint32_t x = R32(e, 0x28 + (bg - 2) * 16), y = R32(e, 0x2C + (bg - 2) * 16);
    e->refx[bg - 2] = (int32_t)(x << 4) >> 4;                       /* 28-bit signed */
    e->refy[bg - 2] = (int32_t)(y << 4) >> 4;
}

void gpu_affine_written(int eng, int bg) { latch_affine(eng, bg); }

void gpu_frame_start(void) {
    for (int eng = 0; eng < 2; eng++) for (int bg = 2; bg < 4; bg++) latch_affine(eng, bg);
}

double g_t2d_ms;
#include <time.h>
#ifdef DQIX_TIMING
static double ms_now2(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#else
static double ms_now2(void) { return 0; }
#endif
void gpu_render_line_(int line);
void gpu_render_line(int line) { double t0 = ms_now2(); gpu_render_line_(line); g_t2d_ms += ms_now2() - t0; }
void gpu_render_line_(int line) {
    if (line == 0) gpu_frame_start();
    gpu3d_render_line(line);
    for (int eng = 0; eng < 2; eng++) {
        render_engine_line(eng, line);
        Engine* e = &g_eng[eng];
        for (int bg = 0; bg < 2; bg++) {                            /* advance affine reference points */
            e->refx[bg] += (int16_t)R16(e, 0x22 + bg * 16);
            e->refy[bg] += (int16_t)R16(e, 0x26 + bg * 16);
        }
    }
}

/* ---------------------------------------------------------------- output */
static char g_dump_dir[512];
static int g_dump_every;
static uint64_t g_last_dump;

static void write_ppm(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n256 384\n255\n");
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 256 * 192; i++) {
            uint32_t c = g_screen[s][i];
            uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

void gpu_frame_done(void) {
    uint16_t pow; memcpy(&pow, &g_io[0x304], 2);
    int a_top = (pow >> 15) & 1;
    if (!(pow & 1)) {                                               /* LCDs off */
        memset(g_screen, 0, sizeof g_screen);
    } else {
        memcpy(g_screen[a_top ? 0 : 1], g_engine_out[0], sizeof g_engine_out[0]);
        memcpy(g_screen[a_top ? 1 : 0], g_engine_out[1], sizeof g_engine_out[1]);
    }
    if (!g_dump_every) {
        const char* env = getenv("DQIX_DUMP");          /* "dir,every_n_frames" */
        if (env && *env) {
            const char* comma = strchr(env, ',');
            size_t n = comma ? (size_t)(comma - env) : strlen(env);
            if (n >= sizeof g_dump_dir) n = sizeof g_dump_dir - 1;
            memcpy(g_dump_dir, env, n); g_dump_dir[n] = 0;
            g_dump_every = comma ? atoi(comma + 1) : 60;
            if (g_dump_every <= 0) g_dump_every = 60;
        } else g_dump_every = -1;
    }
    if (g_dump_every > 0 && g_frame - g_last_dump >= (uint64_t)g_dump_every) {
        g_last_dump = g_frame;
        char path[700];
        snprintf(path, sizeof path, "%s/f%06llu.ppm", g_dump_dir, (unsigned long long)g_frame);
        write_ppm(path);
    }
}
