/* SDL2 frontend: a window with both screens, keyboard / mouse / gamepad
 * input, audio output and pacing to the DS frame rate (~59.83 fps).
 *
 * Keys (DS button <- keyboard):
 *   D-pad <- arrow keys     A <- X      B <- Z      X <- S      Y <- A
 *   L <- Q                  R <- W      START <- Enter         SELECT <- Right Shift / Backspace
 *   touch screen <- left mouse button on the bottom screen
 *   Tab (hold) = fast forward     F11 or Alt+Enter = fullscreen (mouse at the top edge shows the menu bar)
 *   F2 = screens side by side / stacked
 *   F3 = window size 1x..4x       F4 = 3D resolution 1x..4x (Enhanced)  F5 = Classic / Enhanced
 *   F6 = DS Lite screen filter    F7 = DS Lite console around the screens   P = pause
 *   F8 = actual size (the screens as big as a real DS Lite's)
 *   F12 = screenshot (dqix-shot-N.bmp)
 *   F10 = bug report (reports/<date-time>/: screenshot, inputs, saves, log)
 *   Esc = leave fullscreen, or quit (the save file is always up to date)
 * On Windows every setting is also in the menu bar (menu_win32 below).
 * A game controller works too (Nintendo layout by position: right face button = A). */
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "nds.h"
#include "frontend.h"
#include "gl_render.h"
#include "menu.h"
#include "version.h"
#include "session.h"
#include "air.h"
#include "update.h"
#include "text.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define DS_FPS 59.8261

static SDL_Window* g_win;
static SDL_Renderer* g_ren;
static SDL_Texture* g_tex;
static SDL_AudioDeviceID g_audio;
static SDL_GameController* g_pad;
static int g_side_by_side, g_scale = 2, g_fullscreen;
static uint64_t g_next_frame, g_perf_freq;
static int g_fast;
static uint32_t g_fps_frames; static uint64_t g_fps_t0;
static int g_headless;          /* DQIX_NOWINDOW: run without a window (still uses SDL audio off) */
static int g_hidden;            /* another player's console in a session: no window, no sound, no pacing */
static int g_gl;                /* OpenGL renderer active (else SDL_Renderer + software 3D) */
static SDL_GLContext g_glctx;
static int g_scale3d = 2;
extern void (*g_report_capture)(uint32_t out[2][256 * 192]);
extern void (*g_report_perf)(FILE* f);
static void perf_write(FILE* f);
static int g_classic = 1;       /* Classic: the DS's own 3D renderer, exact; Enhanced: 3D on the GPU at 1-4x */
extern int g_comp_gl;
static int g_paused, g_turbo;   /* Game > Pause, Game > Fast forward (latched) */

/* How the screens are shown (View and Screen menus). The DS Lite parts need OpenGL. */
static struct {
    int lcd;                     /* DS Lite screen filter */
    int lcd_color, grid, ghost;  /* ... its LCD colours, pixel grid, motion blur */
    int backlight;               /* 1 (low) .. 4 (max) */
    int gap;                     /* the real distance between the screens */
    int shell;                   /* draw the console around the screens */
    int shell_color;             /* index into SHELLS */
    int integer;                 /* whole-number scaling only */
    int actual;                  /* screens as big as a real DS Lite's */
    float monitor_width_mm;      /* settings.ini override for the monitor's width (0 = ask the monitor) */
    int dlc;                     /* switch on the downloadable quests and special guests when the save loads */
} g_opt = { 0, 1, 1, 1, 4, 0, 0, 0, 0, 0, 0, 0 };

/* Actual size: a DS Lite pixel is 0.238 mm (a 3-inch 4:3 screen, 256 pixels
 * across = 60.96 mm). Window pixels per DS pixel on the current monitor. */
#define DSLITE_MM_PER_PIXEL (60.96f / 256.0f)
static float g_actual_sc;        /* 0 = monitor size unknown */
static void update_actual(void) {
    g_actual_sc = 0;
    if (!g_win) return;
    float mm = 0;
    if (g_opt.monitor_width_mm > 0) {
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(g_win), &dm) == 0 && dm.w > 0) mm = g_opt.monitor_width_mm / (float)dm.w;
    } else mm = display_mm_per_pixel(g_win);
    if (mm > 0) g_actual_sc = DSLITE_MM_PER_PIXEL / mm;
}

/* DS Lite colours: lid, base, buttons, hinge (sRGB). */
typedef struct { const char* name; const char* key; uint32_t lid, base, btn, hinge; } Shell;
static const Shell SHELLS[] = {
    { "Polar White", "white", 0xECECEA, 0xECECEA, 0xDCDCDA, 0xCFCFCC },
    { "Jet Black", "black", 0x222225, 0x222225, 0x2E2E32, 0x161618 },
    { "Ice Blue", "ice_blue", 0xBCD6E6, 0xBCD6E6, 0xE4EDF2, 0xA6C0D0 },
    { "Coral Pink", "coral_pink", 0xF0B7C1, 0xF0B7C1, 0xF6E1E5, 0xDBA0AB },
    { "Enamel Navy", "navy", 0x24314F, 0x24314F, 0x313E5F, 0x1A2440 },
    { "Crimson / Black", "crimson", 0xB21F2B, 0x222225, 0x2E2E32, 0x161618 },
};
#define N_DRAWN ((int)(sizeof SHELLS / sizeof SHELLS[0]))

/* Console photos: skins/<name>.png with skins/<name>.txt giving where the
 * screens are in the picture ("top x y w h", "bottom x y w h", optional
 * "name ..."). Listed after the drawn colours. */
#define MAX_SKINS 10
typedef struct { char file[200], name[64]; float top[4], bot[4]; int w, h; } Skin;
static Skin g_skins[MAX_SKINS];
static int g_nskins;
static int g_skin_loaded = -1;                 /* which photo is in the GPU */
#define N_SHELLS (N_DRAWN + g_nskins)

static void skin_add(const char* png) {
    if (g_nskins >= MAX_SKINS) return;
    Skin* k = &g_skins[g_nskins];
    memset(k, 0, sizeof *k);
    snprintf(k->file, sizeof k->file, "skins/%s", png);
    char txt[220]; snprintf(txt, sizeof txt, "%s", k->file);
    char* dot = strrchr(txt, '.'); if (!dot) return;
    strcpy(dot, ".txt");
    FILE* f = fopen(txt, "r");
    if (!f) return;
    char line[256]; int got = 0;
    snprintf(k->name, sizeof k->name, "%.*s (photo)", (int)(strrchr(png, '.') - png), png);
    while (fgets(line, sizeof line, f)) {
        float a, b, c, d;
        if (sscanf(line, " top %f %f %f %f", &a, &b, &c, &d) == 4) { k->top[0] = a; k->top[1] = b; k->top[2] = c; k->top[3] = d; got |= 1; }
        else if (sscanf(line, " bottom %f %f %f %f", &a, &b, &c, &d) == 4) { k->bot[0] = a; k->bot[1] = b; k->bot[2] = c; k->bot[3] = d; got |= 2; }
        else if (!strncmp(line, "name ", 5)) { snprintf(k->name, sizeof k->name, "%s", line + 5); k->name[strcspn(k->name, "\r\n")] = 0; }
    }
    fclose(f);
    if (got == 3 && k->top[2] > 0) g_nskins++;
}

#ifdef _WIN32
#include <io.h>
static void skins_scan(void) {
    struct _finddata_t fd; intptr_t h = _findfirst("skins/*.png", &fd);
    if (h == -1) return;
    do skin_add(fd.name); while (_findnext(h, &fd) == 0);
    _findclose(h);
}
#else
#include <dirent.h>
static void skins_scan(void) {
    DIR* d = opendir("skins");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) { size_t n = strlen(e->d_name); if (n > 4 && !strcmp(e->d_name + n - 4, ".png")) skin_add(e->d_name); }
    closedir(d);
}
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "stb_image.h"

/* The photo for the chosen console colour (if it is one), in the GPU. */
static const Skin* current_skin(void) {
    int i = g_opt.shell_color - N_DRAWN;
    if (i < 0 || i >= g_nskins || !g_gl) return NULL;
    if (g_skin_loaded != i) {
        int w, h, n;
        uint8_t* px = stbi_load(g_skins[i].file, &w, &h, &n, 4);
        if (!px) { fprintf(stderr, "can't load %s\n", g_skins[i].file); return NULL; }
        gl_set_skin(px, w, h);
        stbi_image_free(px);
        g_skins[i].w = w; g_skins[i].h = h;
        g_skin_loaded = i;
    }
    return &g_skins[i];
}
static const float BACKLIGHT[5] = { 0, 0.36f, 0.56f, 0.78f, 1.0f };
static const char* BACKLIGHT_NAME[5] = { "", "low", "medium", "high", "max" };

/* Canvas (in DS pixels) for the current layout, and where the screens sit on it.
 * The DS Lite: 0.24 mm per pixel; the lid and base are 133 mm wide; the
 * screens are 90 pixels (21.5 mm) apart. */
