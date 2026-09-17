#include "cache.h"
#include <curl/curl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

void cache_init(content_cache_t *c, const char *dir) {
    snprintf(c->dir, sizeof(c->dir), "%s", dir);
    mkdir(dir, 0755); /* best-effort; parent is assumed to exist (created at install time) */
}

static const char *ext_of(const char *path_or_mime, bool is_mime) {
    if (is_mime) {
        if (strstr(path_or_mime, "jpeg")) return ".jpg";
        if (strstr(path_or_mime, "png")) return ".png";
        if (strstr(path_or_mime, "gif")) return ".gif";
        if (strstr(path_or_mime, "webp")) return ".webp";
        if (strstr(path_or_mime, "mp4")) return ".mp4";
        if (strstr(path_or_mime, "webm")) return ".webm";
        return ".bin";
    }
    const char *dot = strrchr(path_or_mime, '.');
    return dot ? dot : ".bin";
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

static bool download(const char *url, const char *dest_path) {
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.part", dest_path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) { LOG("cache: cannot open %s for writing", tmp_path); return false; }

    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "stlinux-player/1.0");
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    fclose(f);

    if (res != CURLE_OK || code < 200 || code >= 300) {
        LOG("cache: download failed for %s (curl=%d http=%ld)", url, res, code);
        unlink(tmp_path);
        return false;
    }
    rename(tmp_path, dest_path);
    return true;
}

/* Best-effort YouTube resolution: shells out to yt-dlp if it's on PATH to
 * get a direct, ffmpeg-openable stream URL. Not cached to disk (these URLs
 * expire), so a youtube item has no offline resilience — that's an inherent
 * limitation of playing a URL a headless resolver has to sign fresh each
 * time, not something the cache layer can fix. */
static bool resolve_youtube(const char *remote_url, char *out, size_t out_sz) {
    if (system("command -v yt-dlp >/dev/null 2>&1") != 0) {
        LOG("cache: yt-dlp not installed, cannot resolve youtube item %s", remote_url);
        return false;
    }
    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
             "yt-dlp -f 'best[ext=mp4]/best' -g '%s' 2>/dev/null", remote_url);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    bool ok = fgets(out, out_sz, p) != NULL;
    pclose(p);
    if (ok) { char *nl = strchr(out, '\n'); if (nl) *nl = 0; }
    return ok && out[0];
}

const char *cache_resolve(content_cache_t *c, const char *server_base_url,
                           const stp_assignment_t *item) {
    static __thread char resolved[1200];

    if (item->kind != ITEM_CONTENT) return NULL;

    if (item->filepath[0]) {
        char dest[700];
        snprintf(dest, sizeof(dest), "%s/%s_%ld%s", c->dir, item->content_id,
                 item->content_rev, ext_of(item->filepath, false));
        if (access(dest, F_OK) == 0) { snprintf(resolved, sizeof(resolved), "%s", dest); return resolved; }

        char url[1200];
        snprintf(url, sizeof(url), "%s/uploads/content/%s", server_base_url, item->filepath);
        LOG("cache: downloading %s -> %s", url, dest);
        if (download(url, dest)) { snprintf(resolved, sizeof(resolved), "%s", dest); return resolved; }
        return NULL;
    }

    if (item->remote_url[0]) {
        if (strcmp(item->mime_type, "video/youtube") == 0) {
            if (resolve_youtube(item->remote_url, resolved, sizeof(resolved))) return resolved;
            return NULL;
        }
        /* Generic remote URL content: try to cache it once so it survives
         * an outage; if the download fails (e.g. offline right now), fall
         * back to handing ffmpeg the URL directly — best-effort streaming,
         * same as a browser player would do for uncached remote content. */
        char dest[700];
        snprintf(dest, sizeof(dest), "%s/%s_%ld%s", c->dir, item->content_id,
                 item->content_rev, ext_of(item->mime_type, true));
        if (access(dest, F_OK) == 0) { snprintf(resolved, sizeof(resolved), "%s", dest); return resolved; }
        if (download(item->remote_url, dest)) { snprintf(resolved, sizeof(resolved), "%s", dest); return resolved; }
        snprintf(resolved, sizeof(resolved), "%s", item->remote_url);
        return resolved;
    }

    return NULL;
}

void cache_prune(content_cache_t *c, const char *keep_ids_space_separated) {
    DIR *d = opendir(c->dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char id[64] = {0};
        sscanf(e->d_name, "%63[^_]", id);
        char needle[80];
        snprintf(needle, sizeof(needle), " %s ", id);
        char haystack[4096];
        snprintf(haystack, sizeof(haystack), " %s ", keep_ids_space_separated);
        if (!strstr(haystack, needle)) {
            char path[700];
            snprintf(path, sizeof(path), "%s/%s", c->dir, e->d_name);
            LOG("cache: pruning stale %s", path);
            unlink(path);
        }
    }
    closedir(d);
}
