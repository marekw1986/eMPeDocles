/*
 * mpd_client.c — MPD protocol implementation
 *
 * See mpd_client.h for the public surface. All functions here are
 * self-contained: each opens its own short-lived connection,
 * issues one or two commands, and closes. This keeps the calling
 * code (menu.c, main.c) free of socket bookkeeping.
 */
#include "mpd_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════
 *  Low-level connection
 * ══════════════════════════════════════════════════════════════ */

const char *mpd_host(void)
{
    const char *h = getenv("MPD_HOST");
    return h ? h : "localhost";
}

int mpd_port(void)
{
    const char *p = getenv("MPD_PORT");
    return p ? atoi(p) : 6600;
}

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

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int mpd_readline(int fd, char *buf, int sz)
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

void mpd_drain(int fd)
{
    char line[512];
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
    }
}

int mpd_open(void)
{
    int fd = mpd_connect(mpd_host(), mpd_port());
    if (fd < 0) return -1;
    char line[128];
    mpd_readline(fd, line, sizeof(line));
    if (strncmp(line, "OK MPD", 6) != 0) { close(fd); return -1; }
    return fd;
}

/* ══════════════════════════════════════════════════════════════
 *  Playback status poll
 * ══════════════════════════════════════════════════════════════ */

void mpd_poll(MpdState *s)
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

    /* currentsong */
    send(fd, "currentsong\n", 12, 0);
    s->title[0] = s->artist[0] = s->album[0] = '\0';
    for (;;) {
        mpd_readline(fd, line, sizeof(line));
        if (strncmp(line, "OK",  2) == 0) break;
        if (strncmp(line, "ACK", 3) == 0) break;
        if      (strncmp(line, "Title: ",  7) == 0) { strncpy(s->title,  line+7, sizeof(s->title)-1);  s->title[sizeof(s->title)-1]  = '\0'; }
        else if (strncmp(line, "Artist: ", 8) == 0) { strncpy(s->artist, line+8, sizeof(s->artist)-1); s->artist[sizeof(s->artist)-1] = '\0'; }
        else if (strncmp(line, "Album: ",  7) == 0) { strncpy(s->album,  line+7, sizeof(s->album)-1);  s->album[sizeof(s->album)-1]  = '\0'; }
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
        if      (strncmp(line, "state: ",          7) == 0) { strncpy(s->state, line+7, sizeof(s->state)-1); s->state[sizeof(s->state)-1] = '\0'; }
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
 *  Menu data loaders
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

void load_playlists(MenuLevel *lv)
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

void load_artists(MenuLevel *lv)
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

void load_albums_for_artist(MenuLevel *lv, const char *artist)
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

void load_albums(MenuLevel *lv)
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

void load_songs_for_album(MenuLevel *lv, const char *album)
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
    /* MPD returns one block per song; collect uri+title, emit on next file/OK */
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
            strncpy(uri, line+6, sizeof(uri)-1); uri[sizeof(uri)-1] = '\0';
            title[0] = '\0';
        } else if (strncmp(line, "Title: ", 7) == 0) {
            strncpy(title, line+7, sizeof(title)-1); title[sizeof(title)-1] = '\0';
        }
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(no tracks)", MTYPE_SUBMENU, NULL);
}

void load_songs(MenuLevel *lv)
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
            strncpy(uri, line+6, sizeof(uri)-1); uri[sizeof(uri)-1] = '\0';
            title[0] = '\0';
        } else if (strncmp(line, "Title: ", 7) == 0) {
            strncpy(title, line+7, sizeof(title)-1); title[sizeof(title)-1] = '\0';
        }
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(empty library)", MTYPE_SUBMENU, NULL);
}

void load_queue(MenuLevel *lv)
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
            strncpy(id_str, line+4, sizeof(id_str)-1); id_str[sizeof(id_str)-1] = '\0';
            title[0] = '\0';
            first = 0;
        } else if (strncmp(line, "Title: ", 7) == 0) {
            strncpy(title, line+7, sizeof(title)-1); title[sizeof(title)-1] = '\0';
        }
    }
    send(fd, "close\n", 6, 0);
    close(fd);
    if (lv->count == 0)
        menu_add(lv, "(queue is empty)", MTYPE_SUBMENU, NULL);
}

/* ══════════════════════════════════════════════════════════════
 *  Playback actions
 * ══════════════════════════════════════════════════════════════ */

void action_play_playlist(const char *name)
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

void action_play_song(const char *uri)
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

void action_queue_jump(const char *id_str)
{
    int fd = mpd_open();
    if (fd < 0) return;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "playid %s\n", id_str);
    send(fd, cmd, strlen(cmd), 0);     mpd_drain(fd);
    send(fd, "close\n", 6, 0);
    close(fd);
}
