# mpd_retro — retro VFD-style MPD display with browse menu
#
# Source layout:
#   app.h          shared types/constants, drawing-primitive prototypes
#   draw_common.*  cairo/pango drawing primitives (no app/MPD state)
#   mpd_client.*   MPD protocol I/O (sockets, polling, menu loaders, actions)
#   menu.*         browse-menu state machine + rendering
#                  (menu_input() is the hardware-button integration point)
#   main_screen.*  playback screen rendering (on_draw)
#   main.c         GTK setup, keyboard handler, timers, entry point

CC      = gcc
TARGET  = mpd_retro

SRCS    = main.c main_screen.c menu.c mpd_client.c draw_common.c
OBJS    = $(SRCS:.c=.o)
HDRS    = app.h main_screen.h menu.h mpd_client.h draw_common.h

# pkg-config may warn about liblzma on some systems; suppress stderr
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs   gtk+-3.0 2>/dev/null)

# Fallback if pkg-config produced nothing (e.g. liblzma .pc missing)
ifeq ($(strip $(GTK_CFLAGS)),)
GTK_CFLAGS := -I/usr/include/gtk-3.0 \
              -I/usr/include/glib-2.0 \
              -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
              -I/usr/include/pango-1.0 \
              -I/usr/include/cairo \
              -I/usr/include/gdk-pixbuf-2.0 \
              -I/usr/include/atk-1.0 \
              -I/usr/include/harfbuzz
GTK_LIBS   := -lgtk-3 -lgdk-3 -lpangocairo-1.0 -lpango-1.0 \
              -lcairo -lgobject-2.0 -lglib-2.0
endif

CFLAGS  = -Wall -Wextra -O2 $(GTK_CFLAGS)
LDFLAGS = $(GTK_LIBS) -lm

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
