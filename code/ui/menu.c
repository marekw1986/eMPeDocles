/*
 * menu.c — browse menu state machine + flat rendering
 *
 * All gradient fills and glow effects removed; replaced with flat
 * solid-colour fills for CPU efficiency on Pi Zero.
 */
#include "menu.h"
#include "draw_common.h"
#include "mpd_client.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════════
 *  Navigation helpers (unchanged from previous version)
 * ══════════════════════════════════════════════════════════════ */

static MenuLevel *menu_current(MenuState *m) { return &m->stack[m->depth]; }

static void menu_scroll_to_cursor(MenuLevel *lv)
{
    if (lv->cursor < lv->offset)
        lv->offset = lv->cursor;
    if (lv->cursor >= lv->offset + MENU_VISIBLE)
        lv->offset = lv->cursor - MENU_VISIBLE + 1;
}

static void menu_push(MenuState *m, MenuLevel *lv)
{
    if (m->depth + 1 >= MENU_STACK_MAX) return;
    m->stack[m->depth + 1] = *lv;
    m->depth++;
}

static void menu_level_init(MenuLevel *lv, const char *title)
{
    memset(lv, 0, sizeof(*lv));
    snprintf(lv->title, sizeof(lv->title), "%s", title);
}

static MenuItem *menu_add(MenuLevel *lv, const char *label,
                          MenuType type, const char *data)
{
    if (lv->count >= MENU_MAX_ITEMS) return NULL;
    MenuItem *it = &lv->items[lv->count++];
    snprintf(it->label, sizeof(it->label), "%s", label);
    it->type = type;
    if (data) snprintf(it->data, sizeof(it->data), "%s", data);
    else       it->data[0] = '\0';
    return it;
}

static void menu_build_root(MenuState *m)
{
    MenuLevel *root = &m->stack[0];
    menu_level_init(root, "MENU");
    menu_add(root, "Playlists", MTYPE_LOAD_PLISTS, NULL);
    menu_add(root, "Library",   MTYPE_SUBMENU,     NULL);
    menu_add(root, "Queue",     MTYPE_LOAD_QUEUE,  NULL);
}

/* ══════════════════════════════════════════════════════════════
 *  Public navigation API
 * ══════════════════════════════════════════════════════════════ */

void menu_open(MenuState *m)
{
    m->open  = 1;
    m->depth = 0;
    menu_build_root(m);
}

void menu_close(MenuState *m) { m->open = 0; }

static void menu_up(MenuState *m)
{
    MenuLevel *lv = menu_current(m);
    if (lv->cursor > 0) lv->cursor--;
    menu_scroll_to_cursor(lv);
}

static void menu_down(MenuState *m)
{
    MenuLevel *lv = menu_current(m);
    if (lv->cursor < lv->count - 1) lv->cursor++;
    menu_scroll_to_cursor(lv);
}

static void menu_back(MenuState *m)
{
    if (m->depth > 0) m->depth--;
    else               menu_close(m);
}

static void menu_enter(MenuState *m)
{
    MenuLevel *lv = menu_current(m);
    if (lv->count == 0) { menu_close(m); return; }
    MenuItem  *it = &lv->items[lv->cursor];
    MenuLevel   nl;

    switch (it->type) {
    case MTYPE_SUBMENU:
        menu_level_init(&nl, "LIBRARY");
        menu_add(&nl, "Artists", MTYPE_LOAD_ARTISTS, NULL);
        menu_add(&nl, "Albums",  MTYPE_LOAD_ALBUMS,  NULL);
        menu_add(&nl, "Songs",   MTYPE_LOAD_SONGS,   NULL);
        menu_push(m, &nl);
        break;
    case MTYPE_LOAD_PLISTS:             load_playlists(&nl);                    menu_push(m, &nl); break;
    case MTYPE_LOAD_ARTISTS:            load_artists(&nl);                      menu_push(m, &nl); break;
    case MTYPE_LOAD_ALBUMS_FOR_ARTIST:  load_albums_for_artist(&nl, it->data);  menu_push(m, &nl); break;
    case MTYPE_LOAD_ALBUMS:             load_albums(&nl);                       menu_push(m, &nl); break;
    case MTYPE_LOAD_SONGS_FOR_ALBUM:    load_songs_for_album(&nl, it->data);    menu_push(m, &nl); break;
    case MTYPE_LOAD_SONGS:              load_songs(&nl);                        menu_push(m, &nl); break;
    case MTYPE_LOAD_QUEUE:              load_queue(&nl);                        menu_push(m, &nl); break;
    case MTYPE_PLAY_PLAYLIST:  action_play_playlist(it->data); menu_close(m); break;
    case MTYPE_PLAY_SONG:      action_play_song(it->data);     menu_close(m); break;
    case MTYPE_QUEUE_JUMP:     action_queue_jump(it->data);    menu_close(m); break;
    }
}

