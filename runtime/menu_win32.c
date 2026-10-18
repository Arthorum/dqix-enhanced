/* Menu bar for the game window on Windows (see menu.h). */
#include "menu.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "SDL_syswm.h"
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>

static HWND g_hwnd;
static HMENU g_bar;

static void item(HMENU m, int id, const wchar_t* text) { AppendMenuW(m, MF_STRING, (UINT_PTR)id, text); }
static void sep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); }
static HMENU sub(HMENU parent, const wchar_t* text) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)m, text);
    return m;
}

void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    g_hwnd = info.info.win.window;
    g_bar = CreateMenu();

    HMENU game = sub(g_bar, L"&Game");
    item(game, MI_PAUSE, L"&Pause\tP");
    item(game, MI_TURBO, L"&Fast forward\t(or hold Tab)");
    sep(game);
    item(game, MI_SHOT, L"Save &screenshot\tF12");
    item(game, MI_REPORT, L"Save &bug report\tF10");
    sep(game);
    item(game, MI_EXIT, L"E&xit\tEsc");

    HMENU gfx = sub(g_bar, L"G&raphics");
    item(gfx, MI_CLASSIC, L"&Classic: exactly like the DS\tF5");
    item(gfx, MI_ENHANCED, L"&Enhanced: sharper 3D\tF5");
    sep(gfx);
    HMENU res = sub(gfx, L"3D &resolution (Enhanced)\tF4");
    item(res, MI_RES1, L"1x (DS)");
    item(res, MI_RES2, L"2x");
    item(res, MI_RES3, L"3x");
    item(res, MI_RES4, L"4x");

    HMENU scr = sub(g_bar, L"&Screen");
    item(scr, MI_LCD, L"&DS Lite screen\tF6");
    sep(scr);
    item(scr, MI_LCDCOL, L"LCD &colours");
    item(scr, MI_GRID, L"&Pixel grid (at 3x and larger)");
    item(scr, MI_GHOST, L"&Motion blur");
    HMENU bl = sub(scr, L"&Backlight");
    item(bl, MI_BL1, L"&Low");
    item(bl, MI_BL2, L"&Medium");
    item(bl, MI_BL3, L"&High");
    item(bl, MI_BL4, L"Ma&x");

    HMENU view = sub(g_bar, L"&View");
    item(view, MI_SHELL, L"Show the DS &Lite\tF7");
    HMENU col = sub(view, L"Console &colour");
    for (int i = 0; i < n_shells; i++) {
        wchar_t w[64];
        MultiByteToWideChar(CP_UTF8, 0, shell_names[i], -1, w, 64);
        item(col, MI_SHELLC0 + i, w);
    }
    item(view, MI_GAP, L"&Gap between the screens");
    item(view, MI_ACTUAL, L"&Actual size (as big as a real DS Lite)\tF8");
    sep(view);
    item(view, MI_STACKED, L"S&tacked\tF2");
    item(view, MI_SIDE, L"&Side by side\tF2");
    sep(view);
    HMENU size = sub(view, L"&Window size\tF3");
    item(size, MI_SIZE1, L"1x");
    item(size, MI_SIZE2, L"2x");
    item(size, MI_SIZE3, L"3x");
    item(size, MI_SIZE4, L"4x");
    item(view, MI_INTEGER, L"&Integer scaling");
    item(view, MI_FULL, L"&Fullscreen\tF11");

    HMENU wl = sub(g_bar, L"&Wireless");
    item(wl, MI_NET, L"&Connect (Nintendo Wi-Fi Connection replacement)");
    HMENU srv = sub(wl, L"&Server");
    item(srv, MI_SRV_BUILTIN, L"&Built-in (offline: DQIX's preserved downloads)");
    sep(srv);
    item(srv, MI_SRV_ALTWFC, L"&AltWFC (172.104.88.237)");
    item(srv, MI_SRV_WIIMMFI, L"&Wiimmfi (164.132.44.106): not for DQIX");
    item(srv, MI_SRV_CUSTOM, L"&Custom (wfc_server in settings.ini)");
    sep(wl);
    HMENU friends = sub(wl, L"&Play with friends far away (tag mode, visiting)");
    item(friends, MI_SES_HOST, L"&Host: your friends join you...");
    item(friends, MI_SES_JOIN, L"&Join a friend...");
    item(friends, MI_SES_START, L"&Start now (everyone is here)\tEnter");
    item(friends, MI_SES_LEAVE, L"&Leave and play alone");
    sep(friends);
    item(friends, MI_SES_NAME, L"Your &name...");
    item(friends, MI_SES_HELP, L"&How does this work?");
    sep(wl);
    item(wl, MI_DLC, L"&Unlock all downloadable content now (without Sellma)");
    sep(wl);
    item(wl, MI_NET_HELP, L"&How does this work?");

    HMENU help = sub(g_bar, L"&Help");
    item(help, MI_CONTROLS, L"&Controls");
    item(help, MI_UPDATE, L"Check for &updates...");
    item(help, MI_UPDATE_AUTO, L"Check for updates at &start");
    sep(help);
    item(help, MI_ABOUT, L"&About");

    SetMenu(g_hwnd, g_bar);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
}