static void canvas_layout(GlPresent* v, int ww, int wh) {
    memset(v, 0, sizeof *v);
    int shell = g_gl && g_opt.shell && !g_side_by_side;
    if (g_side_by_side) {
        v->cw = 512; v->ch = 192;
        v->top[0] = 0; v->bot[0] = 256; v->top[1] = v->bot[1] = 0;
    } else if (shell && current_skin()) {
        /* the photo: canvas units are DS pixels at the photo's top-screen scale */
        const Skin* k = current_skin();
        float kk = k->top[2] / 256.0f;
        v->cw = (float)k->w / kk; v->ch = (float)k->h / kk;
        v->top[0] = k->top[0] / kk; v->top[1] = k->top[1] / kk;
        v->bot[0] = k->bot[0] / kk; v->bot[1] = k->bot[1] / kk;
        v->skin = 1; v->skin_k = kk;
    } else if (shell) {
        v->cw = 560; v->ch = 630;
        v->top[0] = v->bot[0] = 152; v->top[1] = 78; v->bot[1] = 78 + 192 + 90;
    } else {
        int gap = g_gl && g_opt.gap ? 90 : 0;
        v->cw = 256; v->ch = (float)(384 + gap);
        v->top[0] = v->bot[0] = 0; v->top[1] = 0; v->bot[1] = (float)(192 + gap);
    }
    v->top[2] = v->bot[2] = 256; v->top[3] = v->bot[3] = 192;
    v->win_w = ww; v->win_h = wh;
    float sc = fminf((float)ww / v->cw, (float)wh / v->ch);
    if (g_opt.actual && g_actual_sc > 0) { if (g_actual_sc < sc) sc = g_actual_sc; }   /* real size (smaller if it doesn't fit) */
    else if (g_opt.integer && sc >= 1.0f) sc = floorf(sc);
    v->sc = sc;
    v->ox = floorf(((float)ww - v->cw * sc) / 2); v->oy = floorf(((float)wh - v->ch * sc) / 2);
    v->shell = shell;
    v->lcd = g_gl && g_opt.lcd;
    v->lcd_color = g_opt.lcd_color; v->grid = g_opt.grid; v->ghost = g_opt.ghost;
    v->backlight = BACKLIGHT[g_opt.backlight];
    const Shell* sh = &SHELLS[g_opt.shell_color < N_DRAWN ? g_opt.shell_color : 1];
    uint32_t cols[4] = { sh->lid, sh->base, sh->btn, sh->hinge };
    float* dst[4] = { v->lid, v->base, v->btn, v->hinge };
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) dst[i][k] = (float)((cols[i] >> (16 - 8 * k)) & 255) / 255.0f;
}

/* ---------------------------------------------------------------- settings.ini
 * Remembers the player's choices between launches (next to the game). */
static const char* SETTINGS = "settings.ini";

/* Wireless: the community servers that replace Nintendo Wi-Fi Connection.
 * The DS finds a server through its DNS setting (firmware_set_ap), exactly
 * as players set it up by hand on a real DS. */
static const struct { const char* key; const char* dns; } WFC[] = {
    { "builtin", "10.64.0.2" },          /* wfc.c answers; the DNS is the virtual network's */
    { "altwfc", "172.104.88.237" },
    { "wiimmfi", "164.132.44.106" },
};
static char g_wfc_server[64] = "builtin";       /* a key above or a DNS address */
static char g_player_name[32] = "";              /* sessions: what friends see */
static int g_session_port = SESSION_PORT;
static int g_update_auto = 1;                    /* look for a new stable version at start */
static char g_update_skip[32];                   /* ...but not this one (the player said so) */
static int g_net_want;                         /* the menu's "connect" check */
static const char* wfc_dns(void) {
    for (unsigned i = 0; i < sizeof WFC / sizeof WFC[0]; i++) if (!strcmp(g_wfc_server, WFC[i].key)) return WFC[i].dns;
    return g_wfc_server;
}
static int wfc_custom(void) {
    for (unsigned i = 0; i < sizeof WFC / sizeof WFC[0]; i++) if (!strcmp(g_wfc_server, WFC[i].key)) return 0;
    return 1;
}
static void wireless_apply(int boot) {
    g_wireless.online = g_net_want;
    g_wireless.builtin = !strcmp(g_wfc_server, "builtin");
    snprintf(g_wireless.dns, sizeof g_wireless.dns, "%s", wfc_dns());
    if (!boot) {
        firmware_set_ap(g_wireless.online, g_wireless.dns); net_enable(g_wireless.online);
        wfc_enable(g_wireless.online && g_wireless.builtin, NULL);
    }
}
/* every console has its own MAC address, and the servers tell players apart
 * by it: one is made up the first time (Nintendo's prefix, random rest) */
static void wireless_mac_new(void) {
    uint32_t r = (uint32_t)time(NULL) ^ (uint32_t)SDL_GetPerformanceCounter() ^ (uint32_t)(uintptr_t)&r;
    for (int i = 0; i < 8; i++) r = r * 1103515245u + 12345u;
    g_wireless.mac[0] = 0x00; g_wireless.mac[1] = 0x09; g_wireless.mac[2] = 0xBF;
    for (int i = 3; i < 6; i++) { r = r * 1103515245u + 12345u; g_wireless.mac[i] = (uint8_t)(r >> 16); }
    g_wireless.mac_set = 1;
}
/* the console around the screens until the player picks another: the photo of a real DS Lite */
static int g_color_chosen;
static void default_console_color(void) {
    for (int i = 0; i < g_nskins; i++) if (!strcmp(g_skins[i].file + 6, "dslite_black.png")) g_opt.shell_color = N_DRAWN + i;
}
static void settings_load(void) {
    FILE* f = fopen(SETTINGS, "r");
    if (!f) return;
    char line[200], color[64] = "";
    while (fgets(line, sizeof line, f)) {
        char key[64]; char val[64];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
        if (!strcmp(key, "player_name")) {                       /* the whole rest of the line (may have spaces) */
            const char* v = strchr(line, '=') + 1; while (*v == ' ') v++;
            snprintf(g_player_name, sizeof g_player_name, "%s", v);
            g_player_name[strcspn(g_player_name, "\r\n")] = 0;
            continue;
        }
        if (!strcmp(key, "update_check")) { g_update_auto = atoi(val) != 0; continue; }
        if (!strcmp(key, "update_skip")) { snprintf(g_update_skip, sizeof g_update_skip, "%s", val); continue; }
        if (!strcmp(key, "session_port")) { int v = atoi(val); if (v > 0 && v < 65536) g_session_port = v; continue; }
        if (!strcmp(key, "session_consoles")) { if (!strcmp(val, "processes")) { static char e[] = "DQIX_PROCESSES=1"; putenv(e); } continue; }
        if (!strcmp(key, "mode")) g_classic = strcmp(val, "enhanced") != 0;
        else if (!strcmp(key, "resolution_3d")) { int v = atoi(val); if (v >= 1 && v <= 4) g_scale3d = v; }
        else if (!strcmp(key, "window_scale")) { int v = atoi(val); if (v >= 1 && v <= 8) g_scale = v; }
        else if (!strcmp(key, "layout")) g_side_by_side = !strcmp(val, "side_by_side");
        else if (!strcmp(key, "fullscreen")) g_fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "dslite_filter")) g_opt.lcd = atoi(val) != 0;
        else if (!strcmp(key, "lcd_colors")) g_opt.lcd_color = atoi(val) != 0;
        else if (!strcmp(key, "pixel_grid")) g_opt.grid = atoi(val) != 0;
        else if (!strcmp(key, "motion_blur")) g_opt.ghost = atoi(val) != 0;
        else if (!strcmp(key, "backlight")) { for (int i = 1; i <= 4; i++) if (!strcmp(val, BACKLIGHT_NAME[i])) g_opt.backlight = i; }
        else if (!strcmp(key, "screen_gap")) g_opt.gap = atoi(val) != 0;
        else if (!strcmp(key, "console")) g_opt.shell = atoi(val) != 0;
        else if (!strcmp(key, "console_color")) snprintf(color, sizeof color, "%s", val);
        else if (!strcmp(key, "console_color_chosen")) g_color_chosen = atoi(val) != 0;
        else if (!strcmp(key, "integer_scaling")) g_opt.integer = atoi(val) != 0;
        else if (!strcmp(key, "actual_size")) g_opt.actual = atoi(val) != 0;
        else if (!strcmp(key, "monitor_width_mm")) g_opt.monitor_width_mm = (float)atof(val);
        else if (!strcmp(key, "downloadable_content")) g_opt.dlc = atoi(val) != 0;
        else if (!strcmp(key, "wireless")) g_net_want = atoi(val) != 0;
        else if (!strcmp(key, "wfc_server")) snprintf(g_wfc_server, sizeof g_wfc_server, "%s", val);
        else if (!strcmp(key, "wireless_mac")) {
            unsigned b[6];
            if (sscanf(val, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
                for (int i = 0; i < 6; i++) g_wireless.mac[i] = (uint8_t)b[i];
                g_wireless.mac_set = 1;
            }
        }
    }
    fclose(f);
    /* the colour counts once the player picked one (before nightly5 it was saved either way) */
    if (g_color_chosen) {
        for (int i = 0; i < N_DRAWN; i++) if (!strcmp(color, SHELLS[i].key)) g_opt.shell_color = i;
        for (int i = 0; i < g_nskins; i++) if (!strcmp(color, g_skins[i].file + 6)) g_opt.shell_color = N_DRAWN + i;   /* a photo, by file name */
    }
}
static void settings_save(void) {
    if (g_headless || getenv("DQIX_NO_SETTINGS")) return;
    FILE* f = fopen(SETTINGS, "w");
    if (!f) return;
    fprintf(f, "# Dragon Quest IX (PC) settings. The game saves this when you change something.\n");
    fprintf(f, "# mode: classic (exactly like the DS) or enhanced (sharper 3D on the graphics card). F5 switches.\n");
    fprintf(f, "mode = %s\n", g_classic ? "classic" : "enhanced");
    fprintf(f, "# 3D resolution in enhanced mode: 1 to 4 (F4)\n");
    fprintf(f, "resolution_3d = %d\n", g_scale3d);
    fprintf(f, "# window size: 1 to 8 times the DS screens (F3)\n");
    fprintf(f, "window_scale = %d\n", g_scale);
    fprintf(f, "# stacked or side_by_side (F2)\n");
    fprintf(f, "layout = %s\n", g_side_by_side ? "side_by_side" : "stacked");
    fprintf(f, "# 0 or 1 (F11)\n");
    fprintf(f, "fullscreen = %d\n", g_fullscreen);
    fprintf(f, "# DS Lite screen filter (F6) and its parts: 0 or 1; backlight: low, medium, high, max\n");
    fprintf(f, "dslite_filter = %d\n", g_opt.lcd);
    fprintf(f, "lcd_colors = %d\n", g_opt.lcd_color);
    fprintf(f, "pixel_grid = %d\n", g_opt.grid);
    fprintf(f, "motion_blur = %d\n", g_opt.ghost);
    fprintf(f, "backlight = %s\n", BACKLIGHT_NAME[g_opt.backlight]);
    fprintf(f, "# the real gap between the screens; the DS Lite drawn around them (F7) and its colour:\n");
    fprintf(f, "# white, black, ice_blue, coral_pink, navy, crimson, or a photo in the skins folder by file name\n");
    fprintf(f, "screen_gap = %d\n", g_opt.gap);
    fprintf(f, "console = %d\n", g_opt.shell);
    fprintf(f, "console_color_chosen = %d\n", g_color_chosen);
    fprintf(f, "console_color = %s\n", g_opt.shell_color < N_DRAWN ? SHELLS[g_opt.shell_color].key : g_skins[g_opt.shell_color - N_DRAWN].file + 6);
    fprintf(f, "# whole-number scaling only (every DS pixel the same size): 0 or 1\n");
    fprintf(f, "integer_scaling = %d\n", g_opt.integer);
    fprintf(f, "# screens as big as a real DS Lite's (F8): 0 or 1. The size of your monitor is read from the\n");
    fprintf(f, "# monitor itself; if it comes out wrong, put its visible width in millimetres here (0 = automatic)\n");
    fprintf(f, "actual_size = %d\n", g_opt.actual);
    fprintf(f, "monitor_width_mm = %g\n", g_opt.monitor_width_mm);
    fprintf(f, "# switch on every downloadable quest and special guest at start, without connecting: 0 or 1\n");
    fprintf(f, "downloadable_content = %d\n", g_opt.dlc);
    fprintf(f, "# Wireless menu: connect to a community Nintendo Wi-Fi Connection server: 0 or 1\n");
    fprintf(f, "wireless = %d\n", g_net_want);
    fprintf(f, "# which server: builtin (offline, the files in the wfc folder), altwfc, wiimmfi, or another's DNS address\n");
    fprintf(f, "wfc_server = %s\n", g_wfc_server);
    fprintf(f, "# playing with friends far away: your name, and the port friends connect to when you host\n");
    fprintf(f, "player_name = %s\n", g_player_name);
    fprintf(f, "# updates: look for a new stable version when the game starts (Help menu); a version not to offer again\n");
    fprintf(f, "update_check = %d\n", g_update_auto);
    fprintf(f, "update_skip = %s\n", g_update_skip[0] ? g_update_skip : "-");
    fprintf(f, "session_port = %d\n", g_session_port);
    fprintf(f, "# how your friends' DSes run on this PC: thread (one thread, the usual) or processes (if thread misbehaves)\n");
    fprintf(f, "session_consoles = %s\n", getenv("DQIX_PROCESSES") ? "processes" : "thread");
    fprintf(f, "# this copy's own MAC address (made up the first time; the servers know you by it)\n");
    if (g_wireless.mac_set)
        fprintf(f, "wireless_mac = %02x:%02x:%02x:%02x:%02x:%02x\n", g_wireless.mac[0], g_wireless.mac[1],
                g_wireless.mac[2], g_wireless.mac[3], g_wireless.mac[4], g_wireless.mac[5]);
    fclose(f);
}

