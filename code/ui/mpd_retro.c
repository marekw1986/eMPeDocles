/*
 * mpd_retro.c  —  320×240 GTK3 retro MPD display with browse menu
 *
 * Keys (keyboard now; wire GPIO later via menu_input()):
 *   M          — toggle menu open / close
 *   Up / Down  — scroll list
 *   Right / Enter — enter submenu or select item
 *   Left / Esc — go back one level / close menu
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
#define COL_CHASSIS   0.04, 0.04, 0.08
#define COL_BRIGHT    0.40, 0.85, 1.00    /* bright VFD cyan      */
#define COL_MID       0.00, 0.55, 0.88    /* medium VFD           */
#define COL_DIM       0.06, 0.20, 0.36    /* inactive segments    */
#define COL_AMBER     1.00, 0.62, 0.00    /* connection OK LED    */
#define COL_RED       1.00, 0.16, 0.08    /* connection fail LED  */
#define COL_SEL_BG    0.00, 0.28, 0.55    /* selected row fill    */

/* ── menu limits ────────────────────────────────────────────── */
#define MENU_MAX_ITEMS  512
#define MENU_LABEL_LEN  192
#define MENU_STACK_MAX    8
#define MENU_VISIBLE     10   /* rows visible at once in the list */
#define MENU_ROW_H       18   /* px per row                       */

/* ══════════════════════════════════════════════════════════════
 *  MPD state
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
 *  Menu system
 * ══════════════════════════════════════════════════════════════ */

/* what kind of node this item navigates into or acts upon */
typedef enum {
    MTYPE_SUBMENU,     /* opens a static child list          */
    MTYPE_LOAD_PLISTS, /* fetches saved playlists from MPD   */
    MTYPE_LOAD_ARTISTS,
    MTYPE_LOAD_ALBUMS_FOR_ARTIST,
    MTYPE_LOAD_ALBUMS,
    MTYPE_LOAD_SONGS_FOR_ALBUM,
    MTYPE_LOAD_SONGS,
    MTYPE_LOAD_QUEUE,
    MTYPE_PLAY_PLAYLIST, /* action: load & play playlist     */
    MTYPE_PLAY_SONG,     /* action: addid / play song uri    */
    MTYPE_QUEUE_JUMP,    /* action: playid N                 */
} MenuType;

typedef struct MenuItem MenuItem;
struct MenuItem {
    char      label[MENU_LABEL_LEN];
    MenuType  type;
    char      data[MENU_LABEL_LEN];  /* uri / artist / album / playlist name */
};

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
    int        depth;   /* index of current level   */
} MenuState;

/* ── application data ───────────────────────────────────────── */
typedef struct {
    GtkWidget *canvas;
    MpdState   mpd;

    double  scroll_x;
    double  scroll_speed;
    int     title_px_w;

    MenuState  menu;

    guint   anim_id;
    guint   poll_id;
} AppData;

/* ══════════════════════════════════════════════════════════════
 *  MPD helpers
 * ══════════════════════════════════════════════════════════════ */

static int mpd_connect(const char *host, int port)
{
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
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

/* read + discard everything until OK / ACK */
static void mpd_drain(int fd)
{
    char line[512];
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
    }
}

static const char *mpd_host(void) {
    const char *h = getenv("MPD_HOST");
    return h ? h : "localhost";
}
static int mpd_port(void) {
    const char *p = getenv("MPD_PORT");
    return p ? atoi(p) : 6600;
}

/* open connection and consume the banner; returns fd or -1 */
static int mpd_open(void)
{
    int fd = mpd_connect(mpd_host(), mpd_port());
    if (fd < 0) return -1;
    char line[128];
    mpd_readline(fd, line, sizeof(line));
    if (strncmp(line, "OK MPD", 6) != 0) { close(fd); return -1; }
    return fd;
}

/* send a simple command with no arguments and drain the response */
__attribute__((unused)) static void mpd_cmd(const char *cmd)
{
    int fd = mpd_open();
    if (fd < 0) return;
    send(fd, cmd,    strlen(cmd), 0);
    send(fd, "\n",   1, 0);
    mpd_drain(fd);
    send(fd, "close\n", 6, 0);
    close(fd);
}

