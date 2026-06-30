/*
 * mpd_client.h — all MPD protocol I/O
 *
 * This is the only module that opens sockets or speaks the MPD
 * text protocol. main_screen.c and menu.c never touch sockets
 * directly — they call into here.
 */
#ifndef MPD_CLIENT_H
#define MPD_CLIENT_H

#include "app.h"

/* ── connection helpers ───────────────────────────────────────── */

/* host/port resolved from MPD_HOST / MPD_PORT env vars (default
 * localhost:6600) */
const char *mpd_host(void);
int         mpd_port(void);

/* open a connection and consume the "OK MPD ..." banner.
 * Returns a socket fd, or -1 on failure. Caller must close(fd)
 * (after sending "close\n" if a clean disconnect is desired). */
int mpd_open(void);

/* read a single '\n'-terminated line into buf (size sz).
 * Returns the number of bytes read (excluding the newline). */
int mpd_readline(int fd, char *buf, int sz);

/* read and discard lines until an "OK" or "ACK" terminator. */
void mpd_drain(int fd);

/* ── playback status ──────────────────────────────────────────── */

/* poll currentsong + status and fill *s. On connection failure,
 * s->connected is set to 0 and title/artist are set to a
 * diagnostic placeholder. */
void mpd_poll(MpdState *s);

/* ── menu data loaders ────────────────────────────────────────── */
/* Each of these opens its own connection, queries MPD, and fills
 * the given MenuLevel with MenuItem entries. They always leave
 * *lv with at least one entry (a placeholder like "(no connection)"
 * if the query failed or returned nothing), so the menu never
 * renders an empty screen. */

void load_playlists(MenuLevel *lv);
void load_artists(MenuLevel *lv);
void load_albums_for_artist(MenuLevel *lv, const char *artist);
void load_albums(MenuLevel *lv);
void load_songs_for_album(MenuLevel *lv, const char *album);
void load_songs(MenuLevel *lv);
void load_queue(MenuLevel *lv);

/* ── playback actions ─────────────────────────────────────────── */

/* clear queue, load saved playlist by name, play */
void action_play_playlist(const char *name);

/* clear queue, add a single song by uri, play */
void action_play_song(const char *uri);

/* jump playback to a song id already in the queue */
void action_queue_jump(const char *id_str);

#endif /* MPD_CLIENT_H */
