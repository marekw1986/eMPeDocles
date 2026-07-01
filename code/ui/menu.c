/*
 * menu.c — browse menu state machine + rendering
 *
 * Menu tree:
 *   Root
 *   ├─ Playlists      → saved playlists → load & play
 *   ├─ Library
 *   │   ├─ Artists    → artist → albums → tracks → play
 *   │   ├─ Albums     → album  → tracks → play
 *   │   └─ Songs      → flat track list → play
 *   └─ Queue          → current queue   → jump to track
 *
 * See menu.h for the GPIO/hardware-button integration point
 * (menu_input).
 */
#include "menu.h"
#include "draw_common.h"
#include "mpd_client.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════════
 *  Navigation state helpers
 * ══════════════════════════════════════════════════════════════ */

static MenuLevel *menu_current(MenuState *m) { return &m->stack[m->depth]; }

static void menu_scroll_to_cursor(MenuLevel *lv)
{
    if (lv->cursor < lv->offset)
        lv->offset = lv->cursor;
    if (lv->cursor >= lv->offset + MENU_VISIBLE)
        lv->offset = lv->cursor - MENU_VISIBLE + 1;
}

/* push a new level onto the stack (copies *lv by value) */
static void menu_push(MenuState *m, MenuLevel *lv)
{
    if (m->depth + 1 >= MENU_STACK_MAX) return;
    m->stack[m->depth + 1] = *lv;
    m->depth++;
}

/* local helper mirroring mpd_client.c's menu_add — menu.c builds
 * the static "Library" submenu itself, so it needs its own adder */
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

/* build the static root menu */
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

/* enter / select the highlighted item — drills down or fires an action */
static void menu_enter(MenuState *m)
{
    MenuLevel *lv = menu_current(m);
    if (lv->count == 0) { menu_close(m); return; }
    MenuItem  *it = &lv->items[lv->cursor];
    MenuLevel   nl;

    switch (it->type) {

    case MTYPE_SUBMENU:
        /* currently only "Library" reaches here */
        menu_level_init(&nl, "LIBRARY");
        menu_add(&nl, "Artists", MTYPE_LOAD_ARTISTS, NULL);
        menu_add(&nl, "Albums",  MTYPE_LOAD_ALBUMS,  NULL);
        menu_add(&nl, "Songs",   MTYPE_LOAD_SONGS,   NULL);
        menu_push(m, &nl);
        break;

    case MTYPE_LOAD_PLISTS:
        load_playlists(&nl);
        menu_push(m, &nl);
        break;

    case MTYPE_LOAD_ARTISTS:
        load_artists(&nl);
        menu_push(m, &nl);
        break;

    case MTYPE_LOAD_ALBUMS_FOR_ARTIST:
        load_albums_for_artist(&nl, it->data);
        menu_push(m, &nl);
        break;

    case MTYPE_LOAD_ALBUMS:
        load_albums(&nl);
        menu_push(m, &nl);
        break;

    case MTYPE_LOAD_SONGS_FOR_ALBUM:
        load_songs_for_album(&nl, it->data);
        menu_push(m, &nl);
        break;

    case MTYPE_LOAD_SONGS:
        load_songs(&nl);
        menu_push(m, &nl);
        break;

    case MTYPE_LOAD_QUEUE:
        load_queue(&nl);
        menu_push(m, &nl);
        break;

    case MTYPE_PLAY_PLAYLIST:
        action_play_playlist(it->data);
        menu_close(m);
        break;

    case MTYPE_PLAY_SONG:
        action_play_song(it->data);
        menu_close(m);
        break;

    case MTYPE_QUEUE_JUMP:
        action_queue_jump(it->data);
        menu_close(m);
        break;
    }
}

/* single dispatch entry point — see menu.h for GPIO usage */
void menu_input(AppData *app, MenuButton btn)
{
    MenuState *m = &app->menu;
    switch (btn) {
    case BTN_MENU:
        if (m->open) menu_close(m); else menu_open(m);
        break;
    case BTN_UP:    if (m->open) menu_up(m);    break;
    case BTN_DOWN:  if (m->open) menu_down(m);  break;
    case BTN_ENTER: if (m->open) menu_enter(m); break;
    case BTN_BACK:  if (m->open) menu_back(m);  break;
    }
    app_request_redraw(app);
}

/* ══════════════════════════════════════════════════════════════
 *  Rendering
 * ══════════════════════════════════════════════════════════════ */

