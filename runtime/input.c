/* Input: buttons and the touch screen, driven by a script for now.
 *
 * DQIX_INPUT="frame:what:frames;..." where `what` is a button list joined by
 * '+' (A B SELECT START RIGHT LEFT UP DOWN R L X Y) or touch=X,Y (bottom
 * screen pixels). Example: "1800:START:6;2000:touch=128,175:6;2200:A:6". */
#include <stdlib.h>
#include <string.h>
#include "nds.h"

typedef struct { uint64_t frame; uint32_t keys; int touch; int tx, ty; uint32_t len; } Press;
static Press g_script[256];
static int g_nscript, g_loaded;

uint16_t g_keys_down;          /* bit set = pressed, KEYINPUT order (bits 0-9) + X=10, Y=11 */
int g_touch_down, g_touch_x, g_touch_y;
/* set by the frontend (keyboard / mouse / gamepad) */
uint16_t g_fe_keys;
int g_fe_touch, g_fe_tx, g_fe_ty;

static uint32_t key_bit(const char* s, size_t n) {
    static const char* names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y" };
    for (int i = 0; i < 12; i++) if (strlen(names[i]) == n && !strncmp(names[i], s, n)) return 1u << i;
    return 0;
}

static void load_script(void) {
    g_loaded = 1;
    const char* env = getenv("DQIX_INPUT");
    if (!env) return;
    char* buf = strdup(env);
    for (char* tok = strtok(buf, ";"); tok && g_nscript < 256; tok = strtok(NULL, ";")) {
        Press p = { 0 };
        char what[128] = { 0 };
        unsigned long long fr; unsigned len = 6;
        if (sscanf(tok, "%llu:%127[^:]:%u", &fr, what, &len) < 2) continue;
        p.frame = fr; p.len = len;
        if (!strncmp(what, "touch=", 6)) { p.touch = 1; sscanf(what + 6, "%d,%d", &p.tx, &p.ty); }
        else {
            for (char* k = what; *k;) {
                char* plus = strchr(k, '+');
                size_t n = plus ? (size_t)(plus - k) : strlen(k);
                p.keys |= key_bit(k, n);
                k += n + (plus ? 1 : 0);
            }
        }
        g_script[g_nscript++] = p;
    }
    free(buf);
}

/* Called once per frame (VBlank). */
void input_update(void) {
    if (!g_loaded) load_script();
    g_keys_down = 0; g_touch_down = 0;
    for (int i = 0; i < g_nscript; i++) {
        Press* p = &g_script[i];
        if (g_frame >= p->frame && g_frame < p->frame + p->len) {
            g_keys_down |= (uint16_t)p->keys;
            if (p->touch) { g_touch_down = 1; g_touch_x = p->tx; g_touch_y = p->ty; }
        }
    }
    g_keys_down |= g_fe_keys;
    if (g_fe_touch) { g_touch_down = 1; g_touch_x = g_fe_tx; g_touch_y = g_fe_ty; }
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
    if (channel == 5) return (uint16_t)(0x200 + (g_touch_x - 0x20) * (0xE00 - 0x200) / (0xE0 - 0x20));
    if (channel == 1) return (uint16_t)(0x200 + (g_touch_y - 0x20) * (0xE00 - 0x200) / (0xA0 - 0x20));
    if (channel == 3) return 0x100;                   /* Z1 pressure */
    if (channel == 4) return 0xF00;                   /* Z2 */
    return 0x800;
}
