#ifndef DEVICE_H
#define DEVICE_H

#include "stp.h"
#include "sioc.h"
#include <cjson/cJSON.h>

typedef struct {
    stp_identity_t identity;
    char identity_path[256];
    char cache_dir[256];
    char server_base_url[512]; /* http(s)://host[:port], derived from the ws url */
    stp_display_t *disp;
    sioc_t *sioc;
    void *cache; /* actually content_cache_t*; kept opaque here so callers of just device.h don't need cache.h */
    bool registered_this_session; /* has this specific socket connection completed device:register yet? */
} device_ctx_t;

/* One-time setup: loads/creates identity, ensures a fingerprint. */
void device_init(device_ctx_t *ctx, const char *identity_path, const char *cache_dir,
                  const char *server_base_url, stp_display_t *disp);

/* sioc_event_cb-shaped: pass device_ctx_t* as `user` to sioc_connect(). */
void device_on_event(void *user, const char *event, cJSON *data);
void device_on_state(void *user, bool connected);

/* Called once per connect and then periodically (e.g. every 30s once fully
 * registered, more often until then) from the main loop. Every new socket
 * connection — including a reconnect of an already-paired device — MUST
 * send device:register before anything else; the server rejects
 * device:heartbeat on a socket that hasn't done that yet with
 * device:auth-error. This function enforces that ordering internally:
 * unpaired -> device:register with a pairing_code; paired-but-not-yet-
 * registered-on-this-socket -> device:register with device_id/token;
 * otherwise -> device:heartbeat. */
void device_tick_registration_or_heartbeat(device_ctx_t *ctx);

/* True while the main loop should poll device_tick_registration_or_heartbeat
 * frequently (unpaired, or this socket hasn't completed device:register
 * yet) rather than on the normal ~30s heartbeat cadence. */
bool device_needs_frequent_tick(device_ctx_t *ctx);

#endif
