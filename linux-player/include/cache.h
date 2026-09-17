/* cache.h — local content cache.
 *
 * ScreenTinker's own players (web Service Worker, Android ContentCache) keep
 * playing cached media when the server or the internet drops; this mirrors
 * that behaviour for the Linux client. Every filepath-backed content item is
 * downloaded once from the server's public, unauthenticated static mount
 * (/uploads/content/<filepath> — confirmed in server/server.js, no device
 * token required) and kept under cache_dir, keyed by content_id + the
 * server's content_rev so a replaced asset (PUT /:id/replace) is a fresh
 * download instead of a forever-stale cache hit, exactly as the comments in
 * deviceSocket.js's refreshContentRevs() describe for the other players.
 */
#ifndef CACHE_H
#define CACHE_H

#include "stp.h"

typedef struct {
    char dir[512];
} content_cache_t;

void cache_init(content_cache_t *c, const char *dir);

/* Resolves an assignment item to something media_open()/media_play_item()
 * can hand to ffmpeg: a local cached file path if the item is cacheable and
 * caching succeeded (or was already present), otherwise the item's own
 * remote_url as a best-effort direct stream. Returns a pointer into a
 * static/thread-local buffer valid until the next call — copy it if you
 * need it to outlive that. NULL if nothing playable could be resolved. */
const char *cache_resolve(content_cache_t *c, const char *server_base_url,
                           const stp_assignment_t *item);

/* Deletes cached files whose content_id is not present in `keep_ids`
 * (space-separated). Called after every playlist update so a removed
 * asset's disk space is reclaimed instead of accumulating forever. */
void cache_prune(content_cache_t *c, const char *keep_ids_space_separated);

#endif
