/*
 * main_screen.h — the retro VFD playback screen
 */
#ifndef MAIN_SCREEN_H
#define MAIN_SCREEN_H

#include "app.h"

/* Renders the chassis, VFD screen, playback info, and (if open)
 * delegates to draw_menu() for the browse overlay, into the given
 * Cairo context. The context's destination surface is owned by
 * the backend (fb_backend_linux.c / fb_backend_sdl.c) — this
 * function doesn't know or care whether that's a real framebuffer,
 * an SDL window, or (formerly) a GTK widget. */
void on_draw(cairo_t *cr, AppData *app);

#endif /* MAIN_SCREEN_H */
