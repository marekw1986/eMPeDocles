/*
 * main_screen.c — retro VFD playback screen
 *
 * Draws the chassis/bezel, the VFD-style screen with title
 * marquee, artist/album, progress bar, time, tech info, and
 * volume bar. If the menu is open, delegates to draw_menu()
 * (menu.c) to render the browse overlay on top.
 */
#include "main_screen.h"
#include "draw_common.h"
#include "menu.h"

#include <string.h>

void on_draw(cairo_t *cr, AppData *app)
{
    MpdState *s = &app->mpd;

    const double W = WIN_W, H = WIN_H;

    /* screen now fills the entire window — no chassis/bezel margin */
    const double SX = 0, SY = 0, SW = W, SH = H;

    /* ── screen fill ── */
    {
        cairo_pattern_t *p = cairo_pattern_create_linear(SX, SY, SX, SY+SH);
        cairo_pattern_add_color_stop_rgb(p, 0.0, 0.010, 0.025, 0.095);
        cairo_pattern_add_color_stop_rgb(p, 1.0, 0.018, 0.040, 0.140);
        cairo_set_source(cr, p);
        cairo_rectangle(cr, SX, SY, SW, SH);
        cairo_fill(cr);
        cairo_pattern_destroy(p);

        cairo_set_source_rgba(cr, 0, 0, 0, 0.10);
        for (double ly = SY+1; ly < SY+SH; ly += 2)
            cairo_rectangle(cr, SX, ly, SW, 1);
        cairo_fill(cr);

        cairo_pattern_t *g = cairo_pattern_create_radial(
            SX+SW/2, SY+SH*0.45, 8, SX+SW/2, SY+SH*0.45, SW*0.65);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 0.00, 0.12, 0.38, 0.22);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 0.00, 0.00, 0.00, 0.00);
        cairo_set_source(cr, g);
        cairo_rectangle(cr, SX, SY, SW, SH);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* clip all content to screen */
    cairo_save(cr);
    cairo_rectangle(cr, SX, SY, SW, SH);
    cairo_clip(cr);

    /* ── row 0: brand label + connection LED ── */
    draw_text(cr, SX+6, SY+4, "RETRO\xC2\xB7MPD", "Mono 7",
              COL_MID, 0.70, PANGO_ALIGN_LEFT, 0);

    if (s->connected)
        glow_dot(cr, SX+SW-9, SY+10, 4.5, COL_AMBER, 1.0);
    else
        glow_dot(cr, SX+SW-9, SY+10, 4.5, COL_RED,   1.0);

    hairline(cr, SX+5, SY+18, SX+SW-5, COL_MID, 0.25);

    /* ── row 1: play-state glyph + scrolling title ── */
    {
        const char *glyph;
        double gr, gg_c, gb;
        if      (strcmp(s->state, "play")  == 0) { glyph="\xe2\x96\xb6"; gr=0.15; gg_c=0.92; gb=0.35; }
        else if (strcmp(s->state, "pause") == 0) { glyph="\xe2\x8f\xb8"; gr=0.95; gg_c=0.75; gb=0.05; }
        else                                      { glyph="\xe2\x96\xa0"; gr=0.65; gg_c=0.15; gb=0.10; }
        draw_text(cr, SX+4, SY+21, glyph, "Sans Bold 16",
                  gr, gg_c, gb, 1.0, PANGO_ALIGN_LEFT, 0);

        const char *tf = "Mono Bold 16";
        double tx = SX+30, ty = SY+21, tw = SW-36;
        if (app->title_px_w == 0)
            app->title_px_w = measure_text_w(cr, s->title, tf) + 40;
        cairo_save(cr);
        cairo_rectangle(cr, tx, ty, tw, 28);
        cairo_clip(cr);
        double ox = -app->scroll_x;
        draw_text(cr, tx+ox, ty, s->title, tf, COL_BRIGHT, 1.0, PANGO_ALIGN_LEFT, 0);
        if (ox + app->title_px_w < tw)
            draw_text(cr, tx+ox+app->title_px_w, ty, s->title, tf,
                      COL_BRIGHT, 1.0, PANGO_ALIGN_LEFT, 0);
        cairo_restore(cr);
    }

    hairline(cr, SX+5, SY+54, SX+SW-5, COL_MID, 0.25);

    /* ── row 2/3: artist / album ── */
    draw_text(cr, SX+6, SY+58, s->artist, "Mono 13",
              COL_MID, 0.95, PANGO_ALIGN_LEFT, SW-10);
    draw_text(cr, SX+6, SY+79, s->album[0] ? s->album : "", "Mono 11",
              COL_DIM, 1.0, PANGO_ALIGN_LEFT, SW-10);

    hairline(cr, SX+5, SY+100, SX+SW-5, COL_MID, 0.22);

    /* ── row 4: progress bar ── */
    {
        double frac = (s->duration > 0)
                      ? (double)s->elapsed / (double)s->duration : 0.0;
        draw_progress(cr, SX+6, SY+105, SW-12, 14, frac);
    }

    /* ── row 5: time ── */
    {
        char t[32];
        int em=s->elapsed/60, es=s->elapsed%60;
        int dm=s->duration/60, ds=s->duration%60;
        snprintf(t, sizeof(t), "%02d:%02d / %02d:%02d", em, es, dm, ds);
        draw_text(cr, SX+SW/2.0, SY+123, t, "Mono Bold 15",
                  COL_BRIGHT, 0.95, PANGO_ALIGN_CENTER, SW);
    }

    hairline(cr, SX+5, SY+148, SX+SW-5, COL_MID, 0.20);

    /* ── row 6: playlist position (left) + bitrate/rate/vol (centre) ── */
    {
        if (s->song >= 0 && s->playlistlen > 0) {
            char pos[32];
            snprintf(pos, sizeof(pos), "%d/%d", s->song+1, s->playlistlen);
            draw_text(cr, SX+6, SY+152, pos, "Mono Bold 9",
                      COL_MID, 0.90, PANGO_ALIGN_LEFT, 0);
        }
        char tech[128];
        int sr  = s->samplerate / 1000;
        int vol = (s->volume >= 0) ? s->volume : 0;
        if (s->bitrate > 0 && sr > 0)
            snprintf(tech, sizeof(tech),
                     "%d kbps  \xC2\xB7  %d kHz  \xC2\xB7  VOL %d%%",
                     s->bitrate, sr, vol);
        else if (sr > 0)
            snprintf(tech, sizeof(tech), "%d kHz  \xC2\xB7  VOL %d%%", sr, vol);
        else
            snprintf(tech, sizeof(tech), "VOL %d%%", vol);
        draw_text(cr, SX+SW/2.0, SY+152, tech, "Mono 9",
                  COL_DIM, 1.0, PANGO_ALIGN_CENTER, SW);
    }

    hairline(cr, SX+5, SY+170, SX+SW-5, COL_MID, 0.18);

    /* ── row 7: REP / RND / SGL indicator dots ── */
    {
        const char *labels[] = { "REP", "RND", "SGL" };
        double spacing = SW / 4.0;
        for (int i = 0; i < 3; i++) {
            double cx = SX + spacing * (i+1);
            glow_dot(cr, cx, SY+179, 4.0, COL_DIM, 1.0);
            draw_text(cr, cx-9, SY+186, labels[i], "Mono 7",
                      COL_DIM, 0.80, PANGO_ALIGN_LEFT, 0);
        }
    }

    /* ── row 8: remaining time ── */
    if (s->duration > 0) {
        int rem=s->duration-s->elapsed, rm=rem/60, rs=rem%60;
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), "-%02d:%02d remaining", rm, rs);
        draw_text(cr, SX+SW/2.0, SY+200, rbuf, "Mono 8",
                  COL_DIM, 0.60, PANGO_ALIGN_CENTER, SW);
    }

    /* ── row 9: volume bar ── */
    {
        double vol_frac = (s->volume >= 0) ? s->volume / 100.0 : 0.0;
        double bx=SX+6, by=SY+SH-12, bw=SW-12, bh=6;
        cairo_set_source_rgba(cr, COL_DIM, 0.5);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);
        if (vol_frac > 0) {
            cairo_pattern_t *p = cairo_pattern_create_linear(bx, 0, bx+bw, 0);
            cairo_pattern_add_color_stop_rgba(p, 0.0, COL_MID,    0.7);
            cairo_pattern_add_color_stop_rgba(p, 0.8, COL_BRIGHT, 0.9);
            cairo_pattern_add_color_stop_rgba(p, 1.0, COL_BRIGHT, 0.9);
            cairo_set_source(cr, p);
            cairo_rectangle(cr, bx, by, bw*vol_frac, bh);
            cairo_fill(cr);
            cairo_pattern_destroy(p);
        }
        draw_text(cr, SX+6, SY+SH-14, "VOL", "Mono 6",
                  COL_DIM, 0.55, PANGO_ALIGN_LEFT, 0);
    }

    /* ── menu overlay (drawn on top of everything above) ── */
    if (app->menu.open)
        draw_menu(cr, &app->menu, app, SX, SY, SW, SH);

    /* ── screen glare ── */
    {
        cairo_pattern_t *g =
            cairo_pattern_create_linear(SX, SY, SX+SW*0.55, SY+SH*0.35);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 1,1,1, 0.035);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 1,1,1, 0.000);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
    }

    cairo_restore(cr);
}
