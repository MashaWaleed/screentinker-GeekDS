/* main.c — wires it all together.
 *
 *   stlinux-player --server https://signage.example.com \
 *                   [--display auto|drm|fbdev|/dev/dri/cardN|/dev/fbN] \
 *                   [--identity /etc/stlinux-player/device.json] \
 *                   [--cache /var/cache/stlinux-player]
 *
 * The websocket URL is derived from --server: https:// -> wss://, http:// ->
 * ws://, same host/port, connecting to /socket.io/ on the /device namespace
 * (see sioc.c). --server's own scheme+host is also what content URLs are
 * resolved against (/uploads/content/<filepath>), since uploads share the
 * dashboard origin per the real server's static mount.
 */
#include "stp.h"
#include "sioc.h"
#include "device.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void derive_ws_url(const char *server, char *out, size_t out_sz) {
    if (strncmp(server, "https://", 8) == 0) snprintf(out, out_sz, "wss://%s", server + 8);
    else if (strncmp(server, "http://", 7) == 0) snprintf(out, out_sz, "ws://%s", server + 7);
    else snprintf(out, out_sz, "ws://%s", server); /* bare host:port */
}

int main(int argc, char **argv) {
    const char *server = NULL;
    const char *display_want = NULL;
    const char *identity_path = "/etc/stlinux-player/device.json";
    const char *cache_dir = "/var/cache/stlinux-player";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server") && i + 1 < argc) server = argv[++i];
        else if (!strcmp(argv[i], "--display") && i + 1 < argc) display_want = argv[++i];
        else if (!strcmp(argv[i], "--identity") && i + 1 < argc) identity_path = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache_dir = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s --server https://your-screentinker-host "
                   "[--display auto|drm|fbdev|/dev/dri/cardN|/dev/fbN] "
                   "[--identity path] [--cache dir]\n", argv[0]);
            return 0;
        }
    }
    if (!server) {
        fprintf(stderr, "error: --server is required, e.g. --server https://signage.example.com\n");
        return 1;
    }
    if (display_want && !strcmp(display_want, "auto")) display_want = NULL;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    stp_display_t *disp = display_open(display_want);
    if (!disp) {
        fprintf(stderr, "error: no usable display (tried DRM/KMS and /dev/fb0)\n");
        return 1;
    }
    LOG("display: %s backend, %dx%d", disp->backend_name, disp->width, disp->height);

    device_ctx_t ctx;
    device_init(&ctx, identity_path, cache_dir, server, disp);

    char ws_url[600];
    derive_ws_url(server, ws_url, sizeof(ws_url));
    ctx.sioc = sioc_connect(ws_url, &ctx, device_on_event, device_on_state);
    if (!ctx.sioc) {
        fprintf(stderr, "error: could not start websocket client for %s\n", ws_url);
        disp->destroy(disp);
        return 1;
    }

    time_t last_heartbeat = time(NULL);
    const int heartbeat_interval_s = 30;
    const int frequent_interval_s = 5;

    while (!g_stop) {
        sioc_service(ctx.sioc, 200);

        time_t now = time(NULL);
        int interval = device_needs_frequent_tick(&ctx) ? frequent_interval_s : heartbeat_interval_s;
        if (sioc_is_connected(ctx.sioc) && now - last_heartbeat >= interval) {
            device_tick_registration_or_heartbeat(&ctx);
            last_heartbeat = now;
        }
    }

    LOG("shutting down");
    player_stop();
    sioc_close(ctx.sioc);
    disp->destroy(disp);
    return 0;
}