/* ── playback status / current song poll ───────────────────── */
static void mpd_poll(MpdState *s)
{
    int fd = mpd_open();
    if (fd < 0) {
        s->connected = 0;
        snprintf(s->title,  sizeof(s->title),  "NO CONNECTION");
        snprintf(s->artist, sizeof(s->artist), "mpd @ %s:%d", mpd_host(), mpd_port());
        s->album[0] = '\0';
        snprintf(s->state,  sizeof(s->state),  "stop");
        s->elapsed = s->duration = 0;
        return;
    }
    char line[512];

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
 *  Menu data loaders  (each fills a MenuLevel)
 * ══════════════════════════════════════════════════════════════ */

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

/* load saved playlists */
static void load_playlists(MenuLevel *lv)
{
    menu_level_init(lv, "PLAYLISTS");
    int fd = mpd_open();
    if (fd < 0) { menu_add(lv, "(no connection)", MTYPE_SUBMENU, NULL); return; }
    char line[512];
    send(fd, "listplaylists\n", 14, 0);
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
        if (strncmp(line, "playlist: ", 10) == 0)
            menu_add(lv, line+10, MTYPE_PLAY_PLAYLIST, line+10);
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(no saved playlists)", MTYPE_SUBMENU, NULL);
}

/* load all artists */
static void load_artists(MenuLevel *lv)
{
    menu_level_init(lv, "ARTISTS");
    int fd = mpd_open();
    if (fd < 0) { menu_add(lv, "(no connection)", MTYPE_SUBMENU, NULL); return; }
    char line[512];
    send(fd, "list artist\n", 12, 0);
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
        if (strncmp(line, "Artist: ", 8) == 0)
            menu_add(lv, line+8, MTYPE_LOAD_ALBUMS_FOR_ARTIST, line+8);
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(empty library)", MTYPE_SUBMENU, NULL);
}

/* load albums for a given artist */
static void load_albums_for_artist(MenuLevel *lv, const char *artist)
{
    menu_level_init(lv, artist);
    int fd = mpd_open();
    if (fd < 0) { menu_add(lv, "(no connection)", MTYPE_SUBMENU, NULL); return; }
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "list album artist \"%s\"\n", artist);
    send(fd, cmd, strlen(cmd), 0);
    char line[512];
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
        if (strncmp(line, "Album: ", 7) == 0)
            menu_add(lv, line+7, MTYPE_LOAD_SONGS_FOR_ALBUM, line+7);
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(no albums)", MTYPE_SUBMENU, NULL);
}

/* load all albums */
static void load_albums(MenuLevel *lv)
{
    menu_level_init(lv, "ALBUMS");
    int fd = mpd_open();
    if (fd < 0) { menu_add(lv, "(no connection)", MTYPE_SUBMENU, NULL); return; }
    char line[512];
    send(fd, "list album\n", 11, 0);
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
        if (strncmp(line, "Album: ", 7) == 0)
            menu_add(lv, line+7, MTYPE_LOAD_SONGS_FOR_ALBUM, line+7);
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(empty library)", MTYPE_SUBMENU, NULL);
}

/* load songs for an album (find -album) */
static void load_songs_for_album(MenuLevel *lv, const char *album)
{
    menu_level_init(lv, album);
    int fd = mpd_open();
    if (fd < 0) { menu_add(lv, "(no connection)", MTYPE_SUBMENU, NULL); return; }
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "find album \"%s\"\n", album);
    send(fd, cmd, strlen(cmd), 0);
    char line[512];
    char uri[MENU_LABEL_LEN] = "";
    char title[MENU_LABEL_LEN] = "";
    /* MPD returns one block per song; collect uri+title then emit on next file/OK */
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0 || strncmp(line, "ACK", 3) == 0) {
            if (uri[0]) menu_add(lv, title[0] ? title : uri,
                                 MTYPE_PLAY_SONG, uri);
            break;
        }
        if (strncmp(line, "file: ", 6) == 0) {
            if (uri[0]) menu_add(lv, title[0] ? title : uri,
                                 MTYPE_PLAY_SONG, uri);
            snprintf(uri,   sizeof(uri),   "%s", line+6);
            title[0] = '\0';
        } else if (strncmp(line, "Title: ", 7) == 0) {
            snprintf(title, sizeof(title), "%s", line+7);
        }
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(no tracks)", MTYPE_SUBMENU, NULL);
}

