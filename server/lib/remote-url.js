'use strict';

// URL gates for remote content, split by WHO opens the URL.
//
// validateRemoteUrl      — for URLs the SERVER may retrieve (YouTube oEmbed, web
//                          pages, remote images/video the proxy touches). Blocks
//                          RFC1918 / loopback / link-local / .local / .internal:
//                          an SSRF gate.
// validatePlayerOpenedUrl — for URLs only the PLAYER opens on its own LAN, never
//                          the server (video/hls live streams). Private hosts are
//                          REQUIRED here (hotel/venue IPTV is 10.x / .local), so
//                          this ALLOWS them. Still http/https only, and still
//                          rejects credentials-in-URL and every non-web scheme.
//
// The server never fetches an HLS stream (no proxy, no restream), so opening a
// private address carries no SSRF: the request leaves the SCREEN, on the same LAN
// the operator already trusts, not our backend.

// video/hls is the ONLY live mime. A live item is content with this mime + a
// remote_url; there is no widget, no second content type, no live_sources table.
const LIVE_MIME = 'video/hls';

function isLiveItem(item) {
  return !!(item && item.mime_type === LIVE_MIME);
}

// Dwell lives on the PLAYLIST ITEM's duration_sec. 0 / null / absent = infinite
// dwell (stay on the channel until it is skipped). Used to keep a dwell-0 live
// item out of the clock scheduler, which would treat an infinite slot as broken.
function isInfiniteDwell(item) {
  const d = item && item.duration_sec;
  return d == null || Number(d) <= 0;
}

// The player fails a junk URL to a skip (P4), so this only checks SHAPE, never the
// network: a path or query that names an HLS playlist. No HEAD/GET — that would be
// both SSRF and a WAN pull of a live stream.
function looksLikeHlsUrl(url) {
  try {
    const u = new URL(url);
    const path = (u.pathname || '').toLowerCase();
    const q = (u.search || '').toLowerCase();
    return path.endsWith('.m3u8') || path.includes('.m3u8') || q.includes('.m3u8')
      || q.includes('m3u8') || /(^|[^a-z])hls([^a-z]|$)/.test(path);
  } catch { return false; }
}

// SSRF gate for a server-retrievable remote_url. Returns null if valid, else
// { status, error }. Unchanged from the original in-content.js definition.
function validateRemoteUrl(url) {
  let parsed;
  try { parsed = new URL(url); }
  catch { return { status: 400, error: 'Invalid URL format' }; }
  if (!['http:', 'https:'].includes(parsed.protocol)) {
    return { status: 400, error: 'URL must use http or https' };
  }
  const hostname = parsed.hostname.toLowerCase();
  const isPrivate = hostname === 'localhost' || hostname === '0.0.0.0' ||
    hostname.startsWith('127.') || hostname.startsWith('10.') ||
    hostname.startsWith('192.168.') || hostname.startsWith('169.254.') ||
    /^172\.(1[6-9]|2[0-9]|3[0-1])\./.test(hostname) ||
    hostname.startsWith('fc') || hostname.startsWith('fd') || hostname === '::1' ||
    hostname.endsWith('.local') || hostname.endsWith('.internal');
  if (isPrivate) return { status: 400, error: 'Internal URLs are not allowed' };
  return null;
}

// Gate for a URL only the player opens. http/https only (no file:/javascript:/
// ftp:/udp:/rtsp:), private hosts ALLOWED, credentials-in-URL rejected (they would
// travel in the published snapshot to every screen). Returns null if valid, else
// { status, error }.
function validatePlayerOpenedUrl(url) {
  let parsed;
  try { parsed = new URL(url); }
  catch { return { status: 400, error: 'Invalid URL format' }; }
  if (!['http:', 'https:'].includes(parsed.protocol)) {
    return { status: 400, error: 'A live stream URL must use http or https' };
  }
  if (parsed.username || parsed.password) {
    return { status: 400, error: 'A live stream URL must not embed a username or password' };
  }
  return null;
}

module.exports = {
  LIVE_MIME, isLiveItem, isInfiniteDwell, looksLikeHlsUrl,
  validateRemoteUrl, validatePlayerOpenedUrl,
};
