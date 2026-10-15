/* Bug reports: F10 in the game (or a crash) writes reports/<date-time>/ next to
 * the game with everything needed to replay the session exactly (the folder
 * name starts with the version, e.g. reports/v17-20261014-213000/):
 *   screenshot.bmp  what was on screen
 *   input.txt       every button / touch since launch (DQIX_INPUT_FILE syntax)
 *   boot.sav        the save file as it was at launch (absent if there was none)
 *   now.sav         the save file at the time of the report
 *   info.txt        frame, clock start, reason, build, how to replay
 *   log.txt         the last log lines */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif
#include "nds.h"
#include "frontend.h"
#include "version.h"

void input_write_recording(FILE* f);
int backup_write_boot(FILE* f);
void backup_write_now(FILE* f);

#define LOG_LINES 400
static char* g_log_ring[LOG_LINES];
static unsigned g_log_pos;

void report_log_line(const char* line) {
    unsigned i = g_log_pos++ % LOG_LINES;
    free(g_log_ring[i]);
    g_log_ring[i] = strdup(line);
}

void (*g_report_capture)(uint32_t out[2][256 * 192]);    /* set when the picture is not in g_screen (OpenGL) */

static void write_bmp(const char* path) {
    static uint32_t cap[2][256 * 192];
    const uint32_t (*scr)[256 * 192] = (const uint32_t (*)[256 * 192])g_screen;
    if (g_report_capture) { g_report_capture(cap); scr = (const uint32_t (*)[256 * 192])cap; }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int w = 256, h = 384, row = w * 3;
    uint32_t size = 54 + (uint32_t)(row * h);
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54, dib = 40, img = (uint32_t)(row * h); int32_t ww = w, hh = h; uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4); memcpy(hdr + 18, &ww, 4); memcpy(hdr + 22, &hh, 4);
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2); memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t* line = malloc((size_t)row);
    for (int y = h - 1; y >= 0; y--) {                       /* bottom-up */
        const uint32_t* src = &scr[y / 192][(y % 192) * 256];
        for (int x = 0; x < w; x++) { line[x * 3] = (uint8_t)src[x]; line[x * 3 + 1] = (uint8_t)(src[x] >> 8); line[x * 3 + 2] = (uint8_t)(src[x] >> 16); }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
}

static FILE* open_in(const char* dir, const char* name, const char* mode) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return fopen(path, mode);
}

void (*g_report_perf)(FILE* f);                         /* frontend: recent frame timings */

char g_report_last_dir[300];                            /* the folder of the newest report */

void report_write(const char* reason) {
    static int busy;
    if (busy) return;
    busy = 1;
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char dir[300];
    MKDIR("reports");
    snprintf(dir, sizeof dir, "reports/%s-%04d%02d%02d-%02d%02d%02d", DQIX_VERSION, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    MKDIR(dir);
    char path[600];
    snprintf(path, sizeof path, "%s/screenshot.bmp", dir);
    write_bmp(path);
    FILE* f;
    if ((f = open_in(dir, "input.txt", "w"))) { input_write_recording(f); fclose(f); }
    int had_boot_save = 0;
    if ((f = open_in(dir, "boot.sav", "wb"))) { had_boot_save = backup_write_boot(f); fclose(f); }
    if (!had_boot_save) { snprintf(path, sizeof path, "%s/boot.sav", dir); remove(path); }
    if ((f = open_in(dir, "now.sav", "wb"))) { backup_write_now(f); fclose(f); }
    if ((f = open_in(dir, "info.txt", "w"))) {
        fprintf(f, "reason: %s\n", reason);
        fprintf(f, "frame: %llu\n", (unsigned long long)g_frame);
        fprintf(f, "rtc_base: %lld\n", (long long)rtc_base());
        fprintf(f, "boot_save: %s\n", had_boot_save ? "yes" : "no (new game)");
        fprintf(f, "version: %s (%s)\n", DQIX_VERSION, DQIX_VERSION_DATE);
        fprintf(f, "built: %s %s\n", __DATE__, __TIME__);
        fprintf(f, "\nreplay: copy boot.sav (if any) to a save file, then run\n"
                   "  DQIX_RTC=%lld DQIX_INPUT_FILE=input.txt dqix <rom> <extract> %llu 1 <save file>\n",
                (long long)rtc_base(), (unsigned long long)g_frame + 1);
        fclose(f);
    }
    if ((f = open_in(dir, "log.txt", "w"))) {
        unsigned n = g_log_pos < LOG_LINES ? g_log_pos : LOG_LINES;
        for (unsigned k = n; k > 0; k--) { const char* l = g_log_ring[(g_log_pos - k) % LOG_LINES]; if (l) fputs(l, f); }
        trace_dump(f, 40);
        fclose(f);
    }
    if (g_report_perf && (f = open_in(dir, "perf.txt", "w"))) { g_report_perf(f); fclose(f); }
    fprintf(stderr, "report written to %s\n", dir);
    snprintf(g_report_last_dir, sizeof g_report_last_dir, "%s", dir);
    busy = 0;
}