/* load all songs flat */
static void load_songs(MenuLevel *lv)
{
    menu_level_init(lv, "ALL SONGS");
    int fd = mpd_open();
    if (fd < 0) { menu_add(lv, "(no connection)", MTYPE_SUBMENU, NULL); return; }
    send(fd, "listallinfo\n", 12, 0);
    char line[512];
    char uri[MENU_LABEL_LEN] = "";
    char title[MENU_LABEL_LEN] = "";
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0 || strncmp(line, "ACK", 3) == 0) {
            if (uri[0]) menu_add(lv, title[0] ? title : uri,
                                 MTYPE_PLAY_SONG, uri);
            break;
        }
        if (strncmp(line, "file: ", 6) == 0) {
            if (uri[0]) menu_add(lv, title[0] ? title : uri,
                                 MTYPE_PLAY_SONG, uri);
            snprintf(uri,   sizeof(uri),   "%s", line+6);
            title[0] = '\0';
        } else if (strncmp(line, "Title: ", 7) == 0) {
            snprintf(title, sizeof(title), "%s", line+7);
        }
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(empty library)", MTYPE_SUBMENU, NULL);
}

/* load current playback queue */
static void load_queue(MenuLevel *lv)
{
    menu_level_init(lv, "QUEUE");
    int fd = mpd_open();
    if (fd < 0) { menu_add(lv, "(no connection)", MTYPE_SUBMENU, NULL); return; }
    send(fd, "playlistinfo\n", 13, 0);
    char line[512];
    char id_str[16] = "";
    char title[MENU_LABEL_LEN] = "";
    int  first = 1;
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0 || strncmp(line, "ACK", 3) == 0) {
            if (!first && id_str[0])
                menu_add(lv, title[0] ? title : "(no title)",
                         MTYPE_QUEUE_JUMP, id_str);
            break;
        }
        if (strncmp(line, "Id: ", 4) == 0) {
            if (!first && id_str[0])
                menu_add(lv, title[0] ? title : "(no title)",
                         MTYPE_QUEUE_JUMP, id_str);
            snprintf(id_str, sizeof(id_str), "%s", line+4);
            title[0] = '\0';
            first = 0;
        } else if (strncmp(line, "Title: ", 7) == 0) {
            snprintf(title, sizeof(title), "%s", line+7);
        }
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(queue is empty)", MTYPE_SUBMENU, NULL);
}

/* ── build the static root menu ─────────────────────────────── */
static void menu_build_root(MenuState *m)
{
    MenuLevel *root = &m->stack[0];
    menu_level_init(root, "MENU");
    menu_add(root, "Playlists",  MTYPE_LOAD_PLISTS,  NULL);
    menu_add(root, "Library",    MTYPE_SUBMENU,       NULL);  /* handled below */
    menu_add(root, "Queue",      MTYPE_LOAD_QUEUE,    NULL);
    /* Library sub-items: store as a static sub-level at depth 1 */
    /* We'll push a "Library" level dynamically on enter */
}

/* ── actions ─────────────────────────────────────────────────── */

/* load playlist by name into queue and play */
static void action_play_playlist(const char *name)
{
    int fd = mpd_open();
    if (fd < 0) return;
    char cmd[MENU_LABEL_LEN + 32];
    send(fd, "clear\n", 6, 0);         mpd_drain(fd);
    snprintf(cmd, sizeof(cmd), "load \"%s\"\n", name);
    send(fd, cmd, strlen(cmd), 0);     mpd_drain(fd);
    send(fd, "play\n", 5, 0);          mpd_drain(fd);
    send(fd, "close\n", 6, 0);
    close(fd);
}

/* clear queue, add song uri, play */
static void action_play_song(const char *uri)
{
    int fd = mpd_open();
    if (fd < 0) return;
    char cmd[MENU_LABEL_LEN + 32];
    send(fd, "clear\n", 6, 0);         mpd_drain(fd);
    snprintf(cmd, sizeof(cmd), "add \"%s\"\n", uri);
    send(fd, cmd, strlen(cmd), 0);     mpd_drain(fd);
    send(fd, "play\n", 5, 0);          mpd_drain(fd);
    send(fd, "close\n", 6, 0);
    close(fd);
}

/* jump to song by id in current queue */
static void action_queue_jump(const char *id_str)
{
    int fd = mpd_open();
    if (fd < 0) return;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "playid %s\n", id_str);
    send(fd, cmd, strlen(cmd), 0);     mpd_drain(fd);
    send(fd, "close\n", 6, 0);
    close(fd);
}

/* ══════════════════════════════════════════════════════════════
 *  Menu navigation  (called by keyboard / GPIO)
 * ══════════════════════════════════════════════════════════════ */

static MenuLevel *menu_current(MenuState *m) { return &m->stack[m->depth]; }

