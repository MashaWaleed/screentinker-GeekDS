#include "stp.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <cjson/cJSON.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

void stp_log(const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm tmv; localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

bool identity_load(stp_identity_t *out, const char *path) {
    memset(out, 0, sizeof(*out));
    char *txt = read_whole_file(path);
    if (!txt) return false;
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) return false;
    const cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "device_id")) && cJSON_IsString(j))
        snprintf(out->device_id, sizeof(out->device_id), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItem(root, "device_token")) && cJSON_IsString(j))
        snprintf(out->device_token, sizeof(out->device_token), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItem(root, "fingerprint")) && cJSON_IsString(j))
        snprintf(out->fingerprint, sizeof(out->fingerprint), "%s", j->valuestring);
    out->paired = out->device_id[0] && out->device_token[0];
    cJSON_Delete(root);
    return true;
}

bool identity_save(const stp_identity_t *id, const char *path) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", id->device_id);
    cJSON_AddStringToObject(root, "device_token", id->device_token);
    cJSON_AddStringToObject(root, "fingerprint", id->fingerprint);
    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return false;

    /* best-effort mkdir -p of the parent directory */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0700); }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = false;
    if (fd >= 0) {
        size_t len = strlen(txt);
        ok = (write(fd, txt, len) == (ssize_t)len);
        close(fd);
    }
    free(txt);
    return ok;
}

void identity_ensure_fingerprint(stp_identity_t *id) {
    if (id->fingerprint[0]) return;
    /* Stable per-installation id: machine-id if present, else a random uuid-ish string
     * persisted alongside the rest of the identity file by the caller. */
    char *txt = read_whole_file("/etc/machine-id");
    if (txt) {
        size_t len = strlen(txt);
        while (len && (txt[len-1] == '\n' || txt[len-1] == '\r')) txt[--len] = 0;
        snprintf(id->fingerprint, sizeof(id->fingerprint), "stlinux-%s", txt);
        free(txt);
        return;
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    snprintf(id->fingerprint, sizeof(id->fingerprint), "stlinux-%08x%08x", rand(), rand());
}
