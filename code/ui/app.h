/*
 * app.h — shared types, constants and drawing primitive prototypes
 */
#ifndef APP_H
#define APP_H

#include <cairo.h>
#include <pango/pangocairo.h>

/* ── window geometry ─────────────────────────────────────────── */
#define WIN_W  320
#define WIN_H  240

/* ── MPD poll interval (ms) ─────────────────────────────────── */
#define POLL_MS 1000

/* ── VFD flat colour palette ─────────────────────────────────── */
#define COL_BG        0.01, 0.02, 0.08     /* screen background    */
#define COL_BRIGHT    0.40, 0.85, 1.00     /* bright VFD cyan      */
#define COL_MID       0.00, 0.55, 0.88     /* medium VFD           */
#define COL_DIM       0.06, 0.20, 0.36     /* inactive segments    */
#define COL_AMBER     1.00, 0.62, 0.00     /* connection OK LED    */
#define COL_RED       1.00, 0.16, 0.08     /* connection fail LED  */
#define COL_SEL_BG    0.00, 0.28, 0.55     /* menu selection fill  */

/* ── menu limits ─────────────────────────────────────────────── */
#define MENU_MAX_ITEMS  512
#define MENU_LABEL_LEN  192
#define MENU_STACK_MAX    8
#define MENU_VISIBLE     10
#define MENU_ROW_H       18

/* ══════════════════════════════════════════════════════════════
 *  MPD playback state
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    char  title[256];
    char  artist[256];
    char  album[256];
    char  state[16];
    int   elapsed, duration;
    int   volume;
    int   bitrate, samplerate;
    int   song, playlistlen;
    int   connected;
} MpdState;

/* ══════════════════════════════════════════════════════════════
 *  Menu data model
 * ══════════════════════════════════════════════════════════════ */
typedef enum {
    MTYPE_SUBMENU,
    MTYPE_LOAD_PLISTS,
    MTYPE_LOAD_ARTISTS,
    MTYPE_LOAD_ALBUMS_FOR_ARTIST,
    MTYPE_LOAD_ALBUMS,
    MTYPE_LOAD_SONGS_FOR_ALBUM,
    MTYPE_LOAD_SONGS,
    MTYPE_LOAD_QUEUE,
    MTYPE_PLAY_PLAYLIST,
    MTYPE_PLAY_SONG,
    MTYPE_QUEUE_JUMP,
} MenuType;

typedef struct {
    char      label[MENU_LABEL_LEN];
    MenuType  type;
    char      data[MENU_LABEL_LEN];
} MenuItem;

typedef struct {
    MenuItem  items[MENU_MAX_ITEMS];
    int       count, cursor, offset;
    char      title[64];
} MenuLevel;

typedef struct {
    int        open;
    MenuLevel  stack[MENU_STACK_MAX];
    int        depth;
} MenuState;

typedef enum { BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK, BTN_MENU } MenuButton;

/* ══════════════════════════════════════════════════════════════
 *  Render cache — pre-rendered surfaces and Pango layouts that
 *  persist between frames so we don't recreate them every tick.
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    /* static background: solid fill + separator lines, drawn once */
    cairo_surface_t *bg;

    /* cached Pango layouts for strings that change infrequently */
    PangoLayout *lo_title;    char c_title[256];
    PangoLayout *lo_artist;   char c_artist[256];
    PangoLayout *lo_album;    char c_album[256];
    PangoLayout *lo_time;     char c_time[32];
    PangoLayout *lo_tech;     char c_tech[128];
    PangoLayout *lo_pos;      char c_pos[32];
    PangoLayout *lo_remain;   char c_remain[32];

    /* marquee pixel width, cached per title */
    int title_px_w;

    /* true once background has been drawn */
    int bg_valid;
} RenderCache;

/* ══════════════════════════════════════════════════════════════
 *  Top-level application state
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    int        dirty;
    int        running;
    MpdState   mpd;

    double     scroll_x;
    double     scroll_speed;

    RenderCache cache;
    MenuState   menu;
} AppData;

static inline void app_request_redraw(AppData *app) { app->dirty = 1; }

/* ══════════════════════════════════════════════════════════════
 *  Drawing primitives (implemented in draw_common.c)
 *  All are flat/non-gradient for CPU efficiency.
 * ══════════════════════════════════════════════════════════════ */

/* flat filled rectangle */
void fill_rect(cairo_t *cr, double x, double y, double w, double h,
               double rr, double gg, double bb, double alpha);

/* flat progress bar: row of small solid rectangles */
void draw_progress(cairo_t *cr, double x, double y, double w, double h,
                   double frac);

/* crisp pixel-snapped text from a pre-built PangoLayout.
 * Pass a clip_w > 0 to limit rendering width (for marquee clip etc.) */
void draw_layout(cairo_t *cr, double x, double y, PangoLayout *lo,
                 double rr, double gg, double bb, double alpha);

/* build or rebuild a PangoLayout when text/font changes */
PangoLayout *make_layout(cairo_t *cr, const char *text, const char *font,
                         PangoAlignment align, double clip_w);

/* thin horizontal line */
void hairline(cairo_t *cr, double x0, double y, double x1,
              double rr, double gg, double bb, double alpha);

/* small filled square "pixel" — VFD dot substitute */
void draw_pixel(cairo_t *cr, double cx, double cy, double sz,
                double rr, double gg, double bb);

#endif /* APP_H */
