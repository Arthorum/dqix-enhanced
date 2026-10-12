/* SDL2 frontend: a window with both screens, keyboard / mouse / gamepad
 * input, audio output and pacing to the DS frame rate (~59.83 fps).
 *
 * Keys (DS button <- keyboard):
 *   D-pad <- arrow keys     A <- X      B <- Z      X <- S      Y <- A
 *   L <- Q                  R <- W      START <- Enter         SELECT <- Right Shift / Backspace
 *   touch screen <- left mouse button on the bottom screen
 *   Tab (hold) = fast forward     F11 = fullscreen     F2 = screens side by side / stacked
 *   F3 = window size 1x..4x       F12 = screenshot (dqix-shot-N.bmp)
 *   Esc = quit (the save file is always up to date)
 * A game controller works too (Nintendo layout by position: right face button = A). */
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "frontend.h"

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

static void audio_cb(void* u, Uint8* stream, int len) {
    (void)u;
    int16_t* out = (int16_t*)stream;
    uint32_t frames = (uint32_t)len / 4;
    uint32_t got = sound_read(out, frames);
    if (got < frames) {                                   /* underrun: hold the last sample to avoid clicks */
        int16_t l = got ? out[(got - 1) * 2] : 0, r = got ? out[(got - 1) * 2 + 1] : 0;
        for (uint32_t i = got; i < frames; i++) { out[i * 2] = l; out[i * 2 + 1] = r; }
    }
}

static void layout_window(void) {
    int w = g_side_by_side ? 512 : 256, h = g_side_by_side ? 192 : 384;
    SDL_RenderSetLogicalSize(g_ren, w, h);
    if (!g_fullscreen) SDL_SetWindowSize(g_win, w * g_scale, h * g_scale);
}

void frontend_init(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();
    g_headless = getenv("DQIX_NOWINDOW") != NULL;
    if (SDL_Init((g_headless ? 0 : SDL_INIT_VIDEO) | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(1);
    }
    atexit(SDL_Quit);
    if (!g_headless) {
        const char* sc = getenv("DQIX_SCALE");
        if (sc) { g_scale = atoi(sc); if (g_scale < 1) g_scale = 1; if (g_scale > 8) g_scale = 8; }
        g_win = SDL_CreateWindow("Dragon Quest IX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 256 * g_scale, 384 * g_scale,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!g_win) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); exit(1); }
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
        if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 384);
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
    SDL_RenderWindowToLogical(g_ren, wx, wy, &lx, &ly);
    int x = (int)lx, y = (int)ly;
    if (g_side_by_side) { x -= 256; } else { y -= 192; }
    if (x < 0 || x > 255 || y < 0 || y > 191) return 0;
    *tx = x; *ty = y;
    return 1;
}

static int g_shot_req;
static void save_screenshot(void) {
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

static void present(void) {
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
    static long long shot_at = -2;
    if (shot_at == -2) { const char* e = getenv("DQIX_SHOT_AT"); shot_at = e ? atoll(e) : -1; }
    if (g_shot_req || (shot_at >= 0 && g_frame == (uint64_t)shot_at)) { save_screenshot(); g_shot_req = 0; }
    SDL_RenderPresent(g_ren);
}

void frontend_frame(void) {
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
        g_fast = ks[SDL_SCANCODE_TAB];
        int mx, my;
        Uint32 mb = SDL_GetMouseState(&mx, &my);
        int tx, ty;
        g_fe_touch = (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) && to_touch(mx, my, &tx, &ty);
        if (g_fe_touch) { g_fe_tx = tx; g_fe_ty = ty; }
        present();
    } else g_fe_keys = pad_bits();

    /* pacing */
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t period = (uint64_t)(g_perf_freq / DS_FPS);
    if (g_fast) {
        sound_discard();
        g_next_frame = now;
    } else {
        g_next_frame += period;
        if (now > g_next_frame + period * 6) g_next_frame = now;          /* fell far behind: don't try to catch up */
        else while ((now = SDL_GetPerformanceCounter()) < g_next_frame) {
            uint64_t left_ms = (g_next_frame - now) * 1000 / g_perf_freq;
            if (left_ms > 1) SDL_Delay((Uint32)(left_ms - 1));
        }
        /* keep audio latency bounded (~100 ms) */
        uint32_t buffered = sound_buffered();
        if (buffered > 32768 / 4) { int16_t tmp[512]; while (sound_buffered() > 32768 / 10) sound_read(tmp, 256); }
    }
    if (!g_audio) sound_discard();
    g_fps_frames++;
    if (g_win && now - g_fps_t0 > g_perf_freq) {
        char title[96];
        snprintf(title, sizeof title, "Dragon Quest IX - %.1f fps%s", g_fps_frames * (double)g_perf_freq / (double)(now - g_fps_t0), g_fast ? " (fast)" : "");
        SDL_SetWindowTitle(g_win, title);
        g_fps_frames = 0; g_fps_t0 = now;
    }
}

void frontend_shutdown(void) {}