static void menu_scroll_to_cursor(MenuLevel *lv)
{
    if (lv->cursor < lv->offset)
        lv->offset = lv->cursor;
    if (lv->cursor >= lv->offset + MENU_VISIBLE)
        lv->offset = lv->cursor - MENU_VISIBLE + 1;
}

/* push a new level onto the stack */
static void menu_push(MenuState *m, MenuLevel *lv)
{
    if (m->depth + 1 >= MENU_STACK_MAX) return;
    m->stack[m->depth + 1] = *lv;
    m->depth++;
}

static void menu_open(MenuState *m)
{
    m->open  = 1;
    m->depth = 0;
    menu_build_root(m);
}

static void menu_close(MenuState *m) { m->open = 0; }

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

/* enter / select the highlighted item */
static void menu_enter(MenuState *m)
{
    MenuLevel *lv  = menu_current(m);
    if (lv->count == 0) { menu_close(m); return; }
    MenuItem  *it  = &lv->items[lv->cursor];
    MenuLevel   nl;

    switch (it->type) {

    case MTYPE_SUBMENU:
        /* "Library" node → push a static library sub-menu */
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

/* public input dispatcher — call this from GPIO callbacks too */
typedef enum { BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK, BTN_MENU } MenuButton;

static void menu_input(AppData *app, MenuButton btn)
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
    gtk_widget_queue_draw(app->canvas);
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

static void draw_text(cairo_t *cr,
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
 *  Draw: menu overlay
 * ══════════════════════════════════════════════════════════════ */

static void draw_menu(cairo_t *cr, MenuState *m, AppData *app,
                      double SX, double SY, double SW, double SH)
{
    (void)app;
    MenuLevel *lv = menu_current(m);

    /* ── dim the playback screen underneath ── */
    cairo_set_source_rgba(cr, 0.00, 0.01, 0.06, 0.88);
    cairo_rectangle(cr, SX, SY, SW, SH);
    cairo_fill(cr);

    /* ── header bar ── */
    const double HH = 18.0;
    {
        /* build breadcrumb: root > … > current */
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
        draw_text(cr, SX+8, LY0+6, "(loading…)", "Mono 9",
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

        /* row background */
        if (selected) {
            /* highlight: bright fill */
            cairo_pattern_t *p =
                cairo_pattern_create_linear(SX, ry, SX, ry+MENU_ROW_H);
            cairo_pattern_add_color_stop_rgba(p, 0.0, COL_SEL_BG, 0.90);
            cairo_pattern_add_color_stop_rgba(p, 1.0, 0.00, 0.20, 0.42, 0.90);
            cairo_set_source(cr, p);
            cairo_rectangle(cr, SX, ry, SW, MENU_ROW_H);
            cairo_fill(cr);
            cairo_pattern_destroy(p);

            /* left accent bar */
            cairo_set_source_rgba(cr, COL_BRIGHT, 1.0);
            cairo_rectangle(cr, SX, ry, 3, MENU_ROW_H);
            cairo_fill(cr);
        }

        /* item label — truncate display to fit */
        MenuItem *it = &lv->items[idx];

        /* right arrow for items that drill down */
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

    /* ── scroll indicator (dot column, right edge) ── */
    if (lv->count > visible) {
        double tx = SX + SW - 5;
        double ty0 = LY0 + 2;
        double th  = show * MENU_ROW_H - 4;
        int    dots = show;
        for (int i = 0; i < dots; i++) {
            double cy  = ty0 + (i + 0.5) * (th / dots);
            /* which dot corresponds to cursor band */
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

/* ══════════════════════════════════════════════════════════════
 *  Draw: main playback screen
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

    /* ── bezel ── */
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

    const double SX = 10, SY = 8, SW = W-20, SH = H-16;

    /* ── screen fill ── */
    {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
        rrect(cr, SX-2, SY-2, SW+4, SH+4, 5);
        cairo_fill(cr);

        cairo_pattern_t *p = cairo_pattern_create_linear(SX, SY, SX, SY+SH);
        cairo_pattern_add_color_stop_rgb(p, 0.0, 0.010, 0.025, 0.095);
        cairo_pattern_add_color_stop_rgb(p, 1.0, 0.018, 0.040, 0.140);
        cairo_set_source(cr, p);
        rrect(cr, SX, SY, SW, SH, 4);
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
        rrect(cr, SX, SY, SW, SH, 4);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* clip all content to screen */
    cairo_save(cr);
    rrect(cr, SX, SY, SW, SH, 4);
    cairo_clip(cr);

    /* ── playback content ── */
    draw_text(cr, SX+6, SY+4, "RETRO\xC2\xB7MPD", "Mono 7",
              COL_MID, 0.70, PANGO_ALIGN_LEFT, 0);

    if (s->connected)
        glow_dot(cr, SX+SW-9, SY+10, 4.5, COL_AMBER, 1.0);
    else
        glow_dot(cr, SX+SW-9, SY+10, 4.5, COL_RED,   1.0);

    hairline(cr, SX+5, SY+18, SX+SW-5, COL_MID, 0.25);

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
    draw_text(cr, SX+6, SY+58, s->artist, "Mono 13",
              COL_MID, 0.95, PANGO_ALIGN_LEFT, SW-10);
    draw_text(cr, SX+6, SY+79, s->album[0] ? s->album : "", "Mono 11",
              COL_DIM, 1.0, PANGO_ALIGN_LEFT, SW-10);
    hairline(cr, SX+5, SY+100, SX+SW-5, COL_MID, 0.22);

    {
        double frac = (s->duration > 0)
                      ? (double)s->elapsed / (double)s->duration : 0.0;
        draw_progress(cr, SX+6, SY+105, SW-12, 14, frac);
    }

    {
        char t[32];
        int em=s->elapsed/60, es=s->elapsed%60;
        int dm=s->duration/60, ds=s->duration%60;
        snprintf(t, sizeof(t), "%02d:%02d / %02d:%02d", em, es, dm, ds);
        draw_text(cr, SX+SW/2.0, SY+123, t, "Mono Bold 15",
                  COL_BRIGHT, 0.95, PANGO_ALIGN_CENTER, SW);
    }

    hairline(cr, SX+5, SY+148, SX+SW-5, COL_MID, 0.20);

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

    if (s->duration > 0) {
        int rem=s->duration-s->elapsed, rm=rem/60, rs=rem%60;
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), "-%02d:%02d remaining", rm, rs);
        draw_text(cr, SX+SW/2.0, SY+200, rbuf, "Mono 8",
                  COL_DIM, 0.60, PANGO_ALIGN_CENTER, SW);
    }

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

    /* ── menu overlay (drawn on top of everything above) ─────── */
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

    /* ── corner rivets ── */
    {
        double rv[][2] = { {9,9},{W-9,9},{9,H-9},{W-9,H-9} };
        for (int i=0; i<4; i++) {
            cairo_pattern_t *p = cairo_pattern_create_radial(
                rv[i][0]-1,rv[i][1]-1,0, rv[i][0],rv[i][1],4.5);
            cairo_pattern_add_color_stop_rgb(p, 0.0, 0.48,0.48,0.53);
            cairo_pattern_add_color_stop_rgb(p, 1.0, 0.11,0.11,0.14);
            cairo_set_source(cr, p);
            cairo_arc(cr, rv[i][0],rv[i][1], 3.5, 0, 2*G_PI);
            cairo_fill(cr);
            cairo_pattern_destroy(p);
        }
    }

    return FALSE;
}

/* ══════════════════════════════════════════════════════════════
 *  Keyboard handler
 * ══════════════════════════════════════════════════════════════ */

static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)widget;
    AppData *app = (AppData *)data;
    switch (ev->keyval) {
    case GDK_KEY_m: case GDK_KEY_M:
        menu_input(app, BTN_MENU);  break;
    case GDK_KEY_Up:
        menu_input(app, BTN_UP);    break;
    case GDK_KEY_Down:
        menu_input(app, BTN_DOWN);  break;
    case GDK_KEY_Right: case GDK_KEY_Return: case GDK_KEY_KP_Enter:
        menu_input(app, BTN_ENTER); break;
    case GDK_KEY_Left: case GDK_KEY_Escape:
        menu_input(app, BTN_BACK);  break;
    default: return FALSE;
    }
    return TRUE;
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
        app->scroll_x   = 0.0;
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
    gtk_widget_set_can_focus(app.canvas, TRUE);
    g_signal_connect(app.canvas, "draw",         G_CALLBACK(on_draw), &app);
    g_signal_connect(win,        "key-press-event", G_CALLBACK(on_key),  &app);
    gtk_container_add(GTK_CONTAINER(win), app.canvas);

    app.anim_id = g_timeout_add(33,      on_anim, &app);
    app.poll_id = g_timeout_add(POLL_MS, on_poll, &app);

    gtk_widget_show_all(win);
    gtk_main();

    g_source_remove(app.anim_id);
    g_source_remove(app.poll_id);
    return 0;
}
