/* device.c — everything about being *this* device: identity, pairing,
 * telemetry, and reacting to server-pushed commands and playlists.
 *
 * Event names (device:register, device:registered, device:heartbeat,
 * device:playlist-update, device:command, device:paired, device:play-event)
 * and the device:register payload shape (pairing_code on first boot,
 * device_id+device_token to resume) come straight out of the real
 * server/ws/deviceSocket.js, not guessed.
 */
#include "device.h"
#include "cache.h"
#include "player.h"
#include "playlist_parse.h"
#include "textrender.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>

static content_cache_t g_cache_storage;

void device_init(device_ctx_t *ctx, const char *identity_path, const char *cache_dir,
                  const char *server_base_url, stp_display_t *disp) {
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->identity_path, sizeof(ctx->identity_path), "%s", identity_path);
    snprintf(ctx->cache_dir, sizeof(ctx->cache_dir), "%s", cache_dir);
    snprintf(ctx->server_base_url, sizeof(ctx->server_base_url), "%s", server_base_url);
    ctx->disp = disp;
    ctx->screenshot = screenshot_capture_start(disp);
    if (!ctx->screenshot) LOG("screenshot: worker unavailable");

    identity_load(&ctx->identity, identity_path); /* ok if it doesn't exist yet */
    identity_ensure_fingerprint(&ctx->identity);
    identity_save(&ctx->identity, identity_path);

    cache_init(&g_cache_storage, cache_dir);
    ctx->cache = &g_cache_storage;
}

static void show_pairing_code(stp_display_t *disp, const char *code, const char *server_base_url) {
    rect_fill(disp, (stp_rect_t){0, 0, disp->width, disp->height}, 0xFF141414);
    stp_rect_t top = {0, disp->height / 3, disp->width, disp->height / 3};
    text_draw_centered(disp, top, code, disp->height / 220 + 4, 0xFFFFFFFF, 0xFF141414, false);
    char hint[300];
    snprintf(hint, sizeof(hint), "ENTER THIS CODE AT %s", server_base_url);
    stp_rect_t bottom = {0, disp->height / 3 * 2, disp->width, disp->height / 6};
    text_draw_centered(disp, bottom, hint, 1, 0xFFAAAAAA, 0xFF141414, false);
    disp->present(disp);
}

static void gen_pairing_code(char *out, size_t out_sz) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    snprintf(out, out_sz, "%06d", rand() % 1000000);
}

