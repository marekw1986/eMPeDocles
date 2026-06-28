/*
 * mpd_retro.c  —  320×240 GTK3 retro MPD display
 *
 * Looks like a late-90s rack-mount CD/DVD player front panel:
 *   • VFD-style blue-on-dark background
 *   • Scrolling title marquee
 *   • Progress bar (segment-dot style)
 *   • Artist / Album lines
 *   • Time elapsed / total
 *   • Play state glyph  ▶  ▐▐  ■
 *   • Volume knob readout and bitrate
 *
 * Build:
 *   gcc $(pkg-config --cflags --libs gtk+-3.0) -lm -o mpd_retro mpd_retro.c
 *
 * MPD is contacted at localhost:6600 (override with MPD_HOST / MPD_PORT env).
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
#include <fcntl.h>
#include <errno.h>

/* ── geometry ───────────────────────────────────────────────── */
#define WIN_W   320
#define WIN_H   240

/* ── MPD poll interval (ms) ─────────────────────────────────── */
#define POLL_MS 1000

/* ── colours (VFD palette) ──────────────────────────────────── */
#define COL_BG          0.04, 0.04, 0.08       /* near-black chassis    */
#define COL_SCREEN_BG   0.02, 0.04, 0.14       /* deep blue screen      */
#define COL_BRIGHT      0.35, 0.80, 1.00       /* bright VFD cyan-blue  */
#define COL_DIM         0.08, 0.22, 0.38       /* dim VFD (inactive)    */
#define COL_ACCENT      0.00, 0.55, 0.90       /* medium VFD            */
#define COL_AMBER       1.00, 0.65, 0.00       /* amber indicator LED   */
#define COL_RED_LED     1.00, 0.18, 0.10       /* red indicator LED     */
#define COL_BEZEL       0.18, 0.18, 0.22       /* plastic bezel         */
#define COL_RIVET       0.30, 0.30, 0.35       /* decorative rivets     */

/* ── MPD state ──────────────────────────────────────────────── */
typedef struct {
    char  title[256];
    char  artist[256];
    char  album[256];
    char  state[16];      /* "play" "pause" "stop" */
    int   elapsed;        /* seconds               */
    int   duration;       /* seconds               */
    int   volume;         /* 0‥100, -1=unknown     */
    int   bitrate;        /* kbps                  */
    int   samplerate;     /* Hz                    */
    int   connected;
} MpdState;

/* ── widget data ─────────────────────────────────────────────── */
typedef struct {
    GtkWidget *canvas;
    MpdState   mpd;

    /* marquee scroll */
    double     scroll_x;
    double     scroll_px_per_tick;
    int        title_px_width;    /* rendered pixel width of title string */

    guint      poll_id;
    guint      anim_id;
} AppData;

