#!/usr/bin/env node
/**
 * LOAD A SAMPLE, PLAY A NOTE, HEAR IT. The first thirty seconds of any demo.
 *
 * It did not work. `loadSample` defaulted to root 36 with FIXED PITCH, which the engine turns
 * into `keyLow = keyHigh = 36` — a slot that answers exactly one key, C-2. The tracker's
 * default octave writes 48. So the whole gesture a person performs first — put a sampler on a
 * track, give it a file, play a note — produced silence, and nothing said why.
 *
 * Every existing suite worked around it rather than tripping over it, which is why it survived:
 * full-song.mjs reads back the pitches the keyboard wrote and MOVES THE SLOT onto them, in
 * three commands, with a comment explaining that it must. A workaround written once is a
 * workaround; written in every suite that touches the sampler it is the product's behaviour.
 *
 * THE ENGINE WAS ALREADY RIGHT. Not-fixed-pitch mints `keyLow = 0, keyHigh = 127` — the whole
 * keyboard, transposing from the root. The pinning was entirely this side's default, and the
 * command has carried both spellings since it existed.
 *
 * WHAT THIS ASSERTS, and it has to be the audio: a kit read-back showing keyLow 0 / keyHigh 127
 * proves the mapping and not that anything sounds. The render is the only thing that answers
 * "would the demo have made a noise".
 *
 * PINNING IS STILL AVAILABLE AND STILL RIGHT FOR DRUMS — `slot <t> <d> <s> keylow/keyhigh` sets
 * it, and a CHOP mints one slot per key regardless of this default, which is where a drum kit
 * comes from. This is about the lone sample, which is the melodic case and the demo case.
 */

import { chromium } from 'playwright';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startStack } from './stack.mjs';
import { readWav, envelope } from './wav.mjs';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));
let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const SONG = 'smpdef';
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

console.log('\nload a sample, play a note\n');

await run(`new ${SONG}`);
await settle(1300);

// ---------------------------------------------------------------------------
// THE POINTER PATH, because that is the one a person takes on stage: the DEVICES
// rail for the sampler, the SAMPLES rail for the file. No `slot` commands, no
// remapping — if this needs help to make a sound, it has not been fixed.
// ---------------------------------------------------------------------------
const pick = async (cat, want) => {
  const open = await page.evaluate(() => {
    const r = document.querySelector('.br');
    return !!r && r.offsetParent !== null && getComputedStyle(r).display !== 'none';
  });
  if (!open) await page.keyboard.press('Meta+b');
  await settle(500);
  return page.evaluate(async ([c, w]) => {
    const chip = document.querySelector(`.br-chip[data-cat="${c}"]`);
    if (!chip || chip.disabled) return `${c} unavailable`;
    chip.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    await new Promise((r) => setTimeout(r, 400));
    const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
    const row = rows.find((el) => (el.textContent || '').toLowerCase().includes(w.toLowerCase()));
    if (!row) return `no "${w}" in ${c}`;
    row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    return true;
  }, [cat, want]);
};

check(await pick('devs', 'sampler') === true, 'a sampler goes on the track from the rail');
/*
 * AND IT ACTUALLY ARRIVED — because `pick` returning true does not mean it did.
 *
 * `pick` finds a row and dispatches a pointerdown; `true` is "I dispatched an event". Whether the
 * app acted on it is a different question, and the check above cannot fail for the reason its
 * name gives. That is not theoretical: sampler-default flaked in sweep 26, and the failing run's
 * engine log has NO sampler.kit_published at all, no sampler.loaded and no render_built — six
 * lines that its passing retry has. The device add never reached the engine, while "a sampler
 * goes on the track from the rail" reported PASS, and the first visible failure was "the engine
 * answers with a kit — no kit arrived" thirty lines later.
 *
 * So wait for the device to exist, and say so here if it does not. A dispatched event is not an
 * outcome.
 */
