/*
 * mpd_retro.c  —  320×240 GTK3 retro MPD display
 *
 * VFD-style front-panel display, no on-screen buttons —
 * all control is via hardware buttons / IR receiver.
 *
 * Layout (screen fills the full bezel):
 *
 *   ┌─────────────────────────────────────┐
 *   │ RETRO·MPD                       [●] │  ← brand + LED
 *   │ ▶  Scrolling Title Here …           │  ← state + marquee
 *   │─────────────────────────────────────│
 *   │  Artist Name                        │
 *   │  Album Title                        │
 *   │─────────────────────────────────────│
 *   │  · · · · · · · · · · · · · · · ·   │  ← dot progress
 *   │           02:34 / 04:12             │  ← time
 *   │─────────────────────────────────────│
 *   │      320kbps · 44kHz · VOL 85%      │  ← tech info
 *   └─────────────────────────────────────┘
 *
 * Build:
 *   gcc $(pkg-config --cflags --libs gtk+-3.0) -lm -o mpd_retro mpd_retro.c
 *
 * MPD: localhost:6600  (override: MPD_HOST / MPD_PORT env vars)
 */

#include <gtk/gtk.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

/* ── geometry ───────────────────────────────────────────────── */
#define WIN_W  320
#define WIN_H  240

/* ── poll interval ──────────────────────────────────────────── */
#define POLL_MS 1000

/* ── VFD colour palette ─────────────────────────────────────── */
#define COL_CHASSIS   0.04, 0.04, 0.08        /* outer body            */
#define COL_SCREEN    0.015, 0.030, 0.120     /* screen background     */
#define COL_BRIGHT    0.40, 0.85, 1.00        /* bright VFD cyan       */
#define COL_MID       0.00, 0.55, 0.88        /* medium VFD            */
#define COL_DIM       0.06, 0.20, 0.36        /* inactive segments     */
#define COL_AMBER     1.00, 0.62, 0.00        /* connection OK LED     */
#define COL_RED       1.00, 0.16, 0.08        /* connection fail LED   */

/* ── MPD state ──────────────────────────────────────────────── */
typedef struct {
    char  title[256];
    char  artist[256];
    char  album[256];
    char  state[16];   /* "play" | "pause" | "stop" */
    int   elapsed;     /* seconds */
    int   duration;    /* seconds */
    int   volume;      /* 0-100, -1 = unknown */
    int   bitrate;     /* kbps */
    int   samplerate;  /* Hz */
    int   song;        /* 0-based playlist index, -1 = unknown */
    int   playlistlen; /* total tracks in playlist, 0 = unknown */
    int   connected;
} MpdState;

/* ── application data ───────────────────────────────────────── */
typedef struct {
    GtkWidget *canvas;
    MpdState   mpd;

    double  scroll_x;
    double  scroll_speed;   /* px / anim frame */
    int     title_px_w;     /* cached rendered width of title */

    guint   anim_id;
    guint   poll_id;
} AppData;

/* ══════════════════════════════════════════════════════════════
 *  Minimal MPD client
 * ══════════════════════════════════════════════════════════════ */

