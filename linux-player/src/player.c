/* player.c — the playback engine.
 *
 * One thread per zone (or one thread for a fullscreen/no-layout playlist),
 * each an infinite loop over that zone's items: resolve -> play for
 * duration_sec (or the clip's natural length) -> next, wrapping back to the
 * first item. schedule_start/end/days gates whether an item is eligible
 * *when its turn comes up*, same as the "windowed  assignment" idea in the
 * server's own scheduling — an item outside its window is skipped rather
 * than blocking the zone.
 *
 * A generation counter is how player_start() cancels an in-flight run: every
 * zone thread captures the generation it was born with and checks it (via
 * the `cancel` flag threaded through media_play_item and the widget loop)
 * far more often than any playback step takes, so a playlist update or
 * shutdown interrupts within a fraction of a second instead of waiting out
 * a still-playing clip.
 */
#include "player.h"
#include "media.h"
#include "textrender.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <cjson/cJSON.h>

typedef struct {
    stp_display_t *disp;
    stp_display_t *output;
    stp_rect_t rect;
    stp_rect_t screen_rect;
    stp_display_t surface;
    uint8_t *surface_pixels;
    pthread_mutex_t surface_lock;
    bool surface_lock_initialized;
    stp_zone_t zone;              /* zone.background_color etc; zone.id == "" => fullscreen */
    stp_assignment_t items[256];
    int count;
    char server_base_url[512];
    content_cache_t *cache;
    void *user;
    player_play_event_cb cb;
    volatile bool cancel;
    pthread_t thread;
    bool running;
} zone_ctx_t;

#define MAX_ZONES 16
static zone_ctx_t g_zones[MAX_ZONES];
static int g_zone_count = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_composite_lock = PTHREAD_MUTEX_INITIALIZER;
/* Layout zones are first combined here, then the finished rectangle is
 * copied to the actual scanout buffer.  This is essential on fbdev: writing
 * the large zone directly and restoring PIP afterwards lets the display scan
 * the temporary large-zone pixels and looks like overlap flicker. */
static uint8_t *g_composite_pixels = NULL;
static int g_composite_stride = 0;
static int64_t g_last_composite_present_us = 0;

#define COMPOSITE_PRESENT_INTERVAL_US 41667 /* 24 Hz: media is capped at 24 FPS */

static int64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void zone_lock_pixels(stp_display_t *surface) {
    zone_ctx_t *zc = surface->priv;
    pthread_mutex_lock(&zc->surface_lock);
}

static void zone_unlock_pixels(stp_display_t *surface) {
    zone_ctx_t *zc = surface->priv;
    pthread_mutex_unlock(&zc->surface_lock);
}

static size_t zone_area(const zone_ctx_t *zc) {
    return (size_t)zc->screen_rect.w * zc->screen_rect.h;
}

static void composite_zone(zone_ctx_t *zc) {
    uint8_t *canvas = g_composite_pixels ? g_composite_pixels : zc->output->pixels;
    int canvas_stride = g_composite_pixels ? g_composite_stride : zc->output->stride;
    pthread_mutex_lock(&zc->surface_lock);
    for (int y = 0; y < zc->screen_rect.h; y++) {
        uint8_t *dst = canvas + (size_t)(zc->screen_rect.y + y) * canvas_stride
                       + (size_t)zc->screen_rect.x * 4;
        const uint8_t *src = zc->surface_pixels + (size_t)y * zc->surface.stride;
        memcpy(dst, src, (size_t)zc->screen_rect.w * 4);
    }
    pthread_mutex_unlock(&zc->surface_lock);
}

static void union_rect(stp_rect_t *out, bool *valid, stp_rect_t add) {
    if (!*valid) { *out = add; *valid = true; return; }
    int right = out->x + out->w, bottom = out->y + out->h;
    if (add.x < out->x) out->x = add.x;
    if (add.y < out->y) out->y = add.y;
    if (add.x + add.w > right) right = add.x + add.w;
    if (add.y + add.h > bottom) bottom = add.y + add.h;
    out->w = right - out->x;
    out->h = bottom - out->y;
}

static void copy_composite_rect(stp_display_t *output, stp_rect_t rect) {
    for (int y = 0; y < rect.h; y++) {
        uint8_t *dst = output->pixels + (size_t)(rect.y + y) * output->stride + (size_t)rect.x * 4;
        const uint8_t *src = g_composite_pixels + (size_t)(rect.y + y) * g_composite_stride +
                             (size_t)rect.x * 4;
        memcpy(dst, src, (size_t)rect.w * 4);
    }
    if (output->mark_dirty)
        output->mark_dirty(output, rect.x, rect.y, rect.w, rect.h);
}

