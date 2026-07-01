/*
 * main.c — application entry point (framebuffer backend)
 *
 * No GTK: a plain loop that polls input, advances animation/MPD
 * timers manually, draws into an offscreen Cairo surface (owned
 * by the active backend — see fb_backend.h), and presents it.
 *
 * Backend selection happens at build time (see Makefile,
 * BACKEND=linux|sdl) — this file only calls the fb_* interface
 * and never knows which concrete backend is linked in.
 *
 * Keys (same as the original GTK build):
 *   M             — toggle menu open / close
 *   Up / Down     — scroll list
 *   Right / Enter — enter submenu / select item
 *   Left / Esc    — go back one level / close menu
 *   Q             — quit
 *
 * MPD connection: localhost:6600 by default.
 * Override with MPD_HOST / MPD_PORT environment variables.
 */
#include "app.h"
#include "main_screen.h"
#include "menu.h"
#include "mpd_client.h"
#include "fb_backend.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── timing constants ─────────────────────────────────────────
 * Mirrors the old GTK timer intervals: ~30fps animation tick,
 * 1Hz MPD poll. Implemented here as plain elapsed-time checks
 * inside the main loop instead of GLib timeout sources. */
#define ANIM_INTERVAL_MS 33
#define POLL_INTERVAL_MS 1000
#define LOOP_SLEEP_US    5000   /* ~5ms idle sleep between iterations */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void)
{
    AppData app;
    memset(&app, 0, sizeof(app));
    app.scroll_speed = 0.75;
    app.running = 1;
    snprintf(app.mpd.state, sizeof(app.mpd.state), "stop");
    mpd_poll(&app.mpd);

    if (fb_init() != 0) {
        fprintf(stderr, "main: failed to initialise display backend\n");
        return 1;
    }

    long last_anim = now_ms();
    long last_poll = now_ms();
    app.dirty = 1;   /* force an initial draw */

    while (app.running) {
        long t = now_ms();

        /* ── input ── */
        fb_poll_input(&app);
        if (!app.running) break;

        /* ── animation tick (~30fps): advance title marquee ── */
        if (t - last_anim >= ANIM_INTERVAL_MS) {
            last_anim = t;
            if (strcmp(app.mpd.state, "play") == 0) {
                app.scroll_x += app.scroll_speed;
                if (app.title_px_w > 0 && app.scroll_x >= app.title_px_w)
                    app.scroll_x = 0.0;
            }
            app_request_redraw(&app);
        }

        /* ── MPD poll (1Hz): refresh playback status ── */
        if (t - last_poll >= POLL_INTERVAL_MS) {
            last_poll = t;
            char old_title[256];
            snprintf(old_title, sizeof(old_title), "%s", app.mpd.title);

            mpd_poll(&app.mpd);

            if (strcmp(old_title, app.mpd.title) != 0) {
                app.scroll_x   = 0.0;
                app.title_px_w = 0;
            }
            app_request_redraw(&app);
        }

        /* ── draw + present only when something changed ── */
        if (app.dirty) {
            app.dirty = 0;
            cairo_t *cr = fb_get_cairo();
            on_draw(cr, &app);
            fb_present();
        }

        usleep(LOOP_SLEEP_US);
    }

    fb_shutdown();
    return 0;
}