static char *build_device_info(void) {
    struct utsname u; uname(&u);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "player", "stlinux-player");
    cJSON_AddStringToObject(o, "player_version", "0.1.0");
    cJSON_AddStringToObject(o, "os", u.sysname);
    cJSON_AddStringToObject(o, "os_release", u.release);
    cJSON_AddStringToObject(o, "arch", u.machine);
    cJSON_AddStringToObject(o, "hostname", u.nodename);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static void add_capabilities(cJSON *payload) {
    cJSON *caps = cJSON_AddArrayToObject(payload, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("playback.video"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("playback.image"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("playback.zones"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("playback.pip"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("audio.mute"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("audio.volume"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("display.power"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("offline.cache"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("remote.screenshot"));
}

static cJSON *collect_telemetry(device_ctx_t *ctx) {
    cJSON *o = cJSON_CreateObject();

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        cJSON_AddNumberToObject(o, "uptime_sec", si.uptime);
        double mem_used_pct = si.totalram > 0 ?
            100.0 * (double)(si.totalram - si.freeram) / (double)si.totalram : 0;
        cJSON_AddNumberToObject(o, "ram_used_pct", mem_used_pct);
    }

    struct statvfs sv;
    if (statvfs("/", &sv) == 0) {
        double used_pct = sv.f_blocks > 0 ?
            100.0 * (double)(sv.f_blocks - sv.f_bavail) / (double)sv.f_blocks : 0;
        cJSON_AddNumberToObject(o, "storage_used_pct", used_pct);
    }

    /* /proc/loadavg col 1 as a cheap CPU-pressure proxy — avoids parsing
     * /proc/stat deltas for a number that's only ever shown as a rough
     * dashboard gauge. */
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        double load1 = 0;
        if (fscanf(f, "%lf", &load1) == 1) cJSON_AddNumberToObject(o, "load1", load1);
        fclose(f);
    }

    cJSON_AddStringToObject(o, "display_backend", ctx->disp->backend_name);
    cJSON_AddNumberToObject(o, "display_width", ctx->disp->width);
    cJSON_AddNumberToObject(o, "display_height", ctx->disp->height);
    return o;
}

void device_tick_registration_or_heartbeat(device_ctx_t *ctx) {
    if (!ctx->sioc || !sioc_is_connected(ctx->sioc)) return;

    if (!ctx->identity.paired) {
        static char pairing_code[8] = {0};
        if (!pairing_code[0]) {
            gen_pairing_code(pairing_code, sizeof(pairing_code));
            show_pairing_code(ctx->disp, pairing_code, ctx->server_base_url);
        }
        char *info_json = build_device_info();
        cJSON *info = cJSON_Parse(info_json);
        free(info_json);
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "pairing_code", pairing_code);
        cJSON_AddStringToObject(payload, "fingerprint", ctx->identity.fingerprint);
        cJSON_AddItemToObject(payload, "device_info", info);
        add_capabilities(payload);
        sioc_emit(ctx->sioc, "device:register", payload);
        cJSON_Delete(payload);
        return;
    }

    if (!ctx->registered_this_session) {
        /* Resuming a previously-paired identity on a brand new socket: the
         * server requires device:register as the first message on *every*
         * connection, paired or not — it's per-socket auth, not just a
         * one-time provisioning step. Skipping straight to
         * device:heartbeat here is exactly what produces
         * "Not authenticated. Send device:register first." */
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "device_id", ctx->identity.device_id);
        cJSON_AddStringToObject(payload, "device_token", ctx->identity.device_token);
        cJSON_AddStringToObject(payload, "fingerprint", ctx->identity.fingerprint);
        add_capabilities(payload);
        sioc_emit(ctx->sioc, "device:register", payload);
        cJSON_Delete(payload);
        return;
    }

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "device_id", ctx->identity.device_id);
    cJSON_AddStringToObject(payload, "device_token", ctx->identity.device_token);
    cJSON_AddStringToObject(payload, "fingerprint", ctx->identity.fingerprint);
    cJSON *tel = collect_telemetry(ctx);
    cJSON_AddItemToObject(payload, "telemetry", tel);
    sioc_emit(ctx->sioc, "device:heartbeat", payload);
    cJSON_Delete(payload);
}

bool device_needs_frequent_tick(device_ctx_t *ctx) {
    return !ctx->identity.paired || !ctx->registered_this_session;
}

void device_service(device_ctx_t *ctx) {
    char *image_b64 = screenshot_capture_take_result(ctx->screenshot);
    if (!image_b64) return;
    if (ctx->identity.paired && ctx->registered_this_session && sioc_is_connected(ctx->sioc)) {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "device_id", ctx->identity.device_id);
        cJSON_AddStringToObject(payload, "image_b64", image_b64);
        sioc_emit(ctx->sioc, "device:screenshot", payload);
        cJSON_Delete(payload);
    }
    free(image_b64);
}

void device_shutdown(device_ctx_t *ctx) {
    screenshot_capture_stop(ctx->screenshot);
    ctx->screenshot = NULL;
}

static void handle_registered(device_ctx_t *ctx, cJSON *data) {
    cJSON *id = cJSON_GetObjectItem(data, "device_id");
    cJSON *tok = cJSON_GetObjectItem(data, "device_token");
    if (id && cJSON_IsString(id)) snprintf(ctx->identity.device_id, sizeof(ctx->identity.device_id), "%s", id->valuestring);
    if (tok && cJSON_IsString(tok)) snprintf(ctx->identity.device_token, sizeof(ctx->identity.device_token), "%s", tok->valuestring);
    ctx->identity.paired = ctx->identity.device_id[0] && ctx->identity.device_token[0];
    ctx->registered_this_session = ctx->identity.paired;
    identity_save(&ctx->identity, ctx->identity_path);
    LOG("device: registered as %s (paired=%d)", ctx->identity.device_id, ctx->identity.paired);
    if (ctx->identity.paired) {
        rect_fill(ctx->disp, (stp_rect_t){0, 0, ctx->disp->width, ctx->disp->height}, 0xFF000000);
        ctx->disp->present(ctx->disp);
    }
}

static void handle_playlist_update(device_ctx_t *ctx, cJSON *data) {
    stp_playlist_t pl;
    playlist_parse(data, &pl);

    if (pl.suspended) {
        LOG("device: suspended — %s", pl.suspend_message);
        player_show_suspended(ctx->disp, pl.suspend_message);
        return;
    }

    LOG("device: playlist update, %d item(s), %s", pl.item_count,
        pl.has_layout ? "multi-zone layout" : "fullscreen");

    char keep_ids[4096] = " ";
    for (int i = 0; i < pl.item_count; i++) {
        if (pl.items[i].kind == ITEM_CONTENT && pl.items[i].content_id[0]) {
            strncat(keep_ids, pl.items[i].content_id, sizeof(keep_ids) - strlen(keep_ids) - 2);
            strncat(keep_ids, " ", sizeof(keep_ids) - strlen(keep_ids) - 1);
        }
    }
    cache_prune((content_cache_t *)ctx->cache, keep_ids);

    player_start(ctx->disp, &pl, ctx->server_base_url, (content_cache_t *)ctx->cache, ctx, NULL);
}

static void handle_command(device_ctx_t *ctx, cJSON *data) {
    cJSON *t = cJSON_GetObjectItem(data, "type");
    if (!t || !cJSON_IsString(t)) return;
    const char *type = t->valuestring;
    LOG("device: command '%s'", type);

    if (strcmp(type, "reboot") == 0) { sync(); if (system("reboot") != 0) LOG("device: reboot command failed"); }
    else if (strcmp(type, "shutdown") == 0) { sync(); if (system("poweroff") != 0) LOG("device: poweroff command failed"); }
    else if (strcmp(type, "screen_off") == 0) { ctx->disp->set_power(ctx->disp, false); }
    else if (strcmp(type, "screen_on") == 0) { ctx->disp->set_power(ctx->disp, true); }
    else if (strcmp(type, "set_volume") == 0) {
        cJSON *v = cJSON_GetObjectItem(data, "volume");
        if (v && cJSON_IsNumber(v)) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "amixer -q sset Master %d%% >/dev/null 2>&1", (int)v->valuedouble);
            if (system(cmd) != 0) LOG("device: set_volume command failed");
        }
    } else if (strcmp(type, "update") == 0) {
        if (access("/opt/stlinux-player/update.sh", X_OK) == 0) { if (system("/opt/stlinux-player/update.sh &") != 0) LOG("device: update script launch failed"); }
        else LOG("device: update command received but /opt/stlinux-player/update.sh not found");
    } else if (strcmp(type, "refresh") == 0) {
        /* Browser/Android players reload their page; we have no page to
         * reload, so just re-paint the current frame's background as a
         * visible acknowledgement and let the zone threads keep going. */
        LOG("device: refresh acknowledged (no-op beyond this log — playback isn't paused)");
    } else {
        LOG("device: unhandled command type '%s'", type);
    }
}