static int mpd_connect(const char *host, int port)
{
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int mpd_readline(int fd, char *buf, int sz)
{
    int i = 0;
    while (i < sz - 1) {
        char c;
        if (recv(fd, &c, 1, 0) <= 0) break;
        if (c == '\n') { buf[i] = '\0'; return i; }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

static void mpd_poll(MpdState *s)
{
    const char *host = getenv("MPD_HOST") ? getenv("MPD_HOST") : "192.168.1.67";
    int port = getenv("MPD_PORT") ? atoi(getenv("MPD_PORT")) : 6600;

    int fd = mpd_connect(host, port);
    if (fd < 0) {
        s->connected = 0;
        snprintf(s->title,  sizeof(s->title),  "NO CONNECTION");
        snprintf(s->artist, sizeof(s->artist), "mpd @ %s:%d", host, port);
        s->album[0] = '\0';
        snprintf(s->state,  sizeof(s->state),  "stop");
        s->elapsed = s->duration = 0;
        return;
    }

    char line[512];
    mpd_readline(fd, line, sizeof(line));   /* banner */
    if (strncmp(line, "OK MPD", 6) != 0) { close(fd); s->connected = 0; return; }

    /* currentsong */
    send(fd, "currentsong\n", 12, 0);
    s->title[0] = s->artist[0] = s->album[0] = '\0';
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
        if      (strncmp(line, "Title: ",  7) == 0) snprintf(s->title,  sizeof(s->title),  "%s", line+7);
        else if (strncmp(line, "Artist: ", 8) == 0) snprintf(s->artist, sizeof(s->artist), "%s", line+8);
        else if (strncmp(line, "Album: ",  7) == 0) snprintf(s->album,  sizeof(s->album),  "%s", line+7);
    }

    /* status */
    send(fd, "status\n", 7, 0);
    s->volume = -1; s->bitrate = 0; s->samplerate = 0;
    s->song = -1; s->playlistlen = 0;
    snprintf(s->state, sizeof(s->state), "stop");
    s->elapsed = s->duration = 0;
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
        if      (strncmp(line, "state: ",          7) == 0) snprintf(s->state, sizeof(s->state), "%s", line+7);
        else if (strncmp(line, "volume: ",          8) == 0) s->volume      = atoi(line+8);
        else if (strncmp(line, "bitrate: ",         9) == 0) s->bitrate     = atoi(line+9);
        else if (strncmp(line, "elapsed: ",         9) == 0) s->elapsed     = (int)atof(line+9);
        else if (strncmp(line, "duration: ",       10) == 0) s->duration    = (int)atof(line+10);
        else if (strncmp(line, "audio: ",           7) == 0) s->samplerate  = atoi(line+7);
        else if (strncmp(line, "song: ",            6) == 0) s->song        = atoi(line+6);
        else if (strncmp(line, "playlistlength: ", 16) == 0) s->playlistlen = atoi(line+16);
    }

    send(fd, "close\n", 6, 0);
    close(fd);
    s->connected = 1;

    if (!s->title[0])  snprintf(s->title,  sizeof(s->title),  "(no title)");
    if (!s->artist[0]) snprintf(s->artist, sizeof(s->artist), "(unknown artist)");
}

/* ══════════════════════════════════════════════════════════════
 *  Drawing primitives
 * ══════════════════════════════════════════════════════════════ */

static void rrect(cairo_t *cr,
                  double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x+w-r, y+r,   r, -G_PI/2,  0);
    cairo_arc(cr, x+w-r, y+h-r, r,  0,        G_PI/2);
    cairo_arc(cr, x+r,   y+h-r, r,  G_PI/2,   G_PI);
    cairo_arc(cr, x+r,   y+r,   r,  G_PI,    -G_PI/2);
    cairo_close_path(cr);
}

