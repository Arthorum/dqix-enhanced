/* Playing with friends far away: lobby, lockstep inputs over TCP, the hidden
 * consoles (see session.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "nds.h"
#include "air.h"
#include "session.h"
#include "version.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
typedef SOCKET sock_t;
#define BADSOCK INVALID_SOCKET
#define closesock closesocket
static CRITICAL_SECTION g_mx;
static void mx_init(void) { InitializeCriticalSection(&g_mx); }
static void lock(void) { EnterCriticalSection(&g_mx); }
static void unlock(void) { LeaveCriticalSection(&g_mx); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static int sock_err_again(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void make_dir(const char* p) { _mkdir(p); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define BADSOCK (-1)
#define closesock close
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static void mx_init(void) {}
static void lock(void) { pthread_mutex_lock(&g_mx); }
static void unlock(void) { pthread_mutex_unlock(&g_mx); }
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static int sock_err_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
static void make_dir(const char* p) { mkdir(p, 0755); }
#endif

void (*session_lobby_hook)(void);

/* message types */
enum { M_HELLO = 1, M_WELCOME, M_LOBBY, M_START, M_INPUT, M_HASH, M_LEAVE, M_PING, M_PONG, M_REFUSE };
#define MAGIC "DQIXMP01"
#define HASH_EVERY 120

typedef struct {
    sock_t s;
    uint8_t* in; size_t in_len, in_cap;
    uint8_t* out; size_t out_len, out_cap;
    int index;                      /* console number (host side: the guest's) */
    int hello;                      /* host: its HELLO arrived */
    double ping_sent;
} Peer;

typedef struct {
    char name[32];
    uint8_t* save; uint32_t save_len;
    uint8_t* fw; uint32_t fw_len;
} Console;

static int g_hidden, g_running, g_requested, g_host, g_me, g_n = 1, g_delay = 4;
static int g_state;                 /* SessionInfo.state */
static int g_start_req, g_cancel_req, g_auto_players;
static Console g_con[SESSION_MAX];
static int g_ping[SESSION_MAX], g_gone[SESSION_MAX];
static char g_addr[128], g_msg[256], g_name[32] = "Player";
static int g_desync;
static Peer g_peer[SESSION_MAX];    /* host: guests 1..3 (by slot); guest: [0] = the host */
static sock_t g_listen = BADSOCK;
static int64_t g_rtc;
static char g_rom_path[1024], g_extract_path[1024], g_shm[64];
static uint64_t g_sent_frames;      /* my inputs sent so far */
static uint64_t g_hash_next = HASH_EVERY;   /* my next fingerprint to send */
typedef struct { int k; uint64_t frame, hash; } PendingHash;
static PendingHash g_pending[64];
static int g_npending;

int session_hidden(void) { return g_hidden; }
int session_running(void) { return g_running; }
int session_console(void) { return g_me; }
int session_players(void) { return g_n; }
int session_requested(void) { return g_requested || g_hidden; }

static void news(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    lock(); vsnprintf(g_msg, sizeof g_msg, fmt, ap); unlock();
    va_end(ap);
    char line[300]; snprintf(line, sizeof line, "session: %s\n", g_msg);
    LOG_WARN("%s", line);
    report_log_line(line);
}

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#endif
}

/* ---------------------------------------------------------------- packing */
uint32_t session_pack(uint16_t keys, int touch, int x, int y) {
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    if (y < 0) y = 0;
    if (y > 255) y = 255;
    return (uint32_t)(keys & 0x1FFF) | (touch ? 1u << 13 : 0) | (touch ? (uint32_t)x << 14 | (uint32_t)y << 22 : 0);
}
void session_unpack(uint32_t v, uint16_t* keys, int* touch, int* x, int* y) {
    *keys = (uint16_t)(v & 0x1FFF);
    *touch = (v >> 13) & 1;
    if (*touch) { *x = (int)((v >> 14) & 0xFF); *y = (int)((v >> 22) & 0xFF); }
}