void menu_input(AppData *app, MenuButton btn)
{
    MenuState *m = &app->menu;
    switch (btn) {
    case BTN_MENU:  if (m->open) menu_close(m); else menu_open(m); break;
    case BTN_UP:    if (m->open) menu_up(m);    break;
    case BTN_DOWN:  if (m->open) menu_down(m);  break;
    case BTN_ENTER: if (m->open) menu_enter(m); break;
    case BTN_BACK:  if (m->open) menu_back(m);  break;
    }
    app_request_redraw(app);
}

/* ══════════════════════════════════════════════════════════════
 *  Rendering — flat, no gradients
 * ══════════════════════════════════════════════════════════════ */

void draw_menu(cairo_t *cr, MenuState *m, AppData *app,
               double SX, double SY, double SW, double SH)
{
    (void)app;
    MenuLevel *lv = menu_current(m);

    /* dim the playback content underneath */
    cairo_set_source_rgba(cr, 0.00, 0.01, 0.06, 0.90);
    cairo_rectangle(cr, SX, SY, SW, SH);
    cairo_fill(cr);

    /* ── header bar ── */
    const double HH = 16.0;

    /* flat header fill */
    fill_rect(cr, SX, SY, SW, HH, 0.00, 0.25, 0.52, 1.0);
    hairline(cr, SX, SY+HH, SX+SW, COL_BRIGHT, 0.50);

    /* breadcrumb */
    {
        char crumb[160] = "";
        for (int d = 0; d <= m->depth; d++) {
            if (d > 0) strncat(crumb, " > ", sizeof(crumb)-strlen(crumb)-1);
            strncat(crumb, m->stack[d].title,
                    sizeof(crumb)-strlen(crumb)-1);
        }
        PangoLayout *lo = make_layout(cr, crumb, "Mono Bold 8",
                                      PANGO_ALIGN_LEFT, SW - 8);
        draw_layout(cr, SX+4, SY+2, lo, COL_BRIGHT, 1.0);
        g_object_unref(lo);
    }

    /* ── list ── */
    const double LY0 = SY + HH + 1;

    if (lv->count == 0) {
        PangoLayout *lo = make_layout(cr, "(loading...)", "Mono 9",
                                      PANGO_ALIGN_LEFT, SW - 8);
        draw_layout(cr, SX+6, LY0+4, lo, COL_DIM, 1.0);
        g_object_unref(lo);
        return;
    }

    int show = (lv->count < MENU_VISIBLE) ? lv->count : MENU_VISIBLE;

    for (int i = 0; i < show; i++) {
        int idx = lv->offset + i;
        if (idx >= lv->count) break;

        double ry = LY0 + i * MENU_ROW_H;
        int sel = (idx == lv->cursor);

        if (sel) {
            /* flat selection highlight + left accent bar */
            fill_rect(cr, SX,   ry, SW, MENU_ROW_H, COL_SEL_BG, 0.85);
            fill_rect(cr, SX,   ry,  3, MENU_ROW_H, COL_BRIGHT,  1.00);
        }

        MenuItem *it = &lv->items[idx];
        int has_arrow = (it->type != MTYPE_PLAY_PLAYLIST &&
                         it->type != MTYPE_PLAY_SONG     &&
                         it->type != MTYPE_QUEUE_JUMP);

        double text_w = SW - 12 - (has_arrow ? 10 : 0);

        {
            double lr = sel ? 0.40 : 0.00;
            double lg = sel ? 0.85 : 0.55;
            double lb = sel ? 1.00 : 0.88;
            PangoLayout *lo = make_layout(cr, it->label,
                                          sel ? "Mono Bold 9" : "Mono 9",
                                          PANGO_ALIGN_LEFT, text_w);
            draw_layout(cr, SX+6, ry+2, lo, lr, lg, lb, 1.0);
            g_object_unref(lo);
        }

        if (has_arrow) {
            double ar = sel ? 0.40 : 0.06;
            double ag = sel ? 0.85 : 0.20;
            double ab = sel ? 1.00 : 0.36;
            PangoLayout *lo = make_layout(cr, ">", "Mono 9",
                                          PANGO_ALIGN_LEFT, 10);
            draw_layout(cr, SX+SW-12, ry+2, lo, ar, ag, ab, 1.0);
            g_object_unref(lo);
        }
    }

    /* ── scroll position indicator: thin bar on right edge ── */
    if (lv->count > MENU_VISIBLE) {
        double bar_h = (double)MENU_VISIBLE / lv->count * (show * MENU_ROW_H);
        double bar_y = LY0 + (double)lv->offset / lv->count
                              * (show * MENU_ROW_H);
        fill_rect(cr, SX+SW-3, LY0, 2, show*MENU_ROW_H, COL_DIM, 0.6);
        fill_rect(cr, SX+SW-3, bar_y, 2, bar_h, COL_BRIGHT, 0.9);
    }

    /* ── hint line ── */
    hairline(cr, SX, SY+SH-13, SX+SW, COL_MID, 0.20);
    {
        PangoLayout *lo = make_layout(cr,
            "up/dn scroll  rt/Enter select  lt/Esc back  M close",
            "Mono 6", PANGO_ALIGN_LEFT, SW - 4);
        draw_layout(cr, SX+3, SY+SH-12, lo, COL_DIM, 0.75);
        g_object_unref(lo);
    }
}
