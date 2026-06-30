/*
 * main.c — application entry point
 *
 * GTK window/canvas setup, keyboard input (stand-in for hardware
 * buttons — see menu.h for the GPIO integration point), and the
 * two timers that drive the UI: a ~30fps animation tick for the
 * title marquee, and a 1s poll of MPD status.
 *
 * Build:
 *   make
 *
 * MPD connection: localhost:6600 by default.
 * Override with MPD_HOST / MPD_PORT environment variables.
 */
#include "app.h"
#include "main_screen.h"
#include "menu.h"
#include "mpd_client.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════════
 *  Keyboard handler (stand-in for hardware buttons / IR receiver)
 * ══════════════════════════════════════════════════════════════
 *
 * Keys:
 *   M             — toggle menu open / close
 *   Up / Down     — scroll list
 *   Right / Enter — enter submenu / select item
 *   Left / Esc    — go back one level / close menu
 *
 * When moving to physical buttons on the Pi Zero, this function
 * can stay in place (useful for debugging over SSH+X11 forwarding)
 * — just add a second input source that also calls menu_input().
 * See menu.h for details.
 */
static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)widget;
    AppData *app = (AppData *)data;
    switch (ev->keyval) {
    case GDK_KEY_m: case GDK_KEY_M:
        menu_input(app, BTN_MENU);  break;
    case GDK_KEY_Up:
        menu_input(app, BTN_UP);    break;
    case GDK_KEY_Down:
        menu_input(app, BTN_DOWN);  break;
    case GDK_KEY_Right: case GDK_KEY_Return: case GDK_KEY_KP_Enter:
        menu_input(app, BTN_ENTER); break;
    case GDK_KEY_Left: case GDK_KEY_Escape:
        menu_input(app, BTN_BACK);  break;
    default:
        return FALSE;
    }
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 *  Timers
 * ══════════════════════════════════════════════════════════════ */

/* ~30fps: advances the title marquee scroll position */
static gboolean on_anim(gpointer data)
{
    AppData *app = (AppData *)data;
    if (strcmp(app->mpd.state, "play") == 0) {
        app->scroll_x += app->scroll_speed;
        if (app->title_px_w > 0 && app->scroll_x >= app->title_px_w)
            app->scroll_x = 0.0;
    }
    gtk_widget_queue_draw(app->canvas);
    return TRUE;
}

/* 1Hz: polls MPD for playback status / current song */
static gboolean on_poll(gpointer data)
{
    AppData *app = (AppData *)data;
    char old[256];
    snprintf(old, sizeof(old), "%s", app->mpd.title);

    mpd_poll(&app->mpd);

    /* reset marquee when the track changes */
    if (strcmp(old, app->mpd.title) != 0) {
        app->scroll_x   = 0.0;
        app->title_px_w = 0;
    }

    gtk_widget_queue_draw(app->canvas);
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 *  Main
 * ══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);

    AppData app;
    memset(&app, 0, sizeof(app));
    app.scroll_speed = 0.75;
    snprintf(app.mpd.state, sizeof(app.mpd.state), "stop");
    mpd_poll(&app.mpd);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "MPD");
    gtk_window_set_default_size(GTK_WINDOW(win), WIN_W, WIN_H);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    app.canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.canvas, WIN_W, WIN_H);
    gtk_widget_set_can_focus(app.canvas, TRUE);
    g_signal_connect(app.canvas, "draw",            G_CALLBACK(on_draw), &app);
    g_signal_connect(win,        "key-press-event", G_CALLBACK(on_key),  &app);
    gtk_container_add(GTK_CONTAINER(win), app.canvas);

    app.anim_id = g_timeout_add(33,      on_anim, &app);
    app.poll_id = g_timeout_add(POLL_MS, on_poll, &app);

    gtk_widget_show_all(win);
    gtk_main();

    g_source_remove(app.anim_id);
    g_source_remove(app.poll_id);
    return 0;
}