/* A zone decodes into its own surface.  When a larger zone changes, redraw
 * every smaller layer over it, so Picture-in-Picture media remains visible
 * rather than flickering as independent decoders race for the framebuffer. */
static void zone_surface_present(stp_display_t *surface) {
    zone_ctx_t *zc = surface->priv;
    pthread_mutex_lock(&g_composite_lock);
    stp_rect_t changed = {0};
    bool have_changed = false;
    composite_zone(zc);
    union_rect(&changed, &have_changed, zc->screen_rect);
    size_t area = zone_area(zc);
    for (int i = 0; i < g_zone_count; i++) {
        zone_ctx_t *other = &g_zones[i];
        if (other == zc || !other->surface_pixels) continue;
        size_t other_area = zone_area(other);
        if (other_area < area || (other_area == area && other->zone.z_index > zc->zone.z_index)) {
            composite_zone(other);
            union_rect(&changed, &have_changed, other->screen_rect);
        }
    }
    if (have_changed) {
        if (g_composite_pixels) copy_composite_rect(zc->output, changed);
        else if (zc->output->mark_dirty)
            zc->output->mark_dirty(zc->output, changed.x, changed.y, changed.w, changed.h);
    }
    /* Multiple zone threads can finish a frame at the same instant.  Queueing
     * a DRM page flip for each one only serializes on vblank and makes both
     * videos stutter.  The back buffer retains all updates, so present at a
     * single display cadence instead. */
    int64_t now = monotonic_us();
    if (!g_last_composite_present_us ||
        now - g_last_composite_present_us >= COMPOSITE_PRESENT_INTERVAL_US) {
        zc->output->present(zc->output);
        g_last_composite_present_us = monotonic_us();
    }
    pthread_mutex_unlock(&g_composite_lock);
}

static bool item_in_schedule_window(const stp_assignment_t *it) {
    if (!it->schedule_start[0] && !it->schedule_end[0] && !it->schedule_days[0]) return true;
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);

    if (it->schedule_days[0]) {
        char wd = '0' + tmv.tm_wday;
        if (!strchr(it->schedule_days, wd)) return false;
    }
    if (it->schedule_start[0] && it->schedule_end[0]) {
        int nowmin = tmv.tm_hour * 60 + tmv.tm_min;
        int sh, sm, eh, em;
        if (sscanf(it->schedule_start, "%d:%d", &sh, &sm) == 2 &&
            sscanf(it->schedule_end, "%d:%d", &eh, &em) == 2) {
            int start = sh * 60 + sm, end = eh * 60 + em;
            if (start <= end) { if (nowmin < start || nowmin > end) return false; }
            else { if (nowmin < start && nowmin > end) return false; } /* wraps past midnight */
        }
    }
    return true;
}

static void zone_begin_draw(zone_ctx_t *zc) {
    if (zc->disp->lock_pixels) zc->disp->lock_pixels(zc->disp);
}

static void zone_end_draw(zone_ctx_t *zc) {
    if (zc->disp->unlock_pixels) zc->disp->unlock_pixels(zc->disp);
}

static void play_widget(zone_ctx_t *zc, const stp_assignment_t *it) {
    uint32_t bg = media_parse_color(zc->zone.background_color[0] ? zc->zone.background_color : NULL, 0xFF000000);
    uint32_t fg = 0xFFFFFFFF;
    zone_begin_draw(zc);
    rect_fill(zc->disp, zc->rect, bg);
    zone_end_draw(zc);

    int hold = it->duration_sec > 0 ? it->duration_sec : 10;

    if (strcmp(it->widget_type, "clock") == 0) {
        time_t deadline = time(NULL) + hold;
        while (!zc->cancel && time(NULL) < deadline) {
            time_t now = time(NULL);
            struct tm tmv; localtime_r(&now, &tmv);
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            int scale = (zc->rect.h / 8) > 1 ? (zc->rect.h / 8) / 6 + 1 : 1;
            if (scale > 12) scale = 12;
            zone_begin_draw(zc);
            text_draw_centered(zc->disp, zc->rect, buf, scale, fg, bg, true);
            zone_end_draw(zc);
            zc->disp->present(zc->disp);
            usleep(200000);
        }
        return;
    }

    if (strcmp(it->widget_type, "text") == 0) {
        const char *content = "TEXT";
        cJSON *cfg = cJSON_Parse(it->widget_config_json);
        if (cfg) {
            cJSON *c = cJSON_GetObjectItem(cfg, "content");
            if (c && cJSON_IsString(c)) content = c->valuestring;
        }
        int scale = 3;
        zone_begin_draw(zc);
        text_draw_centered(zc->disp, zc->rect, content, scale, fg, bg, true);
        zone_end_draw(zc);
        zc->disp->present(zc->disp);
        if (cfg) cJSON_Delete(cfg);
        time_t deadline = time(NULL) + hold;
        while (!zc->cancel && time(NULL) < deadline) usleep(200000);
        return;
    }

    /* weather / rss / webpage / social / directory-board / directory-search /
     * diag-smoothness: these are server-rendered HTML in every other
     * ScreenTinker client (see server/lib/plugins renderWidgetHtml()) and
     * genuinely need a browser engine to render faithfully — out of scope
     * for a browserless ffmpeg/DRM client. Show a short, honest placeholder
     * and move on rather than stalling the zone. A real deployment wanting
     * these should look at embedding WPE WebKit (no X11/Wayland required)
     * to render the same widget HTML the server already produces, and
     * blit its output buffer into this zone instead. */
    char msg[64];
    snprintf(msg, sizeof(msg), "%s WIDGET N/A", it->widget_type);
    zone_begin_draw(zc);
    text_draw_centered(zc->disp, zc->rect, msg, 2, fg, bg, true);
    zone_end_draw(zc);
    zc->disp->present(zc->disp);
    time_t deadline = time(NULL) + (hold < 3 ? hold : 3);
    while (!zc->cancel && time(NULL) < deadline) usleep(100000);
}

