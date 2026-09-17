/* playlist_parse.h — turns the cJSON payload of a `device:playlist-update`
 * event into the plain stp_playlist_t struct the playback engine walks.
 * Field names below are taken directly from assemblePayload() /
 * buildPlaylistPayloadUnchecked() / refreshContentRevs() in the real
 * server/ws/deviceSocket.js, not guessed.
 */
#ifndef PLAYLIST_PARSE_H
#define PLAYLIST_PARSE_H

#include "stp.h"
#include <cjson/cJSON.h>

void playlist_parse(cJSON *payload, stp_playlist_t *out);

#endif
