#!/usr/bin/env node
/**
 * TEN WAYS THE SAMPLER CAN REFUSE, AND NOT ONE OF THEM HAD EVER BEEN DRIVEN.
 *
 * `UiSamplerRejectPayload` carries a reason 1..10 and the page words all ten — no such track, no
 * such device, no slot, no mod set, no modulator, no source, no slice set, out of range, not a
 * sampler, would not load. Two suites mention `no_such_slot`, both in PROSE explaining a bug they
 * are about; neither asserts a refusal reaches anybody. So the whole surface is wired and untested.
 *
 * ── WHY THIS ONE MATTERS MORE THAN THE CLIP REASONS ─────────────────────────────────────────
 *
 * Because it is where the silence was worst, and index.html says so: every sampler verb refused
 * into the engine's LOG and nowhere else — twenty sites across seven commands — so from a browser
 * each one was a command that reported success and did nothing. One of the twenty was found only
 * because the sound kept playing: `slot 0` is not a wildcard for SamplerSetSlot, the engine
 * answered `no_such_slot`, and a half-second note ran the sample's full eight seconds while the
 * console said the command had worked.
 *
 * That is the failure mode being guarded: not a crash, not a wrong number on screen, but an
 * interface that agrees with you and does nothing.
 *
 * ── WHAT IS DRIVEN, AND WHY THESE FOUR ──────────────────────────────────────────────────────
 *
 *   1  no such track       a command naming a track that is not in the song
 *   2  no such device      a real track, a device id that is not on it
 *   3  no slot             a real sampler, a slot it does not have — the twentieth site above
 *   9  not a sampler       a real device that is a PATCHER, addressed as a sampler
 *
 * Reason 9 is the one with history: a load addressed to device 0 went to the patcher and looked
 * exactly like a sampler that had ignored it. The fixture therefore holds BOTH kinds on one track,
 * with non-consecutive ids (3 and 7) so that an off-by-one between "index" and "id" lands on
 * nothing rather than on the other device and quietly passing.
 *
 * The remaining six need a loaded sample, a chopped slice set or a mod set to reach, which is a
 * fixture apiece; they are left for when something touches that code. Four of ten asserted beats
 * ten of ten described.
 */

import { chromium } from 'playwright';
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const Q = 960000;
const SAMPLER_DEVICE = 3;
const PATCHER_DEVICE = 7;
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
const reject = () => page.evaluate(() => String(window.__uni.state().reject || ''));

/** One track, a SAMPLER and a PATCHER on it, ids deliberately not consecutive. */
writeFileSync(join(stack.dir, 'samprej.uniproj.json'), JSON.stringify({
  schema_version: 4, meta: { name: 'samprej', created_utc: 0, modified_utc: 0 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }], harmony_timeline: [],
  clips: [{ id: 1, name: 'C', length: Q * 4, lines_per_beat: 4, kind: 'symbolic',
            time_sig_numerator: 4, time_sig_denominator: 4, chords: [],
            notes: [{ nanotick: 0, duration: Q, pitch: 60, velocity: 100, column: 0, note_id: 1 }] }],
  tracks: [{
    track_id: 0, name: 'T', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
    device_chain: [
      { device_id: SAMPLER_DEVICE, kind: 'sampler', patcher_node_id: 0, bypass: false,
        sampler: { slots: [{ id: 1, name: 'a', key_low: 0, key_high: 127, root_key: 60, gate: 0 }] } },
      { device_id: PATCHER_DEVICE, kind: 'patcher_event', patcher_node_id: 0, bypass: false },
    ],
    mod_links: [],
    placements: [{ clip_id: 1, at: 0, length: Q * 4, notes: [], chords: [], mutes: [] }],
  }],
}, null, 2));

console.log('\nevery sampler refusal reaches the screen\n');

await run('load samprej');
await settle(2500);

/**
 * Send one raw samplerslot and return the reject line IT produced.
 *
 * WAITS FOR THE LINE TO CHANGE. `state.reject` persists until replaced, so a fixed sleep reads the
 * PREVIOUS case's message whenever this one is slower — which made a sibling of this file pass and
 * fail on unchanged code depending on machine load. Empty string on timeout, so a case that never
 * answered fails with its own story instead of inheriting the last one's.
 */
const refuse = async (over) => {
  const before = await reject();
  await page.evaluate((o) => window.__uni.send(Object.assign(
    // field 4 with a plain value: a field every sampler slot has, so the reason coming back is
    // about the ADDRESS being wrong and never about the field being exotic.
    { type: 'samplerslot', track: 0, device: 3, slot: 1, field: 4, value: 60 }, o)), over);
  try {
    await page.waitForFunction(
      (prev) => String((window.__uni.state() || {}).reject || '') !== prev,
      before, { timeout: 8000 });
  } catch { return ''; }
  return reject();
};

/* ── CONTROL FIRST: a well-formed command must NOT produce a refusal ───────────────────────
 * Every assertion below looks for a message. Without this, an app that refused everything would
 * satisfy all four of them.
 */
{
  const before = await reject();
  await page.evaluate(() => window.__uni.send(
    { type: 'samplerslot', track: 0, device: 3, slot: 1, field: 4, value: 62 }));
  await settle(1500);
  check((await reject()) === before,
        'CONTROL: a well-formed slot edit produces no refusal',
        `reject moved to ${JSON.stringify(await reject())} — if a good command is refused, the `
        + 'four checks below prove nothing about wrong ones');
}

const cases = [
  { name: 'no such TRACK',  over: { track: 60 },              want: /no such track/i,  reason: 1 },
  { name: 'no such DEVICE', over: { device: 99 },             want: /no such device/i, reason: 2 },
  { name: 'no such SLOT',   over: { slot: 99 },               want: /no slot/i,        reason: 3 },
  { name: 'NOT a sampler',  over: { device: PATCHER_DEVICE }, want: /not a sampler/i,  reason: 9 },
];

for (const c of cases) {
  const said = await refuse(c.over);
  console.log(`  reason ${c.reason} (${c.name}) said: ${JSON.stringify(said)}`);
  check(c.want.test(said),
        `${c.name.toUpperCase()} IS REPORTED, not swallowed`,
        `${JSON.stringify(said)} — this is the shape that made twenty sampler refusals invisible: `
        + 'the command reports success and nothing happens');
}

/* ── AND THE APP STILL WORKS ─────────────────────────────────────────────────────────────── */
{
  const before = await reject();
  await page.evaluate(() => window.__uni.send(
    { type: 'samplerslot', track: 0, device: 3, slot: 1, field: 4, value: 64 }));
  await settle(1500);
  check((await reject()) === before,
        'a good command after four refusals is still accepted silently',
        `reject moved to ${JSON.stringify(await reject())} — reporting an error must not become one`);
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