void menu_show(int on) {
    if (g_hwnd) SetMenu(g_hwnd, on ? g_bar : NULL);
}

void menu_check(int id, int on) {
    if (g_bar) CheckMenuItem(g_bar, (UINT)id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

void menu_enable(int id, int on) {
    if (g_bar) EnableMenuItem(g_bar, (UINT)id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
}

int menu_command(const SDL_Event* e) {
    if (e->type != SDL_SYSWMEVENT || !g_bar) return 0;
    const SDL_SysWMmsg* m = e->syswm.msg;
    if (m->subsystem != SDL_SYSWM_WINDOWS || m->msg.win.msg != WM_COMMAND || HIWORD(m->msg.win.wParam) != 0) return 0;
    return LOWORD(m->msg.win.wParam);
}

/* The monitor's EDID (in the registry under its device id) holds its
 * physical size: the first detailed timing block in mm, else the basic
 * size in cm. Divided by the monitor's width in pixels. */
float display_mm_per_pixel(SDL_Window* win) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return 0;
    HMONITOR mon = MonitorFromWindow(info.info.win.window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO*)&mi)) return 0;
    DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth) return 0;
    int px_w = (int)dm.dmPelsWidth, px_h = (int)dm.dmPelsHeight;
    DISPLAY_DEVICEW dd; dd.cb = sizeof dd;
    for (DWORD k = 0; EnumDisplayDevicesW(mi.szDevice, k, &dd, EDD_GET_DEVICE_INTERFACE_NAME); k++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        /* \\?\DISPLAY#<model>#<instance>#{guid} -> SYSTEM\CurrentControlSet\Enum\DISPLAY\<model>\<instance> */
        wchar_t id[256]; wcsncpy(id, dd.DeviceID, 255); id[255] = 0;
        wchar_t* parts[4] = { 0 }; int n = 0;
        for (wchar_t *ctx = NULL, *t = wcstok(id, L"#", &ctx); t && n < 4; t = wcstok(NULL, L"#", &ctx)) parts[n++] = t;
        if (n < 3) continue;
        wchar_t key[512];
        _snwprintf(key, 511, L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%ls\\%ls\\Device Parameters", parts[1], parts[2]);
        key[511] = 0;
        BYTE edid[256]; DWORD len = sizeof edid;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EDID", RRF_RT_REG_BINARY, NULL, edid, &len) != ERROR_SUCCESS || len < 128) continue;
        if (edid[0] != 0 || edid[1] != 0xFF || edid[7] != 0) continue;
        int w_mm = 0, h_mm = 0;
        if (edid[54] | edid[55]) { w_mm = edid[66] | ((edid[68] & 0xF0) << 4); h_mm = edid[67] | ((edid[68] & 0x0F) << 8); }
        if (w_mm < 100 || h_mm < 50) { w_mm = edid[21] * 10; h_mm = edid[22] * 10; }    /* cm */
        if (w_mm < 100 || h_mm < 50) continue;
        if ((px_w < px_h) != (w_mm < h_mm)) { int t = w_mm; w_mm = h_mm; h_mm = t; } /* rotated monitor */
        float mm = (float)w_mm / (float)px_w;
        if (mm > 0.05f && mm < 1.5f) return mm;
    }
    return 0;
}

/* ---------------------------------------------------------------- text box (bug descriptions)
 * A dialog built in memory: a prompt, a large multi-line edit box, and
 * Save / Skip buttons. */
