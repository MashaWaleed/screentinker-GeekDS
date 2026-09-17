/* sioc.h — minimal Socket.IO v4 client.
 *
 * ScreenTinker's device protocol is plain Socket.IO 4.x (server/package.json:
 * "socket.io": "^4.7.2") on the "/device" namespace, connected directly over
 * websocket (?EIO=4&transport=websocket) — no long-polling handshake, which
 * is what every real Socket.IO client library also does once it decides to
 * skip the polling probe. That keeps the protocol to two thin framing
 * layers on top of a plain ws connection, cheap enough to hand-roll instead
 * of dragging in a heavyweight client:
 *
 *   Engine.IO packet: <digit><payload>          0=open 2=ping 3=pong 4=message
 *   Socket.IO packet (carried inside a '4' Engine.IO message):
 *       <digit>[/namespace,]<json>              0=connect 2=event 4=connect_error
 *
 * So a device-side event send looks like:  42/device,["device:register",{...}]
 * and an incoming event looks the same coming back.
 */
#ifndef SIOC_H
#define SIOC_H

#include <cjson/cJSON.h>
#include <stdbool.h>

typedef struct sioc sioc_t;

typedef void (*sioc_event_cb)(void *user, const char *event, cJSON *args_array);
typedef void (*sioc_state_cb)(void *user, bool connected);

/* url: e.g. "wss://signage.example.com" or "ws://192.168.1.10:3001" (no path). */
sioc_t *sioc_connect(const char *url, void *user, sioc_event_cb on_event, sioc_state_cb on_state);

/* emit(["device:heartbeat", {...}]) style — pass a cJSON array you built,
 * ownership stays with the caller (it's stringified and not stored). */
void sioc_emit(sioc_t *s, const char *event, cJSON *payload_or_null);

/* Pump network I/O; call from the main loop with a small timeout (ms).
 * Returns immediately if there's nothing to do. */
void sioc_service(sioc_t *s, int timeout_ms);

bool sioc_is_connected(sioc_t *s);
void sioc_close(sioc_t *s);

#endif
