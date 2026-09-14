'use strict';

// devices.capture_mode — which screen-capture tier a panel can actually use.
//
// The bug it exists for: MediaProjection consent does not survive the app restarting, and an OTA
// restarts the app. A panel silently drops from whole-screen capture to drawing only the player's
// own window — the remote view shows the playlist and goes blank over Settings, with no error
// anywhere. Reported by a customer on two panels at once after one update. The tier was
// unobservable from the server, so nobody could tell a permission state from a broken screenshot.

const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
process.env.DATA_DIR = path.join(os.tmpdir(), 'st-capmode-' + crypto.randomBytes(4).toString('hex'));
process.env.SELF_HOSTED = 'true';
process.env.NODE_ENV = 'test';

const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { db } = require('../db/database');

test('the column exists and is nullable', () => {
  const col = db.prepare("SELECT * FROM pragma_table_info('devices') WHERE name='capture_mode'").get();
  assert.ok(col, 'devices.capture_mode is migrated');
  assert.equal(col.notnull, 0, 'NULL means "not reported" — every non-Android player, and older builds');
});

test('the writer accepts only the four real tiers, and NULLs anything else', () => {
  // Absent or junk must not become a bogus tier the dashboard would then explain to an operator.
  const src = fs.readFileSync(path.join(__dirname, '..', 'ws', 'deviceSocket.js'), 'utf8');
  assert.match(src, /\['projection', 'accessibility', 'view', 'none'\]\.includes\(di\.capture_mode\)/,
    'the persist path validates against the known tiers');
  assert.match(src, /capture_mode = \?/, 'and actually writes the column');
});

test('⚠️ EVERY device_info emit site carries the tier, not just the first', () => {
  // device_info is emitted from three places (register, re-register, heartbeat). Adding the field
  // at one call site is how a panel reports a tier on connect and none on the next heartbeat.
  const src = fs.readFileSync(
    path.join(__dirname, '..', '..', 'android', 'app', 'src', 'main', 'java', 'com',
      'remotedisplay', 'player', 'service', 'WebSocketService.kt'), 'utf8');
  assert.equal((src.match(/deviceInfo\.getDeviceInfo\(\)/g) || []).length, 1,
    'exactly one call to the raw builder — inside deviceInfoPayload()');
  assert.ok((src.match(/deviceInfoPayload\(\)/g) || []).length >= 4,
    'and every emit site goes through the wrapper that adds capture_mode');
});

test('the tier definition matches the capture fallback order', () => {
  const root = path.join(__dirname, '..', '..', 'android', 'app', 'src', 'main', 'java', 'com',
    'remotedisplay', 'player', 'service');
  const mode = fs.readFileSync(path.join(root, 'CaptureMode.kt'), 'utf8');
  const order = [...mode.matchAll(/^\s*(ScreenCaptureService\.isReady|accessibilityCaptureAvailable\(\)|hasActivityCapture)/gm)]
    .map((m) => m[1]);
  assert.deepEqual(order, ['ScreenCaptureService.isReady', 'accessibilityCaptureAvailable()', 'hasActivityCapture'],
    'projection, then accessibility, then view — the same order captureScreen() tries');
});

test('restore-on-start is gated on device owner, so no dialog lands over live content', () => {
  const src = fs.readFileSync(
    path.join(__dirname, '..', '..', 'android', 'app', 'src', 'main', 'java', 'com',
      'remotedisplay', 'player', 'ScreenCapturePermissionActivity.kt'), 'utf8');
  const fn = src.slice(src.indexOf('fun restoreIfPreviouslyGranted'));
  assert.match(fn, /screen_capture_granted/, 'it reads the flag that was previously write-only');
  assert.match(fn, /isDeviceOwner\(\)/, 'and only re-requests where the grant is dialog-free');
  const guard = fn.indexOf('isDeviceOwner()');
  const request = fn.indexOf('requestPermission(context)');
  assert.ok(guard > 0 && guard < request, 'the ownership check comes BEFORE the request');
});

test('the dashboard says nothing when the tier is unknown or already durable', () => {
  const src = fs.readFileSync(path.join(__dirname, '..', '..', 'frontend', 'js', 'views', 'device-detail.js'), 'utf8');
  const fn = src.slice(src.indexOf('function captureModeNotice'), src.indexOf('// Mirrors platformFamily'));
  assert.match(fn, /if \(!mode \|\| mode === 'accessibility'\) return ''/,
    'NULL is "not reported", not a fault, and accessibility needs no nudge');
  assert.match(fn, /device\.remote\.capture_view/, 'and the degraded case is called out');
});
