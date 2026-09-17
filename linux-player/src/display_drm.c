/* display_drm.c — DRM/KMS backend.
 *
 * Deliberately the simplest thing that works: one dumb buffer, set once via
 * drmModeSetCrtc, written in place every frame. No atomic KMS, no page-flip
 * events, no compositor. That matches the "direct hardware access over
 * abstraction layers" approach used throughout GeekDS — an idle signage
 * player has no business fighting a display server for the screen.
 */
#include "stp.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>

typedef struct {
    int fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t fb_id;
    uint32_t handle;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc; /* to restore/blank */
    size_t map_size;
} drm_priv_t;

static void drm_present(stp_display_t *d) { (void)d; /* direct scanout: nothing to do */ }

static void drm_set_power(stp_display_t *d, bool on) {
    drm_priv_t *p = d->priv;
    if (on) {
        drmModeSetCrtc(p->fd, p->crtc_id, p->fb_id, 0, 0, &p->connector_id, 1, &p->mode);
    } else {
        drmModeSetCrtc(p->fd, p->crtc_id, 0, 0, 0, NULL, 0, NULL);
    }
}

static void drm_destroy(stp_display_t *d) {
    drm_priv_t *p = d->priv;
    if (!p) { free(d); return; }
    if (d->pixels) munmap(d->pixels, p->map_size);
    if (p->saved_crtc) {
        drmModeSetCrtc(p->fd, p->saved_crtc->crtc_id, p->saved_crtc->buffer_id,
                        p->saved_crtc->x, p->saved_crtc->y, &p->connector_id, 1,
                        &p->saved_crtc->mode);
        drmModeFreeCrtc(p->saved_crtc);
    }
    if (p->fb_id) drmModeRmFB(p->fd, p->fb_id);
    if (p->handle) {
        struct drm_mode_destroy_dumb destroy = { .handle = p->handle };
        ioctl(p->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    close(p->fd);
    free(p);
    free(d);
}

/* Picks the first connected connector with at least one mode, and the CRTC
 * feeding it (via the current encoder, or the first usable one). */
static bool find_connector_and_crtc(int fd, drmModeRes *res, drmModeConnector **out_conn,
                                     uint32_t *out_crtc_id) {
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            uint32_t crtc_id = 0;
            if (c->encoder_id) {
                drmModeEncoder *enc = drmModeGetEncoder(fd, c->encoder_id);
                if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
            }
            if (!crtc_id) {
                for (int j = 0; j < c->count_encoders; j++) {
                    drmModeEncoder *enc = drmModeGetEncoder(fd, c->encoders[j]);
                    if (!enc) continue;
                    for (int k = 0; k < res->count_crtcs; k++) {
                        if (enc->possible_crtcs & (1 << k)) { crtc_id = res->crtcs[k]; break; }
                    }
                    drmModeFreeEncoder(enc);
                    if (crtc_id) break;
                }
            }
            if (crtc_id) { *out_conn = c; *out_crtc_id = crtc_id; return true; }
        }
        drmModeFreeConnector(c);
    }
    return false;
}

stp_display_t *display_drm_open(const char *card_path) {
    char path_buf[64];
    int fd = -1;

    if (card_path) {
        fd = open(card_path, O_RDWR | O_CLOEXEC);
    } else {
        for (int i = 0; i < 8 && fd < 0; i++) {
            snprintf(path_buf, sizeof(path_buf), "/dev/dri/card%d", i);
            fd = open(path_buf, O_RDWR | O_CLOEXEC);
        }
    }
    if (fd < 0) { LOG("drm: no card device could be opened"); return NULL; }

    drmSetMaster(fd); /* fine if we're not actually master (e.g. logind already granted it) */

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { LOG("drm: drmModeGetResources failed: %s", strerror(errno)); close(fd); return NULL; }

    drmModeConnector *conn = NULL;
    uint32_t crtc_id = 0;
    if (!find_connector_and_crtc(fd, res, &conn, &crtc_id)) {
        LOG("drm: no connected display found on %s", card_path ? card_path : "any card");
        drmModeFreeResources(res);
        close(fd);
        return NULL;
    }
    drmModeModeInfo mode = conn->modes[0]; /* modes[0] is the preferred/highest mode */
    uint32_t connector_id = conn->connector_id;
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    /* Dumb buffer sized to the mode, 32bpp XRGB8888 == BGRA bytes in memory. */
    struct drm_mode_create_dumb creq = {
        .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        LOG("drm: create dumb buffer failed: %s", strerror(errno));
        close(fd); return NULL;
    }

    uint32_t fb_id = 0;
    if (drmModeAddFB(fd, creq.width, creq.height, 24, 32, creq.pitch, creq.handle, &fb_id) < 0) {
        LOG("drm: drmModeAddFB failed: %s", strerror(errno));
        close(fd); return NULL;
    }

    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        LOG("drm: map dumb buffer failed: %s", strerror(errno));
        close(fd); return NULL;
    }
    uint8_t *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED) {
        LOG("drm: mmap failed: %s", strerror(errno));
        close(fd); return NULL;
    }
    memset(map, 0, creq.size);

    drmModeCrtc *saved = drmModeGetCrtc(fd, crtc_id);

    if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &mode) < 0) {
        LOG("drm: drmModeSetCrtc failed: %s", strerror(errno));
        munmap(map, creq.size);
        close(fd);
        return NULL;
    }

    drm_priv_t *p = calloc(1, sizeof(*p));
    p->fd = fd; p->crtc_id = crtc_id; p->connector_id = connector_id;
    p->fb_id = fb_id; p->handle = creq.handle; p->mode = mode;
    p->saved_crtc = saved; p->map_size = creq.size;

    stp_display_t *d = calloc(1, sizeof(*d));
    d->width = creq.width; d->height = creq.height; d->stride = creq.pitch;
    d->pixels = map; d->backend_name = "drm";
    d->present = drm_present; d->set_power = drm_set_power; d->destroy = drm_destroy;
    d->priv = p;

    LOG("drm: %ux%u on connector %u, crtc %u (%s)", creq.width, creq.height, connector_id,
        crtc_id, card_path ? card_path : "auto");
    return d;
}