/* ---------------------------------------------------------------- messages */
static void put_bytes(Peer* p, const void* d, size_t n) {
    if (p->s == BADSOCK) return;
    if (p->out_len + n > p->out_cap) { p->out_cap = (p->out_len + n) * 2 + 4096; p->out = realloc(p->out, p->out_cap); }
    memcpy(p->out + p->out_len, d, n); p->out_len += n;
}
typedef struct { uint8_t* d; size_t n, cap; } Buf;
static void b_put(Buf* b, const void* d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 256; b->d = realloc(b->d, b->cap); }
    memcpy(b->d + b->n, d, n); b->n += n;
}
static void b_u8(Buf* b, uint8_t v) { b_put(b, &v, 1); }
static void b_u32(Buf* b, uint32_t v) { uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; b_put(b, t, 4); }
static void b_u64(Buf* b, uint64_t v) { b_u32(b, (uint32_t)v); b_u32(b, (uint32_t)(v >> 32)); }
static void b_str(Buf* b, const char* s, size_t n) { char t[128] = { 0 }; snprintf(t, sizeof t, "%s", s); b_put(b, t, n); }
static void send_msg(Peer* p, int type, const Buf* b) {
    uint8_t h[5] = { 0 };
    uint32_t n = b ? (uint32_t)b->n : 0;
    h[0] = (uint8_t)n; h[1] = (uint8_t)(n >> 8); h[2] = (uint8_t)(n >> 16); h[3] = (uint8_t)(n >> 24); h[4] = (uint8_t)type;
    put_bytes(p, h, 5);
    if (n) put_bytes(p, b->d, n);
}
typedef struct { const uint8_t* d; size_t n, pos; int bad; } Rd;
static const uint8_t* r_get(Rd* r, size_t n) { if (r->pos + n > r->n) { r->bad = 1; return NULL; } const uint8_t* p = r->d + r->pos; r->pos += n; return p; }
static uint8_t r_u8(Rd* r) { const uint8_t* p = r_get(r, 1); return p ? p[0] : 0; }
static uint32_t r_u32(Rd* r) { const uint8_t* p = r_get(r, 4); return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0; }
static uint64_t r_u64(Rd* r) { uint64_t lo = r_u32(r); return lo | (uint64_t)r_u32(r) << 32; }
static void r_str(Rd* r, char* out, size_t n) { const uint8_t* p = r_get(r, n); if (p) { memcpy(out, p, n); out[n - 1] = 0; } else out[0] = 0; }
static uint8_t* r_blob(Rd* r, uint32_t* len) {
    uint32_t n = r_u32(r);
    const uint8_t* p = r_get(r, n);
    if (!p) return NULL;
    uint8_t* m = malloc(n ? n : 1); memcpy(m, p, n); *len = n; return m;
}

static void peer_close(Peer* p) {
    if (p->s != BADSOCK) closesock(p->s);
    p->s = BADSOCK;
    p->in_len = p->out_len = 0;
}

/* ---------------------------------------------------------------- sockets */
static void sock_nonblock(sock_t s) {
#ifdef _WIN32
    u_long one = 1; ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int one1 = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one1, sizeof one1);
}

static int net_up(void) {
#ifdef _WIN32
    static int done; if (done) return 1;
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
    done = 1;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    return 1;
}

/* flush what's queued; read what's there. -1 = the connection is gone */
static int peer_io(Peer* p) {
    if (p->s == BADSOCK) return -1;
    while (p->out_len) {
        int n = (int)send(p->s, (const char*)p->out, (int)(p->out_len > 65536 ? 65536 : p->out_len), 0);
        if (n < 0) { if (sock_err_again()) break; return -1; }
        memmove(p->out, p->out + n, p->out_len - (size_t)n); p->out_len -= (size_t)n;
    }
    for (;;) {
        if (p->in_len + 65536 > p->in_cap) { p->in_cap = p->in_len + 65536 * 2; p->in = realloc(p->in, p->in_cap); }
        int n = (int)recv(p->s, (char*)p->in + p->in_len, 65536, 0);
        if (n == 0) return -1;
        if (n < 0) { if (sock_err_again()) break; return -1; }
        p->in_len += (size_t)n;
    }
    return 0;
}
/* the next whole message, or 0 */
static int peer_msg(Peer* p, int* type, Rd* r) {
    if (p->in_len < 5) return 0;
    uint32_t n = (uint32_t)p->in[0] | (uint32_t)p->in[1] << 8 | (uint32_t)p->in[2] << 16 | (uint32_t)p->in[3] << 24;
    if (p->in_len < 5 + (size_t)n) return 0;
    *type = p->in[4];
    r->d = p->in + 5; r->n = n; r->pos = 0; r->bad = 0;
    return 1;
}
static void peer_consume(Peer* p, const Rd* r) {
    size_t used = 5 + r->n;
    memmove(p->in, p->in + used, p->in_len - used); p->in_len -= used;
}

