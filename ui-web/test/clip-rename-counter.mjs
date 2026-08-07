#!/usr/bin/env node
/**
 * RENAMING A CLIP ON A TRACK YOU DID NOT EDIT LAST.
 *
 * Reported as "renaming a clip right after a project opens is refused", chased as a race against
 * the load for two days, and filed as backend #120 / local #46. IT IS NOT A RACE. It is the wrong
 * counter, and the load had nothing to do with it beyond being a convenient moment to notice.
 *
 * ── THE MECHANISM ───────────────────────────────────────────────────────────────────────────
 *
 * `SetClipText` is NOT in `uiCommandIsGlobalScope`, so `requireMatchingClipVersion` compares it
 * against that TRACK's `trackClipVersion`. `dockApi.clipText` sent `engine.clipVersion` — the
 * GLOBAL counter. Those two are equal only until the first edit and then diverge for good.
 *
 * So the rename lands on whichever track was edited last and is refused on every other one, in
 * silence, because this path has no retry. After a load, "the track edited last" is nothing, which
 * is why it looked load-shaped.
 *
 * ── WHY THIS REPRODUCTION HAS NO LOAD IN IT ─────────────────────────────────────────────────
 *
 * Deliberately. `clip-rename-race.mjs` already covers the load sequence and passes, which is
 * exactly why the real defect stayed hidden: the load is not the ingredient. Two renames on two
 * different tracks are enough, and a test that reproduces it without the load is a test that names
 * the cause rather than the circumstance.
 *
 * The FIRST rename is the setup and must land — it is on the track whose counter matches. It also
 * serves as the control: if it failed too, SetClipText would be broken outright and the second
 * assertion would prove nothing.
 */

import { chromium } from 'playwright';
import { readFileSync, existsSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const Q = 960000;
const stack = await startStack({ keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await page.waitForTimeout(1500);

const run = (c) => page.evaluate((x) => window.__uni.run(x), c);
const settle = (ms) => page.waitForTimeout(ms);

/** TWO tracks, each with its own clip, so the two per-track counters can diverge. */
const track = (id, clipId, name) => ({
  track_id: id, name: `T${id}`, harmony_quantize: false, lines_per_beat: 4,
  mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
  device_chain: [], mod_links: [],
  placements: [{ clip_id: clipId, at: 0, length: Q * 4, notes: [], chords: [], mutes: [] }],
});
const clip = (id, name) => ({
  id, name, length: Q * 4, lines_per_beat: 4, kind: 'symbolic',
  time_sig_numerator: 4, time_sig_denominator: 4, chords: [],
  notes: [{ nanotick: 0, duration: Q, pitch: 60, velocity: 100, column: 0, note_id: id }],
});
const fixture = {
  schema_version: 4, meta: { name: 'twotrack', created_utc: 0, modified_utc: 0 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }], harmony_timeline: [],
  clips: [clip(1, 'ONE'), clip(2, 'TWO')],
  tracks: [track(0, 1), track(1, 2)],
};

/** Every clip's name in the saved project, by clip id. */
const names = async (tag) => {
  await run(`save ${tag}`);
  for (let i = 0; i < 40; i++) {
    const p = join(stack.dir, `${tag}.uniproj.json`);
    if (existsSync(p)) {
      try {
        const doc = JSON.parse(readFileSync(p, 'utf8'));
        const out = {};
        for (const c of doc.clips || []) out[c.id] = c.name;
        if (Object.keys(out).length >= 2) return out;
      } catch { /* mid-write */ }
    }
    await settle(150);
  }
  return null;
};

console.log('\nrenaming a clip on a track you did not edit last\n');

writeFileSync(join(stack.dir, 'twotrack.uniproj.json'), JSON.stringify(fixture, null, 2));
await run('load twotrack');
await settle(2500);

const start = await names('tt_start');
check(start && start[1] === 'ONE' && start[2] === 'TWO',
      'two tracks, each with its own clip', JSON.stringify(start));

/* ── 1. TRACK 0, which is the control as much as the setup ───────────────────────────────── */
await run('cliptext 0 1 name FIRST');
await settle(1200);
const afterFirst = await names('tt_first');
check(afterFirst && afterFirst[1] === 'FIRST',
      'CONTROL: renaming a clip on track 0 lands',
      `${JSON.stringify(afterFirst)} — if this fails, SetClipText is broken outright and the `
      + 'assertion below would prove nothing');

/* ── 2. TRACK 1, whose counter the global one has now left behind ─────────────────────────── */
await run('cliptext 1 2 name SECOND');
await settle(1500);
const afterSecond = await names('tt_second');
check(afterSecond && afterSecond[2] === 'SECOND',
      'AND THEN RENAMING ONE ON TRACK 1 ALSO LANDS',
      `${JSON.stringify(afterSecond)} — the page stamped the GLOBAL clip version onto a command `
      + 'the engine arbitrates PER TRACK, so this was refused in silence on every track except '
      + 'the one edited last');

check(afterSecond && afterSecond[1] === 'FIRST',
      'and the first rename is still there', JSON.stringify(afterSecond));

/* ── 3. BACK TO TRACK 0, to prove it is not simply "the last one wins" ────────────────────── */
await run('cliptext 0 1 name THIRD');
await settle(1500);
const afterThird = await names('tt_third');
check(afterThird && afterThird[1] === 'THIRD' && afterThird[2] === 'SECOND',
      'and alternating back to track 0 lands too — both counters stay usable',
      JSON.stringify(afterThird));

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
