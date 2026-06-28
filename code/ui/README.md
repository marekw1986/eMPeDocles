# mpd_retro — Retro VFD-style MPD Display

A tiny 320×240 GTK3 window that shows your MPD playback status
styled after the front-panel display of late-90s rack electronics
(CD players, DVD players, AV receivers).

```
┌──────────────────────────────────────────┐
│ RETRO·MPD                             ● │
│ ▶  Track Title Scrolling Here …         │
│────────────────────────────────────────│
│  Artist Name                            │
│  Album Title                            │
│────────────────────────────────────────│
│  · · · · · · · · · · · · · · ·  (bar)  │
│             02:34 / 04:12               │
│────────────────────────────────────────│
│       320kbps  44kHz  VOL:85%           │
└──────────────────────────────────────────┘
│  [⏮]  [⏪]  [⏩]  [⏭]  [⏏]           │
└──────────────────────────────────────────┘
```

## Features

- VFD cyan-on-dark-blue colour scheme with glow effects
- Scrolling (marquee) title when the track is playing
- Progress bar rendered as a row of segment dots
- Artist / album display
- Elapsed / total time
- Bitrate, sample rate and volume readout
- Play / pause / stop state indicator
- Connection LED (amber = OK, red = no MPD)
- Decorative buttons and rivets (visual only)

## Building

```sh
make
```

Requires: `gcc`, `libgtk-3-dev`

## Running

```sh
./mpd_retro
```

Override the MPD connection with environment variables:

```sh
MPD_HOST=192.168.1.10 MPD_PORT=6600 ./mpd_retro
```

Defaults: `localhost:6600`

## Notes

- The window is fixed at 320×240.
- MPD is polled every second; the display animates at ~30 fps.
- The control buttons are purely decorative — click handling is
  left as an exercise (send `previous`, `next`, `pause` commands
  to MPD the same way `currentsong`/`status` are sent).