static void *zone_thread(void *arg) {
    zone_ctx_t *zc = arg;
    if (zc->count == 0) {
        uint32_t bg = media_parse_color(zc->zone.background_color[0] ? zc->zone.background_color : NULL, 0xFF000000);
        zone_begin_draw(zc);
        rect_fill(zc->disp, zc->rect, bg);
        zone_end_draw(zc);
        zc->disp->present(zc->disp);
        while (!zc->cancel) usleep(200000);
        return NULL;
    }

    int i = 0;
    while (!zc->cancel) {
        stp_assignment_t *it = &zc->items[i];
        i = (i + 1) % zc->count;
        if (!item_in_schedule_window(it)) continue;

        if (it->kind == ITEM_WIDGET) {
            if (zc->cb) zc->cb(zc->user, it->widget_id, "play_start");
            play_widget(zc, it);
            if (zc->cb) zc->cb(zc->user, it->widget_id, "play_end");
            continue;
        }

        const char *path = cache_resolve(zc->cache, zc->server_base_url, it);
        if (!path) {
            LOG("player: could not resolve content %s (%s), skipping", it->content_id, it->filepath);
            continue;
        }
        uint32_t bg = media_parse_color(zc->zone.background_color[0] ? zc->zone.background_color : NULL, 0xFF000000);
        if (zc->cb) zc->cb(zc->user, it->content_id, "play_start");
        media_play_item(zc->disp, zc->rect, path, zc->zone.fit_mode[0] ? zc->zone.fit_mode : "contain",
                         bg, it->duration_sec, it->muted, &zc->cancel);
        if (zc->cb) zc->cb(zc->user, it->content_id, "play_end");
    }
    return NULL;
}

static stp_rect_t zone_pixel_rect(const stp_layout_t *layout, const stp_zone_t *z, int disp_w, int disp_h) {
    (void)layout; /* percentages are already 0..100 relative to the design canvas == the screen */
    stp_rect_t r;
    r.x = (int)(z->x_pct / 100.0 * disp_w);
    r.y = (int)(z->y_pct / 100.0 * disp_h);
    r.w = (int)(z->w_pct / 100.0 * disp_w);
    r.h = (int)(z->h_pct / 100.0 * disp_h);
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x + r.w > disp_w) r.w = disp_w - r.x;
    if (r.y + r.h > disp_h) r.h = disp_h - r.y;
    return r;
}

void player_stop(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_zone_count; i++) {
        if (g_zones[i].running) {
            g_zones[i].cancel = true;
            pthread_join(g_zones[i].thread, NULL);
            g_zones[i].running = false;
        }
        free(g_zones[i].surface_pixels);
        g_zones[i].surface_pixels = NULL;
        if (g_zones[i].surface_lock_initialized) {
            pthread_mutex_destroy(&g_zones[i].surface_lock);
            g_zones[i].surface_lock_initialized = false;
        }
    }
    g_zone_count = 0;
    free(g_composite_pixels);
    g_composite_pixels = NULL;
    g_composite_stride = 0;
    pthread_mutex_unlock(&g_lock);
}

