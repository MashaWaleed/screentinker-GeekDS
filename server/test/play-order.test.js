'use strict';

// Shuffle / weighted / sequential next-index. Seeded RNG so the bag is deterministic.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { nextIndex, firstIndex, weightOf } = require('../lib/play-order');

const ROOT = path.join(__dirname, '..', '..');
const items = (n) => Array.from({ length: n }, (_, i) => ({ id: i, weight: 1 }));
const all = () => true;
function rng(seq) {
  let i = 0;
  return () => seq[i++ % seq.length];
}

test('sequential walks from+1 and wraps, skipping ineligible', () => {
  const list = items(4);
  const allows = (it, idx) => idx !== 1;
  assert.equal(nextIndex(list, 0, allows, 'sequential'), 2);
  assert.equal(nextIndex(list, 2, allows, 'sequential'), 3);
  assert.equal(nextIndex(list, 3, allows, 'sequential'), 0);
  assert.equal(firstIndex(list, allows, 'sequential'), 0);
});

test('unknown mode and empty list fail open to sequential', () => {
  assert.equal(nextIndex(items(3), 0, all, 'nope'), 1);
  assert.equal(nextIndex([], 0, all, 'shuffle'), -1);
  assert.equal(nextIndex(null, 0, all, 'weighted'), -1);
});

test('shuffle draws a no-repeat bag and refills when empty', () => {
  const list = items(3);
  const state = {};
  const rnd = rng([0.9, 0.1, 0.5, 0.2, 0.8, 0.3, 0.7, 0.4]);
  const seen = [];
  let from = -1;
  for (let i = 0; i < 6; i++) {
    from = nextIndex(list, from, all, 'shuffle', state, rnd);
    seen.push(from);
  }
  assert.deepEqual([...seen.slice(0, 3)].sort(), [0, 1, 2]);
  for (const i of seen) assert.ok(i === 0 || i === 1 || i === 2);
  for (let i = 1; i < seen.length; i++) assert.notEqual(seen[i], seen[i - 1], 'no immediate repeat');
});

test('shuffle refills when membership changes', () => {
  const list = items(3);
  const state = {};
  const rnd = rng([0.2, 0.8, 0.1, 0.9]);
  const a = nextIndex(list, -1, all, 'shuffle', state, rnd);
  assert.ok(a >= 0);
  const allows = (it, idx) => idx !== a;
  const b = nextIndex(list, a, allows, 'shuffle', state, rnd);
  assert.notEqual(b, a);
  assert.ok(b === 0 || b === 1 || b === 2);
});

test('weighted picks by weight and avoids the item just played', () => {
  const list = [{ weight: 1 }, { weight: 100 }, { weight: 1 }];
  const rnd = rng([0.99]); // always land at the top of the remaining pool
  // from 1 (the heavy item): pool is 0 and 2, 0.99 * 2 = 1.98 -> item 2
  const n = nextIndex(list, 1, all, 'weighted', {}, rnd);
  assert.notEqual(n, 1);
});

test('weightOf clamps and defaults to 1', () => {
  assert.equal(weightOf({}), 1);
  assert.equal(weightOf({ weight: 0 }), 1);
  assert.equal(weightOf({ weight: 12.9 }), 12);
  assert.equal(weightOf({ weight: 9999 }), 1000);
});

test('tizen play-order.js is byte-identical to the canonical module', () => {
  const a = fs.readFileSync(path.join(ROOT, 'server/lib/play-order.js'));
  const b = fs.readFileSync(path.join(ROOT, 'tizen/js/play-order.js'));
  assert.ok(a.equals(b), 'tizen/js/play-order.js has drifted — re-copy it (build-wgt.sh does this)');
});

test('web, Tizen, e-ink and Android players call PlayOrder', () => {
  const web = fs.readFileSync(path.join(ROOT, 'server/player/index.html'), 'utf8');
  const tizen = fs.readFileSync(path.join(ROOT, 'tizen/js/player.js'), 'utf8');
  const embedded = fs.readFileSync(path.join(ROOT, 'server/routes/embedded.js'), 'utf8');
  const android = fs.readFileSync(path.join(ROOT, 'android/app/src/main/java/com/remotedisplay/player/player/PlaylistController.kt'), 'utf8');
  assert.match(web, /PlayOrder\.nextIndex/);
  assert.match(web, /peekNextIndex/);
  assert.match(tizen, /PlayOrder\.nextIndex/);
  assert.match(embedded, /PlayOrder\.nextIndex/);
  assert.match(android, /PlayOrder\.nextIndex/);
  assert.ok(fs.existsSync(path.join(ROOT, 'android/app/src/main/java/com/remotedisplay/player/player/PlayOrder.kt')));
});
