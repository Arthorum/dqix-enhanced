/* SDL2 frontend: a window with both screens, keyboard / mouse / gamepad
 * input, audio output and pacing to the DS frame rate (~59.83 fps).
 *
 * Keys (DS button <- keyboard):
 *   D-pad <- arrow keys     A <- X      B <- Z      X <- S      Y <- A
 *   L <- Q                  R <- W      START <- Enter         SELECT <- Right Shift / Backspace
 *   touch screen <- left mouse button on the bottom screen
 *   Tab (hold) = fast forward     F11 = fullscreen     F2 = screens side by side / stacked
 *   F3 = window size 1x..4x       F4 = 3D resolution 1x..4x (OpenGL)   F12 = screenshot (dqix-shot-N.bmp)
 *   F10 = bug report (reports/<date-time>/: screenshot, inputs, saves, log)
 *   Esc = quit (the save file is always up to date)
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
extern int g_comp_gl;
extern void (*g_report_capture)(uint32_t out[2][256 * 192]);
extern void (*g_report_perf)(FILE* f);
static void perf_write(FILE* f);

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

static void layout_window(void) {
    int w = g_side_by_side ? 512 : 256, h = g_side_by_side ? 192 : 384;
    if (!g_gl) SDL_RenderSetLogicalSize(g_ren, w, h);
    if (!g_fullscreen) SDL_SetWindowSize(g_win, w * g_scale, h * g_scale);
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
    g_renderer3d = gl_renderer3d();
    g_renderer3d_verify = getenv("DQIX_GL_VERIFY") != NULL;
    g_comp_gl = 1;
    g_report_capture = gl_read_screens;
    return 1;
}

void frontend_init(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();
    g_report_perf = perf_write;
    g_headless = getenv("DQIX_NOWINDOW") != NULL;
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
        layout_window();
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
    if (ks[SDL_SCANCODE_RETURN]) k |= 1 << 3;                      /* START */
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
    if (g_gl) {                                                  /* same fit as gl_present() */
        int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
        float cw = g_side_by_side ? 512.0f : 256.0f, ch = g_side_by_side ? 192.0f : 384.0f;
        float sc = fminf((float)ww / cw, (float)wh / ch);
        lx = (wx - ((float)ww - cw * sc) / 2) / sc; ly = (wy - ((float)wh - ch * sc) / 2) / sc;
    } else SDL_RenderWindowToLogical(g_ren, wx, wy, &lx, &ly);
    int x = (int)lx, y = (int)ly;
    if (g_side_by_side) { x -= 256; } else { y -= 192; }
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
    fprintf(f, "renderer: %s\n", g_gl ? gl_renderer_name() : "software (no OpenGL 3.3)");
    if (g_gl) fprintf(f, "3D resolution: %dx\n", gl_render_scale());
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

static void present_gl(void) {
    uint16_t pow; memcpy(&pow, &g_io[0x304], 2);
    gl_compose((pow >> 15) & 1, pow & 1);
    gl_dump_hook();
    test_hooks();
    int w, h; SDL_GL_GetDrawableSize(g_win, &w, &h);
    gl_present(w, h, g_side_by_side, 0);
    SDL_GL_SwapWindow(g_win);
}