#define ID_TEXT 1001
static wchar_t* g_text_out;

static INT_PTR CALLBACK text_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SendDlgItemMessageW(h, ID_TEXT, EM_SETLIMITTEXT, 1 << 20, 0);
        SetFocus(GetDlgItem(h, ID_TEXT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int n = GetWindowTextLengthW(GetDlgItem(h, ID_TEXT));
            g_text_out = calloc((size_t)n + 1, sizeof(wchar_t));
            if (g_text_out) GetDlgItemTextW(h, ID_TEXT, g_text_out, n + 1);
            EndDialog(h, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static WORD* align4(WORD* p) { return (WORD*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }
static WORD* put_str(WORD* p, const wchar_t* s) { do { *p++ = (WORD)*s; } while (*s++); return p; }
static WORD* put_item(WORD* p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
    p = align4(p);
    DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
    it->style = style | WS_CHILD | WS_VISIBLE; it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD*)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;                                   /* 0x80 button, 0x81 edit, 0x82 static */
    p = put_str(p, text);
    *p++ = 0;                                                    /* no creation data */
    return p;
}

char* ask_text(SDL_Window* win, const char* title, const char* prompt) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return NULL;
    wchar_t wtitle[200], wprompt[1000];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 200);
    MultiByteToWideChar(CP_UTF8, 0, prompt, -1, wprompt, 1000);
    static WORD buf[4096];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cdit = 4; d->x = 0; d->y = 0; d->cx = 340; d->cy = 230;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;                                          /* no menu, default class */
    p = put_str(p, wtitle);
    *p++ = 9; p = put_str(p, L"Segoe UI");                       /* font */
    p = put_item(p, 0, 7, 7, 326, 34, 0xFFFF, 0x82, wprompt);
    p = put_item(p, WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 7, 44, 326, 158, ID_TEXT, 0x81, L"");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 213, 208, 60, 15, IDOK, 0x80, L"Save");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 277, 208, 56, 15, IDCANCEL, 0x80, L"Skip");
    g_text_out = NULL;
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, text_proc, 0);
    if (r != 1 || !g_text_out) { free(g_text_out); g_text_out = NULL; return NULL; }
    int n = WideCharToMultiByte(CP_UTF8, 0, g_text_out, -1, NULL, 0, NULL, NULL);
    char* out = malloc((size_t)(n > 0 ? n : 1));
    if (out) { out[0] = 0; if (n > 0) WideCharToMultiByte(CP_UTF8, 0, g_text_out, -1, out, n, NULL, NULL); }
    free(g_text_out); g_text_out = NULL;
    return out;
}


/* ---------------------------------------------------------------- the session window (menu.h) */
enum { ID_ADDR = 1101, ID_COPY, ID_HINT, ID_LIST, ID_STATUS, ID_START, ID_HEAD };
static HWND g_ses;
static int g_ses_action;
static SessionWindow g_ses_shown;

static void set_text(HWND h, int id, const char* utf8) {
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 1024);
    w[1023] = 0;
    SetDlgItemTextW(h, id, w);
}

static void copy_address(HWND h) {
    int n = GetWindowTextLengthW(GetDlgItem(h, ID_ADDR));
    if (n <= 0 || !OpenClipboard(h)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(wchar_t));
    if (mem) {
        wchar_t* p = GlobalLock(mem);
        GetDlgItemTextW(h, ID_ADDR, p, n + 1);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    SetDlgItemTextW(h, ID_COPY, L"Copied!");
    SetTimer(h, 1, 2000, NULL);
}

static INT_PTR CALLBACK ses_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: return TRUE;
    case WM_TIMER: KillTimer(h, 1); SetDlgItemTextW(h, ID_COPY, L"Copy"); return TRUE;
    case WM_CLOSE: g_ses_action = 2; return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_COPY: copy_address(h); return TRUE;
        case ID_START: if (IsWindowEnabled(GetDlgItem(h, ID_START))) g_ses_action = 1; return TRUE;
        case IDCANCEL: g_ses_action = 2; return TRUE;
        }
        break;
    }
    return FALSE;
}