/* ---------------------------------------------------------------- the console data */
static void my_console(Console* c) {
    snprintf(c->name, sizeof c->name, "%s", g_name);
    uint32_t n;
    const uint8_t* s = backup_image(&n);
    c->save = malloc(n); memcpy(c->save, s, n); c->save_len = n;
    const uint8_t* f = firmware_image(&n);
    c->fw = malloc(n); memcpy(c->fw, f, n); c->fw_len = n;
}
static void put_console(Buf* b, const Console* c) {
    b_str(b, c->name, 32);
    b_u32(b, c->save_len); b_put(b, c->save, c->save_len);
    b_u32(b, c->fw_len); b_put(b, c->fw, c->fw_len);
}
static void get_console(Rd* r, Console* c) {
    r_str(r, c->name, 32);
    c->save = r_blob(r, &c->save_len);
    c->fw = r_blob(r, &c->fw_len);
}

/* ---------------------------------------------------------------- the lobby (host) */
static void host_send_lobby(void) {
    Buf b = { 0 };
    b_u8(&b, (uint8_t)g_n);
    for (int k = 0; k < g_n; k++) { b_str(&b, g_con[k].name, 32); b_u32(&b, (uint32_t)g_ping[k]); }
    for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && g_peer[i].hello) send_msg(&g_peer[i], M_LOBBY, &b);
    free(b.d);
}

static void host_accept(void) {
    if (g_listen == BADSOCK) return;
    for (;;) {
        sock_t s = accept(g_listen, NULL, NULL);
        if (s == BADSOCK) return;
        sock_nonblock(s);
        int slot = -1;
        for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s == BADSOCK) { slot = i; break; }
        if (slot < 0 || g_state != 2) {
            Peer tmp; memset(&tmp, 0, sizeof tmp); tmp.s = s; Buf b = { 0 }; b_str(&b, slot < 0 ? "The session is full (4 players)." : "The session has already started.", 96);
            send_msg(&tmp, M_REFUSE, &b); peer_io(&tmp); free(b.d); free(tmp.out); free(tmp.in); closesock(s);
            continue;
        }
        memset(&g_peer[slot], 0, sizeof g_peer[slot]);
        g_peer[slot].s = s; g_peer[slot].index = -1;
    }
}

static void host_hello(Peer* p, Rd* r) {
    char magic[9] = { 0 }; const uint8_t* m = r_get(r, 8); if (m) memcpy(magic, m, 8);
    char ver[32]; r_str(r, ver, 32);
    Console c = { 0 };
    get_console(r, &c);
    Buf b = { 0 };
    if (r->bad || strcmp(magic, MAGIC)) { b_str(&b, "That isn't a Dragon Quest IX PC session.", 96); send_msg(p, M_REFUSE, &b); free(b.d); peer_io(p); peer_close(p); return; }
    if (strcmp(ver, DQIX_VERSION)) {
        char t[96]; snprintf(t, sizeof t, "Different versions: the host has %s, you have %s. Both need the same.", DQIX_VERSION, ver);
        b_str(&b, t, 96); send_msg(p, M_REFUSE, &b); free(b.d); peer_io(p); peer_close(p);
        news("%s tried to join with a different version (%s)", c.name, ver);
        return;
    }
    lock();
    int k = g_n++;
    g_con[k] = c; p->index = k; p->hello = 1; g_ping[k] = 0;
    unlock();
    b_u8(&b, (uint8_t)k);
    send_msg(p, M_WELCOME, &b); free(b.d);
    news("%s joined (player %d)", c.name, k + 1);
    host_send_lobby();
}

/* ---------------------------------------------------------------- starting */
static int compute_delay(void) {
    const char* e = getenv("DQIX_DELAY");
    if (e) return atoi(e) < 1 ? 1 : atoi(e) > 30 ? 30 : atoi(e);
    int worst = 0;
    for (int k = 0; k < g_n; k++) if (g_ping[k] > worst) worst = g_ping[k];
    /* a press reaches the furthest PC in about one round trip (via the host), plus a margin */
    int d = (int)((worst + 25) / 16.7) + 2;
    return d < 3 ? 3 : d > 20 ? 20 : d;
}

static void write_file(const char* path, const uint8_t* d, uint32_t n) {
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}