static void present(void) {
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

void frontend_frame(void) {
    uint64_t t_start = SDL_GetPerformanceCounter();
    if (!g_perf_end) g_perf_end = t_start;
    PerfRec* pr = &g_perf[g_perf_pos % PERF_N];
    pr->frame = g_frame; pr->busy = (float)ticks_ms(t_start - g_perf_end);
    pr->gl = (float)g_gl_stats.render_ms; pr->upload = (float)g_gl_stats.upload_ms; pr->uploads = g_gl_stats.uploads; pr->polys = g_gl_stats.polys;
    memset(&g_gl_stats, 0, sizeof g_gl_stats);
    { extern double g_t2d_ms; static double last; pr->d2 = (float)(g_t2d_ms - last); last = g_t2d_ms; }
    pr->audio_ms = (int)(sound_buffered() * 1000 / 32768);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: exit(0);
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) exit(0);
            if (e.key.keysym.scancode == SDL_SCANCODE_F11 && g_win) {
                g_fullscreen = !g_fullscreen;
                SDL_SetWindowFullscreen(g_win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F2 && g_win) { g_side_by_side = !g_side_by_side; layout_window(); }
            if (e.key.keysym.scancode == SDL_SCANCODE_F3 && g_win) { g_scale = g_scale % 4 + 1; layout_window(); }
            if (e.key.keysym.scancode == SDL_SCANCODE_F12) g_shot_req = 1;
            if (e.key.keysym.scancode == SDL_SCANCODE_F4 && g_gl) { g_scale3d = g_scale3d % 4 + 1; gl_render_set_scale(g_scale3d); }
            if (e.key.keysym.scancode == SDL_SCANCODE_F10) {
                report_write("F10 pressed");
                if (g_win) SDL_SetWindowTitle(g_win, "Dragon Quest IX - report saved (see the reports folder)");
                g_fps_t0 = SDL_GetPerformanceCounter();           /* keep that title up for a second */
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && SDL_GameControllerGetJoystick(g_pad) == SDL_JoystickFromInstanceID(e.cdevice.which)) { SDL_GameControllerClose(g_pad); g_pad = NULL; }
            break;
        }
    }
    if (g_win) {
        const Uint8* ks = SDL_GetKeyboardState(NULL);
        g_fe_keys = key_bits(ks) | pad_bits();
        static int always_fast = -1; if (always_fast < 0) always_fast = getenv("DQIX_FAST") != NULL;   /* tests */
        g_fast = ks[SDL_SCANCODE_TAB] || always_fast;
        int mx, my;
        Uint32 mb = SDL_GetMouseState(&mx, &my);
        int tx, ty;
        g_fe_touch = (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) && to_touch(mx, my, &tx, &ty);
        if (g_fe_touch) { g_fe_tx = tx; g_fe_ty = ty; }
        uint64_t tp = SDL_GetPerformanceCounter();
        present();
        pr->present = (float)ticks_ms(SDL_GetPerformanceCounter() - tp);
    } else g_fe_keys = pad_bits();
    uint64_t t_wait = SDL_GetPerformanceCounter();

    /* pacing */
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t period = (uint64_t)(g_perf_freq / DS_FPS);
    if (g_fast) {
        sound_discard();
        g_next_frame = now;
    } else if (g_audio) {
        /* follow the sound card's clock: wait while enough audio is queued */
        uint64_t give_up = now + g_perf_freq / 5;
        while (sound_buffered() > AUDIO_TARGET && (now = SDL_GetPerformanceCounter()) < give_up) SDL_Delay(1);
        g_next_frame = now;
    } else {
        g_next_frame += period;
        if (now > g_next_frame + period * 6) g_next_frame = now;          /* fell far behind: don't try to catch up */
        else while ((now = SDL_GetPerformanceCounter()) < g_next_frame) {
            uint64_t left_ms = (g_next_frame - now) * 1000 / g_perf_freq;
            if (left_ms > 1) SDL_Delay((Uint32)(left_ms - 1));
        }
    }
    if (!g_audio) sound_discard();
    g_perf_end = SDL_GetPerformanceCounter();
    pr->wait = (float)ticks_ms(g_perf_end - t_wait); pr->fast = (uint8_t)g_fast;
    g_perf_pos++;
    g_fps_frames++;
    if (g_win && now - g_fps_t0 > g_perf_freq) {
        char title[96];
        snprintf(title, sizeof title, "Dragon Quest IX - %.1f fps%s", g_fps_frames * (double)g_perf_freq / (double)(now - g_fps_t0), g_fast ? " (fast)" : "");
        SDL_SetWindowTitle(g_win, title);
        g_fps_frames = 0; g_fps_t0 = now;
    }
}

void frontend_shutdown(void) {}
