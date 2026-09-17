# stlinux-player

A native Linux signage client for **ScreenTinker** (github.com/screentinker/screentinker) that
decodes and scans out media itself — via ffmpeg's libav* libraries straight into a DRM/KMS or
`/dev/fb0` buffer — instead of running a browser. No X11, no Wayland, no browser rendering
pipeline overhead.

## Why this shape

ScreenTinker ships an Android app and a browser-based web player; there's no headless Linux
client. Everything here was reverse-engineered from the actual, real ScreenTinker server source
(cloned from GitHub, not guessed) — specifically `server/ws/deviceSocket.js`,
`server/db/schema.sql`, `server/server.js`'s static mounts, and `server/lib/plugins/reserved.js`.
That's where the protocol details below come from.

## Protocol summary (as implemented)

- **Transport**: plain Socket.IO v4 on the `/device` namespace, connected directly over
  WebSocket — `wss://<host>/socket.io/?EIO=4&transport=websocket` — skipping the HTTP
  long-polling handshake, same as any Socket.IO client configured with `transports: ['websocket']`.
  Implemented by hand in `src/sioc.c` (Engine.IO framing + Socket.IO event framing) rather than
  pulling in a full client library, since the wire format is a handful of digit-prefixed text
  packets once you skip polling.
- **Pairing**: the device itself generates a random 6-digit code, displays it full-screen, and
  sends `device:register {pairing_code, fingerprint, device_info}`. The operator enters that same
  code in the ScreenTinker dashboard's "Add Display" flow, which claims it. The server replies
  `device:registered {device_id, device_token}`, persisted to `/etc/stlinux-player/device.json`.
  Subsequent boots send `device:register {device_id, device_token, fingerprint}` to resume the
  same identity instead of pairing again.
- **Playlists**: pushed as `device:playlist-update`, matching `assemblePayload()` /
  `buildPlaylistPayloadUnchecked()` server-side: `{assignments[], layout|null, orientation,
  background_color, timezone, suspended, ...}`. Each assignment carries `content_id`/`widget_id`,
  `zone_id`, `duration_sec`, `schedule_start/end/days`, and the server patches in fresh
  `filepath`/`mime_type`/`content_rev` at send time (see `refreshContentRevs()`), so a replaced
  asset is never served stale from this client's cache.
- **Content bytes**: served unauthenticated from `/uploads/content/<filepath>` (confirmed in
  `server/server.js` — this static mount has no auth, unlike the JWT-gated
  `/api/content/:id/file`). Downloaded once via libcurl into `/var/cache/stlinux-player/`, keyed
  by `content_id_contentrev`, so a dropped connection doesn't interrupt playback and a replaced
  asset re-downloads automatically.
- **Commands**: `device:command {type}` for `reboot`, `shutdown`, `screen_on`, `screen_off`,
  `set_volume`, `update`, `refresh` (the actual type strings the real Android/web players branch
  on).
- **Telemetry**: `device:heartbeat` every 30s with uptime, RAM/storage usage, 1-minute load
  average, and the active display backend/resolution.

## What's fully supported

- Fullscreen playlists and multi-zone layouts (zones are percentage rects; each zone runs its own
  independent, looping playback thread — matching the server's model of independently-sequenced
  zone queues)
- Images and video, any format ffmpeg's demuxers/decoders handle, decoded and `sws_scale`d
  **directly into the mmap'd scanout buffer** — no intermediate copy, the same trick used in the
  existing `fbplay_video.c` framebuffer player, generalized here to write into an arbitrary
  sub-rect (for zones) and to either a DRM dumb buffer or `/dev/fb0`
- `contain` / `cover` / `fill` zone fit modes, with background-color letterboxing
- Video audio via ALSA (optional — build without `libasound-dev` and it degrades to video-only)
- Schedule windows (`schedule_start`/`end`/`days`) — an item outside its window is skipped when
  its turn comes up rather than blocking the zone
