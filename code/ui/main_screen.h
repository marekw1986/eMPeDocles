/*
 * main_screen.h — the retro VFD playback screen
 */
#ifndef MAIN_SCREEN_H
#define MAIN_SCREEN_H

#include "app.h"

/* GTK "draw" signal handler for the canvas. Renders the chassis,
 * VFD screen, playback info, and (if open) delegates to
 * draw_menu() for the browse overlay. */
gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);

#endif /* MAIN_SCREEN_H */