#ifdef _WIN32
static HANDLE g_job;
/* our environment, one "NAME=value" per entry */
static char** env_list(void) {
    static char** list;
    if (list) return list;
    char* b = GetEnvironmentStringsA();
    int n = 0; for (char* q = b; *q; q += strlen(q) + 1) n++;
    list = calloc((size_t)n + 1, sizeof(char*));
    int i = 0; for (char* q = b; *q; q += strlen(q) + 1) list[i++] = q;
    return list;
}
#else
extern char** environ;
static char** env_list(void) { return environ; }
#endif

/* another player's console on this PC: the same program, no window */
static void spawn_hidden(int k) {
    char save[300], fw[300], log[300];
    snprintf(save, sizeof save, "session/console%d.sav", k + 1);
    snprintf(fw, sizeof fw, "session/console%d.fw", k + 1);
    snprintf(log, sizeof log, "session/console%d.log", k + 1);
    write_file(save, g_con[k].save, g_con[k].save_len);
    write_file(fw, g_con[k].fw, g_con[k].fw_len);
    /* environment: ours without DQIX_* (tests' scripts, dumps), plus the session's */
    char extra[8][200];
    snprintf(extra[0], 200, "DQIX_HIDDEN=1");
    snprintf(extra[1], 200, "DQIX_SESSION_SHM=%s", g_shm);
    snprintf(extra[2], 200, "DQIX_CONSOLE=%d", k);
    snprintf(extra[3], 200, "DQIX_COUNT=%d", g_n);
    snprintf(extra[4], 200, "DQIX_RTC=%lld", (long long)g_rtc);
    snprintf(extra[5], 200, "DQIX_FW_FILE=%s", fw);
    snprintf(extra[6], 200, "DQIX_NO_SETTINGS=1");
    snprintf(extra[7], 200, "DQIX_NOWINDOW=1");
    char** cur = env_list();
    int ne = 0; for (char** e = cur; *e; e++) ne++;
    char** env = calloc((size_t)ne + 10, sizeof(char*));
    int m = 0;
    for (char** e = cur; *e; e++) if (strncmp(*e, "DQIX_", 5) && **e != '=') env[m++] = *e;
    for (int i = 0; i < 8; i++) env[m++] = extra[i];
    env[m] = NULL;
#ifdef _WIN32
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char cmd[4096]; snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" \"%s\" 0 1 \"%s\"", exe, g_rom_path, g_extract_path, save);
    size_t total = 1; for (int i = 0; i < m; i++) total += strlen(env[i]) + 1;
    char* block = malloc(total); char* q = block;
    for (int i = 0; i < m; i++) { size_t l = strlen(env[i]) + 1; memcpy(q, env[i], l); q += l; }
    *q = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE hlog = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = hlog; si.hStdError = hlog; si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    if (!g_job) {
        g_job = CreateJobObjectA(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, block, NULL, &si, &pi)) {
        AssignProcessToJobObject(g_job, pi.hProcess);
        SetPriorityClass(pi.hProcess, ABOVE_NORMAL_PRIORITY_CLASS);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else news("could not start player %d's console (error %lu)", k + 1, GetLastError());
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    free(block);
#else
    pid_t pid = fork();
    if (pid == 0) {
        FILE* f = freopen(log, "w", stderr); (void)f;
        f = freopen(log, "a", stdout); (void)f;
        char self[1024]; ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1); self[l > 0 ? l : 0] = 0;
        char* args[] = { self, g_rom_path, g_extract_path, (char*)"0", (char*)"1", save, NULL };
        execve(self, args, env);
        _exit(127);
    }
#endif
    free(env);
}

#ifndef _WIN32
static void kill_children(void) { signal(SIGTERM, SIG_IGN); kill(0, SIGTERM); }
#endif

static void do_start(void) {
    make_dir("session");
    snprintf(g_shm, sizeof g_shm, "%08x%04x", (unsigned)(g_rtc ^ (int64_t)(now_ms() * 1000)), (unsigned)rand() & 0xFFFF);
    if (!g_host) { /* the host chose the name */ }
    rtc_set_base(g_rtc);
    if (!air_init(g_shm, g_me, g_n)) { news("could not set up the shared memory"); g_state = 5; return; }
    for (int k = 0; k < g_n; k++) for (int f = 0; f < g_delay; f++) air_input_put(k, (uint64_t)f, 0);
#ifndef _WIN32
    setpgid(0, 0); atexit(kill_children);
#endif
    for (int k = 0; k < g_n; k++) if (k != g_me) spawn_hidden(k);
    g_running = 1;
    lock(); g_state = 3; unlock();
    news("started: %d players, input delay %d frames (%d ms)", g_n, g_delay, (int)(g_delay * 16.7));
}