void device_on_state(void *user, bool connected) {
    device_ctx_t *ctx = user;
    if (connected) {
        ctx->registered_this_session = false; /* fresh socket: must device:register again */
        device_tick_registration_or_heartbeat(ctx);
    } else {
        LOG("device: link down, will keep retrying");
    }
}

static void handle_auth_error(device_ctx_t *ctx, cJSON *data) {
    char *dump = data ? cJSON_PrintUnformatted(data) : NULL;
    LOG("device: auth-error from server: %s", dump ? dump : "(no detail)");
    free(dump);
    /* Should be unreachable now that device_tick_registration_or_heartbeat
     * always sends device:register before any device:heartbeat on a fresh
     * socket — kept as a safety net. Just re-arm registered_this_session so
     * the next tick re-sends device:register instead of repeating the same
     * error against device:heartbeat. */
    ctx->registered_this_session = false;
}

void device_on_event(void *user, const char *event, cJSON *data) {
    device_ctx_t *ctx = user;
    if (strcmp(event, "device:registered") == 0) handle_registered(ctx, data);
    else if (strcmp(event, "device:playlist-update") == 0) handle_playlist_update(ctx, data);
    else if (strcmp(event, "device:command") == 0) handle_command(ctx, data);
    else if (strcmp(event, "device:screenshot-request") == 0) {
        LOG("screenshot: request received");
        screenshot_capture_request(ctx->screenshot);
    }
    else if (strcmp(event, "device:paired") == 0) LOG("device: paired notification received");
    else if (strcmp(event, "device:auth-error") == 0) handle_auth_error(ctx, data);
    else if (strcmp(event, "device:heartbeat-ack") == 0) { /* expected, nothing to do */ }
    else LOG("device: unhandled event '%s'", event);
}