/* Classic or Enhanced: which 3D renderer runs. Without OpenGL 3.3 only Classic exists. */
static void apply_mode(void) {
    if (!g_gl) g_classic = 1;
    if (g_classic) {
        g_renderer3d = NULL; g_comp_gl = 0; g_report_capture = NULL;
    } else {
        g_renderer3d = gl_renderer3d(); g_comp_gl = 1; g_report_capture = gl_read_screens;
    }
}

/* Audio drives the pacing: the game runs ~100 ms ahead of what is being
 * heard, so a slow frame only eats into that cushion instead of causing a
 * dropout. If the buffer still runs dry, output pauses until it refills
 * (a clean gap rather than stutter). */
#define AUDIO_TARGET (32768u / 10)        /* stereo frames queued ahead (~100 ms) */
static volatile int g_audio_priming = 1;

static void audio_cb(void* u, Uint8* stream, int len) {
    (void)u;
    int16_t* out = (int16_t*)stream;
    uint32_t frames = (uint32_t)len / 4;
    if (g_audio_priming) {
        if (sound_buffered() < AUDIO_TARGET / 2) { memset(stream, 0, (size_t)len); return; }
        g_audio_priming = 0;
    }
    uint32_t got = sound_read(out, frames);
    if (got < frames) {                                   /* ran dry: fade the tail, then wait to refill */
        int16_t l = got ? out[(got - 1) * 2] : 0, r = got ? out[(got - 1) * 2 + 1] : 0;
        for (uint32_t i = got; i < frames; i++) {
            int k = (int)(frames - i);
            out[i * 2] = (int16_t)(l * k / (int)(frames - got + 1)); out[i * 2 + 1] = (int16_t)(r * k / (int)(frames - got + 1));
        }
        g_audio_priming = 1;
    }
}

/* Window size = canvas x scale, made smaller if it would not fit on the screen. */
static void menu_sync(void);
static void layout_window(void) {
    GlPresent v; canvas_layout(&v, 1, 1);
    int w = (int)v.cw, h = (int)v.ch;
    if (!g_gl) SDL_RenderSetLogicalSize(g_ren, w, h);
    if (g_fullscreen) return;
    float s = g_opt.actual && g_actual_sc > 0 ? g_actual_sc : (float)g_scale;
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(g_win), &r) == 0) {
        float fit = fminf((float)(r.w - 16) / (float)w, (float)(r.h - 64) / (float)h);   /* room for the title and menu bars */
        if (s > fit) s = fit;
    }
    SDL_SetWindowSize(g_win, (int)ceilf((float)w * s), (int)ceilf((float)h * s));
}

/* OpenGL: a 3.3 core context; on any failure fall back to SDL_Renderer + software 3D. */
static int try_gl(void) {
    if (getenv("DQIX_SOFTWARE")) return 0;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_win = SDL_CreateWindow("Dragon Quest IX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 256 * g_scale, 384 * g_scale,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) return 0;
    g_glctx = SDL_GL_CreateContext(g_win);
    if (!g_glctx) { fprintf(stderr, "OpenGL 3.3 not available (%s): using the software renderer\n", SDL_GetError()); SDL_DestroyWindow(g_win); g_win = NULL; return 0; }
    SDL_GL_SetSwapInterval(0);                                   /* pacing follows the audio clock */
    const char* e = getenv("DQIX_SCALE3D");
    if (e) g_scale3d = atoi(e);
    if (!gl_render_init(g_scale3d)) {
        fprintf(stderr, "OpenGL renderer failed to start: using the software renderer\n");
        SDL_GL_DeleteContext(g_glctx); SDL_DestroyWindow(g_win); g_win = NULL; return 0;
    }
    g_gl = 1;
    g_renderer3d_verify = getenv("DQIX_GL_VERIFY") != NULL;
    return 1;
}

static void lobby_hook(void);
static void idle_pump(void);
void frontend_init(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();
    g_report_perf = perf_write;
    g_headless = getenv("DQIX_NOWINDOW") != NULL;
    if (getenv("DQIX_HIDDEN")) { g_hidden = g_headless = 1; return; }
    skins_scan();
    default_console_color();
    if (!getenv("DQIX_NO_SETTINGS")) {
        settings_load();
        if (!g_wireless.mac_set && !g_headless) { wireless_mac_new(); settings_save(); }
    }
    wireless_apply(1);                                          /* read by firmware_init */
    if (g_opt.dlc) backup_unlock_dlc();                        /* before the game reads the save */
    if (getenv("DQIX_MODE")) g_classic = strcmp(getenv("DQIX_MODE"), "enhanced") != 0;   /* tests */
    /* real pixels on high-DPI Windows setups (not stretched by the system), so
     * the pixel grid and actual size are drawn on the monitor's own pixels */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init((g_headless ? 0 : SDL_INIT_VIDEO) | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(1);
    }
    atexit(SDL_Quit);
    if (!g_headless) {
        const char* sc = getenv("DQIX_SCALE");
        if (sc) { g_scale = atoi(sc); if (g_scale < 1) g_scale = 1; if (g_scale > 8) g_scale = 8; }
        if (!try_gl()) {
            g_win = SDL_CreateWindow("Dragon Quest IX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 256 * g_scale, 384 * g_scale,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!g_win) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); exit(1); }
            g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
            if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
            g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 384);
        }
        apply_mode();
        const char* names[N_DRAWN + MAX_SKINS];
        for (int i = 0; i < N_DRAWN; i++) names[i] = SHELLS[i].name;
        for (int i = 0; i < g_nskins; i++) names[N_DRAWN + i] = g_skins[i].name;
        menu_init(g_win, names, N_SHELLS);
        if (g_update_auto && !session_requested() && update_project_dir()) update_check_async();
        update_actual();
        menu_sync();
        layout_window();
        if (g_fullscreen) { menu_show(0); SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP); }
        atexit(settings_save);
        session_lobby_hook = lobby_hook;
        air_idle_hook = idle_pump;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 32768; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);           /* SDL converts to the device's format */
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else fprintf(stderr, "no audio: %s\n", SDL_GetError());
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; }
    g_perf_freq = SDL_GetPerformanceFrequency();
    g_next_frame = SDL_GetPerformanceCounter();
    g_fps_t0 = g_next_frame;
}