static void host_start(void) {
    g_delay = compute_delay();
    Buf b = { 0 };
    b_u8(&b, (uint8_t)g_n); b_u8(&b, (uint8_t)g_delay); b_u64(&b, (uint64_t)g_rtc);
    char shm[64]; snprintf(shm, sizeof shm, "%s", ""); (void)shm;
    for (int k = 0; k < g_n; k++) put_console(&b, &g_con[k]);
    for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && g_peer[i].hello) send_msg(&g_peer[i], M_START, &b);
    free(b.d);
    /* push it out before the game starts */
    double t0 = now_ms();
    for (;;) {
        int pending = 0;
        for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK) { if (peer_io(&g_peer[i]) < 0) peer_close(&g_peer[i]); else if (g_peer[i].out_len) pending = 1; }
        if (!pending || now_ms() - t0 > 20000) break;
        sleep_ms(2);
    }
    do_start();
}

/* ---------------------------------------------------------------- running */
static void console_left(int k, uint64_t last) {
    if (k < 0 || k >= g_n || g_gone[k]) return;
    g_gone[k] = 1;
    air_input_put(k, last, AIR_INPUT_END);
    news("%s left (their DS is switched off)", g_con[k].name);
}

static void check_hashes(void) {
    for (int i = 0; i < g_npending;) {
        uint64_t mine;
        if (air_hash_get(g_pending[i].k, g_pending[i].frame, &mine)) {
            static int matched; if (mine == g_pending[i].hash && ++matched % 10 == 1) LOG_INFO("session: in sync with %s at frame %llu (%d checks)\n", g_con[g_pending[i].k].name, (unsigned long long)g_pending[i].frame, matched);
            if (getenv("DQIX_SYNCLOG")) fprintf(stderr, "sync %d %llu %s\n", g_pending[i].k, (unsigned long long)g_pending[i].frame, mine == g_pending[i].hash ? "ok" : "MISMATCH");
            if (mine != g_pending[i].hash && !g_desync) {
                g_desync = 1;
                news("OUT OF SYNC with %s at frame %llu: the PCs no longer agree. Leave and start a new session.",
                     g_con[g_pending[i].k].name, (unsigned long long)g_pending[i].frame);
            }
            g_pending[i] = g_pending[--g_npending];
        } else i++;
    }
}

static void got_input(int from_peer, Rd* r) {
    int k = r_u8(r);
    uint64_t first = r_u32(r);
    uint32_t n = r_u32(r);
    if (r->bad || k < 0 || k >= g_n || k == g_me) return;
    for (uint32_t i = 0; i < n; i++) { uint32_t v = r_u32(r); if (r->bad) return; air_input_put(k, first + i, v); }
    if (g_host) {                                                  /* pass it on to the others */
        Buf b = { 0 }; b_put(&b, r->d, r->n);
        for (int i = 1; i < SESSION_MAX; i++) if (i != from_peer && g_peer[i].s != BADSOCK && g_peer[i].hello) send_msg(&g_peer[i], M_INPUT, &b);
        free(b.d);
    }
}

static void got_hash(int from_peer, Rd* r) {
    int k = r_u8(r); uint64_t frame = r_u64(r), h = r_u64(r);
    if (r->bad || k < 0 || k >= g_n) return;
    if (k != g_me && g_npending < 64) { g_pending[g_npending].k = k; g_pending[g_npending].frame = frame; g_pending[g_npending].hash = h; g_npending++; }
    if (g_host) {
        Buf b = { 0 }; b_put(&b, r->d, r->n);
        for (int i = 1; i < SESSION_MAX; i++) if (i != from_peer && g_peer[i].s != BADSOCK && g_peer[i].hello) send_msg(&g_peer[i], M_HASH, &b);
        free(b.d);
    }
}

