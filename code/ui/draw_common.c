/*
 * draw_common.c — low-level cairo/pango drawing helpers shared by
 * main_screen.c (playback screen) and menu.c (browse overlay).
 *
 * These are pure rendering functions: no MPD calls, no app state,
 * just cairo_t in, pixels out.
 */
#include "draw_common.h"
#include <math.h>

/* rounded rectangle path (caller fills/strokes) */
void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x+w-r, y+r,   r, -G_PI/2,  0);
    cairo_arc(cr, x+w-r, y+h-r, r,  0,        G_PI/2);
    cairo_arc(cr, x+r,   y+h-r, r,  G_PI/2,   G_PI);
    cairo_arc(cr, x+r,   y+r,   r,  G_PI,    -G_PI/2);
    cairo_close_path(cr);
}

/* radial "glow" dot — used for LEDs, progress segments, scroll dots */
void glow_dot(cairo_t *cr,
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
void draw_progress(cairo_t *cr,
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

/* crisp, pixel-snapped Pango text (no blur — see project history:
 * an earlier multi-pass bloom effect made text unreadable on real
 * displays, replaced with simple pixel snapping). */
void draw_text(cairo_t *cr,
              double x, double y,
              const char *text,
              const char *font,
              double rr, double gg, double bb, double alpha,
              PangoAlignment align,
              double clip_w)
{
    PangoLayout *lo = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(lo, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(lo, text, -1);
    pango_layout_set_alignment(lo, align);
    if (clip_w > 0)
        pango_layout_set_width(lo, (int)(clip_w * PANGO_SCALE));

    cairo_save(cr);
    cairo_translate(cr, floor(x), floor(y));
    cairo_set_source_rgba(cr, rr, gg, bb, alpha);
    pango_cairo_show_layout(cr, lo);
    cairo_restore(cr);

    g_object_unref(lo);
}

/* measure rendered pixel width of a string in a given font */
int measure_text_w(cairo_t *cr, const char *text, const char *font)
{
    PangoLayout *lo = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(lo, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(lo, text, -1);
    int w, h;
    pango_layout_get_pixel_size(lo, &w, &h);
    g_object_unref(lo);
    return w;
}

/* thin horizontal divider line */
void hairline(cairo_t *cr,
             double x0, double y, double x1,
             double rr, double gg, double bb, double alpha)
{
    cairo_set_source_rgba(cr, rr, gg, bb, alpha);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, x0, y);
    cairo_line_to(cr, x1, y);
    cairo_stroke(cr);
}