static uint16_t key_bits(const Uint8* ks) {
    uint16_t k = 0;
    if (ks[SDL_SCANCODE_X]) k |= 1 << 0;                           /* A */
    if (ks[SDL_SCANCODE_Z]) k |= 1 << 1;                           /* B */
    if (ks[SDL_SCANCODE_RSHIFT] || ks[SDL_SCANCODE_BACKSPACE]) k |= 1 << 2;   /* SELECT */
    if (ks[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & KMOD_ALT)) k |= 1 << 3;   /* START (not with Alt+Enter = fullscreen) */
    if (ks[SDL_SCANCODE_RIGHT]) k |= 1 << 4;
    if (ks[SDL_SCANCODE_LEFT]) k |= 1 << 5;
    if (ks[SDL_SCANCODE_UP]) k |= 1 << 6;
    if (ks[SDL_SCANCODE_DOWN]) k |= 1 << 7;
    if (ks[SDL_SCANCODE_W]) k |= 1 << 8;                           /* R */
    if (ks[SDL_SCANCODE_Q]) k |= 1 << 9;                           /* L */
    if (ks[SDL_SCANCODE_S]) k |= 1 << 10;                          /* X */
    if (ks[SDL_SCANCODE_A]) k |= 1 << 11;                          /* Y */
    return k;
}

static uint16_t pad_bits(void) {
    if (!g_pad) return 0;
    uint16_t k = 0;
    SDL_GameController* p = g_pad;
    /* positions: east = A, south = B, north = X, west = Y (like a DS) */
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_B)) k |= 1 << 0;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_A)) k |= 1 << 1;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_BACK)) k |= 1 << 2;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_START)) k |= 1 << 3;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) k |= 1 << 4;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) k |= 1 << 5;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP)) k |= 1 << 6;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) k |= 1 << 7;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) k |= 1 << 8;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) k |= 1 << 9;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_Y)) k |= 1 << 10;
    if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_X)) k |= 1 << 11;
    int lx = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx > 16000) k |= 1 << 4; if (lx < -16000) k |= 1 << 5;
    if (ly < -16000) k |= 1 << 6; if (ly > 16000) k |= 1 << 7;
    return k;
}

/* Window pixel -> bottom-screen pixel; returns 0 when outside it, unless
 * clamp is set (a drag that started on the screen stays on its edge, as a
 * stylus would at the rim). */
static int to_touch(int wx, int wy, int* tx, int* ty, int clamp) {
    float lx, ly;
    GlPresent v;
    if (g_gl) {                                                  /* same layout as gl_present() */
        int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
        canvas_layout(&v, ww, wh);
        lx = ((float)wx + 0.5f - v.ox) / v.sc; ly = ((float)wy + 0.5f - v.oy) / v.sc;
    } else { canvas_layout(&v, 1, 1); SDL_RenderWindowToLogical(g_ren, wx, wy, &lx, &ly); }
    int x = (int)floorf(lx - v.bot[0]), y = (int)floorf(ly - v.bot[1]);
    if (clamp) { x = x < 0 ? 0 : x > 255 ? 255 : x; y = y < 0 ? 0 : y > 191 ? 191 : y; }
    if (x < 0 || x > 255 || y < 0 || y > 191) return 0;
    *tx = x; *ty = y;
    return 1;
}

static int g_shot_req;
static void save_bmp_screens(const uint32_t scr[2][256 * 192], const char* path) {
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, 256, 384, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return;
    for (int y = 0; y < 384; y++) memcpy((uint8_t*)sf->pixels + y * sf->pitch, &scr[y / 192][(y % 192) * 256], 256 * 4);
    SDL_SaveBMP(sf, path);
    SDL_FreeSurface(sf);
}

static void save_screenshot(void) {
    if (g_gl) {
        static uint32_t scr[2][256 * 192];
        static int n;
        char path[64];
        gl_read_screens(scr);
        snprintf(path, sizeof path, "dqix-shot-%d.bmp", n++);
        save_bmp_screens(scr, path);
        fprintf(stderr, "screenshot: %s\n", path);
        return;
    }
    int w, h;
    SDL_GetRendererOutputSize(g_ren, &w, &h);
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return;
    if (SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch) == 0) {
        static int n;
        char path[64];
        snprintf(path, sizeof path, "dqix-shot-%d.bmp", n++);
        SDL_SaveBMP(sf, path);
        fprintf(stderr, "screenshot: %s\n", path);
    }
    SDL_FreeSurface(sf);
}

static void test_hooks(void) {
    static long long report_at = -2;
    if (report_at == -2) { const char* e = getenv("DQIX_REPORT_AT"); report_at = e ? atoll(e) : -1; }
    if (report_at >= 0 && g_frame == (uint64_t)report_at) report_write("DQIX_REPORT_AT (test)");
    static long long shot_at = -2;
    if (shot_at == -2) { const char* e = getenv("DQIX_SHOT_AT"); shot_at = e ? atoll(e) : -1; }
    if (g_shot_req || (shot_at >= 0 && g_frame == (uint64_t)shot_at)) { save_screenshot(); g_shot_req = 0; }
}

