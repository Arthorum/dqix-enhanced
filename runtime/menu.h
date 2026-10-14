/* The window's menu bar (Windows): every setting in one place. The frontend
 * owns the settings; this file only builds the menu, reports which item was
 * chosen, and shows check marks. Elsewhere the functions do nothing. */
#pragma once
#include "SDL.h"

enum {
    MI_PAUSE = 100, MI_TURBO, MI_SHOT, MI_REPORT, MI_EXIT,
    MI_CLASSIC = 110, MI_ENHANCED, MI_RES1, MI_RES2, MI_RES3, MI_RES4,
    MI_LCD = 120, MI_LCDCOL, MI_GRID, MI_GHOST, MI_BL1, MI_BL2, MI_BL3, MI_BL4,
    MI_SHELL = 130, MI_GAP, MI_ACTUAL, MI_SHELLC0,                      /* MI_SHELLC0 + colour index */
    MI_STACKED = 150, MI_SIDE, MI_SIZE1, MI_SIZE2, MI_SIZE3, MI_SIZE4, MI_INTEGER, MI_FULL,
    MI_CONTROLS = 170, MI_ABOUT,
};

/* shell_names: the console colours, in order (MI_SHELLC0 + i) */
void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells);
void menu_show(int on);                       /* hidden in fullscreen */
void menu_check(int id, int on);
void menu_enable(int id, int on);
int menu_command(const SDL_Event* e);         /* the chosen item, or 0 */

/* Size of one pixel of the monitor the window is on, in millimetres, from
 * the monitor's own description of itself (EDID). 0 if unknown. */
float display_mm_per_pixel(SDL_Window* win);
