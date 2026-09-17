#include "sioc.h"
#include "stp.h"
#include <libwebsockets.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define OUTQ_MAX 64

typedef struct outmsg { char *data; size_t len; struct outmsg *next; } outmsg_t;

struct sioc {
    struct lws_context *ctx;
    struct lws *wsi;
    char host[256];
    int port;
    char path[256];
    bool use_ssl;

    void *user;
    sioc_event_cb on_event;
    sioc_state_cb on_state;

    bool ns_connected;   /* got Socket.IO CONNECT ack for /device */
    bool want_stop;

    pthread_mutex_t qlock;
    outmsg_t *qhead, *qtail;

    char *rxbuf;   /* accumulates a possibly-fragmented ws message */
    size_t rxlen, rxcap;

    /* reconnect backoff */
    const char *url_copy;
    time_t next_retry_at;
    int backoff_s;
};

static void enqueue(sioc_t *s, const char *engineio_type, const char *body /* may be NULL */) {
    size_t blen = body ? strlen(body) : 0;
    size_t total = 1 + blen;
    outmsg_t *m = calloc(1, sizeof(*m));
    m->data = malloc(LWS_PRE + total);
    m->len = total;
    m->data[LWS_PRE] = engineio_type[0];
    if (body) memcpy(m->data + LWS_PRE + 1, body, blen);

    pthread_mutex_lock(&s->qlock);
    if (s->qtail) { s->qtail->next = m; s->qtail = m; } else { s->qhead = s->qtail = m; }
    pthread_mutex_unlock(&s->qlock);
    if (s->wsi) lws_callback_on_writable(s->wsi);
}

/* Socket.IO EVENT packet is engine.io type '4' (message) followed by socket.io
 * type '2' (event): "42/device,[\"name\",{...}]" */
static void send_event(sioc_t *s, const char *event, cJSON *payload) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString(event));
    if (payload) cJSON_AddItemToArray(arr, cJSON_Duplicate(payload, true));
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    size_t need = strlen("2/device,") + strlen(json) + 1;
    char *body = malloc(need);
    snprintf(body, need, "2/device,%s", json);
    free(json);
    enqueue(s, "4", body);
    free(body);
}

void sioc_emit(sioc_t *s, const char *event, cJSON *payload_or_null) {
    if (!s) return;
    send_event(s, event, payload_or_null);
}

static void handle_socketio_packet(sioc_t *s, const char *p, size_t len) {
    if (len == 0) return;
    char type = p[0];
    const char *rest = p + 1;
    size_t rlen = len - 1;

    /* skip an optional "/namespace," prefix */
    if (rlen && rest[0] == '/') {
        const char *comma = memchr(rest, ',', rlen);
        if (comma) { rlen -= (comma + 1 - rest); rest = comma + 1; }
    }
    /* skip an optional numeric ack id right before the JSON payload */
    while (rlen && rest[0] >= '0' && rest[0] <= '9') { rest++; rlen--; }

    switch (type) {
    case '0': /* CONNECT ack for the namespace */
        s->ns_connected = true;
        LOG("sioc: namespace connected");
        if (s->on_state) s->on_state(s->user, true);
        break;
    case '1': /* DISCONNECT */
        LOG("sioc: server disconnected the namespace");
        s->ns_connected = false;
        if (s->on_state) s->on_state(s->user, false);
        break;
    case '2': { /* EVENT: rest is a JSON array ["name", data...] */
        char *buf = malloc(rlen + 1);
        memcpy(buf, rest, rlen); buf[rlen] = 0;
        cJSON *arr = cJSON_Parse(buf);
        free(buf);
        if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) >= 1) {
            cJSON *name = cJSON_GetArrayItem(arr, 0);
            if (cJSON_IsString(name) && s->on_event) {
                cJSON *data = cJSON_GetArraySize(arr) >= 2 ? cJSON_GetArrayItem(arr, 1) : NULL;
                s->on_event(s->user, name->valuestring, data);
            }
        }
        if (arr) cJSON_Delete(arr);
        break;
    }
    case '4': /* CONNECT_ERROR */
        LOG("sioc: namespace connect_error: %.*s", (int)rlen, rest);
        break;
    default:
        break;
    }
}

static void handle_engineio_packet(sioc_t *s, const char *p, size_t len) {
    if (len == 0) return;
    char type = p[0];
    switch (type) {
    case '0': /* open — server handshake info (sid, pingInterval, pingTimeout) */
        LOG("sioc: engine.io open, connecting /device namespace");
        enqueue(s, "4", "0/device,");
        break;
    case '2': /* ping from server -> reply pong */
        enqueue(s, "3", NULL);
        break;
    case '4': /* message: carries a socket.io packet */
        handle_socketio_packet(s, p + 1, len - 1);
        break;
    case '1': /* close */
        LOG("sioc: engine.io close");
        break;
    default:
        break;
    }
}

