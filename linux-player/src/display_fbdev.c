/* display_fbdev.c — plain /dev/fb0 backend.
 *
 * Same approach GeekDS's fbplay_video.c uses: mmap the framebuffer, hand
 * back a pointer + stride, let the caller (sws_scale, in our case) write
 * straight into it. No DirectFB, no SDL2, no display server.
 */
#include "stp.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

typedef struct {
    int fd;
    size_t map_size;
    struct fb_var_screeninfo vinfo;
} fb_priv_t;

static void fb_present(stp_display_t *d) { (void)d; }

static void fb_set_power(stp_display_t *d, bool on) {
    fb_priv_t *p = d->priv;
    /* FB_BLANK_UNBLANK = 0, FB_BLANK_POWERDOWN = 4 */
    ioctl(p->fd, FBIOBLANK, on ? 0 : 4);
}

static void fb_destroy(stp_display_t *d) {
    fb_priv_t *p = d->priv;
    if (d->pixels) munmap(d->pixels, p->map_size);
    if (p->fd >= 0) close(p->fd);
    free(p);
    free(d);
}

stp_display_t *display_fbdev_open(const char *dev_path) {
    const char *path = dev_path ? dev_path : "/dev/fb0";
    int fd = open(path, O_RDWR);
    if (fd < 0) { LOG("fbdev: open(%s) failed: %s", path, strerror(errno)); return NULL; }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        LOG("fbdev: FBIOGET_*SCREENINFO failed: %s", strerror(errno));
        close(fd); return NULL;
    }

    if (vinfo.bits_per_pixel != 32) {
        LOG("fbdev: %ubpp framebuffer unsupported (need 32bpp XRGB/ARGB); "
            "set the mode via fbset or /etc/fb.modes first", vinfo.bits_per_pixel);
        close(fd); return NULL;
    }

    size_t map_size = (size_t)finfo.line_length * vinfo.yres;
    uint8_t *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        LOG("fbdev: mmap failed: %s", strerror(errno));
        close(fd); return NULL;
    }
    memset(map, 0, map_size);

    fb_priv_t *p = calloc(1, sizeof(*p));
    p->fd = fd; p->map_size = map_size; p->vinfo = vinfo;

    stp_display_t *d = calloc(1, sizeof(*d));
    d->width = vinfo.xres; d->height = vinfo.yres; d->stride = finfo.line_length;
    d->pixels = map; d->backend_name = "fbdev";
    d->present = fb_present; d->set_power = fb_set_power; d->destroy = fb_destroy;
    d->priv = p;

    LOG("fbdev: %ux%u @ %ubpp on %s (red offset %u, blue offset %u — verify this matches "
        "XRGB8888 on non-standard hardware, see /etc/directfbrc note in GeekDS notes)",
        vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, path, vinfo.red.offset, vinfo.blue.offset);
    return d;
}
