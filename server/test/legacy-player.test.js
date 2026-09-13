'use strict';

const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const transitionBundle = require('../lib/transition-bundle');

function executableCode(source) {
  return source.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*|(['"`])(?:\\.|(?!\1)[^\\])*\1/g, ' ');
}

const chrome53Breakers = /\?\.(?!\d)|\?\?(?![=?])|\basync\s+(?:function\b|\([^)]*\)\s*=>|[A-Za-z_$][\w$]*\s*=>)|\bawait\s+|catch\s*\{/;

const PLAYER = path.join(__dirname, '..', 'player');

function coreScript(html) {
  const start = html.indexOf('<script>', html.indexOf('/player/transitions.js'));
  const end = html.indexOf('\n  </script>', start);
  assert.ok(start >= 0 && end > start);
  return html.slice(start, end);
}

test('legacy player artifacts are current and Chrome 53 compatible', () => {
  const html = fs.readFileSync(path.join(PLAYER, 'legacy.html'), 'utf8');
  const scripts = [
    ['player', coreScript(html)],
    ['service worker', fs.readFileSync(path.join(PLAYER, 'sw-legacy.js'), 'utf8')],
    ['live publish', fs.readFileSync(path.join(PLAYER, 'live-publish-legacy.js'), 'utf8')],
    ['talk', fs.readFileSync(path.join(PLAYER, 'talk-legacy.js'), 'utf8')],
  ];
  for (const [name, script] of scripts) {
    assert.doesNotMatch(executableCode(script), chrome53Breakers, name);
  }
  assert.doesNotMatch(html, /\binset\s*:/, 'legacy player CSS uses four supported edges');
  assert.doesNotMatch(html, /\.padStart\(/, 'legacy player avoids Chrome 57 String.padStart');
  assert.match(html, /src="\/player\/live-publish-legacy\.js"/);
  assert.match(html, /src="\/player\/talk-legacy\.js"/);
});

test('legacy player dependencies remain Chrome 53 syntax compatible', () => {
  const scripts = [
    ['debug overlay', path.join(__dirname, '..', 'player', 'debug-overlay.js')],
    ['ST bridge', path.join(__dirname, '..', '..', 'brightsign', 'st-bridge.js')],
    ['ST sync', path.join(__dirname, '..', '..', 'brightsign', 'st-sync.js')],
    ['schedule evaluator', path.join(__dirname, '..', 'lib', 'schedule-eval.js')],
    ['offline play queue', path.join(__dirname, '..', 'lib', 'offline-play-queue.js')],
    ['trigger resolver', path.join(__dirname, '..', 'lib', 'trigger-resolve.js')],
    ['media mute', path.join(__dirname, '..', 'lib', 'media-mute.js')],
    ['orientation style', path.join(__dirname, '..', 'lib', 'orientation-style.js')],
    ['wall geometry', path.join(__dirname, '..', 'lib', 'wall-geometry.js')],
    ['media health', path.join(__dirname, '..', 'lib', 'player-media-health.js')],
  ];

  for (const [name, file] of scripts) {
    assert.doesNotMatch(executableCode(fs.readFileSync(file, 'utf8')), chrome53Breakers, name);
  }
  assert.doesNotMatch(executableCode(transitionBundle.bundle()), chrome53Breakers, 'transitions');
});

test('legacy artifact builder has no uncommitted output', () => {
  const { status, stderr } = require('node:child_process').spawnSync(
    process.execPath,
    [path.join(__dirname, '..', 'scripts', 'build-legacy-player.js'), '--check'],
    { encoding: 'utf8' }
  );
  assert.equal(status, 0, stderr);
});

test('legacy route preserves legacy navigation and serves prebuilt assets', () => {
  const source = fs.readFileSync(path.join(__dirname, '..', 'player', 'index.html'), 'utf8');
  const server = fs.readFileSync(path.join(__dirname, '..', 'server.js'), 'utf8');
  assert.match(source, /IS_LEGACY_PLAYER \? '\/player\/legacy' : '\/player'/);
  assert.match(source, /host \? '&host=' \+ encodeURIComponent\(host\) : ''/);
  assert.match(source, /IS_LEGACY_PLAYER \? '\/sw-legacy\.js' : '\/sw\.js'/);
  for (const asset of ['legacy.html', 'sw-legacy.js', 'live-publish-legacy.js', 'talk-legacy.js']) {
    assert.ok(server.includes(asset), `server route serves ${asset}`);
  }
  assert.doesNotMatch(server, /require\(['"]\.\/lib\/legacy-player['"]\)/);
});

test('player-rendered HTML uses CSS edges supported by Chrome 53', () => {
  for (const file of [
    path.join(PLAYER, 'index.html'),
    path.join(__dirname, '..', 'lib', 'slide-render.js'),
    path.join(__dirname, '..', 'lib', 'talk-web.js'),
    path.join(__dirname, '..', 'routes', 'kiosk.js'),
    path.join(__dirname, '..', 'routes', 'widgets.js'),
  ]) {
    assert.doesNotMatch(fs.readFileSync(file, 'utf8'), /\binset\s*:/, file);
  }
});