/* DQIX_GL_DUMP="dir,every": write the GL-composed screens (256x384) like DQIX_DUMP (tests) */
static void gl_dump_hook(void) {
    static int every = -1; static char dir[400];
    if (every < 0) {
        const char* e = getenv("DQIX_GL_DUMP");
        every = 0;
        if (e) { const char* c = strchr(e, ','); size_t n = c ? (size_t)(c - e) : strlen(e); if (n >= sizeof dir) n = sizeof dir - 1; memcpy(dir, e, n); dir[n] = 0; every = c ? atoi(c + 1) : 60; }
    }
    if (every <= 0 || g_frame % (uint64_t)every) return;
    char path[600]; snprintf(path, sizeof path, "%s/f%06llu.ppm", dir, (unsigned long long)g_frame);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    if (getenv("DQIX_GL_DUMP_FULL")) {                       /* at the rendering resolution */
        int w, h; uint8_t* rgb = gl_read_full(&w, &h);
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        fwrite(rgb, 1, (size_t)w * h * 3, f);
        free(rgb);
    } else {                                                 /* native 256x384, for comparisons */
        static uint32_t scr[2][256 * 192];
        gl_read_screens(scr);
        fprintf(f, "P6\n256 384\n255\n");
        for (int sc = 0; sc < 2; sc++) for (int i = 0; i < 256 * 192; i++) { uint32_t c = scr[sc][i]; uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c }; fwrite(rgb, 1, 3, f); }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- frame timings (for bug reports) */
#define PERF_N 1800                                          /* the last 30 seconds */
typedef struct { uint64_t frame; float busy, gl, upload, present, wait, d2; int uploads, polys, audio_ms; uint8_t fast; } PerfRec;
static PerfRec g_perf[PERF_N];
static unsigned g_perf_pos;
static uint64_t g_perf_end;                                  /* end of the previous frame (after pacing) */
static double ticks_ms(uint64_t t) { return (double)t * 1000.0 / (double)g_perf_freq; }

static void perf_write(FILE* f) {
    fprintf(f, "mode: %s\n", g_classic ? "classic (DS 3D renderer)" : "enhanced (3D on the GPU)");
    fprintf(f, "renderer: %s\n", g_gl ? gl_renderer_name() : "software (no OpenGL 3.3)");
    if (g_gl && !g_classic) fprintf(f, "3D resolution: %dx\n", gl_render_scale());
    fprintf(f, "audio: %s\n\n", g_audio ? "on" : "off");
    fprintf(f, "Per frame, in ms. A frame must take under 16.7 ms in total (busy + present) to keep full speed.\n");
    fprintf(f, "busy = all work for the frame: game = game code, 2d = 2D layers, gl = 3D on the GPU, upload = new textures (inside gl),\n");
    fprintf(f, "present = showing the frame, wait = idle time to keep the pace, audio = sound queued.\n\n");
    unsigned n = g_perf_pos < PERF_N ? g_perf_pos : PERF_N - 1;     /* the slot at g_perf_pos is the frame in progress */
    unsigned slow = 0; double worst = 0; uint64_t worst_f = 0;
    for (unsigned k = n; k > 0; k--) {
        const PerfRec* r = &g_perf[(g_perf_pos - k) % PERF_N];
        double t = r->busy + r->present;
        if (t > 16.7) slow++;
        if (t > worst) { worst = t; worst_f = r->frame; }
    }
    fprintf(f, "frames: %u, over 16.7 ms: %u, worst %.1f ms at frame %llu\n\n", n, slow, worst, (unsigned long long)worst_f);
    fprintf(f, "  frame    busy    game      2d      gl  upload (n)   polys  present    wait  audio\n");
    for (unsigned k = n; k > 0; k--) {
        const PerfRec* r = &g_perf[(g_perf_pos - k) % PERF_N];
        fprintf(f, "%7llu %7.1f %7.1f %7.1f %7.1f %7.1f (%3d) %6d %8.1f %7.1f %6d%s%s\n", (unsigned long long)r->frame, r->busy,
                r->busy - r->d2 - r->gl, r->d2, r->gl, r->upload, r->uploads,
                r->polys, r->present, r->wait, r->audio_ms, r->fast ? " fast" : "", r->busy + r->present > 16.7 ? "  <-- slow" : "");
    }
}

/* DQIX_WINSHOT_AT=frame[,frame...]: save the whole window as shown (tests) */
static void window_shot_hook(int w, int h) {
    static char list[400]; static int init;
    if (!init) { init = 1; const char* e = getenv("DQIX_WINSHOT_AT"); if (e) snprintf(list, sizeof list, ",%s,", e); }
    if (!list[0]) return;
    char key[32]; snprintf(key, sizeof key, ",%llu,", (unsigned long long)g_frame);
    if (!strstr(list, key)) return;
    uint8_t* rgb = gl_read_window(w, h);
    char path[64]; snprintf(path, sizeof path, "win%06llu.ppm", (unsigned long long)g_frame);
    FILE* f = fopen(path, "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, (size_t)w * h * 3, f); fclose(f); }
    free(rgb);
}

static void present_gl(void) {
    uint16_t pow; memcpy(&pow, &g_io[0x304], 2);
    gl_compose((pow >> 15) & 1, pow & 1);
    gl_dump_hook();
    test_hooks();
    int w, h; SDL_GL_GetDrawableSize(g_win, &w, &h);
    GlPresent v; canvas_layout(&v, w, h);
    gl_present(&v);
    window_shot_hook(w, h);
    SDL_GL_SwapWindow(g_win);
}

static void present(void) {
    static long long skip = -2;                               /* DQIX_PRESENT_FROM=frame: don't draw before it (tests) */
    if (skip == -2) { const char* e = getenv("DQIX_PRESENT_FROM"); skip = e ? atoll(e) : -1; }
    if (skip >= 0 && g_frame < (uint64_t)skip) return;
    if (g_gl) { present_gl(); return; }
    void* pix; int pitch;
    if (SDL_LockTexture(g_tex, NULL, &pix, &pitch) == 0) {
        for (int s = 0; s < 2; s++)
            for (int y = 0; y < 192; y++)
                memcpy((uint8_t*)pix + (size_t)(s * 192 + y) * pitch, &g_screen[s][y * 256], 256 * 4);
        SDL_UnlockTexture(g_tex);
    }
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    if (g_side_by_side) {
        SDL_Rect top = { 0, 0, 256, 192 }, bot = { 0, 192, 256, 192 };
        SDL_Rect dt = { 0, 0, 256, 192 }, db = { 256, 0, 256, 192 };
        SDL_RenderCopy(g_ren, g_tex, &top, &dt);
        SDL_RenderCopy(g_ren, g_tex, &bot, &db);
    } else SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    test_hooks();
    SDL_RenderPresent(g_ren);
}

/* ---------------------------------------------------------------- settings changes (keys and menu) */
static void update_offer(const UpdateInfo* u);
static void update_title(double fps) {
    { UpdateInfo u; if (update_poll(&u)) update_offer(&u); }
    session_window_close();                                     /* the lobby is over */
    char title[400], ses[300] = "";
    if (session_running()) {
        SessionInfo si; session_info(&si);
        int here = 0; for (int k = 0; k < si.n; k++) here += !si.gone[k];
        if (si.desync) snprintf(ses, sizeof ses, " - OUT OF SYNC: leave the session (Wireless menu)");
        else if (si.state == 4) snprintf(ses, sizeof ses, " - session over: %s", si.message);
        else snprintf(ses, sizeof ses, " - together: %d of %d players, delay %d", here, si.n, si.delay);
        static char last[256]; static uint64_t shown;
        if (strcmp(last, si.message)) { snprintf(last, sizeof last, "%s", si.message); shown = SDL_GetPerformanceCounter(); }
        if (!si.desync && si.state == 3 && SDL_GetPerformanceCounter() - shown < g_perf_freq * 8) snprintf(ses + strlen(ses), sizeof ses - strlen(ses), " - %s", si.message);
    }
    if (g_paused) snprintf(title, sizeof title, "Dragon Quest IX " DQIX_VERSION " - paused (P to continue)%s", ses);
    else snprintf(title, sizeof title, "Dragon Quest IX " DQIX_VERSION " - %s%s - %.1f fps%s%s", g_classic ? "Classic" : "Enhanced",
                  g_gl && g_opt.lcd ? " - DS Lite screen" : "", fps, g_fast ? " (fast)" : "", ses);
    SDL_SetWindowTitle(g_win, title);
}

static void menu_sync(void) {
    {
        SessionInfo si; session_info(&si);
        int in = session_requested();
        menu_enable(MI_SES_START, in && si.host && si.state == 2 && si.n >= 2);
        menu_enable(MI_SES_LEAVE, in);
        menu_enable(MI_SES_HOST, !in); menu_enable(MI_SES_JOIN, !in);
        menu_enable(MI_NET, !in);                            /* a session uses the built-in Wi-Fi only (the PCs must agree) */
    }
    int stacked = !g_side_by_side;
    menu_check(MI_PAUSE, g_paused);
    menu_check(MI_UPDATE_AUTO, g_update_auto);
    menu_check(MI_TURBO, g_turbo);
    menu_check(MI_DLC, g_opt.dlc);
    menu_check(MI_NET, g_net_want);
    menu_check(MI_SRV_BUILTIN, !strcmp(g_wfc_server, "builtin"));
    menu_check(MI_SRV_ALTWFC, !strcmp(g_wfc_server, "altwfc"));
    menu_check(MI_SRV_WIIMMFI, !strcmp(g_wfc_server, "wiimmfi"));
    menu_check(MI_SRV_CUSTOM, wfc_custom()); menu_enable(MI_SRV_CUSTOM, wfc_custom());
    menu_check(MI_CLASSIC, g_classic); menu_check(MI_ENHANCED, !g_classic);
    menu_enable(MI_ENHANCED, g_gl);
    for (int i = 0; i < 4; i++) { menu_check(MI_RES1 + i, g_scale3d == i + 1); menu_enable(MI_RES1 + i, g_gl && !g_classic); }
    menu_check(MI_LCD, g_opt.lcd); menu_enable(MI_LCD, g_gl);
    int lcd = g_gl && g_opt.lcd;
    menu_check(MI_LCDCOL, g_opt.lcd_color); menu_enable(MI_LCDCOL, lcd);
    menu_check(MI_GRID, g_opt.grid); menu_enable(MI_GRID, lcd);
    menu_check(MI_GHOST, g_opt.ghost); menu_enable(MI_GHOST, lcd);
    for (int i = 0; i < 4; i++) { menu_check(MI_BL1 + i, g_opt.backlight == i + 1); menu_enable(MI_BL1 + i, lcd); }
    menu_check(MI_SHELL, g_opt.shell); menu_enable(MI_SHELL, g_gl && stacked);
    for (int i = 0; i < N_SHELLS; i++) { menu_check(MI_SHELLC0 + i, g_opt.shell_color == i); menu_enable(MI_SHELLC0 + i, g_gl && stacked && g_opt.shell); }
    menu_check(MI_GAP, g_opt.gap || g_opt.shell); menu_enable(MI_GAP, g_gl && stacked && !g_opt.shell);
    menu_check(MI_STACKED, stacked); menu_check(MI_SIDE, !stacked);
    for (int i = 0; i < 4; i++) { menu_check(MI_SIZE1 + i, g_scale == i + 1); menu_enable(MI_SIZE1 + i, !g_opt.actual); }
    menu_check(MI_INTEGER, g_opt.integer); menu_enable(MI_INTEGER, !g_opt.actual);
    menu_check(MI_ACTUAL, g_opt.actual); menu_enable(MI_ACTUAL, g_gl);
    menu_check(MI_FULL, g_fullscreen);
}

static const char* CONTROLS_TEXT =
    "DS button      Keyboard              Controller\n"
    "D-pad          arrow keys            D-pad / left stick\n"
    "A / B          X / Z                 right / bottom button\n"
    "X / Y          S / A                 top / left button\n"
    "L / R          Q / W                 shoulder buttons\n"
    "START          Enter                 Start\n"
    "SELECT         Right Shift           Back\n"
    "Touch screen   left mouse button on the bottom screen\n\n"
    "Tab (hold) fast forward   P pause   F5 Classic / Enhanced\n"
    "F6 DS Lite screen   F7 DS Lite console   F8 actual size   F2 layout   F3 window size\n"
    "F4 3D resolution   F11 or Alt+Enter fullscreen (mouse at the top: menu bar)\n"
    "F12 screenshot   F10 bug report (with a description)   Esc leave fullscreen / quit";

/* A message box whose long lines are wrapped (the system boxes size themselves
 * to the longest line, which made the help as wide as the screen). */
static int info_box(Uint32 flags, const char* title, const char* text, SDL_Window* win) {
    size_t n = strlen(text);
    char* out = malloc(n * 2 + 1);
    if (!out) return SDL_ShowSimpleMessageBox(flags, title, text, win);
    const int W = 76;
    size_t o = 0; int col = 0, wrapped = 0;
    for (size_t i = 0; i < n;) {
        if (text[i] == '\n') { out[o++] = '\n'; col = 0; i++; continue; }
        size_t j = i; while (j < n && text[j] != ' ' && text[j] != '\n') j++;    /* a word */
        int len = (int)(j - i);
        if (col > 0 && col + len > W) { while (o > 0 && out[o - 1] == ' ') o--; out[o++] = '\n'; col = 0; wrapped = 1; }
        memcpy(out + o, text + i, (size_t)len); o += (size_t)len; col += len; i = j;
        if (len) wrapped = 0;
        while (i < n && text[i] == ' ') { if (!(wrapped && col == 0)) { out[o++] = ' '; col++; } i++; }
    }
    out[o] = 0;
    int r = SDL_ShowSimpleMessageBox(flags, title, out, win);
    free(out);
    return r;
}

static int g_menu_peek;          /* fullscreen: the menu bar is showing because the mouse is at the top */
static void settings_save(void);

/* ---------------------------------------------------------------- playing with friends far away */
static const char SESSION_HELP[] =
    "Tag mode, visiting other worlds and inviting friends (Pavo) need DS systems close to each other. "
    "This lets friends anywhere play those parts together, up to 4 players.\n\n"
    "How: one of you hosts (Wireless > Play with friends far away > Host), the others join with the address "
    "the host's game shows. When everyone is in, the host presses Start the game. All the games restart together, "
    "each from its own save, and from then on every PC runs every player's DS: yours in the window, your "
    "friends' unseen. Only the buttons travel between the PCs, so the DSes really are side by side - "
    "on every PC at once.\n\n"
    "What to expect:\n"
    "- Save your adventure before hosting or joining: the game restarts.\n"
    "- Everyone needs the same version of this game.\n"
    "- Buttons react a little later than usual (the input delay, a few frames, depending on the distance).\n"
    "- If someone's PC is slow, everyone waits for it while your DSes are talking wirelessly.\n"
    "- The built-in Wi-Fi works as usual (DQVC, the downloads), with the host's files for everyone. Other online servers are off during a session.\n"
    "- Your game saves to your own save as always. Leaving (or closing the game) switches your DS off for the others.\n\n"
    "Getting in: the host's game asks the home router to let friends in. If the router won't, forward TCP port "
    "27900 to the host's PC, or all of you use a free virtual LAN such as Tailscale, ZeroTier or Radmin VPN and "
    "use the host's address there. Windows may ask whether to allow the game on networks: allow it.";

/* start this program again, in a session or not (the save is written first) */
static void restart_with(const char* session) {
    backup_flush();
    settings_save();
#ifdef _WIN32
    SetEnvironmentVariableA("DQIX_SESSION", session);
    SetEnvironmentVariableA("DQIX_PLAYER_NAME", g_player_name[0] ? g_player_name : NULL);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    wchar_t* cmd = _wcsdup(GetCommandLineW());
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        exit(0);
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dragon Quest IX", "The game couldn't start again by itself. Close it and start it again.", g_win);
#else
    if (session) setenv("DQIX_SESSION", session, 1); else unsetenv("DQIX_SESSION");
    if (g_player_name[0]) setenv("DQIX_PLAYER_NAME", g_player_name, 1);
    extern char** g_argv;
    execv("/proc/self/exe", g_argv);
#endif
}

static int ask_yes(const char* title, const char* text, const char* yes) {
    const SDL_MessageBoxButtonData b[2] = { { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes }, { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" } };
    const SDL_MessageBoxData d = { SDL_MESSAGEBOX_INFORMATION, g_win, title, text, 2, b, NULL };
    int r = 0;
    return SDL_ShowMessageBox(&d, &r) == 0 && r == 1;
}

static int ask_name(void) {
    char* t = g_win ? ask_text(g_win, "Your name", "The name your friends see in the session (your hero's name, say):") : NULL;
    if (!t) return g_player_name[0] != 0;
    t[strcspn(t, "\r\n")] = 0;
    if (t[0]) snprintf(g_player_name, sizeof g_player_name, "%s", t);
    free(t);
    settings_save();
    return g_player_name[0] != 0;
}

static void session_action(int id) {
    static const char RESTART[] = "\n\nThe game restarts for this: save your adventure first (in the game) if you haven't.";
    switch (id) {
    case MI_SES_HOST: {
        if (!g_player_name[0] && !ask_name()) return;
        char text[400]; snprintf(text, sizeof text, "Host a session: your friends join you (up to 3 of them).%s", RESTART);
        if (!ask_yes("Host a session", text, "Host")) return;
        char ses[64]; snprintf(ses, sizeof ses, "host:%d", g_session_port);
        restart_with(ses);
        break;
    }
    case MI_SES_JOIN: {
        if (!g_player_name[0] && !ask_name()) return;
        char* t = g_win ? ask_text(g_win, "Join a friend",
            "The address your friend's game shows (for example 203.0.113.7:27900). "
            "The game restarts and connects: save your adventure first if you haven't.") : NULL;
        if (!t) return;
        char* a = t; while (*a == ' ') a++;
        a[strcspn(a, " \r\n")] = 0;
        if (!a[0]) { free(t); return; }
        char ses[200]; snprintf(ses, sizeof ses, "join:%s", a);
        free(t);
        restart_with(ses);
        break;
    }
    case MI_SES_LEAVE:
        if (!ask_yes("Leave the session", "Leave and play alone? Your DS is switched off for the others; your game restarts "
                     "(it was saved whenever you saved in the game).", "Leave")) return;
        restart_with(NULL);
        break;
    case MI_SES_START: session_start_now(); break;
    case MI_SES_NAME: ask_name(); break;
    case MI_SES_HELP: info_box(SDL_MESSAGEBOX_INFORMATION, "Playing with friends far away", SESSION_HELP, g_win); break;
    }
}

/* ---------------------------------------------------------------- updates (update.c) */
static void update_go(const char* branch, const char* version) {
    if (session_requested()) { info_box(SDL_MESSAGEBOX_INFORMATION, "Update", "Leave the session first (Wireless menu), then update.", g_win); return; }
    if (!update_project_dir()) {
        info_box(SDL_MESSAGEBOX_INFORMATION, "Update", "This copy of the game wasn't made with BUILD.bat, so it can't update itself. "
                 "Download the new version from https://github.com/" UPDATE_REPO " instead.", g_win);
        return;
    }
    char text[700];
    snprintf(text, sizeof text, "Update to %s?\n\nThe game closes, downloads %s, builds it from your ROM again (a few minutes; "
             "a window shows how far it is) and starts again. Your saves, settings and extras are kept, and the current "
             "version is kept in the update folder in case you want to go back (update\\rollback.bat).", version, version);
    if (!ask_yes("Update", text, "Update")) return;
    backup_flush();
    settings_save();
    if (g_win) SDL_SetWindowTitle(g_win, "Dragon Quest IX - downloading the update...");
    char err[200] = "";
    if (update_install(branch, version, err, sizeof err)) exit(0);
    char m[400]; snprintf(m, sizeof m, "The update couldn't be downloaded: %s.", err);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Update", m, g_win);
}

/* at start: a newer stable version */
static void update_offer(const UpdateInfo* u) {
    if (!u->ok || !update_newer(u->stable, DQIX_VERSION) || !strcmp(u->stable, g_update_skip) || session_requested()) return;
    char text[400];
    snprintf(text, sizeof text, "Dragon Quest IX %s is available (you have %s).\n\nUpdate now? (Help > Check for updates "
             "also has the nightly version, with the newest work.)", u->stable, DQIX_VERSION);
    const SDL_MessageBoxButtonData b[3] = { { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Update" }, { 0, 2, "Skip this version" },
                                            { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Later" } };
    const SDL_MessageBoxData d = { SDL_MESSAGEBOX_INFORMATION, g_win, "Update available", text, 3, b, NULL };
    int r = 0;
    if (SDL_ShowMessageBox(&d, &r) != 0) return;
    if (r == 1) update_go("stable", u->stable);
    else if (r == 2) { snprintf(g_update_skip, sizeof g_update_skip, "%s", u->stable); settings_save(); }
}

/* Help > Check for updates */
static void update_menu(void) {
    if (g_win) SDL_SetWindowTitle(g_win, "Dragon Quest IX - checking for updates...");
    UpdateInfo u; update_check_now(&u);
    if (!u.ok) {
        char m[300]; snprintf(m, sizeof m, "Couldn't check for updates: %s.", u.error[0] ? u.error : "no answer");
        info_box(SDL_MESSAGEBOX_INFORMATION, "Updates", m, g_win);
        return;
    }
    char text[700];
    snprintf(text, sizeof text,
             "This copy: %s\n\nStable: %s%s\nTested; recommended for everyone.\n\nNightly: %s%s\nThe newest work: new features "
             "and fixes first, but less tested.\n\nFriends playing together need the same version.",
             DQIX_VERSION, u.stable, update_newer(u.stable, DQIX_VERSION) ? " (newer)" : !strcmp(u.stable, DQIX_VERSION) ? " (this one)" : "",
             u.nightly, update_newer(u.nightly, DQIX_VERSION) ? " (newer)" : !strcmp(u.nightly, DQIX_VERSION) ? " (this one)" : "");
    char bs[48], bn[48];
    snprintf(bs, sizeof bs, "Install stable %s", u.stable);
    snprintf(bn, sizeof bn, "Install nightly %s", u.nightly);
    SDL_MessageBoxButtonData b[3]; int nb = 0;
    if (strcmp(u.stable, DQIX_VERSION)) b[nb++] = (SDL_MessageBoxButtonData){ 0, 1, bs };
    if (strcmp(u.nightly, DQIX_VERSION) && strcmp(u.nightly, u.stable)) b[nb++] = (SDL_MessageBoxButtonData){ 0, 2, bn };
    b[nb++] = (SDL_MessageBoxButtonData){ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Close" };
    const SDL_MessageBoxData d = { SDL_MESSAGEBOX_INFORMATION, g_win, "Updates", text, nb, b, NULL };
    int r = 0;
    if (SDL_ShowMessageBox(&d, &r) != 0) return;
    if (r == 1) update_go("stable", u.stable);
    else if (r == 2) update_go("main", u.nightly);
}

/* the lobby: until everyone is in */
static void present(void);
static void do_action(int id);
static void lobby_draw(void) {
    SessionInfo si; session_info(&si);
    static int copied;
    if (si.host && si.ready && !copied) { SDL_SetClipboardText(si.address); copied = 1; }
    for (int s = 0; s < 2; s++) for (int i = 0; i < 256 * 192; i++) g_screen[s][i] = s ? 0x10203Au : 0x0B1830u;
    uint32_t* top = g_screen[0]; uint32_t* bot = g_screen[1];
    const uint32_t W = 0xFFFFFFu, Y = 0xFFD860u, G = 0xA8B8D8u, R = 0xFF8080u;
    if (!text_ready()) {                                     /* no font: the title bar says it */
        char t[300]; snprintf(t, sizeof t, "Session: %s (Enter = start, Esc = cancel)", si.state == 2 ? (si.host ? si.address : "waiting for the host") : si.message);
        if (g_win) SDL_SetWindowTitle(g_win, t);
        present(); return;
    }
    int y = 6;
    text_draw(top, 256, 192, 8, y, 15, Y, "Playing with friends far away"); y += 22;
    if (si.state == 5) { text_wrap(top, 256, 192, 8, y, 240, 12, R, si.message); text_wrap(bot, 256, 192, 8, 8, 240, 12, G, "Esc: back (the game restarts on its own)."); present(); return; }
    if (si.host) {
        if (!si.ready) { text_draw(top, 256, 192, 8, y, 12, W, "Finding your address..."); y += 18; }
        else {
            text_draw(top, 256, 192, 8, y, 12, G, "Give your friends this address:"); y += 15;
            text_draw(top, 256, 192, 12, y, 15, W, si.address); y += 19;
            text_draw(top, 256, 192, 12, y, 10, G, "(copied - paste it into a message)"); y += 15;
            y = text_wrap(top, 256, 192, 8, y, 240, 10, G, si.hint);
        }
    } else {
        text_draw(top, 256, 192, 8, y, 12, G, si.state == 1 ? "Connecting to" : "Connected to"); y += 15;
        text_draw(top, 256, 192, 12, y, 14, W, si.address); y += 20;
        if (si.state == 2) text_wrap(top, 256, 192, 8, y, 240, 12, G, "You're in. Waiting for the host to start the session.");
    }
    int by = 6;
    text_draw(bot, 256, 192, 8, by, 13, Y, "Players"); by += 18;
    for (int k = 0; k < si.n && si.state >= 2; k++) {
        char line[96];
        if (k == si.me) snprintf(line, sizeof line, "%d.  %s  (you)", k + 1, si.names[k]);
        else if (si.host && k > 0) snprintf(line, sizeof line, "%d.  %s  (%d ms)", k + 1, si.names[k], si.ping_ms[k]);
        else snprintf(line, sizeof line, "%d.  %s", k + 1, si.names[k]);
        text_draw(bot, 256, 192, 14, by, 13, W, line); by += 17;
    }
    by = 128;
    if (si.message[0]) by = text_wrap(bot, 256, 192, 8, by, 240, 10, G, si.message);
    text_wrap(bot, 256, 192, 8, 164, 240, 11, Y, si.host ? (si.n >= 2 ? "Enter: start (everyone is here)    Esc: cancel" : "Waiting for friends...    Esc: cancel")
                                                         : "Esc: cancel");
    { static int calls; const char* shot = getenv("DQIX_LOBBY_SHOT");     /* tests: the lobby after a few seconds */
      if (shot && ++calls == 120) save_bmp_screens((const uint32_t(*)[256 * 192])g_screen, shot); }
    present();
}

/* Windows: the same in a window of its own, with buttons */
static void lobby_window(void) {
    SessionInfo si; session_info(&si);
    if (!g_win || si.state == 3 || si.state == 4 || !session_window_open(g_win)) { session_window_close(); return; }
    SessionWindow w; memset(&w, 0, sizeof w);
    w.host = si.host; w.failed = si.state == 5; w.can_start = si.host && si.state == 2 && si.n >= 2;
    if (si.host) {
        snprintf(w.heading, sizeof w.heading, "%s", si.ready ? "Give your friends this address (they choose Join a friend and paste it):" : "Finding your address...");
        if (si.ready) snprintf(w.address, sizeof w.address, "%s", si.address);
        snprintf(w.hint, sizeof w.hint, "%s", si.ready ? si.hint : "");
    } else {
        snprintf(w.heading, sizeof w.heading, "%s", si.state == 1 ? "Connecting to:" : "Connected to:");
        snprintf(w.address, sizeof w.address, "%s", si.address);
        snprintf(w.hint, sizeof w.hint, "%s", si.state == 2 ? "You're in. The game starts when the host presses Start." : "");
    }
    for (int k = 0; k < si.n && si.state >= 2 && k < 4; k++) {
        if (k == si.me) snprintf(w.players[k], sizeof w.players[k], "%d.   %s   (you)", k + 1, si.names[k]);
        else if (si.host && k > 0) snprintf(w.players[k], sizeof w.players[k], "%d.   %s   (ping %d ms)", k + 1, si.names[k], si.ping_ms[k]);
        else snprintf(w.players[k], sizeof w.players[k], "%d.   %s%s", k + 1, si.names[k], k == 0 ? "   (host)" : "");
        w.n_players = k + 1;
    }
    if (si.state == 5) snprintf(w.status, sizeof w.status, "%s", si.message);
    else if (si.host) snprintf(w.status, sizeof w.status, "%s%s%s", si.message, si.message[0] ? "\n" : "",
                               si.n >= 2 ? "When everyone is here, press Start the game." : "Waiting for friends to join (up to 3)...");
    else snprintf(w.status, sizeof w.status, "%s", si.message);
    session_window_update(&w);
    switch (session_window_action()) {
    case 1: if (w.can_start) session_start_now(); break;
    case 2: if (si.state == 5) restart_with(NULL); else if (ask_yes("Cancel", "Stop and play alone? The game restarts.", "Stop")) restart_with(NULL); break;
    }
}

static void lobby_hook(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        int mi = menu_command(&e);
        if (mi) { do_action(mi); continue; }
        if (e.type == SDL_QUIT) exit(0);
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            SDL_Scancode k = e.key.keysym.scancode;
            SessionInfo si; session_info(&si);
            if (k == SDL_SCANCODE_ESCAPE) { if (si.state == 5) restart_with(NULL); else if (ask_yes("Cancel", "Stop and play alone? The game restarts.", "Stop")) restart_with(NULL); }
            if ((k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) && si.host && si.n >= 2) session_start_now();
        }
    }
    menu_sync();
    lobby_draw();
    lobby_window();
    SDL_Delay(30);
}

/* waiting for a friend's DS: keep the window alive */
/* while this console waits for another (sessions): keep the window alive and say why it stands still */
static void idle_pump(void) {
    if (!g_win) return;
    SDL_PumpEvents();
    static uint64_t last;
    uint64_t now = SDL_GetPerformanceCounter();
    if (now - last < g_perf_freq / 2) return;
    last = now;
    double w = air_waiting_ms();
    if (w < 1500) return;
    char d[300], t[400]; int k;
    if (!air_wait_describe(d, sizeof d, &k)) return;
    char* paren = strstr(d, " ("); if (paren) *paren = 0;                /* the numbers are for the log */
    snprintf(t, sizeof t, "Dragon Quest IX - %s (%.0f s) - details in session\\front.log", d, w / 1000.0);
    SDL_SetWindowTitle(g_win, t);
}

static void do_action(int id) {
    int relayout = 0;
    switch (id) {
    case MI_PAUSE: g_paused = !g_paused; break;
    case MI_TURBO: g_turbo = !g_turbo; break;
    case MI_DLC:
        g_opt.dlc = !g_opt.dlc;
        info_box(SDL_MESSAGEBOX_INFORMATION, "Downloadable content", g_opt.dlc ?
            "The 64 downloadable quests and the 23 special guests will be switched on in your save the next "
            "time the game starts (they come with the save, as if downloaded from Nintendo Wi-Fi Connection).\n\n"
            "Close the game and start it again, load your adventure log, and save once in the game to keep them.\n\n"
            "The quests are offered by their quest givers once you reach them in the story; the guests visit "
            "Quester's Rest." :
            "Off. A save that already has them keeps them; this only stops switching them on at start.", g_win);
        break;
    case MI_NET:
        g_net_want = !g_net_want;
        wireless_apply(0);
        if (g_net_want) info_box(SDL_MESSAGEBOX_INFORMATION, "Wireless",
            "Connected mode is on. In the game, talk to Sellma at the DQVC counter in Stornway's Quester's Rest "
            "and the DS will connect through this PC "
            "to the server chosen under Wireless > Server.\n\n"
            "If the connection fails, press F10 right after: the bug report records how far it got.", g_win);
        break;
    case MI_SRV_BUILTIN: case MI_SRV_ALTWFC: case MI_SRV_WIIMMFI:
        snprintf(g_wfc_server, sizeof g_wfc_server, "%s", WFC[id == MI_SRV_BUILTIN ? 0 : id == MI_SRV_ALTWFC ? 1 : 2].key);
        wireless_apply(0);
        break;
    case MI_SRV_CUSTOM: break;
    case MI_NET_HELP:
        info_box(SDL_MESSAGEBOX_INFORMATION, "Wireless",
            "Nintendo Wi-Fi Connection closed in 2014. Fans run replacement servers (AltWFC, Wiimmfi) that a "
            "real DS reaches by changing its DNS setting; this game does the same.\n\n"
            "The PC plays the part of a wireless access point called DQIX-PC and passes the DS's traffic to the "
            "internet. Nothing else on your PC is changed.\n\n"
            "Server > Built-in: nothing goes to the internet. The PC answers the game itself with DQIX's "
            "downloads as Nintendo served them (preserved by fans, in the wfc folder).\n\n"
            "Connect to the internet: the DS gets online when the game asks for it, e.g. when Sellma downloads "
            "DQVC items and new quests. Turn it off and the game behaves like a DS with no access point.\n\n"
            "Unlock all downloadable content now: switches on every downloadable quest and special guest in "
            "your save when the game starts, without visiting Sellma.", g_win);
        break;
    case MI_SES_HOST: case MI_SES_JOIN: case MI_SES_LEAVE: case MI_SES_NAME: case MI_SES_START: case MI_SES_HELP:
        session_action(id);
        break;
    case MI_SHOT: g_shot_req = 1; break;
    case MI_REPORT: {
        extern char g_report_last_dir[300];
        report_write("F10 pressed");                               /* captured first: the moment you pressed it */
        char* text = g_win ? ask_text(g_win, "Bug report " DQIX_VERSION,
            "The report is saved. Describe what went wrong (what you did, what you expected, what happened) - "
            "as much as you like. Save adds it to the report; Skip keeps the report without a description.") : NULL;
        if (text && g_report_last_dir[0]) {
            char path[400]; snprintf(path, sizeof path, "%s/description.txt", g_report_last_dir);
            FILE* f = fopen(path, "wb");
            if (f) { fputs(text, f); fputs("\n", f); fclose(f); }
        }
        free(text);
        if (g_win) SDL_SetWindowTitle(g_win, "Dragon Quest IX - report saved (see the reports folder)");
        g_fps_t0 = SDL_GetPerformanceCounter();                    /* keep that title up for a second */
        g_next_frame = SDL_GetPerformanceCounter();                /* the game waited while you typed */
        break;
    }
    case MI_EXIT: exit(0);
    case MI_CLASSIC: case MI_ENHANCED:
        if (!g_gl) break;
        g_classic = id == MI_CLASSIC; apply_mode(); g_fps_t0 = 0;
        break;
    case MI_RES1: case MI_RES2: case MI_RES3: case MI_RES4:
        if (!g_gl || g_classic) break;
        g_scale3d = id - MI_RES1 + 1; gl_render_set_scale(g_scale3d);
        break;
    case MI_LCD: if (g_gl) g_opt.lcd = !g_opt.lcd; g_fps_t0 = 0; break;
    case MI_LCDCOL: g_opt.lcd_color = !g_opt.lcd_color; break;
    case MI_GRID: g_opt.grid = !g_opt.grid; break;
    case MI_GHOST: g_opt.ghost = !g_opt.ghost; break;
    case MI_BL1: case MI_BL2: case MI_BL3: case MI_BL4: g_opt.backlight = id - MI_BL1 + 1; break;
    case MI_SHELL: if (g_gl && !g_side_by_side) { g_opt.shell = !g_opt.shell; relayout = 1; } break;
    case MI_GAP: if (g_gl) { g_opt.gap = !g_opt.gap; relayout = 1; } break;
    case MI_STACKED: case MI_SIDE: g_side_by_side = id == MI_SIDE; relayout = 1; break;
    case MI_SIZE1: case MI_SIZE2: case MI_SIZE3: case MI_SIZE4: g_scale = id - MI_SIZE1 + 1; relayout = 1; break;
    case MI_INTEGER: g_opt.integer = !g_opt.integer; break;
    case MI_ACTUAL:
        if (!g_gl) break;
        g_opt.actual = !g_opt.actual; relayout = 1;
        update_actual();
        if (g_opt.actual && g_actual_sc <= 0)
            info_box(SDL_MESSAGEBOX_INFORMATION, "Actual size",
                "Your monitor didn't say how big it is, so the real size can't be worked out.\n\n"
                "Measure the width of its picture area in millimetres and put it in settings.ini\n"
                "(next to the game) as monitor_width_mm, e.g. monitor_width_mm = 597", g_win);
        break;
    case MI_FULL:
        g_fullscreen = !g_fullscreen;
        g_menu_peek = 0;
        menu_show(!g_fullscreen);
        SDL_SetWindowFullscreen(g_win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        relayout = !g_fullscreen;
        break;
    case MI_CONTROLS: info_box(SDL_MESSAGEBOX_INFORMATION, "Controls", CONTROLS_TEXT, g_win); break;
    case MI_UPDATE: update_menu(); break;
    case MI_UPDATE_AUTO: g_update_auto = !g_update_auto; settings_save(); break;
    case MI_ABOUT:
        info_box(SDL_MESSAGEBOX_INFORMATION, "About",
            "Dragon Quest IX for PC, " DQIX_VERSION " (" DQIX_VERSION_DATE "), built " __DATE__ "\n\n"
            "Classic draws the game exactly like the DS; Enhanced draws its 3D\n"
            "on the graphics card at up to 4x.\n\n"
            "DS Lite screen colours: Pokefan531's measured DS Lite profile.", g_win);
        break;
    default:
        if (id >= MI_SHELLC0 && id < MI_SHELLC0 + N_SHELLS) { g_opt.shell_color = id - MI_SHELLC0; g_color_chosen = 1; relayout = 1; }
        else return;
    }
    if (relayout && g_win) layout_window();
    menu_sync();
    if (id != MI_SHOT && id != MI_REPORT && id != MI_PAUSE && id != MI_TURBO) settings_save();
}

static void handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        int mi = menu_command(&e);
        if (mi) { do_action(mi); continue; }
        switch (e.type) {
        case SDL_QUIT: exit(0);
        case SDL_KEYDOWN: {
            if (e.key.repeat || !g_win) break;
            SDL_Scancode k = e.key.keysym.scancode;
            if (k == SDL_SCANCODE_ESCAPE) { if (g_fullscreen) do_action(MI_FULL); else exit(0); break; }
            if (k == SDL_SCANCODE_F11 || (k == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) { do_action(MI_FULL); break; }
            if (k == SDL_SCANCODE_F2) do_action(g_side_by_side ? MI_STACKED : MI_SIDE);
            if (k == SDL_SCANCODE_F3) do_action(MI_SIZE1 + g_scale % 4);
            if (k == SDL_SCANCODE_F4 && g_gl && !g_classic) do_action(MI_RES1 + g_scale3d % 4);
            if (k == SDL_SCANCODE_F5 && g_gl) do_action(g_classic ? MI_ENHANCED : MI_CLASSIC);
            if (k == SDL_SCANCODE_F6) do_action(MI_LCD);
            if (k == SDL_SCANCODE_F7) do_action(MI_SHELL);
            if (k == SDL_SCANCODE_F8) do_action(MI_ACTUAL);
            if (k == SDL_SCANCODE_F12) do_action(MI_SHOT);
            if (k == SDL_SCANCODE_F10) do_action(MI_REPORT);
            if (k == SDL_SCANCODE_P || k == SDL_SCANCODE_PAUSE) do_action(MI_PAUSE);
            break;
        }
        case SDL_MOUSEMOTION:
            /* fullscreen: the menu bar appears while the mouse is at the top edge */
            if (g_fullscreen) {
                if (!g_menu_peek && e.motion.y <= 1) { g_menu_peek = 1; menu_show(1); }
                else if (g_menu_peek && e.motion.y > 80) { g_menu_peek = 0; menu_show(0); }
            }
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED && g_opt.actual) { update_actual(); if (!g_fullscreen) layout_window(); }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && SDL_GameControllerGetJoystick(g_pad) == SDL_JoystickFromInstanceID(e.cdevice.which)) { SDL_GameControllerClose(g_pad); g_pad = NULL; }
            break;
        }
    }
}

void frontend_frame(void) {
    if (g_hidden) { sound_discard(); return; }
    uint64_t t_start = SDL_GetPerformanceCounter();
    if (!g_perf_end) g_perf_end = t_start;
    PerfRec* pr = &g_perf[g_perf_pos % PERF_N];
    pr->frame = g_frame; pr->busy = (float)ticks_ms(t_start - g_perf_end);
    pr->gl = (float)g_gl_stats.render_ms; pr->upload = (float)g_gl_stats.upload_ms; pr->uploads = g_gl_stats.uploads; pr->polys = g_gl_stats.polys;
    memset(&g_gl_stats, 0, sizeof g_gl_stats);
    { extern double g_t2d_ms; static double last; pr->d2 = (float)(g_t2d_ms - last); last = g_t2d_ms; }
    pr->audio_ms = (int)(sound_buffered() * 1000 / 32768);
    handle_events();
    if (g_win) {
        const Uint8* ks = SDL_GetKeyboardState(NULL);
        g_fe_keys = key_bits(ks) | pad_bits();
        static int always_fast = -1; if (always_fast < 0) always_fast = getenv("DQIX_FAST") != NULL;   /* tests */
        g_fast = ks[SDL_SCANCODE_TAB] || always_fast || g_turbo;
        int mx, my;
        Uint32 mb = SDL_GetMouseState(&mx, &my);
        int tx, ty;
        static int dragging;                                  /* the press began on the touch screen */
        int held = (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        g_fe_touch = held && to_touch(mx, my, &tx, &ty, dragging);
        dragging = g_fe_touch;
        if (g_fe_touch) { g_fe_tx = tx; g_fe_ty = ty; }
        uint64_t tp = SDL_GetPerformanceCounter();
        present();
        pr->present = (float)ticks_ms(SDL_GetPerformanceCounter() - tp);
        if (g_paused) {                                       /* Game > Pause: keep the window alive, the game waits */
            update_title(0);
            while (g_paused) { handle_events(); present(); SDL_Delay(15); }
            g_next_frame = SDL_GetPerformanceCounter();
            g_fps_t0 = 0;
        }
    } else g_fe_keys = pad_bits();
    uint64_t t_wait = SDL_GetPerformanceCounter();

    /* pacing */
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t period = (uint64_t)(g_perf_freq / DS_FPS);
    if (g_fast) {
        sound_discard();
        g_next_frame = now;
    } else {
        /* An even beat of one frame per 1/59.83 s from the PC's precise clock.
         * The sound card's clock drifts slightly from it, so the amount of sound
         * queued (averaged, since the card takes it in ~31 ms chunks) nudges the
         * beat by at most 0.5%: too much queued -> slightly slower, too little ->
         * slightly faster. Pacing by the queue directly released frames in pairs. */
        double adjust = 1.0;
        if (g_audio) {
            static double avg = -1;
            double q = (double)sound_buffered();
            avg = avg < 0 ? q : avg + (q - avg) * 0.02;
            double err = (avg - (double)AUDIO_TARGET) / (double)AUDIO_TARGET;
            adjust = 1.0 + fmax(-0.005, fmin(0.005, err * 0.02));
        }
        g_next_frame += (uint64_t)((double)period * adjust);
        if (now > g_next_frame + period * 6) g_next_frame = now;          /* fell far behind: don't try to catch up */
        else {
            while ((now = SDL_GetPerformanceCounter()) < g_next_frame) {
                uint64_t left_us = (g_next_frame - now) * 1000000 / g_perf_freq;
                if (left_us > 2500) SDL_Delay((Uint32)(left_us / 1000 - 2));   /* sleep, then wait out the last ~2 ms precisely */
            }
        }
    }
    if (!g_audio) sound_discard();
    g_perf_end = SDL_GetPerformanceCounter();
    pr->wait = (float)ticks_ms(g_perf_end - t_wait); pr->fast = (uint8_t)g_fast;
    g_perf_pos++;
    g_fps_frames++;
    if (g_win && now - g_fps_t0 > g_perf_freq) {
        update_title(g_fps_frames * (double)g_perf_freq / (double)(now - g_fps_t0));
        g_fps_frames = 0; g_fps_t0 = now;
    }
}

void frontend_shutdown(void) {}
