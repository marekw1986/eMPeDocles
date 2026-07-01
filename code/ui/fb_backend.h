/*
 * fb_backend.h — display + input backend interface
 *
 * Two implementations exist behind this one interface:
 *
 *   fb_backend_linux.c   mmaps /dev/fb0 directly — for the
 *                         Raspberry Pi (or any Linux box with a
 *                         real framebuffer console, accessible
 *                         after Ctrl+Alt+F2 / no X11 running).
 *
 *   fb_backend_sdl.c     opens an SDL2 window and treats its pixel
 *                         buffer as a simulated 320x240 framebuffer
 *                         — for desktop development/testing without
 *                         a Pi or a framebuffer console.
 *
 * main.c only calls these functions; it never touches /dev/fb0 or
 * SDL directly. Pick the backend at build time with the Makefile's
 * BACKEND=linux|sdl variable.
 */
#ifndef FB_BACKEND_H
#define FB_BACKEND_H

#include "app.h"
#include <cairo.h>
#include <stdint.h>

/* Opens the display device (or window) and allocates an offscreen
 * Cairo ARGB32 image surface of WIN_W x WIN_H that draw calls
 * target. Returns 0 on success, -1 on failure (prints to stderr). */
int fb_init(void);

/* Returns the Cairo context to draw into this frame. Always the
 * same offscreen surface — callers don't need to re-fetch it
 * every frame, but it's cheap to do so. */
cairo_t *fb_get_cairo(void);

/* Pushes the offscreen surface's pixels to the real display
 * (memcpy to the mmap'd framebuffer, or an SDL texture update +
 * present, depending on backend). Call once per drawn frame. */
void fb_present(void);

/* Polls for input events (keyboard for now; same role keyboard
 * handling played under GTK) and translates them into MenuButton
 * presses via menu_input(app, ...). Also responsible for noticing
 * window-close / quit requests and setting app->running = 0.
 * Non-blocking — called once per main loop iteration. */
void fb_poll_input(AppData *app);

/* Releases the framebuffer mapping / SDL window and any other
 * backend resources. Safe to call once at shutdown. */
void fb_shutdown(void);

#endif /* FB_BACKEND_H */
