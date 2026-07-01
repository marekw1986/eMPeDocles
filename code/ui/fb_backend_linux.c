/*
 * fb_backend_linux.c — real /dev/fb0 framebuffer backend
 *
 * Targets the Linux console framebuffer directly via mmap. This
 * is what runs on the Raspberry Pi (or any Linux box, switched
 * away from X11/Wayland to a text console — Ctrl+Alt+F2, etc.).
 *
 * Rendering: Cairo draws into an offscreen ARGB32 image surface
 * at the *fixed* 320x240 app resolution; fb_present() then scales
 * + format-converts that into the real framebuffer's native pixel
 * format and resolution (commonly RGB565 or XRGB8888) and memcpy's
 * it into the mmap'd region.
 *
 * Input: raw evdev keyboard events read from /dev/input/event*.
 * Requires read access to that device node — on a stock Pi OS,
 * either run as root or add your user to the `input` group.
 */
#include "fb_backend.h"
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <dirent.h>
#include <poll.h>

/* ── app-resolution offscreen surface ───────────────────────── */
static cairo_surface_t *s_surface = NULL;
static cairo_t         *s_cr      = NULL;

/* ── real framebuffer device ────────────────────────────────── */
static int                       s_fb_fd = -1;
static uint8_t                  *s_fb_mem = NULL;
static size_t                    s_fb_size = 0;
static struct fb_var_screeninfo  s_vinfo;
static struct fb_fix_screeninfo  s_finfo;

/* ── evdev keyboard ──────────────────────────────────────────── */
#define MAX_KBD_FDS 8
static int s_kbd_fds[MAX_KBD_FDS];
static int s_kbd_count = 0;

/* env override for the fb device path, default /dev/fb0 */
static const char *fb_device_path(void)
{
    const char *p = getenv("FB_DEVICE");
    return p ? p : "/dev/fb0";
}

/* ══════════════════════════════════════════════════════════════
 *  init / shutdown
 * ══════════════════════════════════════════════════════════════ */

static int open_framebuffer(void)
{
    s_fb_fd = open(fb_device_path(), O_RDWR);
    if (s_fb_fd < 0) {
        fprintf(stderr, "fb_backend_linux: open(%s): %s\n",
                fb_device_path(), strerror(errno));
        return -1;
    }

    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &s_vinfo) < 0) {
        fprintf(stderr, "fb_backend_linux: FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        close(s_fb_fd); s_fb_fd = -1;
        return -1;
    }
    if (ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &s_finfo) < 0) {
        fprintf(stderr, "fb_backend_linux: FBIOGET_FSCREENINFO: %s\n", strerror(errno));
        close(s_fb_fd); s_fb_fd = -1;
        return -1;
    }

    s_fb_size = (size_t)s_finfo.line_length * s_vinfo.yres;
    s_fb_mem = mmap(NULL, s_fb_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, s_fb_fd, 0);
    if (s_fb_mem == MAP_FAILED) {
        fprintf(stderr, "fb_backend_linux: mmap: %s\n", strerror(errno));
        close(s_fb_fd); s_fb_fd = -1;
        return -1;
    }

    fprintf(stderr,
            "fb_backend_linux: %s  %ux%u  %ubpp  line_length=%u\n",
            fb_device_path(), s_vinfo.xres, s_vinfo.yres,
            s_vinfo.bits_per_pixel, s_finfo.line_length);
    return 0;
}

/* find keyboard-capable /dev/input/eventN nodes and open them
 * non-blocking; we don't care which one in particular, so we
 * just poll all of them */
static void open_keyboards(void)
{
    s_kbd_count = 0;
    DIR *d = opendir("/dev/input");
    if (!d) {
        fprintf(stderr, "fb_backend_linux: opendir(/dev/input): %s "
                "(no keyboard input available)\n", strerror(errno));
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && s_kbd_count < MAX_KBD_FDS) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        /* check this device reports EV_KEY (keyboards do) */
        unsigned long evbits = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) >= 0 &&
            (evbits & (1 << EV_KEY))) {
            s_kbd_fds[s_kbd_count++] = fd;
        } else {
            close(fd);
        }
    }
    closedir(d);

    if (s_kbd_count == 0)
        fprintf(stderr, "fb_backend_linux: no keyboard-capable "
                "/dev/input/event* found (need read access — try "
                "root or the 'input' group)\n");
}

int fb_init(void)
{
    s_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WIN_W, WIN_H);
    if (!s_surface || cairo_surface_status(s_surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "fb_backend_linux: failed to create Cairo surface\n");
        return -1;
    }
    s_cr = cairo_create(s_surface);

    if (open_framebuffer() < 0) {
        cairo_destroy(s_cr);
        cairo_surface_destroy(s_surface);
        s_cr = NULL; s_surface = NULL;
        return -1;
    }

    open_keyboards();
    return 0;
}

