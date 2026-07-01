/*
 * menu.h — browse menu: state machine, navigation, rendering
 *
 * ── Hardware button integration point ──────────────────────────
 * To wire physical buttons on the Raspberry Pi Zero, do NOT touch
 * the state machine in menu.c. Instead, from your GPIO edge
 * callback (pigpio / libgpiod / wiringPi, your choice), call:
 *
 *     menu_input(app, BTN_UP);     // or BTN_DOWN / BTN_ENTER / ...
 *
 * exactly as main.c's on_key() does for the keyboard. If your GPIO
 * library callback fires on a separate thread, marshal the call
 * back onto the GTK main thread first, e.g.:
 *
 *     gboolean gpio_press_idle(gpointer data) {
 *         menu_input((AppData *)data, BTN_UP);
 *         return FALSE;  // run once
 *     }
 *     // in the interrupt handler:
 *     g_idle_add(gpio_press_idle, app);
 *
 * menu_input() already calls app_request_redraw() internally,
 * so no further redraw bookkeeping is needed by the caller.
 */
#ifndef MENU_H
#define MENU_H

#include "app.h"

/* open the menu at the root level */
void menu_open(MenuState *m);

/* close the menu (returns to the playback screen) */
void menu_close(MenuState *m);

/* single entry point for all menu navigation — keyboard, GPIO,
 * IR receiver, anything. See header comment above for GPIO use. */
void menu_input(AppData *app, MenuButton btn);

/* render the menu overlay on top of the playback screen.
 * (SX, SY, SW, SH) is the screen's content rectangle, matching
 * what main_screen.c uses for its own drawing area. */
void draw_menu(cairo_t *cr, MenuState *m, AppData *app,
               double SX, double SY, double SW, double SH);

#endif /* MENU_H */
