/*
 * draw_common.c — flat, CPU-efficient drawing primitives
 *
 * Everything here is solid-colour fills and pixel-snapped Pango
 * text. No radial gradients, no multi-pass glow, no scanlines.
 * On a Pi Zero this is the difference between ~50% and ~3% CPU.
 */
#include "draw_common.h"
#include <math.h>
#include <string.h>

/* ── flat filled rectangle ─────────────────────────────────── */
void fill_rect(cairo_t *cr, double x, double y, double w, double h,
               double rr, double gg, double bb, double alpha)
{
    cairo_set_source_rgba(cr, rr, gg, bb, alpha);
    cairo_rectangle(cr, floor(x), floor(y), floor(w), floor(h));
    cairo_fill(cr);
}

/* ── VFD "pixel" — small filled square ─────────────────────── */
void draw_pixel(cairo_t *cr, double cx, double cy, double sz,
                double rr, double gg, double bb)
{
    cairo_set_source_rgb(cr, rr, gg, bb);
    cairo_rectangle(cr, floor(cx - sz/2), floor(cy - sz/2), sz, sz);
    cairo_fill(cr);
}

/* ── progress bar: row of small squares ────────────────────── */
void draw_progress(cairo_t *cr, double x, double y, double w, double h,
                   double frac)
{
    const double pitch = 5.5;   /* dot spacing              */
    const double sz    = 3.0;   /* square side length       */
    int n   = (int)(w / pitch);
    int lit = (int)(frac * n);

    for (int i = 0; i < n; i++) {
        double cx = x + i * pitch + pitch / 2.0;
        double cy = y + h / 2.0;
        if (i < lit)
            draw_pixel(cr, cx, cy, sz, COL_BRIGHT);
        else
            draw_pixel(cr, cx, cy, sz, COL_DIM);
    }
}

/* ── thin horizontal line ───────────────────────────────────── */
void hairline(cairo_t *cr, double x0, double y, double x1,
              double rr, double gg, double bb, double alpha)
{
    cairo_set_source_rgba(cr, rr, gg, bb, alpha);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, floor(x0), floor(y) + 0.5);
    cairo_line_to(cr, floor(x1), floor(y) + 0.5);
    cairo_stroke(cr);
}

/* ── Pango layout factory ───────────────────────────────────── */

/*
 * Create (or recreate) a PangoLayout with the given text and font.
 * Caller owns the returned object; call g_object_unref() to free.
 * clip_w > 0 sets a pixel-width wrap/clip limit (PANGO_WRAP_WORD_CHAR).
 */
PangoLayout *make_layout(cairo_t *cr, const char *text, const char *font,
                         PangoAlignment align, double clip_w)
{
    PangoLayout *lo = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(lo, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(lo, text, -1);
    pango_layout_set_alignment(lo, align);
    if (clip_w > 0) {
        pango_layout_set_width(lo, (int)(clip_w * PANGO_SCALE));
        pango_layout_set_ellipsize(lo, PANGO_ELLIPSIZE_END);
    }
    return lo;
}

/* ── draw a pre-built layout at (x, y), pixel-snapped ─────── */
void draw_layout(cairo_t *cr, double x, double y, PangoLayout *lo,
                 double rr, double gg, double bb, double alpha)
{
    cairo_save(cr);
    cairo_translate(cr, floor(x), floor(y));
    cairo_set_source_rgba(cr, rr, gg, bb, alpha);
    pango_cairo_show_layout(cr, lo);
    cairo_restore(cr);
}
