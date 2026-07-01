# mpd_retro — framebuffer build
#
# BACKEND=linux   real /dev/fb0 (Raspberry Pi / Linux console)
# BACKEND=sdl     SDL2 window   (desktop testing)
#
#   make BACKEND=sdl
#   make BACKEND=linux
#   make              (defaults to linux)

BACKEND ?= linux

CC      = gcc
TARGET  = mpd_retro

SRCS_COMMON = main.c main_screen.c menu.c mpd_client.c draw_common.c
HDRS        = app.h fb_backend.h main_screen.h menu.h mpd_client.h draw_common.h

ifeq ($(BACKEND),sdl)
  SRCS_BACKEND = fb_backend_sdl.c
  SDL_CFLAGS  := $(shell sdl2-config --cflags 2>/dev/null)
  SDL_LIBS    := $(shell sdl2-config --libs   2>/dev/null)
  ifeq ($(strip $(SDL_CFLAGS)),)
    # pkg-config fallback
    SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
    SDL_LIBS   := $(shell pkg-config --libs   sdl2 2>/dev/null)
  endif
  ifeq ($(strip $(SDL_CFLAGS)),)
    $(error SDL2 not found. Install libsdl2-dev: sudo apt install libsdl2-dev)
  endif
  EXTRA_CFLAGS = $(SDL_CFLAGS)
  EXTRA_LIBS   = $(SDL_LIBS)
else
  SRCS_BACKEND = fb_backend_linux.c
  EXTRA_CFLAGS =
  EXTRA_LIBS   =
endif

SRCS = $(SRCS_COMMON) $(SRCS_BACKEND)
OBJS = $(SRCS:.c=.o)

# Cairo + Pango (no GTK)
CAIRO_CFLAGS := $(shell pkg-config --cflags cairo pangocairo 2>/dev/null)
CAIRO_LIBS   := $(shell pkg-config --libs   cairo pangocairo 2>/dev/null)

ifeq ($(strip $(CAIRO_CFLAGS)),)
  CAIRO_CFLAGS := -I/usr/include/cairo \
                  -I/usr/include/pango-1.0 \
                  -I/usr/include/glib-2.0 \
                  -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
                  -I/usr/include/harfbuzz
  CAIRO_LIBS   := -lcairo -lpangocairo-1.0 -lpango-1.0 \
                  -lgobject-2.0 -lglib-2.0
endif

CFLAGS  = -Wall -Wextra -O2 $(CAIRO_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS = $(CAIRO_LIBS) $(EXTRA_LIBS) -lm

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