static int cb(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    sioc_t *s = (sioc_t *)lws_context_user(lws_get_context(wsi));
    (void)user;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        LOG("sioc: websocket established");
        s->wsi = wsi;
        s->backoff_s = 1;
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        size_t remaining = lws_remaining_packet_payload(wsi);
        bool first = s->rxlen == 0;
        (void)first;
        if (s->rxlen + len > s->rxcap) {
            s->rxcap = s->rxlen + len + 256;
            s->rxbuf = realloc(s->rxbuf, s->rxcap);
        }
        memcpy(s->rxbuf + s->rxlen, in, len);
        s->rxlen += len;
        if (remaining == 0 && lws_is_final_fragment(wsi)) {
            handle_engineio_packet(s, s->rxbuf, s->rxlen);
            s->rxlen = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        pthread_mutex_lock(&s->qlock);
        outmsg_t *m = s->qhead;
        if (m) { s->qhead = m->next; if (!s->qhead) s->qtail = NULL; }
        pthread_mutex_unlock(&s->qlock);
        if (m) {
            lws_write(wsi, (unsigned char *)m->data + LWS_PRE, m->len, LWS_WRITE_TEXT);
            free(m->data);
            free(m);
            pthread_mutex_lock(&s->qlock);
            bool more = s->qhead != NULL;
            pthread_mutex_unlock(&s->qlock);
            if (more) lws_callback_on_writable(wsi);
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        LOG("sioc: connection error: %s", in ? (char *)in : "?");
        s->wsi = NULL;
        s->ns_connected = false;
        if (s->on_state) s->on_state(s->user, false);
        break;

    case LWS_CALLBACK_CLOSED:
        LOG("sioc: closed");
        s->wsi = NULL;
        s->ns_connected = false;
        if (s->on_state) s->on_state(s->user, false);
        break;

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "sioc", cb, 0, 65536, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

/* very small "scheme://host[:port]" splitter — no path/query, sioc always
 * talks to /socket.io/ itself. */
static bool parse_url(const char *url, sioc_t *s) {
    const char *p = url;
    if (strncmp(p, "wss://", 6) == 0) { s->use_ssl = true; p += 6; }
    else if (strncmp(p, "ws://", 5) == 0) { s->use_ssl = false; p += 5; }
    else if (strncmp(p, "https://", 8) == 0) { s->use_ssl = true; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { s->use_ssl = false; p += 7; }
    else return false;

    const char *slash = strchr(p, '/');
    size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
    char hostport[300];
    snprintf(hostport, sizeof(hostport), "%.*s", (int)hostlen, p);

    char *colon = strrchr(hostport, ':');
    if (colon) {
        *colon = 0;
        s->port = atoi(colon + 1);
        snprintf(s->host, sizeof(s->host), "%.255s", hostport);
    } else {
        snprintf(s->host, sizeof(s->host), "%.255s", hostport);
        s->port = s->use_ssl ? 443 : 80;
    }
    return true;
}

sioc_t *sioc_connect(const char *url, void *user, sioc_event_cb on_event, sioc_state_cb on_state) {
    sioc_t *s = calloc(1, sizeof(*s));
    s->user = user; s->on_event = on_event; s->on_state = on_state;
    s->backoff_s = 1;
    pthread_mutex_init(&s->qlock, NULL);
    snprintf(s->path, sizeof(s->path), "/socket.io/?EIO=4&transport=websocket");

    if (!parse_url(url, s)) {
        LOG("sioc: could not parse server url '%s'", url);
        free(s);
        return NULL;
    }

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1; info.uid = -1;
    info.user = s;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    s->ctx = lws_create_context(&info);
    if (!s->ctx) { LOG("sioc: lws_create_context failed"); free(s); return NULL; }

    struct lws_client_connect_info cc;
    memset(&cc, 0, sizeof(cc));
    cc.context = s->ctx;
    cc.address = s->host;
    cc.port = s->port;
    cc.path = s->path;
    cc.host = s->host;
    cc.origin = s->host;
    cc.protocol = protocols[0].name;
    cc.pwsi = &s->wsi;
    if (s->use_ssl) {
        cc.ssl_connection = LCCSCF_USE_SSL;
        if (getenv("STP_TLS_INSECURE"))
            cc.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_ALLOW_INSECURE;
    }

    if (!lws_client_connect_via_info(&cc)) {
        LOG("sioc: initial connect failed");
        lws_context_destroy(s->ctx);
        free(s);
        return NULL;
    }

    LOG("sioc: connecting to %s://%s:%d%s", s->use_ssl ? "wss" : "ws", s->host, s->port, s->path);
    return s;
}

void sioc_service(sioc_t *s, int timeout_ms) {
    if (!s || !s->ctx) return;
    lws_service(s->ctx, timeout_ms);

    /* very small reconnect: if the wsi died, retry with linear-ish backoff */
    if (!s->wsi && !s->want_stop) {
        time_t now = time(NULL);
        if (now >= s->next_retry_at) {
            struct lws_client_connect_info cc;
            memset(&cc, 0, sizeof(cc));
            cc.context = s->ctx;
            cc.address = s->host;
            cc.port = s->port;
            cc.path = s->path;
            cc.host = s->host;
            cc.origin = s->host;
            cc.protocol = protocols[0].name;
            cc.pwsi = &s->wsi;
            if (s->use_ssl) {
                cc.ssl_connection = LCCSCF_USE_SSL;
                if (getenv("STP_TLS_INSECURE"))
                    cc.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_ALLOW_INSECURE;
            }
            lws_client_connect_via_info(&cc);
            s->backoff_s = s->backoff_s < 30 ? s->backoff_s * 2 : 30;
            s->next_retry_at = now + s->backoff_s;
        }
    }
}

bool sioc_is_connected(sioc_t *s) { return s && s->ns_connected; }

void sioc_close(sioc_t *s) {
    if (!s) return;
    s->want_stop = true;
    if (s->ctx) lws_context_destroy(s->ctx);
    free(s->rxbuf);
    pthread_mutex_destroy(&s->qlock);
    free(s);
}