/* my new inputs and fingerprints go out */
static void send_mine(void) {
    uint64_t have = air_input_count(g_me);
    if (have > g_sent_frames) {
        Buf b = { 0 };
        uint32_t n = (uint32_t)(have - g_sent_frames);
        if (n > 4096) n = 4096;
        b_u8(&b, (uint8_t)g_me); b_u32(&b, (uint32_t)g_sent_frames); b_u32(&b, n);
        for (uint32_t i = 0; i < n; i++) { uint32_t v = 0; air_input_peek(g_me, g_sent_frames + i, &v); b_u32(&b, v); }
        for (int i = 0; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && (g_host ? i > 0 && g_peer[i].hello : i == 0)) send_msg(&g_peer[i], M_INPUT, &b);
        free(b.d);
        g_sent_frames += n;
    }
    uint64_t h;
    while (air_hash_get(g_me, g_hash_next, &h)) {
        Buf b = { 0 }; b_u8(&b, (uint8_t)g_me); b_u64(&b, g_hash_next); b_u64(&b, h);
        for (int i = 0; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && (g_host ? i > 0 && g_peer[i].hello : i == 0)) send_msg(&g_peer[i], M_HASH, &b);
        free(b.d);
        g_hash_next += HASH_EVERY;
    }
}

/* ---------------------------------------------------------------- the network thread */
static void handle(int i, int type, Rd* r) {
    Peer* p = &g_peer[i];
    switch (type) {
    case M_HELLO: if (g_host && !p->hello) host_hello(p, r); break;
    case M_WELCOME: g_me = r_u8(r); lock(); g_state = 2; unlock(); news("connected: you are player %d. Waiting for the host to start.", g_me + 1); break;
    case M_LOBBY: {
        int n = r_u8(r);
        lock();
        if (n >= 1 && n <= SESSION_MAX) { g_n = n; for (int k = 0; k < n; k++) { r_str(r, g_con[k].name, 32); g_ping[k] = (int)r_u32(r); } }
        unlock();
        break;
    }
    case M_START: {
        int n = r_u8(r), d = r_u8(r); int64_t rtc = (int64_t)r_u64(r);
        if (n < 1 || n > SESSION_MAX) break;
        lock();
        g_n = n; g_delay = d; g_rtc = rtc;
        for (int k = 0; k < n; k++) get_console(r, &g_con[k]);
        unlock();
        if (r->bad) { news("the start message was damaged"); g_state = 5; break; }
        do_start();
        break;
    }
    case M_INPUT: if (g_running) got_input(i, r); break;
    case M_HASH: if (g_running) got_hash(i, r); break;
    case M_LEAVE: { int k = r_u8(r); uint64_t last = r_u64(r); if (g_running) console_left(k, last); break; }
    case M_PING: { Buf b = { 0 }; b_put(&b, r->d, r->n); send_msg(p, M_PONG, &b); free(b.d); break; }
    case M_PONG: { double t = now_ms(); if (p->index > 0 && p->index < SESSION_MAX) { lock(); g_ping[p->index] = (int)(t - p->ping_sent); unlock(); } break; }
    case M_REFUSE: { char why[96]; r_str(r, why, 96); news("%s", why); lock(); g_state = 5; unlock(); peer_close(p); break; }
    }
}

static void peer_lost(int i) {
    Peer* p = &g_peer[i];
    int k = p->index;
    peer_close(p);
    if (g_host) {
        if (k <= 0) return;
        if (g_running) {
            uint64_t last = air_input_count(k);
            console_left(k, last);
            Buf b = { 0 }; b_u8(&b, (uint8_t)k); b_u64(&b, last);
            for (int j = 1; j < SESSION_MAX; j++) if (g_peer[j].s != BADSOCK && g_peer[j].hello) send_msg(&g_peer[j], M_LEAVE, &b);
            free(b.d);
        } else {                                                   /* left the lobby: the others move up */
            lock();
            for (int j = k; j + 1 < g_n; j++) { g_con[j] = g_con[j + 1]; g_ping[j] = g_ping[j + 1]; }
            g_n--;
            for (int j = 1; j < SESSION_MAX; j++) if (g_peer[j].index > k) g_peer[j].index--;
            unlock();
            news("a player left the lobby");
            /* tell everyone their new numbers */
            for (int j = 1; j < SESSION_MAX; j++) if (g_peer[j].s != BADSOCK && g_peer[j].hello) {
                Buf b = { 0 }; b_u8(&b, (uint8_t)g_peer[j].index); send_msg(&g_peer[j], M_WELCOME, &b); free(b.d);
            }
            host_send_lobby();
        }
    } else {
        if (g_running) {
            for (int j = 0; j < g_n; j++) if (j != g_me) console_left(j, air_input_count(j));
            news("the host has gone: the session is over (your game goes on by itself)");
            lock(); g_state = 4; unlock();
        } else { news("lost the connection to the host"); lock(); g_state = 5; unlock(); }
    }
}

