/* Input: buttons and the touch screen, from the frontend (keyboard, mouse,
 * gamepad) and/or a script, and a recording of everything pressed so a
 * session can be replayed exactly (reports, debugging).
 *
 * Script syntax (DQIX_INPUT="..." or a file named by DQIX_INPUT_FILE):
 *   frame:what:frames  separated by ';' or newlines.
 *   `what` is buttons and/or a touch joined by '+':
 *   A B SELECT START RIGHT LEFT UP DOWN R L X Y touch=X,Y (bottom-screen pixels)
 *   e.g. "1800:START:6;2000:touch=128,175:6;2200:A+touch=20,30:6".
 * The recording uses the same syntax. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"

typedef struct { uint64_t frame; uint32_t keys; int touch; int tx, ty; uint32_t len; } Press;

static Press* g_script;
static int g_nscript, g_cap, g_head, g_loaded;

uint16_t g_keys_down;          /* bit set = pressed, KEYINPUT order (bits 0-9) + X=10, Y=11 */
int g_touch_down, g_touch_x, g_touch_y;
/* set by the frontend (keyboard / mouse / gamepad) */
uint16_t g_fe_keys;
int g_fe_touch, g_fe_tx, g_fe_ty;

static const char* const k_names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y" };

static uint32_t key_bit(const char* s, size_t n) {
    for (int i = 0; i < 12; i++) if (strlen(k_names[i]) == n && !strncmp(k_names[i], s, n)) return 1u << i;
    return 0;
}

static void push(Press** arr, int* n, int* cap, Press p) {
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *arr = realloc(*arr, sizeof(Press) * (size_t)*cap); }
    (*arr)[(*n)++] = p;
}

static void parse_script(const char* text) {
    char* buf = strdup(text);
    for (char* tok = strtok(buf, ";\r\n"); tok; tok = strtok(NULL, ";\r\n")) {
        Press p = { 0 };
        char what[160] = { 0 };
        unsigned long long fr; unsigned len = 6;
        if (sscanf(tok, "%llu:%159[^:]:%u", &fr, what, &len) < 2) continue;
        p.frame = fr; p.len = len;
        for (char* k = what; *k;) {
            char* plus = strchr(k, '+');
            size_t n = plus ? (size_t)(plus - k) : strlen(k);
            if (n > 6 && !strncmp(k, "touch=", 6)) { p.touch = 1; sscanf(k + 6, "%d,%d", &p.tx, &p.ty); }
            else p.keys |= key_bit(k, n);
            k += n + (plus ? 1 : 0);
        }
        push(&g_script, &g_nscript, &g_cap, p);
    }
    free(buf);
}

static int cmp_press(const void* a, const void* b) {
    const Press* x = a; const Press* y = b;
    return x->frame < y->frame ? -1 : x->frame > y->frame ? 1 : 0;
}

static void load_script(void) {
    g_loaded = 1;
    const char* env = getenv("DQIX_INPUT");
    if (env) parse_script(env);
    const char* path = getenv("DQIX_INPUT_FILE");
    if (path) {
        FILE* f = fopen(path, "rb");
        if (!f) { LOG_WARN("input: cannot open %s\n", path); }
        else {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            char* text = malloc((size_t)n + 1);
            size_t got = fread(text, 1, (size_t)n, f); text[got] = 0;
            fclose(f);
            parse_script(text);
            free(text);
        }
    }
    if (g_nscript) qsort(g_script, (size_t)g_nscript, sizeof(Press), cmp_press);
}

/* ---------------------------------------------------------------- recording */
static Press* g_rec;
static int g_nrec, g_rec_cap;
static Press g_cur;                    /* the segment being recorded */
static int g_cur_open;

static int same_state(const Press* p, uint32_t keys, int touch, int tx, int ty) {
    return p->keys == keys && p->touch == touch && (!touch || (p->tx == tx && p->ty == ty));
}