- `clock` and `text` widgets, rendered with a small dependency-free stroke font (no FreeType, no
  font files needed on a stripped-down box)
- Offline resilience: cached content keeps playing through a dropped connection; a reconnect
  resumes with exponential backoff
- Device commands (reboot/shutdown/screen blank via DRM DPMS or fbdev `FBIOBLANK`/set_volume)
- Remote-URL content (non-YouTube) is downloaded and cached the same as uploaded content;
  best-effort direct streaming if the download fails while offline
- YouTube content: resolved via `yt-dlp -g` if installed, handed to ffmpeg as a direct stream URL
  (not cached — these URLs expire, so this item has no offline resilience, which is inherent to
  how such URLs work, not a gap in the cache layer)

## What isn't (and why)

ScreenTinker's `weather`, `rss`, `webpage`, `social`, `directory-board`, `directory-search` and
`diag-smoothness` widgets are **server-rendered HTML** (see `renderWidgetHtml()` in
`server/lib/plugins/`), meant for a browser engine — that's true for the Android app too, which
renders them in a WebView. A browserless ffmpeg/DRM client fundamentally can't reproduce these
pixel-for-pixel without embedding a browser engine somewhere. This client shows a plain text
placeholder for a few seconds and moves on rather than stalling a zone.

If you need these, the natural extension point is embedding **WPE WebKit** (it has an offscreen/
`libwpe` mode with no X11/Wayland dependency, unlike Chromium headless) to render the same widget
HTML the server already serves, and blitting its output buffer into the zone instead of the
placeholder in `player.c`'s `play_widget()`. That's a real, scoped addition — it just needs its
own writeup rather than being crammed into this one.

Also out of scope here: video-wall cross-device sync, kiosk touch-injection, remote screenshot/
live-view capture, and custom GLSL shader transitions — all real ScreenTinker features, all
requiring either a second protocol surface (screenshot upload) or a GPU compositing path this
project deliberately avoids in favor of direct scanout. The code is structured (one file per
concern) so any of these can be added without touching the rest.

## Building

```sh
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
                  libdrm-dev libwebsockets-dev libcurl4-openssl-dev libcjson-dev libasound2-dev \
                  build-essential pkg-config
make
```

Drop `-DWITH_ALSA` from the Makefile's `CFLAGS` (and `libasound2-dev`/`alsa` from the deps) to
build without audio.

## Running

```sh
sudo ./stlinux-player --server https://your-screentinker-host
```

First run shows a pairing code full-screen; enter it in the dashboard. `--display` defaults to
`auto` (tries DRM/KMS, falls back to `/dev/fb0`); pass `drm`, `fbdev`, `/dev/dri/card1` or
`/dev/fb0` to force one. See `systemd/stlinux-player.service` for running it as a persistent
service — it needs access to `/dev/dri/*` or `/dev/fb0`, plus `/dev/snd/*` for audio, so running
as root on a dedicated box is the simplest setup; otherwise add the service user to the `video`,
`render` and `audio` groups.

## Layout

```
include/stp.h            shared types (display interface, playlist/zone/assignment structs)
src/display_drm.c        DRM/KMS backend — dumb buffer, direct drmModeSetCrtc scanout
src/display_fbdev.c      /dev/fb0 backend, same interface
src/display.c            picks DRM first, falls back to fbdev
src/sioc.c               Socket.IO v4 client over libwebsockets
src/cache.c              content download + on-disk cache, keyed by content_id + rev
src/playlist_parse.c     cJSON -> stp_playlist_t
src/media.c              ffmpeg decode/scale of one item into a display sub-rect (+ ALSA audio)
src/textrender.c         dependency-free stroke-font text rendering (pairing code, clock, text)
src/player.c             per-zone playback threads, scheduling, widget dispatch
src/device.c             identity/pairing, telemetry, command handling
src/main.c               argv parsing, event loop
```
