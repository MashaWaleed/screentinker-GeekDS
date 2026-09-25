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
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>

typedef struct {
    uint32_t fb_id, handle, pitch;
    size_t map_size;
    uint8_t *map;
} drm_buffer_t;

typedef struct {
    int fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t format;
    stp_pixel_layout_t pixel_layout;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc; /* to restore/blank */
    drm_buffer_t buffers[2];
    int front, back;
    bool composited;
    bool dirty;
    int dirty_x, dirty_y, dirty_w, dirty_h;
    pthread_mutex_t lock;
} drm_priv_t;

static void flip_done(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data) {
    (void)fd; (void)frame; (void)sec; (void)usec;
    *(bool *)data = true;
}

static bool page_flip(drm_priv_t *p, uint32_t fb_id) {
    bool done = false;
    if (drmModePageFlip(p->fd, p->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, &done) < 0) return false;
    drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };
    while (!done) {
        struct pollfd fds = { .fd = p->fd, .events = POLLIN };
        if (poll(&fds, 1, 1000) <= 0 || drmHandleEvent(p->fd, &ev) != 0) return false;
    }
    return true;
}

static void drm_present(stp_display_t *d) {
    drm_priv_t *p = d->priv;
    if (!p->composited) return;
    pthread_mutex_lock(&p->lock);
    if (p->dirty && page_flip(p, p->buffers[p->back].fb_id)) {
        int old_front = p->front;
        p->front = p->back;
        p->back = old_front;
        d->pixels = p->buffers[p->back].map;
        /* The old front is already identical outside the areas the compositor
         * changed.  Copy just those pixels, not a full display frame, before
         * reusing it as the next back buffer. */
        const drm_buffer_t *front = &p->buffers[p->front];
        drm_buffer_t *back = &p->buffers[p->back];
        for (int y = 0; y < p->dirty_h; y++) {
            size_t off = (size_t)(p->dirty_y + y) * front->pitch + (size_t)p->dirty_x * 4;
            memcpy(back->map + off, front->map + off, (size_t)p->dirty_w * 4);
        }
        p->dirty = false;
    } else {
        if (p->dirty) LOG("drm: page flip failed: %s", strerror(errno));
    }
    pthread_mutex_unlock(&p->lock);
}

static void drm_mark_dirty(stp_display_t *d, int x, int y, int w, int h) {
    drm_priv_t *p = d->priv;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > d->width) w = d->width - x;
    if (y + h > d->height) h = d->height - y;
    if (w <= 0 || h <= 0) return;

    pthread_mutex_lock(&p->lock);
    if (!p->dirty) {
        p->dirty = true;
        p->dirty_x = x; p->dirty_y = y; p->dirty_w = w; p->dirty_h = h;
    } else {
        int right = p->dirty_x + p->dirty_w;
        int bottom = p->dirty_y + p->dirty_h;
        if (x < p->dirty_x) p->dirty_x = x;
        if (y < p->dirty_y) p->dirty_y = y;
        if (x + w > right) right = x + w;
        if (y + h > bottom) bottom = y + h;
        p->dirty_w = right - p->dirty_x;
        p->dirty_h = bottom - p->dirty_y;
    }
    pthread_mutex_unlock(&p->lock);
}

static void drm_set_composited(stp_display_t *d, bool enabled) {
    drm_priv_t *p = d->priv;
    pthread_mutex_lock(&p->lock);
    if (enabled && !p->composited) {
        memcpy(p->buffers[p->back].map, p->buffers[p->front].map, p->buffers[p->front].map_size);
        d->pixels = p->buffers[p->back].map;
        p->composited = true;
        p->dirty = false;
    } else if (!enabled && p->composited) {
        d->pixels = p->buffers[p->front].map;
        p->composited = false;
        p->dirty = false;
    }
    pthread_mutex_unlock(&p->lock);
}

static void drm_set_power(stp_display_t *d, bool on) {
    drm_priv_t *p = d->priv;
    if (on) {
        drmModeSetCrtc(p->fd, p->crtc_id, p->buffers[p->front].fb_id, 0, 0,
                       &p->connector_id, 1, &p->mode);
    } else {
        drmModeSetCrtc(p->fd, p->crtc_id, 0, 0, 0, NULL, 0, NULL);
    }
}

