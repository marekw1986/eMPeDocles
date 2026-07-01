/*
 * fb_backend_sdl.c — SDL2 window simulating the framebuffer
 *
 * For desktop development/testing without a Raspberry Pi or a
 * framebuffer console. Opens a normal SDL2 window at WIN_W x
 * WIN_H (no scaling — 1:1, since this *is* the target resolution
 * unlike the real fb backend which may scale to a different
 * panel size), and routes SDL keyboard events into the same
 * menu_input() calls the Linux fb backend uses.
 *
 * Build with `make BACKEND=sdl` (requires libsdl2-dev).
 */
#include "fb_backend.h"
#include "menu.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

static cairo_surface_t *s_surface = NULL;
static cairo_t         *s_cr      = NULL;

static SDL_Window   *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture  *s_tex = NULL;

int fb_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "fb_backend_sdl: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    s_win = SDL_CreateWindow(
        "MPD Retro (SDL framebuffer simulator)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!s_win) {
        fprintf(stderr, "fb_backend_sdl: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    s_ren = SDL_CreateRenderer(s_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren) {
        /* fall back to software renderer if accelerated isn't available
         * (common in headless / VM / CI environments) */
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_ren) {
        fprintf(stderr, "fb_backend_sdl: SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_win);
        SDL_Quit();
        return -1;
    }

    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, WIN_W, WIN_H);
    if (!s_tex) {
        fprintf(stderr, "fb_backend_sdl: SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(s_ren);
        SDL_DestroyWindow(s_win);
        SDL_Quit();
        return -1;
    }

    s_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WIN_W, WIN_H);
    if (!s_surface || cairo_surface_status(s_surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "fb_backend_sdl: failed to create Cairo surface\n");
        SDL_DestroyTexture(s_tex);
        SDL_DestroyRenderer(s_ren);
        SDL_DestroyWindow(s_win);
        SDL_Quit();
        return -1;
    }
    s_cr = cairo_create(s_surface);

    fprintf(stderr, "fb_backend_sdl: window %dx%d ready "
            "(Q to quit, M for menu, arrows + Enter/Esc to navigate)\n",
            WIN_W, WIN_H);
    return 0;
}

void fb_shutdown(void)
{
    if (s_cr)      cairo_destroy(s_cr);
    if (s_surface) cairo_surface_destroy(s_surface);
    s_cr = NULL; s_surface = NULL;

    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);
    s_tex = NULL; s_ren = NULL; s_win = NULL;

    SDL_Quit();
}

cairo_t *fb_get_cairo(void) { return s_cr; }

void fb_present(void)
{
    if (!s_surface || !s_tex) return;

    cairo_surface_flush(s_surface);
    uint8_t *src = cairo_image_surface_get_data(s_surface);
    int stride = cairo_image_surface_get_stride(s_surface);

    /* cairo ARGB32 is already 32bpp native-endian, premultiplied —
     * SDL_PIXELFORMAT_ARGB8888 matches directly, so this is a
     * straight stride-respecting memcpy via SDL_UpdateTexture */
    SDL_UpdateTexture(s_tex, NULL, src, stride);

    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

/* ══════════════════════════════════════════════════════════════
 *  input: SDL keyboard -> MenuButton (same key layout as the
 *  original GTK keyboard handler, for muscle-memory continuity)
 * ══════════════════════════════════════════════════════════════ */

static void handle_key(AppData *app, SDL_Keycode key)
{
    switch (key) {
    case SDLK_m:      menu_input(app, BTN_MENU);  break;
    case SDLK_UP:     menu_input(app, BTN_UP);    break;
    case SDLK_DOWN:   menu_input(app, BTN_DOWN);  break;
    case SDLK_RIGHT:
    case SDLK_RETURN:
    case SDLK_KP_ENTER: menu_input(app, BTN_ENTER); break;
    case SDLK_LEFT:
    case SDLK_ESCAPE: menu_input(app, BTN_BACK);  break;
    case SDLK_q:      app->running = 0;           break;
    default: break;
    }
}

void fb_poll_input(AppData *app)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            app->running = 0;
            break;
        case SDL_KEYDOWN:
            handle_key(app, ev.key.keysym.sym);
            break;
        default:
            break;
        }
    }
}