/* ══════════════════════════════════════════════════════════════
 *  MPD client (minimal, non-blocking read with timeout)
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
        .sin_port   = htons(port),
    };
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int mpd_readline(int fd, char *buf, int sz)
{
    int i = 0;
    while (i < sz - 1) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r <= 0) break;
        if (c == '\n') { buf[i] = '\0'; return i; }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

static void mpd_poll(MpdState *s)
{
    const char *host = getenv("MPD_HOST") ? getenv("MPD_HOST") : "localhost";
    int port = getenv("MPD_PORT") ? atoi(getenv("MPD_PORT")) : 6600;

    int fd = mpd_connect(host, port);
    if (fd < 0) {
        s->connected = 0;
        snprintf(s->title,  sizeof(s->title),  "NO CONNECTION");
        snprintf(s->artist, sizeof(s->artist), "MPD @ %s:%d", host, port);
        snprintf(s->album,  sizeof(s->album),  "");
        snprintf(s->state,  sizeof(s->state),  "stop");
        s->elapsed = s->duration = 0;
        return;
    }

    char line[512];
    /* read banner */
    mpd_readline(fd, line, sizeof(line));
    if (strncmp(line, "OK MPD", 6) != 0) { close(fd); s->connected=0; return; }

    /* ── currentsong ── */
    send(fd, "currentsong\n", 12, 0);

    s->title[0] = s->artist[0] = s->album[0] = '\0';

    while (1) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK", 2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;

        if      (strncmp(line, "Title: ",  7) == 0)
            snprintf(s->title,  sizeof(s->title),  "%s", line+7);
        else if (strncmp(line, "Artist: ", 8) == 0)
            snprintf(s->artist, sizeof(s->artist), "%s", line+8);
        else if (strncmp(line, "Album: ",  7) == 0)
            snprintf(s->album,  sizeof(s->album),  "%s", line+7);
    }

    /* ── status ── */
    send(fd, "status\n", 7, 0);

    s->volume = -1; s->bitrate = 0; s->samplerate = 0;
    snprintf(s->state, sizeof(s->state), "stop");
    s->elapsed = s->duration = 0;

    while (1) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK", 2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;

        if      (strncmp(line, "state: ",    7) == 0)
            snprintf(s->state, sizeof(s->state), "%s", line+7);
        else if (strncmp(line, "volume: ",   8) == 0)
            s->volume = atoi(line+8);
        else if (strncmp(line, "bitrate: ",  9) == 0)
            s->bitrate = atoi(line+9);
        else if (strncmp(line, "elapsed: ", 9) == 0)
            s->elapsed = (int)atof(line+9);
        else if (strncmp(line, "duration: ",10) == 0)
            s->duration = (int)atof(line+10);
        else if (strncmp(line, "audio: ",   7) == 0) {
            /* "44100:16:2" */
            s->samplerate = atoi(line+7);
        }
    }

    send(fd, "close\n", 6, 0);
    close(fd);
    s->connected = 1;

    /* fallback display strings */
    if (!s->title[0])  snprintf(s->title,  sizeof(s->title),  "(no title)");
    if (!s->artist[0]) snprintf(s->artist, sizeof(s->artist), "(unknown artist)");
}

/* ══════════════════════════════════════════════════════════════
 *  Drawing helpers
 * ══════════════════════════════════════════════════════════════ */

static void set_vfd_bright(cairo_t *cr) { cairo_set_source_rgb(cr, COL_BRIGHT); }
static void set_vfd_dim   (cairo_t *cr) { cairo_set_source_rgb(cr, COL_DIM);    }
static void set_vfd_accent(cairo_t *cr) { cairo_set_source_rgb(cr, COL_ACCENT); }

/* rounded rectangle */
static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x+w-r, y+r,   r,  -G_PI/2,  0);
    cairo_arc(cr, x+w-r, y+h-r, r,   0,        G_PI/2);
    cairo_arc(cr, x+r,   y+h-r, r,   G_PI/2,   G_PI);
    cairo_arc(cr, x+r,   y+r,   r,   G_PI,    -G_PI/2);
    cairo_close_path(cr);
}

/* glowing dot (LED / VFD segment pixel) */
static void glow_dot(cairo_t *cr, double cx, double cy, double r,
                     double rr, double gg, double bb)
{
    cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0, cx, cy, r*2.5);
    cairo_pattern_add_color_stop_rgba(p, 0.0, rr, gg, bb, 1.0);
    cairo_pattern_add_color_stop_rgba(p, 0.5, rr*0.6, gg*0.6, bb*0.6, 0.6);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0, 0, 0, 0);
    cairo_set_source(cr, p);
    cairo_arc(cr, cx, cy, r, 0, 2*G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

/* progress bar as a row of segment dots */
static void draw_progress(cairo_t *cr, double x, double y, double w, double h,
                          double frac)
{
    int  n   = (int)(w / 5.0);
    int  lit = (int)(frac * n);
    double r = h * 0.38;

    for (int i = 0; i < n; i++) {
        double cx = x + i * 5.0 + 2.5;
        double cy = y + h / 2.0;
        if (i < lit)
            glow_dot(cr, cx, cy, r, COL_BRIGHT);
        else
            glow_dot(cr, cx, cy, r*0.5, COL_DIM);
    }
}

/* text with glow using Pango */
static void draw_text_glow(cairo_t *cr,
                           double x, double y,
                           const char *text,
                           const char *font_desc,
                           double rr, double gg, double bb,
                           double alpha,
                           PangoAlignment align,
                           double clip_w)   /* 0 = no clip */
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font_desc);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_alignment(layout, align);
    if (clip_w > 0) pango_layout_set_width(layout, (int)(clip_w * PANGO_SCALE));

    /* glow pass */
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_set_source_rgba(cr, rr, gg, bb, alpha * 0.35);
    for (int dx = -2; dx <= 2; dx++)
    for (int dy = -2; dy <= 2; dy++) {
        cairo_save(cr);
        cairo_translate(cr, dx, dy);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
    }
    /* crisp pass */
    cairo_set_source_rgba(cr, rr, gg, bb, alpha);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    g_object_unref(layout);
}