void player_start(stp_display_t *disp, const stp_playlist_t *pl, const char *server_base_url,
                   content_cache_t *cache, void *user, player_play_event_cb cb) {
    player_stop();
    pthread_mutex_lock(&g_lock);
    g_last_composite_present_us = 0;

    if (disp->set_composited) disp->set_composited(disp, pl->has_layout);

    uint32_t bg = media_parse_color(pl->background_color[0] ? pl->background_color : NULL, 0xFF000000);
    rect_fill(disp, (stp_rect_t){0, 0, disp->width, disp->height}, bg);
    if (disp->mark_dirty)
        disp->mark_dirty(disp, 0, 0, disp->width, disp->height);
    disp->present(disp);

    if (pl->has_layout) {
        g_composite_stride = disp->stride;
        g_composite_pixels = malloc((size_t)g_composite_stride * disp->height);
        if (!g_composite_pixels) {
            LOG("player: cannot allocate composition buffer");
            g_composite_stride = 0;
        } else {
            /* Start from the exact displayed background, including driver
             * pitch padding, so untouched parts of the canvas stay valid. */
            memcpy(g_composite_pixels, disp->pixels,
                   (size_t)g_composite_stride * disp->height);
        }
        g_zone_count = pl->layout.zone_count < MAX_ZONES ? pl->layout.zone_count : MAX_ZONES;
        for (int zi = 0; zi < g_zone_count; zi++) {
            zone_ctx_t *zc = &g_zones[zi];
            memset(zc, 0, sizeof(*zc));
            zc->output = disp;
            zc->zone = pl->layout.zones[zi];
            zc->screen_rect = zone_pixel_rect(&pl->layout, &zc->zone, disp->width, disp->height);
            zc->rect = (stp_rect_t){0, 0, zc->screen_rect.w, zc->screen_rect.h};
            zc->surface_pixels = calloc((size_t)zc->rect.w * zc->rect.h, 4);
            if (!zc->surface_pixels) {
                LOG("player: cannot allocate surface for zone %s", zc->zone.id);
                continue;
            }
            pthread_mutex_init(&zc->surface_lock, NULL);
            zc->surface_lock_initialized = true;
            zc->surface.width = zc->rect.w;
            zc->surface.height = zc->rect.h;
            zc->surface.stride = zc->rect.w * 4;
            zc->surface.pixels = zc->surface_pixels;
            zc->surface.pixel_layout = disp->pixel_layout;
            zc->surface.backend_name = "zone-surface";
            zc->surface.present = zone_surface_present;
            zc->surface.lock_pixels = zone_lock_pixels;
            zc->surface.unlock_pixels = zone_unlock_pixels;
            zc->surface.priv = zc;
            zc->disp = &zc->surface;
            snprintf(zc->server_base_url, sizeof(zc->server_base_url), "%s", server_base_url);
            zc->cache = cache; zc->user = user; zc->cb = cb;
            for (int i = 0; i < pl->item_count; i++)
                if (strcmp(pl->items[i].zone_id, zc->zone.id) == 0 && zc->count < 256)
                    zc->items[zc->count++] = pl->items[i];
            uint32_t zone_bg = media_parse_color(zc->zone.background_color[0] ? zc->zone.background_color : NULL,
                                                 0xFF000000);
            rect_fill(zc->disp, zc->rect, zone_bg);
        }
        for (int zi = 0; zi < g_zone_count; zi++) {
            zone_ctx_t *zc = &g_zones[zi];
            if (!zc->surface_pixels) continue;
            zc->running = true;
            pthread_create(&zc->thread, NULL, zone_thread, zc);
        }
    } else {
        zone_ctx_t *zc = &g_zones[0];
        memset(zc, 0, sizeof(*zc));
        zc->disp = disp;
        zc->output = disp;
        zc->zone.fit_mode[0] = 0;
        snprintf(zc->zone.fit_mode, sizeof(zc->zone.fit_mode), "contain");
        snprintf(zc->zone.background_color, sizeof(zc->zone.background_color), "%s", pl->background_color);
        zc->rect = (stp_rect_t){0, 0, disp->width, disp->height};
        zc->screen_rect = zc->rect;
        snprintf(zc->server_base_url, sizeof(zc->server_base_url), "%s", server_base_url);
        zc->cache = cache; zc->user = user; zc->cb = cb;
        for (int i = 0; i < pl->item_count && zc->count < 256; i++)
            if (pl->items[i].zone_id[0] == 0) zc->items[zc->count++] = pl->items[i];
        zc->running = true;
        pthread_create(&zc->thread, NULL, zone_thread, zc);
        g_zone_count = 1;
    }

    pthread_mutex_unlock(&g_lock);
}

void player_show_suspended(stp_display_t *disp, const char *message) {
    player_stop();
    rect_fill(disp, (stp_rect_t){0, 0, disp->width, disp->height}, 0xFF101010);
    text_draw_centered(disp, (stp_rect_t){0, 0, disp->width, disp->height},
                        message && message[0] ? message : "DEVICE SUSPENDED", 3, 0xFFFFFFFF, 0xFF101010, false);
    if (disp->mark_dirty)
        disp->mark_dirty(disp, 0, 0, disp->width, disp->height);
    disp->present(disp);
}
