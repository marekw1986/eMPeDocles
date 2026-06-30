/*
 * app.h — shared types, constants and drawing primitives
 *
 * Included by every other module. Keep this file free of anything
 * that pulls in MPD socket code or GTK event handling — it's the
 * common vocabulary the other modules share.
 */
#ifndef APP_H
#define APP_H

#include <gtk/gtk.h>
#include <cairo.h>
#include <pango/pangocairo.h>

/* ── window geometry ───────────────────────────────────────────── */
#define WIN_W  320
#define WIN_H  240

/* ── MPD poll interval (ms) ───────────────────────────────────── */
#define POLL_MS 1000

/* ── VFD colour palette (used as comma-expanded RGB triples) ─────
 * Usage: cairo_set_source_rgb(cr, COL_BRIGHT);
 * Do NOT use these inside a ternary expression — see draw_menu.c
 * for the workaround pattern (extract to local doubles first).
 */
#define COL_CHASSIS   0.04, 0.04, 0.08
#define COL_BRIGHT    0.40, 0.85, 1.00    /* bright VFD cyan      */
#define COL_MID       0.00, 0.55, 0.88    /* medium VFD           */
#define COL_DIM       0.06, 0.20, 0.36    /* inactive segments    */
#define COL_AMBER     1.00, 0.62, 0.00    /* connection OK LED    */
#define COL_RED       1.00, 0.16, 0.08    /* connection fail LED  */
#define COL_SEL_BG    0.00, 0.28, 0.55    /* selected row fill    */

/* ── menu limits ──────────────────────────────────────────────── */
#define MENU_MAX_ITEMS  512
#define MENU_LABEL_LEN  192
#define MENU_STACK_MAX    8
#define MENU_VISIBLE     10   /* rows visible at once in the list */
#define MENU_ROW_H       18   /* px per row                       */

/* ══════════════════════════════════════════════════════════════
 *  MPD playback state — filled by mpd_poll()
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    char  title[256];
    char  artist[256];
    char  album[256];
    char  state[16];    /* "play" | "pause" | "stop" */
    int   elapsed;
    int   duration;
    int   volume;        /* 0-100, -1 = unknown */
    int   bitrate;
    int   samplerate;
    int   song;          /* 0-based playlist index, -1 = unknown */
    int   playlistlen;
    int   connected;
} MpdState;

/* ══════════════════════════════════════════════════════════════
 *  Menu data model
 * ══════════════════════════════════════════════════════════════ */
typedef enum {
    MTYPE_SUBMENU,             /* opens a static child list        */
    MTYPE_LOAD_PLISTS,         /* fetches saved playlists from MPD */
    MTYPE_LOAD_ARTISTS,
    MTYPE_LOAD_ALBUMS_FOR_ARTIST,
    MTYPE_LOAD_ALBUMS,
    MTYPE_LOAD_SONGS_FOR_ALBUM,
    MTYPE_LOAD_SONGS,
    MTYPE_LOAD_QUEUE,
    MTYPE_PLAY_PLAYLIST,       /* action: load & play playlist     */
    MTYPE_PLAY_SONG,           /* action: clear/add/play song uri  */
    MTYPE_QUEUE_JUMP,          /* action: playid N                 */
} MenuType;

typedef struct {
    char      label[MENU_LABEL_LEN];
    MenuType  type;
    char      data[MENU_LABEL_LEN];  /* uri / artist / album / playlist name */
} MenuItem;

typedef struct {
    MenuItem  items[MENU_MAX_ITEMS];
    int       count;
    int       cursor;   /* selected index           */
    int       offset;   /* scroll offset (top row)  */
    char      title[64];
} MenuLevel;

typedef struct {
    int        open;
    MenuLevel  stack[MENU_STACK_MAX];
    int        depth;   /* index of current level */
} MenuState;

/* logical button events — fire these from keyboard OR GPIO.
 * This is the single integration point for hardware buttons:
 * wire each physical button's interrupt handler to call
 * menu_input(app, BTN_xxx) with the matching value below. */
typedef enum { BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK, BTN_MENU } MenuButton;

/* ══════════════════════════════════════════════════════════════
 *  Top-level application state
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    GtkWidget *canvas;
    MpdState   mpd;

    double  scroll_x;       /* marquee scroll offset           */
    double  scroll_speed;   /* px / animation frame            */
    int     title_px_w;     /* cached rendered width of title  */

    MenuState  menu;

    guint   anim_id;
    guint   poll_id;
} AppData;

/* ══════════════════════════════════════════════════════════════
 *  Shared drawing primitives (implemented in draw_common.c)
 * ══════════════════════════════════════════════════════════════ */

/* rounded rectangle path (does not fill/stroke) */
void rrect(cairo_t *cr, double x, double y, double w, double h, double r);

/* radial "glow" dot — used for LEDs, progress segments, scroll dots */
void glow_dot(cairo_t *cr, double cx, double cy, double r,
             double rr, double gg, double bb, double alpha);

/* dot-segment progress bar */
void draw_progress(cairo_t *cr, double x, double y, double w, double h,
                   double frac);

/* crisp, pixel-snapped Pango text (no blur) */
void draw_text(cairo_t *cr, double x, double y,
               const char *text, const char *font,
               double rr, double gg, double bb, double alpha,
               PangoAlignment align, double clip_w);

/* measure rendered pixel width of a string in a given font */
int measure_text_w(cairo_t *cr, const char *text, const char *font);

/* thin horizontal divider line */
void hairline(cairo_t *cr, double x0, double y, double x1,
             double rr, double gg, double bb, double alpha);

#endif /* APP_H */