/* measure text width in pixels */
static int text_pixel_width(cairo_t *cr, const char *text, const char *font_desc)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font_desc);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(layout, text, -1);
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    return w;
}

/* ══════════════════════════════════════════════════════════════
 *  Main draw callback
 * ══════════════════════════════════════════════════════════════ */

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    AppData  *app = (AppData *)user_data;
    MpdState *s   = &app->mpd;

    const double W = WIN_W;
    const double H = WIN_H;

    /* ── chassis background ── */
    cairo_set_source_rgb(cr, COL_BG);
    cairo_paint(cr);

    /* ── outer bezel (brushed metal feel via gradient) ── */
    {
        cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, W, H);
        cairo_pattern_add_color_stop_rgb(p, 0.0, 0.22, 0.22, 0.26);
        cairo_pattern_add_color_stop_rgb(p, 0.5, 0.14, 0.14, 0.18);
        cairo_pattern_add_color_stop_rgb(p, 1.0, 0.10, 0.10, 0.14);
        cairo_set_source(cr, p);
        rrect(cr, 4, 4, W-8, H-8, 8);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    }

    /* ── screen area ── */
    const double SX = 12, SY = 10, SW = W-24, SH = H-55;
    {
        /* deep recess shadow */
        cairo_set_source_rgba(cr, 0,0,0, 0.8);
        rrect(cr, SX-2, SY-2, SW+4, SH+4, 5);
        cairo_fill(cr);

        cairo_pattern_t *p = cairo_pattern_create_linear(SX, SY, SX, SY+SH);
        cairo_pattern_add_color_stop_rgb(p, 0.0, 0.01, 0.03, 0.10);
        cairo_pattern_add_color_stop_rgb(p, 1.0, 0.02, 0.05, 0.16);
        cairo_set_source(cr, p);
        rrect(cr, SX, SY, SW, SH, 4);
        cairo_fill(cr);
        cairo_pattern_destroy(p);

        /* subtle scanlines */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
        for (double ly = SY; ly < SY+SH; ly += 2) {
            cairo_rectangle(cr, SX, ly, SW, 1);
        }
        cairo_fill(cr);

        /* screen inner glow */
        cairo_pattern_t *g = cairo_pattern_create_radial(SX+SW/2, SY+SH/2, 10,
                                                          SX+SW/2, SY+SH/2, SW*0.7);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 0.0, 0.15, 0.40, 0.25);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 0.0, 0.00, 0.00, 0.00);
        cairo_set_source(cr, g);
        rrect(cr, SX, SY, SW, SH, 4);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* ── brand label ── */
    draw_text_glow(cr, SX+4, SY+3, "RETRO·MPD", "Mono 6",
                   COL_ACCENT, 0.7, PANGO_ALIGN_LEFT, 0);

    /* ── connection status LED ── */
    if (s->connected)
        glow_dot(cr, SW+SX-7, SY+7, 3.5, COL_AMBER);
    else
        glow_dot(cr, SW+SX-7, SY+7, 3.5, COL_RED_LED);

    /* ── play state glyph ── */
    {
        const char *glyph =
            strcmp(s->state, "play")  == 0 ? "▶" :
            strcmp(s->state, "pause") == 0 ? "⏸" : "■";
        double rr = strcmp(s->state,"play")==0 ? 0.2 : 0.7;
        double gg = strcmp(s->state,"play")==0 ? 0.9 : 0.4;
        double bb = strcmp(s->state,"play")==0 ? 0.3 : 0.1;
        draw_text_glow(cr, SX+6, SY+18, glyph, "Sans Bold 14",
                       rr, gg, bb, 1.0, PANGO_ALIGN_LEFT, 0);
    }

    /* ── scrolling title (marquee) ── */
    {
        const char *title_font = "Mono Bold 13";
        double tx = SX + 28;
        double ty = SY + 17;
        double tw = SW - 36;

        /* measure once per title change */
        if (app->title_px_width == 0)
            app->title_px_width = text_pixel_width(cr, s->title, title_font) + 30;

        cairo_save(cr);
        cairo_rectangle(cr, tx, ty, tw, 22);
        cairo_clip(cr);

        double ox = -app->scroll_x;
        draw_text_glow(cr, tx + ox, ty, s->title, title_font,
                       COL_BRIGHT, 1.0, PANGO_ALIGN_LEFT, 0);

        /* wrap-around copy */
        if (ox + app->title_px_width < tw)
            draw_text_glow(cr, tx + ox + app->title_px_width, ty, s->title,
                           title_font, COL_BRIGHT, 1.0, PANGO_ALIGN_LEFT, 0);

        cairo_restore(cr);
    }

    /* ── separator line ── */
    cairo_set_source_rgba(cr, COL_ACCENT, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, SX+4,    SY+42);
    cairo_line_to(cr, SX+SW-4, SY+42);
    cairo_stroke(cr);

    /* ── artist / album ── */
    {
        char info[300];
        snprintf(info, sizeof(info), "%s", s->artist);
        draw_text_glow(cr, SX+6, SY+46, info, "Mono 9",
                       COL_ACCENT, 0.9, PANGO_ALIGN_LEFT, SW-10);

        snprintf(info, sizeof(info), "%s", s->album);
        draw_text_glow(cr, SX+6, SY+60, info, "Mono 8",
                       COL_DIM, 1.0, PANGO_ALIGN_LEFT, SW-10);
    }

    /* ── separator line ── */
    cairo_set_source_rgba(cr, COL_ACCENT, 0.2);
    cairo_move_to(cr, SX+4,    SY+78);
    cairo_line_to(cr, SX+SW-4, SY+78);
    cairo_stroke(cr);

    /* ── progress bar ── */
    {
        double frac = (s->duration > 0)
                      ? (double)s->elapsed / s->duration : 0.0;
        draw_progress(cr, SX+6, SY+82, SW-12, 8, frac);
    }

    /* ── time readout ── */
    {
        char tbuf[32];
        int es = s->elapsed,  em = es/60; es %= 60;
        int ds = s->duration, dm = ds/60; ds %= 60;
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d / %02d:%02d", em, es, dm, ds);
        draw_text_glow(cr, SX + (SW/2), SY+94, tbuf, "Mono Bold 9",
                       COL_BRIGHT, 0.85, PANGO_ALIGN_CENTER, SW);
    }

    /* ── separator line ── */
    cairo_set_source_rgba(cr, COL_ACCENT, 0.2);
    cairo_move_to(cr, SX+4,    SY+110);
    cairo_line_to(cr, SX+SW-4, SY+110);
    cairo_stroke(cr);

    /* ── tech readout: bitrate / samplerate / volume ── */
    {
        char tech[128];
        int  sr_khz = s->samplerate / 1000;
        if (s->bitrate > 0 || s->samplerate > 0)
            snprintf(tech, sizeof(tech), "%dkbps  %dkHz  VOL:%d%%",
                     s->bitrate, sr_khz,
                     s->volume >= 0 ? s->volume : 0);
        else
            snprintf(tech, sizeof(tech), "VOL: %d%%",
                     s->volume >= 0 ? s->volume : 0);

        draw_text_glow(cr, SX + SW/2, SY+114, tech, "Mono 8",
                       COL_DIM, 1.0, PANGO_ALIGN_CENTER, SW);
    }

    /* ── bottom panel: control buttons (decorative) ── */
    {
        const double BY  = H - 42;
        const double BH  = 22;
        const double BW  = 36;
        const double GAP = 6;
        const char  *labels[] = { "⏮", "⏪", "⏩", "⏭", "⏏" };
        const int    NB = 5;
        double total = NB*BW + (NB-1)*GAP;
        double bx    = (W - total) / 2.0;

        for (int i = 0; i < NB; i++) {
            double x = bx + i*(BW+GAP);

            /* button body */
            cairo_pattern_t *p = cairo_pattern_create_linear(x, BY, x, BY+BH);
            cairo_pattern_add_color_stop_rgb(p, 0.0, 0.28, 0.28, 0.32);
            cairo_pattern_add_color_stop_rgb(p, 1.0, 0.14, 0.14, 0.18);
            cairo_set_source(cr, p);
            rrect(cr, x, BY, BW, BH, 3);
            cairo_fill(cr);
            cairo_pattern_destroy(p);

            /* button highlight */
            cairo_set_source_rgba(cr, 1,1,1, 0.08);
            rrect(cr, x+1, BY+1, BW-2, BH/2, 3);
            cairo_fill(cr);

            /* button border */
            cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
            cairo_set_line_width(cr, 1);
            rrect(cr, x, BY, BW, BH, 3);
            cairo_stroke(cr);

            /* label */
            draw_text_glow(cr, x + BW/2 - 6, BY+3, labels[i], "Sans 10",
                           0.70, 0.70, 0.75, 1.0, PANGO_ALIGN_LEFT, 0);
        }
    }

    /* ── decorative rivets ── */
    {
        double rv[][2] = {{9,9},{W-9,9},{9,H-9},{W-9,H-9}};
        for (int i=0;i<4;i++) {
            cairo_pattern_t *p = cairo_pattern_create_radial(
                rv[i][0]-1, rv[i][1]-1, 0,
                rv[i][0],   rv[i][1],   4);
            cairo_pattern_add_color_stop_rgb(p, 0, 0.45,0.45,0.50);
            cairo_pattern_add_color_stop_rgb(p, 1, 0.12,0.12,0.15);
            cairo_set_source(cr, p);
            cairo_arc(cr, rv[i][0], rv[i][1], 3.5, 0, 2*G_PI);
            cairo_fill(cr);
            cairo_pattern_destroy(p);
        }
    }

    /* ── screen reflection glare ── */
    {
        cairo_save(cr);
        cairo_rectangle(cr, SX, SY, SW, SH);
        cairo_clip(cr);
        cairo_pattern_t *g = cairo_pattern_create_linear(SX, SY, SX+SW*0.6, SY+SH*0.4);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 1,1,1, 0.04);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 1,1,1, 0.00);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
        cairo_restore(cr);
    }

    return FALSE;
}