int session_window_open(SDL_Window* win) {
    if (g_ses) return 1;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return 0;
    static WORD buf[4096];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    d->cdit = 9; d->x = 0; d->y = 0; d->cx = 270; d->cy = 252;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    p = put_str(p, L"Play with friends far away");
    *p++ = 10; p = put_str(p, L"Segoe UI");
    p = put_item(p, 0, 8, 8, 254, 10, ID_HEAD, 0x82, L"");
    p = put_item(p, WS_BORDER | ES_READONLY | ES_AUTOHSCROLL | ES_CENTER, 8, 21, 196, 16, ID_ADDR, 0x81, L"");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 210, 21, 52, 16, ID_COPY, 0x80, L"Copy");
    p = put_item(p, 0, 8, 42, 254, 66, ID_HINT, 0x82, L"");
    p = put_item(p, 0, 8, 112, 254, 10, 0xFFFF, 0x82, L"Players:");
    p = put_item(p, WS_BORDER | WS_VSCROLL | LBS_NOSEL, 8, 124, 254, 54, ID_LIST, 0x83, L"");
    p = put_item(p, 0, 8, 184, 254, 34, ID_STATUS, 0x82, L"");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 118, 228, 84, 17, ID_START, 0x80, L"Start the game");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 208, 228, 54, 17, IDCANCEL, 0x80, L"Cancel");
    g_ses = CreateDialogIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, ses_proc, 0);
    if (!g_ses) return 0;
    /* a bigger, bolder address */
    HFONT f = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (f) SendDlgItemMessageW(g_ses, ID_ADDR, WM_SETFONT, (WPARAM)f, TRUE);
    memset(&g_ses_shown, 0, sizeof g_ses_shown);
    g_ses_shown.can_start = -1;
    g_ses_action = 0;
    return 1;
}

void session_window_update(const SessionWindow* w) {
    if (!g_ses) return;
    SessionWindow* o = &g_ses_shown;
    if (strcmp(o->heading, w->heading)) set_text(g_ses, ID_HEAD, w->heading);
    if (strcmp(o->address, w->address)) set_text(g_ses, ID_ADDR, w->address);
    if (strcmp(o->hint, w->hint)) set_text(g_ses, ID_HINT, w->hint);
    if (strcmp(o->status, w->status)) set_text(g_ses, ID_STATUS, w->status);
    int list_changed = o->n_players != w->n_players;
    for (int i = 0; i < w->n_players && !list_changed; i++) list_changed = strcmp(o->players[i], w->players[i]) != 0;
    if (list_changed) {
        HWND l = GetDlgItem(g_ses, ID_LIST);
        SendMessageW(l, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < w->n_players; i++) {
            wchar_t t[128]; MultiByteToWideChar(CP_UTF8, 0, w->players[i], -1, t, 128); t[127] = 0;
            SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)t);
        }
    }
    int start = w->host && w->can_start && !w->failed;
    if (o->can_start != start || o->host != w->host || o->failed != w->failed) {
        EnableWindow(GetDlgItem(g_ses, ID_START), start);
        ShowWindow(GetDlgItem(g_ses, ID_START), w->host && !w->failed ? SW_SHOW : SW_HIDE);
        SetDlgItemTextW(g_ses, IDCANCEL, w->failed ? L"Back" : L"Cancel");
        EnableWindow(GetDlgItem(g_ses, ID_COPY), w->address[0] != 0);
    }
    *o = *w;
    o->can_start = start;
}

int session_window_action(void) { int a = g_ses_action; g_ses_action = 0; return a; }

void session_window_close(void) {
    if (!g_ses) return;
    DestroyWindow(g_ses);
    g_ses = NULL;
}
#else
char* ask_text(SDL_Window* win, const char* title, const char* prompt) { (void)win; (void)title; (void)prompt; return NULL; }
float display_mm_per_pixel(SDL_Window* win) { (void)win; return 0; }
void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells) { (void)win; (void)shell_names; (void)n_shells; }
void menu_show(int on) { (void)on; }
void menu_check(int id, int on) { (void)id; (void)on; }
void menu_enable(int id, int on) { (void)id; (void)on; }
int menu_command(const SDL_Event* e) { (void)e; return 0; }
int session_window_open(SDL_Window* win) { (void)win; return 0; }
void session_window_update(const SessionWindow* w) { (void)w; }
int session_window_action(void) { return 0; }
void session_window_close(void) {}
#endif
