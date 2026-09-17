/* stp.h — ScreenTinker Linux Player: shared types
 *
 * One struct set shared by the display backends, the media decoder, the
 * text/widget rasterizer and the playback engine. Kept in one header on
 * purpose (per GeekDS conventions): this project favours a handful of fat
 * files you can read top to bottom over a maze of tiny ones.
 */
#ifndef STP_H
#define STP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/* ---------- logging ---------- */
void stp_log(const char *fmt, ...);
#define LOG(...) stp_log(__VA_ARGS__)

/* ---------- display abstraction (DRM/KMS or fbdev, chosen at runtime) --- */

typedef struct stp_display {
    int width, height;      /* pixels */
    int stride;              /* bytes per scanline of `pixels` */
    uint8_t *pixels;         /* mmap'd scanout memory, format = DRM_FORMAT_XRGB8888
                               * i.e. bytes in memory are B,G,R,X (little endian) —
                               * this is AV_PIX_FMT_BGRA in ffmpeg's naming, matching
                               * the pixel layout already used by fbplay_video.c. */
    const char *backend_name; /* "drm" or "fbdev", for logs */

    void (*present)(struct stp_display *d);              /* flush/no-op */
    void (*set_power)(struct stp_display *d, bool on);    /* DPMS/blank */
    void (*destroy)(struct stp_display *d);
    void *priv;
} stp_display_t;

/* Tries DRM first (iterates /dev/dri/card0..7 for a connected connector),
 * falls back to /dev/fb0. `want` may be "drm", "fbdev" or NULL (auto). */
stp_display_t *display_open(const char *want);

/* ---------- rectangle (a zone in device pixel space) -------------------- */
typedef struct { int x, y, w, h; } stp_rect_t;

/* ---------- playlist model, mirrors the server's assignment payload ----- */

typedef enum { ITEM_CONTENT, ITEM_WIDGET } stp_item_kind_t;

typedef struct {
    stp_item_kind_t kind;

    /* content items */
    char content_id[64];
    char filepath[512];      /* server-relative path under /uploads/content/ */
    char mime_type[128];
    char remote_url[1024];   /* set instead of filepath for remote-URL content */
    long content_rev;

    /* widget items */
    char widget_id[64];
    char widget_type[32];    /* clock | text | weather | rss | webpage | social | ... */
    char widget_config_json[2048];

    char zone_id[64];        /* "" => fullscreen */
    int sort_order;
    int duration_sec;
    char schedule_start[16], schedule_end[16]; /* "HH:MM" or "" */
    char schedule_days[16];  /* "0123456" subset or "" = every day */
    bool enabled;
} stp_assignment_t;

typedef struct {
    char id[64];
    char name[64];
    double x_pct, y_pct, w_pct, h_pct;
    int z_index;
    char fit_mode[16];        /* "contain" | "cover" | "fill" */
    char background_color[16];/* "#rrggbb" or "" */
} stp_zone_t;

typedef struct {
    char id[64];
    int width, height;        /* design-space size the % coords are relative to */
    stp_zone_t zones[16];
    int zone_count;
} stp_layout_t;

typedef struct {
    stp_assignment_t items[256];
    int item_count;
    stp_layout_t layout;      /* zone_count == 0 => fullscreen single-item playback */
    bool has_layout;
    char orientation[16];
    char background_color[16];
    bool suspended;
    char suspend_message[256];
} stp_playlist_t;

/* ---------- device identity, persisted under /etc/stlinux-player -------- */
typedef struct {
    char device_id[64];
    char device_token[128];
    char fingerprint[64];
    bool paired;
} stp_identity_t;

bool identity_load(stp_identity_t *out, const char *path);
bool identity_save(const stp_identity_t *id, const char *path);
void identity_ensure_fingerprint(stp_identity_t *id);

#endif /* STP_H */