/* ══════════════════════════════════════════════════════════════
 *  Timers
 * ══════════════════════════════════════════════════════════════ */

static gboolean on_anim_tick(gpointer data)
{
    AppData *app = (AppData *)data;

    if (strcmp(app->mpd.state, "play") == 0) {
        app->scroll_x += app->scroll_px_per_tick;
        if (app->title_px_width > 0 &&
            app->scroll_x >= app->title_px_width)
            app->scroll_x = 0;
    }

    gtk_widget_queue_draw(app->canvas);
    return TRUE;
}

static gboolean on_poll_tick(gpointer data)
{
    AppData *app = (AppData *)data;

    char old_title[256];
    snprintf(old_title, sizeof(old_title), "%s", app->mpd.title);

    mpd_poll(&app->mpd);

    /* reset marquee when track changes */
    if (strcmp(old_title, app->mpd.title) != 0) {
        app->scroll_x = 0;
        app->title_px_width = 0;
    }

    gtk_widget_queue_draw(app->canvas);
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 *  Entry point
 * ══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);

    AppData app;
    memset(&app, 0, sizeof(app));
    app.scroll_px_per_tick = 0.8;   /* pixels per animation frame */
    snprintf(app.mpd.state, sizeof(app.mpd.state), "stop");

    /* initial poll */
    mpd_poll(&app.mpd);

    /* window */
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "MPD – Retro Display");
    gtk_window_set_default_size(GTK_WINDOW(win), WIN_W, WIN_H);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* drawing area */
    app.canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.canvas, WIN_W, WIN_H);
    g_signal_connect(app.canvas, "draw", G_CALLBACK(on_draw), &app);
    gtk_container_add(GTK_CONTAINER(win), app.canvas);

    /* timers */
    app.anim_id = g_timeout_add(33,       on_anim_tick, &app);  /* ~30 fps */
    app.poll_id = g_timeout_add(POLL_MS,  on_poll_tick, &app);

    gtk_widget_show_all(win);
    gtk_main();

    g_source_remove(app.anim_id);
    g_source_remove(app.poll_id);
    return 0;
}
