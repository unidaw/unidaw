#!/usr/bin/env node
/**
 * DOES ADDING A PLUGIN TO A RUNNING ENGINE LEAVE IT ABLE TO PLAY?
 *
 * Found in about ninety seconds of using the app, with every suite green behind it. Adding
 * Zebralette to a track made the song go silent — not quietly wrong, SILENT — and it stayed
 * that way. The description of the recovery is the diagnosis: "the zebralette silence
 * eventually came back with a flood", which is a producer that fell behind realtime and finally
 * dumped its backlog all at once.
 *
 * WHY NOTHING CAUGHT IT, which is the more useful half.
 *
 * `audible.mjs` already loads Zebralette. Six real hosts, the real Zebra2.vst3, and it passes.
 * The difference is WHEN: it names the plugin in the project, so the hosts launch during load,
 * everything settles, and only then does anything play. Every other suite does the same. Not one
 * of them adds a plugin to an engine that is ALREADY UP, which is the only way a person ever
 * does it.
 *
 * The reason no suite did is not an oversight about coverage: inserting a scanned plugin is
 * reachable ONLY by clicking a row in the browser rail — there is no console verb for it — and
 * every suite in this directory drives the console. An operation no test can express is an
 * operation no test covers, however many tests there are.
 *
 * So this one drives the pointer. That also makes "Zebralette and not Zebra2" expressible:
 * both are inside Zebra2.vst3, and choosing by NAME from the engine's scan is the only thing
 * that tells them apart.
 *
 * STATUS: THE BUG IT WAS WRITTEN FOR IS FIXED, AND THIS IS NOW THE GUARD.
 *
 * The cause was a producer DEADLOCK, not pacing. Loading a VST holds that track's
 * controllerMutex across a blocking round-trip; produceBlock try_locks it, fails and RETURNS
 * without sending; the track then rejoins back-pressure carrying a stale block id further behind
 * than numBlocks, and `inFlight >= numBlocks` makes the producer skip dispatching to EVERYONE.
 * The host can never catch up, because the blocks it is missing are exactly the ones the closed
 * gate prevents being sent. Back-pressure now asks "do you still owe me work" instead of "how far
 * along are you".
 *
 * Re-measured on this reproduction after the fix: 0 dropouts through 18 seconds with the
 * transport still running, where it used to go 0 -> 16 -> 551 -> 1067.
 *
 * WHAT THAT MEANS FOR THIS FILE. It was committed deliberately weaker than the reported case —
 * silent control track, empty target chain, one host — with those gaps written down. Now that
 * the cause is known they are the wrong gaps to close: the deadlock does not need a SOUNDING
 * track, it needs a plugin inserted into a RUNNING transport, which is exactly what this does.
 * The assertion below is unchanged and is the right one either way.
 *
 * WHAT IT ASSERTS, and why this shape.
 *
 * The engine reports dropouts as a running total: "(2728 total, worst shortfall 1427 blocks)".
 * A burst at the moment a plugin is instantiated is defensible — a VST constructor is not
 * realtime-safe and nobody claims otherwise. What is NOT defensible is that the total keeps
 * climbing forever afterwards, because the producer's loop can never run faster than realtime and
 * therefore can never repay the debt.
 *
 * So the measurement is a DERIVATIVE, not a level: let it settle, read the total, wait, read it
 * again. Equal means the engine recovered. Growing means every callback is still starving, which
 * is silence, which is the bug. Asserting "zero dropouts" instead would fail on the honest
 * instantiation transient and would have to be excused — and an excused check is one nobody
 * believes the day it matters (see tests-that-verify-nothing, and audio-validation-loop on why an
 * excuse is only safe when the second signal is INDEPENDENT of the failure mode).
 */

import { chromium } from 'playwright';
import { join } from 'node:path';
import { readFileSync, unlinkSync, writeFileSync } from 'node:fs';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0, blocked = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};
const block = (what, why) => { blocked++; console.log('  BLOCKED ', what, `— ${why}`); };

/*
 * The plugin this reaches for. A REAL out-of-process host is the whole point: the in-process
 * Identity fixture is swapped in for anything named Identity.vst3 (apps/host_controller.cpp) and
 * costs nothing to instantiate, so it cannot reproduce a stall caused by instantiation.
 */
const PLUGIN = 'Zebralette';

const CAPTURE_SECONDS = 40;
const WAV = join(process.cwd(), 'ui-web/test/out/live-plugin-add.wav');
try { unlinkSync(WAV); } catch { /* nothing to remove is the normal case */ }

const stack = await startStack({ keepDir: true, capture: WAV, captureSeconds: CAPTURE_SECONDS,
                                 runSeconds: CAPTURE_SECONDS + 14, numBlocks: 8 });

const ENGINE_LOG = join(stack.root, 'engine.log');
/**
 * The engine's cumulative dropout count, or 0 before it has reported any.
 *
 * Reads the LAST total in the file rather than counting report lines: the reports are periodic
 * and a count of them measures how long the run was, not how bad it was.
 */