#ifdef _WIN32
static DWORD WINAPI net_thread(LPVOID u)
#else
static void* net_thread(void* u)
#endif
{
    (void)u;
    double last_ping = 0;
    for (;;) {
        if (g_host && g_state == 2) host_accept();
        if (g_host && g_state == 2 && now_ms() - last_ping > 500) {
            last_ping = now_ms();
            for (int i = 1; i < SESSION_MAX; i++) if (g_peer[i].s != BADSOCK && g_peer[i].hello) { g_peer[i].ping_sent = last_ping; Buf b = { 0 }; b_u64(&b, 0); send_msg(&g_peer[i], M_PING, &b); free(b.d); }
            host_send_lobby();
        }
        if (g_host && g_state == 2 && (g_start_req || (g_auto_players && g_n >= g_auto_players))) { g_start_req = 0; host_start(); }
        if (g_running) { send_mine(); check_hashes(); }
        for (int i = 0; i < SESSION_MAX; i++) {
            Peer* p = &g_peer[i];
            if (p->s == BADSOCK) continue;
            if (peer_io(p) < 0) { peer_lost(i); continue; }
            int type; Rd r;
            while (p->s != BADSOCK && peer_msg(p, &type, &r)) { handle(i, type, &r); if (p->s != BADSOCK) peer_consume(p, &r); }
            if (p->s != BADSOCK && peer_io(p) < 0) peer_lost(i);
        }
        if (g_cancel_req) break;
        sleep_ms(1);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---------------------------------------------------------------- setup */
static int resolve(const char* host, int port, struct sockaddr_storage* out, socklen_t* len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return 0;
    memcpy(out, res->ai_addr, res->ai_addrlen); *len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 1;
}

static int start_host(int port) {
    sock_t s = socket(AF_INET6, SOCK_STREAM, 0);
    int v6 = s != BADSOCK;
    if (!v6) s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == BADSOCK) return 0;
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    if (v6) { int off = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off); }
    int ok;
    if (v6) { struct sockaddr_in6 a; memset(&a, 0, sizeof a); a.sin6_family = AF_INET6; a.sin6_port = htons((uint16_t)port); a.sin6_addr = in6addr_any; ok = bind(s, (struct sockaddr*)&a, sizeof a) == 0; }
    else { struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = INADDR_ANY; ok = bind(s, (struct sockaddr*)&a, sizeof a) == 0; }
    if (!ok || listen(s, 4) != 0) { closesock(s); return 0; }
    sock_nonblock(s);
    g_listen = s;
    return 1;
}

static int start_join(const char* host, int port) {
    struct sockaddr_storage a; socklen_t al;
    if (!resolve(host, port, &a, &al)) { news("can't find %s", host); return 0; }
    sock_t s = socket(a.ss_family, SOCK_STREAM, 0);
    if (s == BADSOCK) return 0;
    if (connect(s, (struct sockaddr*)&a, al) != 0) { closesock(s); news("can't connect to %s:%d (is the host waiting, and its port open?)", host, port); return 0; }
    sock_nonblock(s);
    memset(&g_peer[0], 0, sizeof g_peer[0]);
    g_peer[0].s = s; g_peer[0].index = 0;
    Console me = { 0 };
    my_console(&me);
    Buf b = { 0 };
    b_put(&b, MAGIC, 8); b_str(&b, DQIX_VERSION, 32); put_console(&b, &me);
    send_msg(&g_peer[0], M_HELLO, &b);
    free(b.d);
    return 1;
}

void session_pre_boot(int argc, char** argv) {
    if (argc > 1) snprintf(g_rom_path, sizeof g_rom_path, "%s", argv[1]);
    if (argc > 2) snprintf(g_extract_path, sizeof g_extract_path, "%s", argv[2]);
    if (getenv("DQIX_HIDDEN")) { g_hidden = 1; g_wireless.online = 0; return; }
    if (getenv("DQIX_SESSION")) { g_requested = 1; g_wireless.online = 0; }
    const char* nm = getenv("DQIX_PLAYER_NAME");
    if (nm && *nm) snprintf(g_name, sizeof g_name, "%s", nm);
}