static void drm_destroy(stp_display_t *d) {
    drm_priv_t *p = d->priv;
    if (!p) { free(d); return; }
    if (p->saved_crtc) {
        drmModeSetCrtc(p->fd, p->saved_crtc->crtc_id, p->saved_crtc->buffer_id,
                        p->saved_crtc->x, p->saved_crtc->y, &p->connector_id, 1,
                        &p->saved_crtc->mode);
        drmModeFreeCrtc(p->saved_crtc);
    }
    for (int i = 0; i < 2; i++) {
        if (p->buffers[i].map) munmap(p->buffers[i].map, p->buffers[i].map_size);
        if (p->buffers[i].fb_id) drmModeRmFB(p->fd, p->buffers[i].fb_id);
        if (p->buffers[i].handle) {
            struct drm_mode_destroy_dumb destroy = { .handle = p->buffers[i].handle };
            ioctl(p->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
    }
    pthread_mutex_destroy(&p->lock);
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

static bool plane_has_format(const drmModePlane *plane, uint32_t format) {
    for (uint32_t i = 0; i < plane->count_formats; i++)
        if (plane->formats[i] == format) return true;
    return false;
}

static bool plane_is_primary(int fd, uint32_t plane_id) {
    drmModeObjectProperties *properties =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!properties) return false;
    bool primary = false;
    for (uint32_t i = 0; i < properties->count_props; i++) {
        drmModePropertyRes *property = drmModeGetProperty(fd, properties->props[i]);
        if (property) {
            if (strcmp(property->name, "type") == 0 &&
                properties->prop_values[i] == DRM_PLANE_TYPE_PRIMARY)
                primary = true;
            drmModeFreeProperty(property);
        }
        if (primary) break;
    }
    drmModeFreeObjectProperties(properties);
    return primary;
}

/* The DRM fourcc names describe bits in a native-endian 32-bit word.  On the
 * little-endian CPUs used by our targets, XRGB/ARGB is B,G,R,X in memory and
 * XBGR/ABGR is R,G,B,X.  Select the plane's actual format before asking swscale
 * to render, rather than assuming every KMS driver is XRGB8888. */
static bool scanout_layout_for_format(uint32_t format, stp_pixel_layout_t *layout) {
    if (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888) {
        *layout = STP_PIXELS_BGRX;
        return true;
    }
    if (format == DRM_FORMAT_XBGR8888 || format == DRM_FORMAT_ABGR8888) {
        *layout = STP_PIXELS_RGBX;
        return true;
    }
    return false;
}

static uint32_t choose_scanout_format(int fd, drmModeRes *res, uint32_t crtc_id,
                                      uint32_t active_format, stp_pixel_layout_t *layout) {
    static const uint32_t preferred[] = {
        DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
    };
    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; i++)
        if (res->crtcs[i] == crtc_id) { crtc_index = i; break; }

    drmModePlaneRes *planes = crtc_index >= 0 ? drmModeGetPlaneResources(fd) : NULL;
    if (planes) {
        for (uint32_t i = 0; i < planes->count_planes; i++) {
            drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
            if (!plane) continue;
            bool usable = (plane->possible_crtcs & (1u << crtc_index)) != 0;
            bool primary = plane_is_primary(fd, plane->plane_id);
            if (usable && primary) {
                /* If a compositor or firmware splash already drives this
                 * CRTC with ARGB, retain that exact format.  Some simple DRM
                 * drivers advertise XRGB too but only scan out ARGB reliably. */
                if (scanout_layout_for_format(active_format, layout) &&
                    plane_has_format(plane, active_format)) {
                    drmModeFreePlane(plane);
                    drmModeFreePlaneResources(planes);
                    return active_format;
                }
                for (size_t j = 0; j < sizeof(preferred) / sizeof(preferred[0]); j++) {
                    if (plane_has_format(plane, preferred[j])) {
                        uint32_t chosen = preferred[j];
                        scanout_layout_for_format(chosen, layout);
                        drmModeFreePlane(plane);
                        drmModeFreePlaneResources(planes);
                        return chosen;
                    }
                }
            }
            drmModeFreePlane(plane);
        }
    }
    if (planes) drmModeFreePlaneResources(planes);
    /* Older legacy KMS drivers do not expose primary planes through the plane
     * API.  Their established default is XRGB8888, retained as a fallback. */
    *layout = STP_PIXELS_BGRX;
    return DRM_FORMAT_XRGB8888;
}

static bool create_buffer(int fd, uint32_t w, uint32_t h, uint32_t format, drm_buffer_t *b) {
    struct drm_mode_create_dumb req = { .width = w, .height = h, .bpp = 32 };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &req) < 0) return false;
    b->handle = req.handle; b->pitch = req.pitch; b->map_size = req.size;
    uint32_t handles[4] = { req.handle, 0, 0, 0 };
    uint32_t pitches[4] = { req.pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(fd, w, h, format, handles, pitches, offsets, &b->fb_id, 0) < 0) {
        /* Keep the legacy AddFB path for drivers that predate AddFB2. */
        if (format != DRM_FORMAT_XRGB8888 ||
            drmModeAddFB(fd, w, h, 24, 32, req.pitch, req.handle, &b->fb_id) < 0)
            return false;
    }
    struct drm_mode_map_dumb map_req = { .handle = req.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) return false;
    b->map = mmap(NULL, req.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_req.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; return false; }
    memset(b->map, 0, b->map_size);
    return true;
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
    /* Needed for primary-plane format discovery on modern atomic-capable KMS. */
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

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
    int mode_index = 0;
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode_index = i; break; }
    drmModeModeInfo mode = conn->modes[mode_index];
    uint32_t connector_id = conn->connector_id;
    drmModeCrtc *saved = drmModeGetCrtc(fd, crtc_id);
    uint32_t active_format = 0;
    if (saved && saved->buffer_id) {
        drmModeFB2 *active_fb = drmModeGetFB2(fd, saved->buffer_id);
        if (active_fb) {
            active_format = active_fb->pixel_format;
            drmModeFreeFB2(active_fb);
        }
    }
    stp_pixel_layout_t pixel_layout = STP_PIXELS_BGRX;
    uint32_t format = choose_scanout_format(fd, res, crtc_id, active_format, &pixel_layout);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    drm_priv_t *p = calloc(1, sizeof(*p));
    if (!p) { close(fd); return NULL; }
    p->fd = fd; p->crtc_id = crtc_id; p->connector_id = connector_id;
    p->mode = mode; p->saved_crtc = saved; p->front = 0; p->back = 1;
    p->format = format; p->pixel_layout = pixel_layout;
    pthread_mutex_init(&p->lock, NULL);
    if (!create_buffer(fd, mode.hdisplay, mode.vdisplay, format, &p->buffers[0]) ||
        !create_buffer(fd, mode.hdisplay, mode.vdisplay, format, &p->buffers[1])) {
        LOG("drm: double-buffer allocation failed: %s", strerror(errno));
        stp_display_t *failed = calloc(1, sizeof(*failed));
        if (failed) { failed->priv = p; drm_destroy(failed); }
        return NULL;
    }
    if (drmModeSetCrtc(fd, crtc_id, p->buffers[0].fb_id, 0, 0, &connector_id, 1, &mode) < 0) {
        LOG("drm: drmModeSetCrtc failed: %s", strerror(errno));
        stp_display_t *failed = calloc(1, sizeof(*failed));
        if (failed) { failed->priv = p; drm_destroy(failed); }
        return NULL;
    }

    stp_display_t *d = calloc(1, sizeof(*d));
    d->width = mode.hdisplay; d->height = mode.vdisplay; d->stride = p->buffers[0].pitch;
    d->pixels = p->buffers[0].map; d->pixel_layout = pixel_layout; d->backend_name = "drm";
    d->present = drm_present; d->set_power = drm_set_power; d->destroy = drm_destroy;
    d->set_composited = drm_set_composited;
    d->mark_dirty = drm_mark_dirty;
    d->priv = p;

    char format_name[5] = {
        (char)(format & 0xff), (char)((format >> 8) & 0xff),
        (char)((format >> 16) & 0xff), (char)((format >> 24) & 0xff), 0
    };
    LOG("drm: %ux%u on connector %u, crtc %u (%s), scanout %s", mode.hdisplay, mode.vdisplay,
        connector_id, crtc_id, card_path ? card_path : path_buf, format_name);
    return d;
}