/* radial glow dot — used for LEDs and progress segments */
static void glow_dot(cairo_t *cr,
                     double cx, double cy, double r,
                     double rr, double gg, double bb, double alpha)
{
    cairo_pattern_t *p =
        cairo_pattern_create_radial(cx, cy, 0, cx, cy, r * 2.8);
    cairo_pattern_add_color_stop_rgba(p, 0.00, rr,       gg,       bb,       alpha);
    cairo_pattern_add_color_stop_rgba(p, 0.45, rr * 0.7, gg * 0.7, bb * 0.7, alpha * 0.55);
    cairo_pattern_add_color_stop_rgba(p, 1.00, 0, 0, 0, 0);
    cairo_set_source(cr, p);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

/* dot-segment progress bar */
static void draw_progress(cairo_t *cr,
                           double x, double y, double w, double h,
                           double frac)
{
    const double pitch = 5.5;
    int n   = (int)(w / pitch);
    int lit = (int)(frac * n);
    double r = h * 0.40;

    for (int i = 0; i < n; i++) {
        double cx = x + i * pitch + pitch / 2.0;
        double cy = y + h / 2.0;
        if (i < lit)
            glow_dot(cr, cx, cy, r,     COL_BRIGHT, 1.0);
        else
            glow_dot(cr, cx, cy, r*0.6, COL_DIM,    1.0);
    }
}

/* Crisp Pango text — pixel-snapped, no blur */
static void draw_text(cairo_t *cr,
                      double x, double y,
                      const char *text,
                      const char *font,
                      double rr, double gg, double bb, double alpha,
                      PangoAlignment align,
                      double clip_w)   /* 0 = no width constraint */
{
    PangoLayout *lo = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(lo, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(lo, text, -1);
    pango_layout_set_alignment(lo, align);
    if (clip_w > 0)
        pango_layout_set_width(lo, (int)(clip_w * PANGO_SCALE));

    /* snap to whole pixels so hinting works properly */
    double px = floor(x), py = floor(y);

    cairo_save(cr);
    cairo_translate(cr, px, py);
    cairo_set_source_rgba(cr, rr, gg, bb, alpha);
    pango_cairo_show_layout(cr, lo);
    cairo_restore(cr);

    g_object_unref(lo);
}

static int measure_text_w(cairo_t *cr, const char *text, const char *font)
{
    PangoLayout *lo = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(lo, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(lo, text, -1);
    int w, h; pango_layout_get_pixel_size(lo, &w, &h);
    g_object_unref(lo);
    return w;
}

static void hairline(cairo_t *cr,
                     double x0, double y, double x1,
                     double rr, double gg, double bb, double alpha)
{
    cairo_set_source_rgba(cr, rr, gg, bb, alpha);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, x0, y);
    cairo_line_to(cr, x1, y);
    cairo_stroke(cr);
}

/* ══════════════════════════════════════════════════════════════
 *  Main draw
 * ══════════════════════════════════════════════════════════════ */

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    (void)widget;
    AppData  *app = (AppData *)user_data;
    MpdState *s   = &app->mpd;

    const double W = WIN_W, H = WIN_H;

    /* ── outer chassis ── */
    cairo_set_source_rgb(cr, COL_CHASSIS);
    cairo_paint(cr);

    /* ── bezel gradient ── */
    {
        cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, W, H);
        cairo_pattern_add_color_stop_rgb(p, 0.0, 0.24, 0.24, 0.28);
        cairo_pattern_add_color_stop_rgb(p, 0.5, 0.15, 0.15, 0.19);
        cairo_pattern_add_color_stop_rgb(p, 1.0, 0.09, 0.09, 0.13);
        cairo_set_source(cr, p);
        rrect(cr, 4, 4, W-8, H-8, 8);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    }

    /* ── screen ── */
    /* screen fills the bezel with a small inset margin */
    const double SX = 10, SY = 8, SW = W-20, SH = H-16;
    {
        /* drop-shadow / recess */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
        rrect(cr, SX-2, SY-2, SW+4, SH+4, 5);
        cairo_fill(cr);

        /* screen fill */
        cairo_pattern_t *p = cairo_pattern_create_linear(SX, SY, SX, SY+SH);
        cairo_pattern_add_color_stop_rgb(p, 0.0, 0.010, 0.025, 0.095);
        cairo_pattern_add_color_stop_rgb(p, 1.0, 0.018, 0.040, 0.140);
        cairo_set_source(cr, p);
        rrect(cr, SX, SY, SW, SH, 4);
        cairo_fill(cr);
        cairo_pattern_destroy(p);

        /* scanlines */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.10);
        for (double ly = SY + 1; ly < SY+SH; ly += 2) {
            cairo_rectangle(cr, SX, ly, SW, 1);
        }
        cairo_fill(cr);

        /* centre bloom */
        cairo_pattern_t *g = cairo_pattern_create_radial(
            SX+SW/2, SY+SH*0.45, 8,
            SX+SW/2, SY+SH*0.45, SW * 0.65);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 0.00, 0.12, 0.38, 0.22);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 0.00, 0.00, 0.00, 0.00);
        cairo_set_source(cr, g);
        rrect(cr, SX, SY, SW, SH, 4);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* all further drawing is inside screen area; clip to it */
    cairo_save(cr);
    rrect(cr, SX, SY, SW, SH, 4);
    cairo_clip(cr);

    /*
     * Vertical layout — absolute Y values derived from SY=8, SH=224:
     *
     *   SY+ 4  …  15   brand label / LED              (row 0)
     *   SY+16  …  17   separator
     *   SY+19  …  52   state glyph + title marquee    (row 1, 16pt)
     *   SY+53  …  54   separator
     *   SY+57  …  77   artist                         (row 2, 13pt)
     *   SY+80  …  97   album                          (row 3, 11pt)
     *   SY+99  … 100   separator
     *   SY+104 … 118   progress bar                   (row 4, 14px tall)
     *   SY+121 … 143   time elapsed / total           (row 5, 15pt)
     *   SY+145 … 146   separator
     *   SY+150 … 162   tech info (bitrate/rate/vol)   (row 6, 9pt)
     *   SY+165 … 166   separator
     *   SY+170 … 185   REP · RND · SGL indicators     (row 7)
     *   SY+189 … 200   remaining time                 (row 8, dim)
     *   SY+204 … 220   volume bar                     (row 9)
     */

    /* ── row 0: brand label + connection LED ─────────────────── */
    draw_text(cr, SX+6, SY+4, "RETRO\xC2\xB7MPD", "Mono 7",
              COL_MID, 0.70, PANGO_ALIGN_LEFT, 0);

    if (s->connected)
        glow_dot(cr, SX+SW-9, SY+10, 4.5, COL_AMBER, 1.0);
    else
        glow_dot(cr, SX+SW-9, SY+10, 4.5, COL_RED,   1.0);

    /* ── separator ── */
    hairline(cr, SX+5, SY+18, SX+SW-5, COL_MID, 0.25);

    /* ── row 1: play-state glyph + scrolling title ───────────── */
    {
        const char *glyph;
        double gr, gg_c, gb;
        if      (strcmp(s->state, "play")  == 0) { glyph = "\xe2\x96\xb6"; gr=0.15; gg_c=0.92; gb=0.35; }
        else if (strcmp(s->state, "pause") == 0) { glyph = "\xe2\x8f\xb8"; gr=0.95; gg_c=0.75; gb=0.05; }
        else                                      { glyph = "\xe2\x96\xa0"; gr=0.65; gg_c=0.15; gb=0.10; }
        draw_text(cr, SX+4, SY+21, glyph, "Sans Bold 16",
                  gr, gg_c, gb, 1.0, PANGO_ALIGN_LEFT, 0);

        const char *tf = "Mono Bold 16";
        double tx = SX + 30, ty = SY + 21, tw = SW - 36;

        if (app->title_px_w == 0)
            app->title_px_w = measure_text_w(cr, s->title, tf) + 40;

        cairo_save(cr);
        cairo_rectangle(cr, tx, ty, tw, 28);
        cairo_clip(cr);
        double ox = -app->scroll_x;
        draw_text(cr, tx + ox, ty, s->title, tf,
                  COL_BRIGHT, 1.0, PANGO_ALIGN_LEFT, 0);
        if (ox + app->title_px_w < tw)
            draw_text(cr, tx + ox + app->title_px_w, ty, s->title, tf,
                      COL_BRIGHT, 1.0, PANGO_ALIGN_LEFT, 0);
        cairo_restore(cr);
    }

    /* ── separator ── */
    hairline(cr, SX+5, SY+54, SX+SW-5, COL_MID, 0.25);

    /* ── row 2: artist ───────────────────────────────────────── */
    draw_text(cr, SX+6, SY+58, s->artist, "Mono 13",
              COL_MID, 0.95, PANGO_ALIGN_LEFT, SW-10);

    /* ── row 3: album ────────────────────────────────────────── */
    draw_text(cr, SX+6, SY+79, s->album[0] ? s->album : "", "Mono 11",
              COL_DIM, 1.0, PANGO_ALIGN_LEFT, SW-10);

    /* ── separator ── */
    hairline(cr, SX+5, SY+100, SX+SW-5, COL_MID, 0.22);

    /* ── row 4: progress bar (taller for easier reading) ─────── */
    {
        double frac = (s->duration > 0)
                      ? (double)s->elapsed / (double)s->duration : 0.0;
        draw_progress(cr, SX+6, SY+105, SW-12, 14, frac);
    }

    /* ── row 5: time — centred, prominent ───────────────────── */
    {
        char t[32];
        int em = s->elapsed/60, es = s->elapsed%60;
        int dm = s->duration/60, ds = s->duration%60;
        snprintf(t, sizeof(t), "%02d:%02d / %02d:%02d", em, es, dm, ds);
        draw_text(cr, SX + SW/2.0, SY+123, t, "Mono Bold 15",
                  COL_BRIGHT, 0.95, PANGO_ALIGN_CENTER, SW);
    }

    /* ── separator ── */
    hairline(cr, SX+5, SY+148, SX+SW-5, COL_MID, 0.20);

    /* ── row 6: playlist position (left) + bitrate/rate/vol (centre) ── */
    {
        /* left: track index "11/11" */
        if (s->song >= 0 && s->playlistlen > 0) {
            char pos[16];
            snprintf(pos, sizeof(pos), "%d/%d", s->song + 1, s->playlistlen);
            draw_text(cr, SX+6, SY+152, pos, "Mono Bold 9",
                      COL_MID, 0.90, PANGO_ALIGN_LEFT, 0);
        }

        /* centre: bitrate / sample-rate / volume */
        char tech[128];
        int  sr  = s->samplerate / 1000;
        int  vol = (s->volume >= 0) ? s->volume : 0;

        if (s->bitrate > 0 && sr > 0)
            snprintf(tech, sizeof(tech),
                     "%d kbps  \xC2\xB7  %d kHz  \xC2\xB7  VOL %d%%",
                     s->bitrate, sr, vol);
        else if (sr > 0)
            snprintf(tech, sizeof(tech),
                     "%d kHz  \xC2\xB7  VOL %d%%", sr, vol);
        else
            snprintf(tech, sizeof(tech), "VOL %d%%", vol);

        draw_text(cr, SX + SW/2.0, SY+152, tech, "Mono 9",
                  COL_DIM, 1.0, PANGO_ALIGN_CENTER, SW);
    }

    /* ── separator ── */
    hairline(cr, SX+5, SY+170, SX+SW-5, COL_MID, 0.18);

    /* ── row 7: REP / RND / SGL indicator dots + labels ─────── */
    {
        const char *labels[] = { "REP", "RND", "SGL" };
        double spacing = SW / 4.0;
        for (int i = 0; i < 3; i++) {
            double cx = SX + spacing * (i + 1);
            /* dots dim until repeat/random/single parsed from status */
            glow_dot(cr, cx, SY+179, 4.0, COL_DIM, 1.0);
            draw_text(cr, cx - 9, SY+186, labels[i], "Mono 7",
                      COL_DIM, 0.80, PANGO_ALIGN_LEFT, 0);
        }
    }

    /* ── row 8: remaining time (right-aligned, subtle) ───────── */
    if (s->duration > 0) {
        int rem = s->duration - s->elapsed;
        int rm = rem/60, rs = rem%60;
        char rbuf[24];
        snprintf(rbuf, sizeof(rbuf), "-%02d:%02d remaining", rm, rs);
        draw_text(cr, SX + SW/2.0, SY+200, rbuf, "Mono 8",
                  COL_DIM, 0.60, PANGO_ALIGN_CENTER, SW);
    }

    /* ── row 9: volume bar ───────────────────────────────────── */
    {
        double vol_frac = (s->volume >= 0) ? s->volume / 100.0 : 0.0;
        /* thin filled bar across bottom */
        double bx = SX + 6, by = SY + SH - 12, bw = SW - 12, bh = 6;
        /* track */
        cairo_set_source_rgba(cr, COL_DIM, 0.5);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);
        /* fill */
        if (vol_frac > 0) {
            cairo_pattern_t *p =
                cairo_pattern_create_linear(bx, 0, bx + bw, 0);
            cairo_pattern_add_color_stop_rgba(p, 0.0, COL_MID,    0.7);
            cairo_pattern_add_color_stop_rgba(p, 0.8, COL_BRIGHT, 0.9);
            cairo_pattern_add_color_stop_rgba(p, 1.0, COL_BRIGHT, 0.9);
            cairo_set_source(cr, p);
            cairo_rectangle(cr, bx, by, bw * vol_frac, bh);
            cairo_fill(cr);
            cairo_pattern_destroy(p);
        }
        /* "VOL" label left of bar */
        draw_text(cr, SX+6, SY+SH-14, "VOL", "Mono 6",
                  COL_DIM, 0.55, PANGO_ALIGN_LEFT, 0);
    }

    /* ── screen glare ── */
    {
        cairo_pattern_t *g =
            cairo_pattern_create_linear(SX, SY, SX + SW*0.55, SY + SH*0.35);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 1,1,1, 0.035);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 1,1,1, 0.000);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
    }

    cairo_restore(cr);  /* end screen clip */

    /* ── corner rivets ── */
    {
        double rv[][2] = { {9,9}, {W-9,9}, {9,H-9}, {W-9,H-9} };
        for (int i = 0; i < 4; i++) {
            cairo_pattern_t *p = cairo_pattern_create_radial(
                rv[i][0]-1, rv[i][1]-1, 0,
                rv[i][0],   rv[i][1],   4.5);
            cairo_pattern_add_color_stop_rgb(p, 0.0, 0.48, 0.48, 0.53);
            cairo_pattern_add_color_stop_rgb(p, 1.0, 0.11, 0.11, 0.14);
            cairo_set_source(cr, p);
            cairo_arc(cr, rv[i][0], rv[i][1], 3.5, 0, 2*G_PI);
            cairo_fill(cr);
            cairo_pattern_destroy(p);
        }
    }

    return FALSE;
}

/* ══════════════════════════════════════════════════════════════
 *  Timers
 * ══════════════════════════════════════════════════════════════ */

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

static gboolean on_poll(gpointer data)
{
    AppData *app = (AppData *)data;
    char old[256];
    snprintf(old, sizeof(old), "%s", app->mpd.title);

    mpd_poll(&app->mpd);

    if (strcmp(old, app->mpd.title) != 0) {
        app->scroll_x  = 0.0;
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
    g_signal_connect(app.canvas, "draw", G_CALLBACK(on_draw), &app);
    gtk_container_add(GTK_CONTAINER(win), app.canvas);

    app.anim_id = g_timeout_add(33,      on_anim, &app);   /* ~30 fps */
    app.poll_id = g_timeout_add(POLL_MS, on_poll, &app);

    gtk_widget_show_all(win);
    gtk_main();

    g_source_remove(app.anim_id);
    g_source_remove(app.poll_id);
    return 0;
}