void draw_menu(cairo_t *cr, MenuState *m, AppData *app,
              double SX, double SY, double SW, double SH)
{
    (void)app;
    MenuLevel *lv = menu_current(m);

    /* dim the playback screen underneath */
    cairo_set_source_rgba(cr, 0.00, 0.01, 0.06, 0.88);
    cairo_rectangle(cr, SX, SY, SW, SH);
    cairo_fill(cr);

    /* ── header bar with breadcrumb ── */
    const double HH = 18.0;
    {
        char crumb[160] = "";
        for (int d = 0; d <= m->depth; d++) {
            if (d > 0) strncat(crumb, " \xC2\xBB ", sizeof(crumb)-strlen(crumb)-1);
            strncat(crumb, m->stack[d].title, sizeof(crumb)-strlen(crumb)-1);
        }

        cairo_pattern_t *p =
            cairo_pattern_create_linear(SX, SY, SX, SY+HH);
        cairo_pattern_add_color_stop_rgb(p, 0.0, 0.00, 0.30, 0.60);
        cairo_pattern_add_color_stop_rgb(p, 1.0, 0.00, 0.18, 0.40);
        cairo_set_source(cr, p);
        cairo_rectangle(cr, SX, SY, SW, HH);
        cairo_fill(cr);
        cairo_pattern_destroy(p);

        hairline(cr, SX, SY+HH, SX+SW, COL_BRIGHT, 0.40);

        draw_text(cr, SX+5, SY+2, crumb, "Mono Bold 8",
                  COL_BRIGHT, 1.0, PANGO_ALIGN_LEFT, SW-10);
    }

    /* ── list area ── */
    const double LY0 = SY + HH + 2;

    if (lv->count == 0) {
        draw_text(cr, SX+8, LY0+6, "(loading\xE2\x80\xA6)", "Mono 9",
                  COL_DIM, 1.0, PANGO_ALIGN_LEFT, SW-10);
        return;
    }

    int visible = MENU_VISIBLE;
    int show    = (lv->count < visible) ? lv->count : visible;

    for (int i = 0; i < show; i++) {
        int idx = lv->offset + i;
        if (idx >= lv->count) break;

        double ry = LY0 + i * MENU_ROW_H;
        int selected = (idx == lv->cursor);

        if (selected) {
            cairo_pattern_t *p =
                cairo_pattern_create_linear(SX, ry, SX, ry+MENU_ROW_H);
            cairo_pattern_add_color_stop_rgba(p, 0.0, COL_SEL_BG, 0.90);
            cairo_pattern_add_color_stop_rgba(p, 1.0, 0.00, 0.20, 0.42, 0.90);
            cairo_set_source(cr, p);
            cairo_rectangle(cr, SX, ry, SW, MENU_ROW_H);
            cairo_fill(cr);
            cairo_pattern_destroy(p);

            cairo_set_source_rgba(cr, COL_BRIGHT, 1.0);
            cairo_rectangle(cr, SX, ry, 3, MENU_ROW_H);
            cairo_fill(cr);
        }

        MenuItem *it = &lv->items[idx];

        int has_arrow = (it->type != MTYPE_PLAY_PLAYLIST &&
                         it->type != MTYPE_PLAY_SONG     &&
                         it->type != MTYPE_QUEUE_JUMP);

        double text_w = SW - 14 - (has_arrow ? 12 : 0);

        {
            double lr = selected ? 0.40 : 0.00;
            double lg = selected ? 0.85 : 0.55;
            double lb = selected ? 1.00 : 0.88;
            double la = selected ? 1.00 : 0.85;
            draw_text(cr, SX+7, ry+2, it->label,
                      selected ? "Mono Bold 9" : "Mono 9",
                      lr, lg, lb, la, PANGO_ALIGN_LEFT, text_w);

            if (has_arrow) {
                double ar = selected ? 0.40 : 0.06;
                double ag = selected ? 0.85 : 0.20;
                double ab = selected ? 1.00 : 0.36;
                double aa = selected ? 1.00 : 0.70;
                draw_text(cr, SX+SW-13, ry+2, "\xE2\x80\xBA", "Mono 9",
                          ar, ag, ab, aa, PANGO_ALIGN_LEFT, 12);
            }
        }
    }

    /* ── scroll indicator (right edge dot column) ── */
    if (lv->count > visible) {
        double tx  = SX + SW - 5;
        double ty0 = LY0 + 2;
        double th  = show * MENU_ROW_H - 4;
        int    dots = show;
        for (int i = 0; i < dots; i++) {
            double cy   = ty0 + (i + 0.5) * (th / dots);
            int    band = (int)((double)lv->cursor / lv->count * dots);
            if (i == band)
                glow_dot(cr, tx, cy, 2.5, COL_BRIGHT, 1.0);
            else
                glow_dot(cr, tx, cy, 1.5, COL_DIM, 1.0);
        }
    }

    /* ── hint line ── */
    hairline(cr, SX, SY+SH-14, SX+SW, COL_MID, 0.18);
    draw_text(cr, SX+4, SY+SH-13,
              "\xe2\x86\x91\xe2\x86\x93 scroll  \xe2\x86\x92/\xe2\x8f\x8e select  \xe2\x86\x90/Esc back  M close",
              "Mono 6", COL_DIM, 0.80, PANGO_ALIGN_LEFT, SW-6);
}
