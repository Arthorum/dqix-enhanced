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
#include "nds.h"
#include "frontend.h"
#include "gl_render.h"
#include "menu.h"
#include "version.h"

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
} g_opt = { 0, 1, 1, 1, 4, 0, 0, 0, 0, 0, 0 };

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
#define N_SHELLS ((int)(sizeof SHELLS / sizeof SHELLS[0]))
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
    const Shell* sh = &SHELLS[g_opt.shell_color];
    uint32_t cols[4] = { sh->lid, sh->base, sh->btn, sh->hinge };
    float* dst[4] = { v->lid, v->base, v->btn, v->hinge };
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) dst[i][k] = (float)((cols[i] >> (16 - 8 * k)) & 255) / 255.0f;
}

/* ---------------------------------------------------------------- settings.ini
 * Remembers the player's choices between launches (next to the game). */
static const char* SETTINGS = "settings.ini";
static void settings_load(void) {
    FILE* f = fopen(SETTINGS, "r");
    if (!f) return;
    char line[200];
    while (fgets(line, sizeof line, f)) {
        char key[64]; char val[64];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
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
        else if (!strcmp(key, "console_color")) { for (int i = 0; i < N_SHELLS; i++) if (!strcmp(val, SHELLS[i].key)) g_opt.shell_color = i; }
        else if (!strcmp(key, "integer_scaling")) g_opt.integer = atoi(val) != 0;
        else if (!strcmp(key, "actual_size")) g_opt.actual = atoi(val) != 0;
        else if (!strcmp(key, "monitor_width_mm")) g_opt.monitor_width_mm = (float)atof(val);
    }
    fclose(f);
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
    fprintf(f, "# white, black, ice_blue, coral_pink, navy, crimson\n");
    fprintf(f, "screen_gap = %d\n", g_opt.gap);
    fprintf(f, "console = %d\n", g_opt.shell);
    fprintf(f, "console_color = %s\n", SHELLS[g_opt.shell_color].key);
    fprintf(f, "# whole-number scaling only (every DS pixel the same size): 0 or 1\n");
    fprintf(f, "integer_scaling = %d\n", g_opt.integer);
    fprintf(f, "# screens as big as a real DS Lite's (F8): 0 or 1. The size of your monitor is read from the\n");
    fprintf(f, "# monitor itself; if it comes out wrong, put its visible width in millimetres here (0 = automatic)\n");
    fprintf(f, "actual_size = %d\n", g_opt.actual);
    fprintf(f, "monitor_width_mm = %g\n", g_opt.monitor_width_mm);
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

void frontend_init(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();
    g_report_perf = perf_write;
    g_headless = getenv("DQIX_NOWINDOW") != NULL;
    if (!getenv("DQIX_NO_SETTINGS")) settings_load();
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
        const char* names[N_SHELLS];
        for (int i = 0; i < N_SHELLS; i++) names[i] = SHELLS[i].name;
        menu_init(g_win, names, N_SHELLS);
        update_actual();
        menu_sync();
        layout_window();
        if (g_fullscreen) { menu_show(0); SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP); }
        atexit(settings_save);
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

/* Window pixel -> bottom-screen pixel; returns 0 when outside it. */
static int to_touch(int wx, int wy, int* tx, int* ty) {
    float lx, ly;
    GlPresent v;
    if (g_gl) {                                                  /* same layout as gl_present() */
        int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
        canvas_layout(&v, ww, wh);
        lx = ((float)wx + 0.5f - v.ox) / v.sc; ly = ((float)wy + 0.5f - v.oy) / v.sc;
    } else { canvas_layout(&v, 1, 1); SDL_RenderWindowToLogical(g_ren, wx, wy, &lx, &ly); }
    int x = (int)floorf(lx - v.bot[0]), y = (int)floorf(ly - v.bot[1]);
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
static void update_title(double fps) {
    char title[128];
    if (g_paused) snprintf(title, sizeof title, "Dragon Quest IX " DQIX_VERSION " - paused (P to continue)");
    else snprintf(title, sizeof title, "Dragon Quest IX " DQIX_VERSION " - %s%s - %.1f fps%s", g_classic ? "Classic" : "Enhanced",
                  g_gl && g_opt.lcd ? " - DS Lite screen" : "", fps, g_fast ? " (fast)" : "");
    SDL_SetWindowTitle(g_win, title);
}

static void menu_sync(void) {
    int stacked = !g_side_by_side;
    menu_check(MI_PAUSE, g_paused);
    menu_check(MI_TURBO, g_turbo);
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

static int g_menu_peek;          /* fullscreen: the menu bar is showing because the mouse is at the top */

static void do_action(int id) {
    int relayout = 0;
    switch (id) {
    case MI_PAUSE: g_paused = !g_paused; break;
    case MI_TURBO: g_turbo = !g_turbo; break;
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
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Actual size",
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
    case MI_CONTROLS: SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Controls", CONTROLS_TEXT, g_win); break;
    case MI_ABOUT:
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "About",
            "Dragon Quest IX for PC, " DQIX_VERSION " (" DQIX_VERSION_DATE "), built " __DATE__ "\n\n"
            "Classic draws the game exactly like the DS; Enhanced draws its 3D\n"
            "on the graphics card at up to 4x.\n\n"
            "DS Lite screen colours: Pokefan531's measured DS Lite profile.", g_win);
        break;
    default:
        if (id >= MI_SHELLC0 && id < MI_SHELLC0 + N_SHELLS) g_opt.shell_color = id - MI_SHELLC0;
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
        g_fe_touch = (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) && to_touch(mx, my, &tx, &ty);
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