const dropouts = () => {
  let text = '';
  try { text = readFileSync(ENGINE_LOG, 'utf8'); } catch { return 0; }
  const all = [...text.matchAll(/\((\d+) total,/g)];
  return all.length ? Number(all[all.length - 1][1]) : 0;
};

const Q = 960000;
writeFileSync(`${stack.dir}/live.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'live', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [],
  clips: [{ id: 1, name: 'live', length: Q * 240, lines_per_beat: 4, kind: 'symbolic',
            time_sig_numerator: 4, time_sig_denominator: 4, notes: [], chords: [] }],
  // Two tracks and both chains EMPTY: track 0 gets the sampler that makes the control sound,
  // track 1 gets the plugin added mid-run. Empty so each device this test adds is device 0 on
  // its own track, which is the id every command below passes.
  tracks: [
    { track_id: 0, name: 'Control', harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
      device_chain: [], mod_links: [],
      placements: [{ clip_id: 1, at: 0, length: Q * 240, notes: [], chords: [], mutes: [] }] },
    { track_id: 1, name: 'Target', harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
      device_chain: [], mod_links: [],
      placements: [] },
  ],
}));

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 20000 }).catch(() => {});
await page.waitForTimeout(1500);

const said = [];
const run = async (c) => { const r = await page.evaluate((x) => window.__uni.run(x), c);
                           said.push(`${c} -> ${r}`); return r; };

console.log('\nadding a plugin to a RUNNING engine\n');

await run('load live');
await page.waitForTimeout(1200);

/*
 * A CONTROL THAT SOUNDS, on a different track from the one the plugin lands on.
 *
 * Without it a silent capture cannot be told apart from a machine that was never going to make
 * noise, and this suite's whole claim is about audio DISAPPEARING — which is unprovable if it was
 * never there. Same reason audio-validation-loop insists on a control in the SAME capture.
 */
await run('sampler');
await page.waitForTimeout(800);
await run('note 36');
await run('note 40');
await page.waitForTimeout(400);

await run('goto 0');
await run('play');
await page.waitForTimeout(3000);

// Settle, then take the baseline. Anything the engine dropped getting to this point is startup,
// and startup is not what this measures.
const before = dropouts();

/*
 * THE OPERATION UNDER TEST. Everything above is setup; this is the thing a person does.
 *
 * Through the RAIL, which is the only way to insert a scanned plugin — there is no console
 * verb for it, and that absence is why no suite had ever performed this operation. Driving
 * the pointer also means the name is resolved through the catalogue, which is what makes
 * "Zebralette and not Zebra2" expressible at all: both live in Zebra2.vst3, and choosing by
 * NAME from the scan is the difference between the two.
 */
await run('goto 0 1');                     // row 0, TRACK 1 — the rail inserts into the cursor's track
await page.waitForTimeout(300);
const added = await page.evaluate(async (name) => {
  const rail = document.querySelector('.br');
  const open = !!rail && rail.offsetParent !== null;
  if (!open) {
    // ⌘B is the app's own binding; dispatching it is what a person's hand does.
    document.dispatchEvent(new KeyboardEvent('keydown',
      { key: 'b', metaKey: true, bubbles: true }));
  }
  await new Promise((r) => setTimeout(r, 500));
  const chip = document.querySelector('.br-chip[data-cat="plug"]');
  if (!chip || chip.disabled) return 'the PLUGINS category is not available';
  chip.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
  await new Promise((r) => setTimeout(r, 400));
  const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
  const row = rows.find((el) => (el.textContent || '').toLowerCase().includes(name.toLowerCase()));
  if (!row) return `no ${name} row in the catalogue on this machine`;
  row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
  return true;
}, PLUGIN);
said.push(`insert ${PLUGIN} from the rail -> ${JSON.stringify(added)}`);

const reachable = added === true;
if (!reachable) {
  /*
   * NOT A FAILURE OF THE ENGINE. If the console cannot express the insert, or this machine has no
   * Zebralette, the question below is unanswerable — and reporting it as "the engine starves"
   * would be a green-check-on-the-wrong-object of the kind that costs days.
   */
  block('a scanned plugin can be inserted from the rail',
        `${added} — is ${PLUGIN} scanned on this machine?`);
} else {
  check(true, 'a scanned plugin can be inserted from the rail');
}

if (reachable) {
  // The host has to launch and the chain has to come back before any of this means anything.
  await page.waitForFunction(() => {
    const c = window.__uni.state && window.__uni.state();
    const ch = c && c.chains && c.chains[1];
    return !!(ch && ch.devices && ch.devices.length);
  }, null, { timeout: 30000 }).catch(() => {});

  // Generous: a VST constructor, a host process and a parameter query. The transient is allowed.
  await page.waitForTimeout(8000);

  const settled = dropouts();
  await page.waitForTimeout(6000);
  const later = dropouts();

  /*
   * THE ASSERTION. Not "no dropouts" — "no dropouts STILL ACCRUING". A producer that recovered
   * has a flat total; one that fell behind permanently climbs by roughly one per callback, so at
   * 44100/512 a healthy six seconds adds 0 and a starved one adds ~500.
   */
  check(later === settled,
        'the engine recovers after the plugin is inserted (dropout total stops growing)',
        `dropouts went ${before} -> ${settled} -> ${later}; still climbing means every callback `
        + 'is starving, which is silence — see engine_consumer.cpp and the producer pacing loop');

  check(errors.length === 0, 'no page errors while inserting a plugin', errors.join(' | '));
}

await run('stop');
await browser.close();
await stack.stop();

if (!stack.audioRunning) {
  console.log('\n  NOTE: the audio device never started, so the dropout numbers above describe '
            + 'a run nothing was pulling — see daw_audio_probe.');
}

console.log('\n  what the console said:');
for (const s of said) console.log('   ', s);

const note = blocked ? ` · ${blocked} BLOCKED` : '';
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}${note}\n`);
process.exit(fail ? 1 : 0);
