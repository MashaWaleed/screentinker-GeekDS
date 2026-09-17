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
    stp_rect_t rect;
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

static void play_widget(zone_ctx_t *zc, const stp_assignment_t *it) {
    uint32_t bg = media_parse_color(zc->zone.background_color[0] ? zc->zone.background_color : NULL, 0xFF000000);
    uint32_t fg = 0xFFFFFFFF;
    rect_fill(zc->disp, zc->rect, bg);

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
            text_draw_centered(zc->disp, zc->rect, buf, scale, fg, bg, true);
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
        text_draw_centered(zc->disp, zc->rect, content, scale, fg, bg, true);
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
    text_draw_centered(zc->disp, zc->rect, msg, 2, fg, bg, true);
    zc->disp->present(zc->disp);
    time_t deadline = time(NULL) + (hold < 3 ? hold : 3);
    while (!zc->cancel && time(NULL) < deadline) usleep(100000);
}

static void *zone_thread(void *arg) {
    zone_ctx_t *zc = arg;
    if (zc->count == 0) {
        uint32_t bg = media_parse_color(zc->zone.background_color[0] ? zc->zone.background_color : NULL, 0xFF000000);
        rect_fill(zc->disp, zc->rect, bg);
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
                         bg, it->duration_sec, &zc->cancel);
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
    }
    g_zone_count = 0;
    pthread_mutex_unlock(&g_lock);
}

void player_start(stp_display_t *disp, const stp_playlist_t *pl, const char *server_base_url,
                   content_cache_t *cache, void *user, player_play_event_cb cb) {
    player_stop();
    pthread_mutex_lock(&g_lock);

    uint32_t bg = media_parse_color(pl->background_color[0] ? pl->background_color : NULL, 0xFF000000);
    rect_fill(disp, (stp_rect_t){0, 0, disp->width, disp->height}, bg);
    disp->present(disp);

    if (pl->has_layout) {
        for (int zi = 0; zi < pl->layout.zone_count && zi < MAX_ZONES; zi++) {
            zone_ctx_t *zc = &g_zones[zi];
            memset(zc, 0, sizeof(*zc));
            zc->disp = disp;
            zc->zone = pl->layout.zones[zi];
            zc->rect = zone_pixel_rect(&pl->layout, &zc->zone, disp->width, disp->height);
            snprintf(zc->server_base_url, sizeof(zc->server_base_url), "%s", server_base_url);
            zc->cache = cache; zc->user = user; zc->cb = cb;
            for (int i = 0; i < pl->item_count; i++)
                if (strcmp(pl->items[i].zone_id, zc->zone.id) == 0 && zc->count < 256)
                    zc->items[zc->count++] = pl->items[i];
            zc->running = true;
            pthread_create(&zc->thread, NULL, zone_thread, zc);
        }
        g_zone_count = pl->layout.zone_count < MAX_ZONES ? pl->layout.zone_count : MAX_ZONES;
    } else {
        zone_ctx_t *zc = &g_zones[0];
        memset(zc, 0, sizeof(*zc));
        zc->disp = disp;
        zc->zone.fit_mode[0] = 0;
        snprintf(zc->zone.fit_mode, sizeof(zc->zone.fit_mode), "contain");
        snprintf(zc->zone.background_color, sizeof(zc->zone.background_color), "%s", pl->background_color);
        zc->rect = (stp_rect_t){0, 0, disp->width, disp->height};
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
    disp->present(disp);
}