void session_boot(void) {
    for (int i = 0; i < SESSION_MAX; i++) g_peer[i].s = BADSOCK;
    if (g_hidden) {
        const char* shm = getenv("DQIX_SESSION_SHM");
        g_me = getenv("DQIX_CONSOLE") ? atoi(getenv("DQIX_CONSOLE")) : 0;
        g_n = getenv("DQIX_COUNT") ? atoi(getenv("DQIX_COUNT")) : 1;
        if (!shm || !air_init(shm, g_me, g_n)) { LOG_WARN("session: hidden console without a session\n"); exit(3); }
        g_running = 1;
        return;
    }
    const char* e = getenv("DQIX_SESSION");
    if (!e) return;
    mx_init();
    if (!net_up()) { news("networking is not available"); return; }
    char mode[16] = { 0 }, a[128] = { 0 };
    int port = SESSION_PORT, players = 0;
    if (!strncmp(e, "host", 4)) {
        sscanf(e, "host:%d:%d", &port, &players);
        g_host = 1; g_me = 0; g_n = 1;
        my_console(&g_con[0]);
        g_rtc = rtc_base();
        g_auto_players = players;
        if (!start_host(port)) { news("can't wait for players on port %d (is another game hosting?)", port); g_state = 5; return; }
        snprintf(g_addr, sizeof g_addr, "port %d", port);
        g_state = 2;
        news("hosting on port %d: waiting for players", port);
    } else if (!strncmp(e, "join:", 5)) {
        /* join:address[:port]; an IPv6 address goes in [brackets] */
        const char* s = e + 5;
        if (*s == '[') { const char* c = strchr(s, ']'); if (c) { snprintf(a, sizeof a, "%.*s", (int)(c - s - 1), s + 1); if (c[1] == ':') port = atoi(c + 2); } }
        else { const char* c = strrchr(s, ':'); if (c) { snprintf(a, sizeof a, "%.*s", (int)(c - s), s); port = atoi(c + 1); } else snprintf(a, sizeof a, "%s", s); }
        snprintf(g_addr, sizeof g_addr, "%s:%d", a, port);
        g_state = 1;
        news("connecting to %s", g_addr);
        if (!start_join(a, port)) { g_state = 5; }
    } else { snprintf(mode, sizeof mode, "%.15s", e); news("unknown DQIX_SESSION %s", mode); return; }
    if (g_state == 5) {
        if (!session_lobby_hook) exit(4);
        while (!g_cancel_req) session_lobby_hook();                /* shows the error until the player closes it */
        exit(0);
    }
#ifdef _WIN32
    CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
#else
    pthread_t t; pthread_create(&t, NULL, net_thread, NULL);
#endif
    while (g_state != 3) {
        if (g_cancel_req) exit(0);
        if (session_lobby_hook) session_lobby_hook();
        else { if (g_state == 5) exit(4); sleep_ms(10); }
    }
}

/* ---------------------------------------------------------------- per frame */
static uint64_t ram_hash(void) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t* p = (const uint64_t*)g_main_ram;
    for (size_t i = 0; i < MAIN_RAM_SIZE / 8; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint32_t session_frame_input(uint64_t frame, uint32_t local) {
    if (!g_running) return local;
    if (frame && frame % HASH_EVERY == 0) air_hash_put(g_me, frame, ram_hash());
    if (!g_hidden) air_input_put(g_me, frame + (uint64_t)g_delay, local);
    uint32_t v = air_input_get(g_me, frame);
    if (v == AIR_INPUT_END) { LOG_WARN("session: this console was switched off at frame %llu\n", (unsigned long long)frame); backup_flush(); exit(0); }
    return v;
}

/* ---------------------------------------------------------------- for the window */
void session_info(SessionInfo* o) {
    memset(o, 0, sizeof *o);
    if (!g_requested) return;
    lock();
    o->state = g_state; o->host = g_host; o->me = g_me; o->n = g_n; o->delay = g_delay; o->desync = g_desync;
    for (int k = 0; k < g_n && k < SESSION_MAX; k++) { snprintf(o->names[k], 32, "%s", g_con[k].name); o->ping_ms[k] = g_ping[k]; o->gone[k] = g_gone[k]; }
    snprintf(o->address, sizeof o->address, "%s", g_addr);
    snprintf(o->message, sizeof o->message, "%s", g_msg);
    unlock();
}
void session_start_now(void) { if (g_host) g_start_req = 1; }
void session_cancel(void) { g_cancel_req = 1; }
void session_set_name(const char* name) { if (name && *name) snprintf(g_name, sizeof g_name, "%s", name); }