static void record(uint32_t keys, int touch, int tx, int ty) {
    if (g_cur_open && same_state(&g_cur, keys, touch, tx, ty)) { g_cur.len++; return; }
    if (g_cur_open) push(&g_rec, &g_nrec, &g_rec_cap, g_cur);
    g_cur_open = keys || touch;
    if (g_cur_open) { g_cur.frame = g_frame; g_cur.keys = keys; g_cur.touch = touch; g_cur.tx = tx; g_cur.ty = ty; g_cur.len = 1; }
}

static void write_press(FILE* f, const Press* p) {
    fprintf(f, "%llu:", (unsigned long long)p->frame);
    int first = 1;
    for (int i = 0; i < 12; i++) if (p->keys & (1u << i)) { fprintf(f, "%s%s", first ? "" : "+", k_names[i]); first = 0; }
    if (p->touch) fprintf(f, "%stouch=%d,%d", first ? "" : "+", p->tx, p->ty);
    fprintf(f, ":%u\n", p->len);
}

/* Everything pressed since launch, in script syntax. */
void input_write_recording(FILE* f) {
    for (int i = 0; i < g_nrec; i++) write_press(f, &g_rec[i]);
    if (g_cur_open) write_press(f, &g_cur);
}

/* ---------------------------------------------------------------- per frame */
/* Called once per frame (VBlank). */
void input_update(void) {
    if (!g_loaded) load_script();
    g_keys_down = 0; g_touch_down = 0;
    while (g_head < g_nscript && g_script[g_head].frame + g_script[g_head].len <= g_frame) g_head++;
    for (int i = g_head; i < g_nscript && g_script[i].frame <= g_frame; i++) {
        Press* p = &g_script[i];
        if (g_frame < p->frame + p->len) {
            g_keys_down |= (uint16_t)p->keys;
            if (p->touch) { g_touch_down = 1; g_touch_x = p->tx; g_touch_y = p->ty; }
        }
    }
    g_keys_down |= g_fe_keys;
    if (g_fe_touch) { g_touch_down = 1; g_touch_x = g_fe_tx; g_touch_y = g_fe_ty; }
    record(g_keys_down, g_touch_down, g_touch_x, g_touch_y);
}

/* Register views (active-low). */
uint16_t input_keyinput(void) { return (uint16_t)(~g_keys_down & 0x3FF); }
uint16_t input_extkeyin(void) {
    uint16_t v = 0x7F;
    if (g_keys_down & (1 << 10)) v &= ~1;            /* X */
    if (g_keys_down & (1 << 11)) v &= ~2;            /* Y */
    if (g_touch_down) v &= ~0x40;                     /* pen down */
    return v;
}

/* Touch screen ADC values, using the calibration in our firmware image:
 * ADC 0x200 -> pixel 0x20, ADC 0xE00 -> pixel 0xE0 (x) / 0xA0 (y). */
uint16_t input_touch_adc(int channel) {
    if (!g_touch_down) return channel == 1 || channel == 5 ? 0 : 0x800;
    /* invert the firmware's touch calibration (two ADC -> pixel points) */
    const uint8_t* u = firmware_user_settings();
    int ax1 = u[0x58] | u[0x59] << 8, ay1 = u[0x5A] | u[0x5B] << 8, px1 = u[0x5C], py1 = u[0x5D];
    int ax2 = u[0x5E] | u[0x5F] << 8, ay2 = u[0x60] | u[0x61] << 8, px2 = u[0x62], py2 = u[0x63];
    if (channel == 5) return (uint16_t)(ax1 + (g_touch_x - px1) * (ax2 - ax1) / (px2 - px1));
    if (channel == 1) return (uint16_t)(ay1 + (g_touch_y - py1) * (ay2 - ay1) / (py2 - py1));
    if (channel == 3) return 0x100;                   /* Z1 pressure */
    if (channel == 4) return 0xF00;                   /* Z2 */
    return 0x800;
}