void fb_shutdown(void)
{
    for (int i = 0; i < s_kbd_count; i++) close(s_kbd_fds[i]);
    s_kbd_count = 0;

    if (s_fb_mem && s_fb_mem != MAP_FAILED) munmap(s_fb_mem, s_fb_size);
    if (s_fb_fd >= 0) close(s_fb_fd);
    s_fb_mem = NULL; s_fb_fd = -1;

    if (s_cr) cairo_destroy(s_cr);
    if (s_surface) cairo_surface_destroy(s_surface);
    s_cr = NULL; s_surface = NULL;
}

cairo_t *fb_get_cairo(void) { return s_cr; }

/* ══════════════════════════════════════════════════════════════
 *  present: blit the ARGB32 offscreen surface into the real fb,
 *  nearest-neighbour scaled to the panel's actual resolution and
 *  converted to its native pixel format.
 * ══════════════════════════════════════════════════════════════ */

static inline uint32_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void fb_present(void)
{
    if (!s_surface || !s_fb_mem) return;

    cairo_surface_flush(s_surface);
    uint8_t *src = cairo_image_surface_get_data(s_surface);
    int src_stride = cairo_image_surface_get_stride(s_surface);

    unsigned dst_w  = s_vinfo.xres;
    unsigned dst_h  = s_vinfo.yres;
    unsigned dst_bpp = s_vinfo.bits_per_pixel;
    unsigned dst_stride = s_finfo.line_length;

    for (unsigned dy = 0; dy < dst_h; dy++) {
        /* nearest-neighbour sample from the WIN_W x WIN_H source */
        unsigned sy = (unsigned)((uint64_t)dy * WIN_H / dst_h);
        if (sy >= (unsigned)WIN_H) sy = WIN_H - 1;
        const uint32_t *srow = (const uint32_t *)(src + sy * src_stride);

        uint8_t *drow = s_fb_mem + (size_t)dy * dst_stride;

        for (unsigned dx = 0; dx < dst_w; dx++) {
            unsigned sx = (unsigned)((uint64_t)dx * WIN_W / dst_w);
            if (sx >= (unsigned)WIN_W) sx = WIN_W - 1;

            uint32_t px = srow[sx];   /* cairo ARGB32 is premultiplied, native-endian */
            uint8_t a = (px >> 24) & 0xFF; (void)a; /* surface is opaque-painted, ignore */
            uint8_t r = (px >> 16) & 0xFF;
            uint8_t g = (px >>  8) & 0xFF;
            uint8_t b = (px >>  0) & 0xFF;

            if (dst_bpp == 32) {
                uint32_t *dp = (uint32_t *)(drow + dx * 4);
                *dp = (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)b;
            } else if (dst_bpp == 16) {
                uint16_t *dp = (uint16_t *)(drow + dx * 2);
                *dp = (uint16_t)pack_rgb565(r, g, b);
            } else {
                /* uncommon depth (e.g. 24bpp packed) — write 3 bytes BGR-ish;
                 * adjust here if your panel uses a different fb layout */
                uint8_t *dp = drow + dx * 3;
                dp[0] = b; dp[1] = g; dp[2] = r;
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  input: evdev keyboard -> MenuButton
 * ══════════════════════════════════════════════════════════════ */

static void handle_key(AppData *app, unsigned short code)
{
    switch (code) {
    case KEY_M:      menu_input(app, BTN_MENU);  break;
    case KEY_UP:     menu_input(app, BTN_UP);    break;
    case KEY_DOWN:   menu_input(app, BTN_DOWN);  break;
    case KEY_RIGHT:
    case KEY_ENTER:
    case KEY_KPENTER:menu_input(app, BTN_ENTER); break;
    case KEY_LEFT:
    case KEY_ESC:    menu_input(app, BTN_BACK);  break;
    case KEY_Q:      app->running = 0;           break;
    default: break;
    }
}

void fb_poll_input(AppData *app)
{
    if (s_kbd_count == 0) return;

    struct pollfd pfds[MAX_KBD_FDS];
    for (int i = 0; i < s_kbd_count; i++) {
        pfds[i].fd = s_kbd_fds[i];
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    int r = poll(pfds, (nfds_t)s_kbd_count, 0 /* non-blocking */);
    if (r <= 0) return;

    struct input_event ev;
    for (int i = 0; i < s_kbd_count; i++) {
        if (!(pfds[i].revents & POLLIN)) continue;
        while (read(s_kbd_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1 /* key down */)
                handle_key(app, ev.code);
        }
    }
}