const samplerArrived = await page.waitForFunction(() => {
  const c = window.__uni.chainProbe();
  return !!(c && c.titles && c.titles.some((t) => /sampler/i.test(String(t))));
}, null, { timeout: 15000, polling: 500 }).then(() => true).catch(() => false);
check(samplerArrived,
      'and the chain really holds a sampler, not just a click that was dispatched',
      'no sampler in the chain within 15s — the rail row was clicked and nothing came of it, '
      + 'which is what made this suite fail later as "no kit arrived"');
/*
 * A MUSICAL ONE-SHOT, NOT THE PROBE ASSET. `demo_pluck_c4.wav` is middle C with its attack in
 * the first millisecond (tools/make_demo_samples.py). The probe files that used to be the only
 * thing in this directory are silent for their first second, which the sampler STRETCHES when
 * transposing down — and this test plays 48 against a root of 60, an octave below, so the probe
 * would be mute for two seconds under notes that are barely longer than that. Every reading here
 * would have been about the asset.
 */
check(await pick('smpl', 'demo_pluck') === true, 'and a sample goes into it from the rail');
await settle(2500);

// ---------------------------------------------------------------------------
// The mapping the engine minted. Read back, because "it sounds" without this
// would not say WHY, and the next person would have to find out again.
// ---------------------------------------------------------------------------
const kit = await page.waitForFunction(() => {
  const t = window.__uni.state().cursor.track;
  for (let d = 0; d < 6; d++) {
    const k = window.__uni.samplerKitCached(t, d);
    if (k && k.slots && k.slots.length) return k;
  }
  return null;
}, null, { timeout: 25000 }).then((h) => h.jsonValue()).catch(() => null);

check(!!kit, 'the engine answers with a kit', 'no kit arrived');
if (kit) {
  const s = kit.slots[0];
  check(s.keyHigh > s.keyLow,
        'the slot answers a RANGE of keys, not one — this is the fix',
        `keyLow ${s.keyLow}, keyHigh ${s.keyHigh}, root ${s.root}`);
  /*
   * AND THE TRACKER'S OWN DEFAULT OCTAVE IS INSIDE IT. A range that happened to miss the
   * keys a person types would satisfy the check above and fail the demo — the whole defect
   * was a mapping that was perfectly consistent and nowhere near the keyboard.
   */
  check(s.keyLow <= 48 && s.keyHigh >= 48,
        "and it covers the octave the tracker types in — 48 is `z` at the default `oct 4`",
        `keyLow ${s.keyLow}, keyHigh ${s.keyHigh}`);
}

// ---------------------------------------------------------------------------
// NOW THE ONLY QUESTION THAT MATTERS. Notes placed where the sample actually makes
// sound, so this measures the key mapping and not the asset's silent opening.
// ---------------------------------------------------------------------------
/*
 * SPACED IN TICKS, NOT IN ROWS. `goto` counts DISPLAYED rows and how many nanoticks a row
 * spans depends on the ZOOM, so "rows 2, 4, 6" is a different stretch of time at every zoom
 * level. The ticks are asserted below rather than assumed.
 */
for (const row of [4, 8, 12, 16]) {
  await run(`goto ${row} 0`);
  await run('note 48');
  await settle(250);
}
await settle(900);
/*
 * READ WHERE THE FIRST NOTE IS. DO NOT ASSUME IT.
 *
 * `goto 4` does not mean "one beat in": a row is a DISPLAYED row and how much time one spans
 * depends on the zoom, so the four notes below landed on quarters 4/8/12/16 rather than the
 * 1/2/3/4 the row numbers suggest. Assuming the first was at 0.5s made the render look two
 * seconds late, and the phantom shift survived everything: constant across playback rates,
 * scaling with tempo, indifferent to the time signature. I built five fixtures chasing an engine
 * bug that was my own arithmetic.
 *
 * These ticks are SONG-ABSOLUTE, which is worth stating because the near-miss fix was to add the
 * placement's `at` — and a placement begins at its first note, so `at` EQUALS the first note's
 * tick and adding it doubles the offset. Both readings predict the same time for the first note
 * on this fixture, so the fixture cannot tell them apart; the saved project can, and does.
 */
