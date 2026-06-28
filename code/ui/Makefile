CC      = gcc
TARGET  = mpd_retro
SRC     = mpd_retro.c

# pkg-config may warn about liblzma on some systems; suppress stderr
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs   gtk+-3.0 2>/dev/null)

# Fallback if pkg-config produced nothing (liblzma .pc missing)
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

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
