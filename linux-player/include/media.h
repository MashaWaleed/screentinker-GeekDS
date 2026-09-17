#ifndef STP_MEDIA_H
#define STP_MEDIA_H
#include "stp.h"

typedef enum { MEDIA_OK, MEDIA_ERROR } media_result_t;

/* Decodes `path` (local file path or a URL ffmpeg's protocols understand)
 * into `rect` of `disp`, scaled per `fit_mode` ("contain"/"cover"/"fill"),
 * painting `bg_bgra` into any letterbox/pillarbox area first. Blocks until
 * `duration_sec` has elapsed (0/-1 = play once through and return), or until
 * `*cancel` becomes true. A video is looped to fill duration_sec; a still
 * image (or a video played with no duration override) just holds its last
 * frame. */
media_result_t media_play_item(stp_display_t *disp, stp_rect_t rect, const char *path,
                                const char *fit_mode, uint32_t bg_bgra,
                                int duration_sec, const volatile bool *cancel);

/* "#rrggbb" -> packed XRGB8888 (bytes B,G,R,X in memory), or `fallback` if
 * `hex` is NULL/empty/malformed. */
uint32_t media_parse_color(const char *hex, uint32_t fallback);

#endif