const ticks = await page.evaluate(() =>
  (window.__uni.notes() || []).map((x) => x.on ?? x.t ?? x.tOn).sort((a, b) => a - b));
check(ticks.length === 4 && ticks[ticks.length - 1] >= 960000,
      'the notes are spread over real time, not bunched at the start',
      `ticks ${JSON.stringify(ticks)}`);
await run(`save ${SONG}`);
await settle(1800);

let wav = null;
try {
  const out = join(stack.dir, 'take.wav');
  try { unlinkSync(out); } catch { /* absent is normal */ }
  execFileSync(join(ROOT, 'build', 'daw_engine'),
               ['--project', SONG, '--render', 'take', '--run-seconds', '8'],
               { cwd: join(ROOT, 'build'),
                 env: { ...process.env, DAW_PROJECT_DIR: stack.dir,
                        DAW_HOST_BINARY: join(ROOT, 'build', 'juce_host_process'),
                        DAW_UI_SHM_NAME: `/smpdef_${process.pid}` },
                 stdio: ['ignore', 'pipe', 'pipe'], timeout: 120000 });
  wav = existsSync(out) ? readFileSync(out) : null;
} catch (e) { check(false, 'the render runs', String(e).slice(0, 180)); }

check(wav && wav.length > 44, 'the song renders', wav ? `${wav.length} bytes` : 'nothing');
if (wav) {
  const w = readWav(join(stack.dir, 'take.wav'));
  const per = 0.05;
  const env = envelope(w.mono, w.rate, per);
  const pk = env.reduce((m, v) => Math.max(m, v), 0);
  check(pk > 0.001,
        'A SAMPLE LOADED FROM THE RAIL SOUNDS WHEN YOU TYPE A NOTE — no remapping',
        `peak ${pk.toFixed(4)} — silence here means the slot is pinned away from the keyboard again`);

  /*
   * AND IT SOUNDS *AT* THE NOTE, NOT A SECOND AFTER IT.
   *
   * A peak somewhere in eight seconds is a weak claim: it is satisfied by audio arriving
   * whenever, and that is exactly what the old probe asset produced — silence for its first
   * second, stretched by any downward transposition — which got reported as an engine fault
   * twice. `demo_pluck_c4.wav` has its attack in the first millisecond, so lateness here means
   * the sampler and not the file.
   *
   * Asserted against the SONG, which only a render permits: sample zero IS song zero, so there
   * is no origin to locate. A device capture starts when the DEVICE does and could not carry
   * this claim at all.
   */
  /*
   * NEGATIVE CONTROL, RUN: swapping this suite back to `waveform_probe` leaves the peak check
   * above PASSING and fails only this one — "first sound at 4.00s, first note at 2.00s". Two
   * seconds, which is the asset's one silent second doubled by the octave-down transposition.
   *
   * So the check has teeth, and the check it replaces demonstrably did not: a peak anywhere in
   * eight seconds is satisfied by the exact defect this file exists to catch.
   */
  const FLOOR = 0.004;
  const Q = 960000, SEC_PER_TICK = 0.5 / Q;         // 120bpm: a quarter is half a second
  const expected = ticks[0] * SEC_PER_TICK;
  const firstLoud = env.findIndex((v) => v > FLOOR);
  const heard = firstLoud < 0 ? -1 : firstLoud * per;
  check(heard >= 0 && Math.abs(heard - expected) < 0.3,
        'and it sounds AT the note rather than a second late — the attack is immediate',
        `first sound at ${heard.toFixed(2)}s, first note at ${expected.toFixed(2)}s ` +
        `(tick ${ticks[0]})`);
}

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
