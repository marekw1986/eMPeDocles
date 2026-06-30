# mpd_retro — Retro VFD-style MPD Display (multi-file build)

A 320×240 GTK3 window styled after a late-90s rack-mount CD/DVD
player display, showing live MPD playback status plus a browse
menu for playlists, library, and queue.

## File layout

```
app.h            shared types (MpdState, AppData, MenuState, ...),
                 constants, VFD colour palette, drawing-primitive
                 prototypes — included by every module

draw_common.h/.c cairo/pango drawing primitives (rrect, glow_dot,
                 draw_progress, draw_text, hairline, ...).
                 Pure rendering, no app state, no MPD calls.

mpd_client.h/.c  all MPD protocol I/O: socket connect, status
                 polling, menu data loaders (playlists / artists /
                 albums / songs / queue), playback actions
                 (play playlist, play song, queue jump).
                 The only module that touches sockets.

menu.h/.c        the browse-menu state machine + its renderer.
                 menu_input() is the single entry point for all
                 navigation — keyboard, GPIO, IR, anything.

main_screen.h/.c the playback screen renderer (on_draw). Draws
                 the chassis/bezel/VFD screen and delegates to
                 draw_menu() when the menu is open.

main.c           GTK window/canvas setup, keyboard handler
                 (stand-in for hardware input), the two timers
                 (30fps animation, 1Hz MPD poll), and main().

Makefile         builds all .c files into mpd_retro
```

## Why split this way

`mpd_client.c` is isolated so the network/protocol layer can be
tested or swapped without touching any rendering code.

`menu.c` and `main_screen.c` only depend on `app.h`,
`draw_common.h`, and `mpd_client.h` — neither knows about GTK
window setup or keyboard handling, so they don't change when you
move from keyboard to hardware buttons.

`main.c` is the only file that creates the GTK window and binds
input. Hardware buttons replace or extend `on_key()`, nothing else.

## Building

```sh
make
./mpd_retro
```

Requires `gcc` and `libgtk-3-dev`. Override the MPD connection
with environment variables:

```sh
MPD_HOST=192.168.1.10 MPD_PORT=6600 ./mpd_retro
```

## Controls (keyboard, for now)

| Key                  | Action                          |
|-----------------------|----------------------------------|
| `M`                   | toggle menu open / close        |
| `↑` / `↓`             | scroll list                     |
| `→` / `Enter`         | enter submenu / select item     |
| `←` / `Esc`           | go back one level / close menu  |

## Menu tree

```
Root
├─ Playlists   → saved playlists → load & play
├─ Library
│   ├─ Artists → artist → albums → tracks → play
│   ├─ Albums  → album  → tracks → play
│   └─ Songs   → flat track list → play
└─ Queue       → current queue   → jump to track
```

## Wiring up hardware buttons (Raspberry Pi Zero)

The whole integration point is `menu_input()` in `menu.h`/`menu.c`.
It already does everything needed (state transition + redraw
request) — you just need to call it from your GPIO callback.

Example using a GPIO library of your choice (pseudocode, the exact
API depends on whether you use `pigpio`, `libgpiod`, or
`wiringPi`):

```c
#include "menu.h"

/* runs once per call, on the GTK main thread */
static gboolean gpio_press_idle(gpointer data)
{
    MenuButton btn = (MenuButton)(intptr_t)data;
    /* app is a global or passed via a wrapper struct in real code */
    menu_input(g_app, btn);
    return FALSE;  /* run once, don't repeat */
}

/* GPIO edge-detect callback — often fires on its own thread,
 * so marshal back onto the GTK main loop with g_idle_add */
void on_gpio_edge(int gpio_pin, int level, uint32_t tick)
{
    if (level != 0) return;  /* falling edge = button press */

    MenuButton btn;
    switch (gpio_pin) {
        case PIN_BTN_UP:    btn = BTN_UP;    break;
        case PIN_BTN_DOWN:  btn = BTN_DOWN;  break;
        case PIN_BTN_ENTER: btn = BTN_ENTER; break;
        case PIN_BTN_BACK:  btn = BTN_BACK;  break;
        case PIN_BTN_MENU:  btn = BTN_MENU;  break;
        default: return;
    }
    g_idle_add(gpio_press_idle, (gpointer)(intptr_t)btn);
}
```

You'd register `on_gpio_edge` with your GPIO library's interrupt
setup in `main.c` (or a new `gpio.c` module, following the same
pattern as the rest of the project), alongside the existing
keyboard handler — both can coexist, which is handy for debugging
over SSH while the physical buttons are still being wired up.

## Notes

- The window is fixed at 320×240, no on-screen buttons (control
  is keyboard for now, hardware buttons/IR later).
- MPD is polled every second; UI animates at ~30fps regardless of
  poll rate so the title marquee stays smooth.
- Each menu data loader (`load_artists`, `load_albums`, etc.) opens
  its own short-lived MPD connection — simple and robust, at the
  cost of a small connection-setup delay per menu level. Fine for
  a local Raspberry Pi <-> MPD link; if you later want snappier
  library browsing, switch `mpd_client.c` to hold one persistent
  connection.
