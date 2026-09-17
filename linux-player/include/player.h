#ifndef PLAYER_H
#define PLAYER_H

#include "stp.h"
#include "cache.h"

typedef void (*player_play_event_cb)(void *user, const char *content_or_widget_id,
                                      const char *event_type /* "play_start"|"play_end" */);

/* Tears down any playback in progress and starts fresh from `pl` — one
 * thread per zone (or one thread total for a fullscreen/no-layout
 * playlist), each independently cycling its own items forever so zones on
 * a multi-zone layout run on their own clocks, matching the server's model
 * where each zone is an independently-sequenced list. Safe to call again
 * at any time (e.g. on every device:playlist-update) — the previous run is
 * cancelled and joined before the new one starts. `pl` is deep-copied. */
void player_start(stp_display_t *disp, const stp_playlist_t *pl, const char *server_base_url,
                   content_cache_t *cache, void *user, player_play_event_cb cb);

void player_stop(void);

/* Suspended-state placard (device disabled / over plan limit / etc). */
void player_show_suspended(stp_display_t *disp, const char *message);

#endif
