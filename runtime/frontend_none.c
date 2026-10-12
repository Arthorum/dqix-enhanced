/* Headless frontend: no window. Audio can be written to a .wav file with
 * DQIX_WAV=path (for testing); otherwise it is discarded. */
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"
#include "frontend.h"

static FILE* g_wav;
static uint32_t g_wav_frames;

static void wav_header(FILE* f, uint32_t frames) {
    uint32_t rate = 32768, bytes = frames * 4;
    uint32_t h[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746D66, 16, 0x00020001, rate, rate * 4, 0x00100004, 0x61746164, bytes };
    fseek(f, 0, SEEK_SET);
    fwrite(h, 4, 11, f);
    fseek(f, 0, SEEK_END);
}

static void wav_close(void) {
    if (!g_wav) return;
    wav_header(g_wav, g_wav_frames);
    fclose(g_wav);
    g_wav = NULL;
}

void frontend_init(int argc, char** argv) {
    (void)argc; (void)argv;
    const char* p = getenv("DQIX_WAV");
    if (p && (g_wav = fopen(p, "wb"))) { wav_header(g_wav, 0); atexit(wav_close); }
}

void frontend_frame(void) {
    int16_t buf[2048 * 2];
    uint32_t n;
    while ((n = sound_read(buf, 2048)) > 0)
        if (g_wav) { fwrite(buf, 4, n, g_wav); g_wav_frames += n; }
}

void frontend_shutdown(void) { wav_close(); }
