/* Menu bar for the game window on Windows (see menu.h). */
#include "menu.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "SDL_syswm.h"

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

    HMENU help = sub(g_bar, L"&Help");
    item(help, MI_CONTROLS, L"&Controls");
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

#else
void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells) { (void)win; (void)shell_names; (void)n_shells; }
void menu_show(int on) { (void)on; }
void menu_check(int id, int on) { (void)id; (void)on; }
void menu_enable(int id, int on) { (void)id; (void)on; }
int menu_command(const SDL_Event* e) { (void)e; return 0; }
#endif
