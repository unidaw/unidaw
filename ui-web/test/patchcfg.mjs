/**
 * Turning a patcher node's knob changes the SOUND.
 *
 * Reported twice — "I'm editing T1 and T2's patchers Euclidean settings, nothing
 * seems to change", then "I was editing T1's patcher's Euclidean and I don't
 * think it had any effect". The first cause was the unscoped patcher view:
 * rack.uni has a generator on track 0 and on no other track, so the view showed
 * track 0's euclidean while sitting on track 1, and every edit landed on a graph
 * feeding a track nobody was listening to. That is fixed.
 *
 * But relocating a control does not prove it WORKS, and "the knob is in the right
 * place now and still does nothing" is the same complaint with a longer path to
 * it. The evidence up to here was a read-back — the engine publishing the number
 * back — which proves the command was parsed and stored and proves nothing about
 * whether the audio thread ever reads it. A UI mirror that round-trips perfectly
 * and never reaches the DSP is exactly the bug being claimed. Writing this found
 * four knobs where that is literally true (see DEAD FIELDS below).
 *
 * So this asserts on audio, four phases against one capture:
 *
 *   A  the preset as authored           -> sound          (non-vacuity)
 *   B  random_degree vel 1              -> nearly silent
 *   C  random_degree vel 127            -> loud again
 *   D  euclidean hits 1                 -> sparse
 *
 * C is what makes it an argument rather than a way of breaking the engine: the
 * sound has to COME BACK when the knob comes back, which no crash, no stuck voice
 * and no dead plugin can fake. D turns a different FIELD on a different NODE, so
 * one lucky code path cannot carry the whole file. A is the non-vacuity check —
 * without it everything here passes on a run where Zebralette never loaded.
 *
 * WHY THESE TWO KNOBS AND NOT THE OBVIOUS ONE. `hits 0` looks like the perfect
 * test — no hits, no notes, silence — and it is not: 0 is an UNSET SENTINEL in
 * patcher_rust, so the engine substitutes the default and plays five. Measured,
 * before believing it: hits 5 -> rms 0.033, hits 0 -> 0.021, and the difference
 * is the random pitches, not the knob.
 *
 * LEVEL, NOT ONSETS. A long release smears every note into the next, so density
 * read 0.96 at five notes a bar and 1.00 at sixteen — a change you can see in the
 * envelope and cannot measure by counting attacks. Velocity moves the level 6x
 * and does not care about the patch's tail.
 *
 * `generator` is the fixture because it is ONE track — euclidean -> random_degree
 * -> event_out -> Zebralette, one written note. On `maximal` five other Zebras
 * would be playing over the answer.
 *
 * The windows come from stack.captureOffset rather than from guessing how long
 * setup took: plugin scanning and opening the audio device dominate that and vary
 * by seconds between machines, and a window two seconds out reads the wrong phase
 * and reports a working feature broken.
 *
 * DEAD FIELDS, measured here and reported to backend — euclidean's `degree`,
 * `oct`, `vel` and `base` never reach the DSP. patcher_process_euclidean reads
 * steps/hits/offset/duration and emits a payload with degree, velocity,
 * base_octave and octave_offset hard-zeroed, though all four are in the ABI
 * struct and all four round-trip through the UI. Turning `base` from 4 to 9 —
 * nine octaves — moved the capture's zero-crossing rate from 1430/s to 1685/s,
 * which is noise; the control knob in the same run moved rms 5.6x. They are not
 * asserted on here because they are backend's to wire, and a test that asserts
 * the current behaviour would have to be deleted when they do.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { readWav, rmsBetween, loudFraction } from './wav.mjs';

const WAV = '/tmp/patchcfg_check.wav';
/** The engine's whole life. Everything below is placed inside it. */
const RUN = 70;
const KEEP = 55;
/** How long each phase holds. */
const PHASE = 9_000;
/** When the first phase starts, after the audio device opens. */
const PLAY_AT = 24_000;

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

// numBlocks 8 for audible.mjs's reason: a box also running Chrome and a plugin
// host starves the producer, and a starved run captures silence — which here is
// indistinguishable from the thing under test.
const stack = await startStack({ capture: WAV, captureSeconds: KEEP, runSeconds: RUN,
                                 numBlocks: 8, keepDir: !!process.env.KEEP_DIR });
const t0 = stack.audioStartedAt;
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1000);
await page.evaluate(() => window.__uni.loadProject('generator'));
// A NAMED plugin. The engine puts a stand-in on a track while it resolves the
// real one, and a stand-in makes no sound — which would read as "the knob
// silenced it" in every phase.
await page.waitForFunction(
  () => JSON.stringify(window.__uni.chainProbe() || '').includes('Zebra'),
  null, { timeout: 30000 }).catch(() => {});

/** The two generator nodes, by TYPE — a pooled node id is not its authored one. */
const graph = await page.evaluate(() => {
  const p = window.__uni.patcher();
  const by = (t) => (p ? p.nodes.find((n) => n.type === t) : null);
  const e = by(1), r = by(5);
  return e && r ? { euclidean: e.id, random: r.id, hits: e.cfg[1], vel: r.cfg[1] } : null;
});
check(!!graph, 'the project has a euclidean and a random_degree to turn',
      JSON.stringify(graph));
if (!graph) { await browser.close(); stack.stop(); process.exit(1); }

/**
 * Move one field to `want`, through the same function the arrow keys call, and
 * return what the ENGINE says afterwards.
 *
 * The count is computed once and the answer waited for, rather than nudging until
 * the read-back agrees. The read-back cannot agree inside a synchronous loop:
 * `patcher()` reports the engine's last published frame, the app holds the
 * un-acknowledged value in `patchPending`, and a loop that re-reads `patcher()`
 * sees the same stale number every iteration. The first version did exactly that
 * — eighty commands in a burst to move a value five steps, then reporting the
 * stale 5 it started from.
 */
const set = (id, field, want) => page.evaluate(({ id, field, want }) => {
  const u = window.__uni;
  u.patchSelect(id); u.patchField(field);
  const at = () => (u.patcher().nodes.find((n) => n.id === id) || { cfg: [] }).cfg[field];
  const from = at();
  const dir = want > from ? 1 : -1;
  // One nudge per step, because the limit table owns the step size and a test
  // that hardcodes it drifts the day that table changes.
  for (let i = 0; i < Math.abs(want - from); i++) if (!u.patchNudge(dir)) break;
  return new Promise((resolve) => {
    const started = Date.now();
    const poll = () => {
      if (at() === want || Date.now() - started > 3000) return resolve(at());
      setTimeout(poll, 30);
    };
    poll();
  });
}, { id, field, want });

const ready = Date.now() - t0;
if (ready > PLAY_AT - 2000) {
  // Loudly, because the alternative is windows that silently point at the wrong
  // phases and a green run that means nothing.
  check(false, 'setup finished in time to place the phases', `${(ready / 1000).toFixed(1)}s`);
}

const waitUntil = async (ms) => {
  const left = t0 + ms - Date.now();
  if (left > 0) await page.waitForTimeout(left);
};

// ---- A: the preset as authored. ------------------------------------------
await waitUntil(PLAY_AT - 1500);
await page.keyboard.press(' ');
const aAt = PLAY_AT;

// ---- B: the quietest velocity the control allows. ------------------------
await waitUntil(aAt + PHASE);
const bAt = aAt + PHASE;
check(await set(graph.random, 1, 1) === 1, 'the engine published velocity back as 1');

// ---- C: the loudest. -----------------------------------------------------
await waitUntil(bAt + PHASE);
const cAt = bAt + PHASE;
check(await set(graph.random, 1, 127) === 127, 'and back as 127');

// ---- D: a different field on a different node. ---------------------------
await waitUntil(cAt + PHASE);
const dAt = cAt + PHASE;
check(await set(graph.euclidean, 1, 1) === 1, 'and the euclidean back at one hit');

await waitUntil(dAt + PHASE - 500);
await browser.close();

// Let the engine reach its own end and write the file: the WAV appears when the
// capture window CLOSES, and a SIGTERM on the way out skips that entirely.
await new Promise((r) => setTimeout(r, Math.max(0, t0 + RUN * 1000 + 2500 - Date.now())));
stack.stop();

const at = (ms) => stack.captureOffset(t0 + ms);
let wav = null;
try { wav = readWav(WAV); } catch (e) { check(false, 'a capture was produced', e.message); }

if (wav) {
  const { mono, rate } = wav;
  // Each window sits inside its phase, 4s clear of the front for whatever was
  // already sounding when the knob moved and 0.5s clear of the back.
  const win = (start) => [at(start + 4000), at(start + PHASE - 500)];
  const level = (start) => rmsBetween(mono, rate, ...win(start));
  const dens = (start) => loudFraction(mono, rate, ...win(start));
  const [a, b, c, d] = [aAt, bAt, cAt, dAt].map(level);
  const [, , cD, dD] = [aAt, bAt, cAt, dAt].map(dens);
  console.log(`  preset ${a.toFixed(5)}   vel 1 ${b.toFixed(5)}   vel 127 ${c.toFixed(5)}` +
              `   hits 1 ${d.toFixed(5)}`);
  console.log(`  density at vel 127 ${cD.toFixed(2)}, at one hit ${dD.toFixed(2)}` +
              `   (capture ${(mono.length / rate).toFixed(1)}s, phase A at ${win(aAt)[0].toFixed(1)}s)`);

  check(a > 0.005, 'the generator was sounding to begin with', a.toFixed(5));
  check(b < a * 0.5, 'the quietest velocity made it quiet',
        `${b.toFixed(5)} vs ${a.toFixed(5)}`);
  check(c > b * 3, 'and the loudest brought it back',
        `${(c / Math.max(b, 1e-9)).toFixed(1)}x`);
  // A different node and a different field, so one working path cannot carry it.
  check(d < c * 0.5 && dD < cD - 0.25, 'one hit a bar is sparser than sixteen',
        `rms ${d.toFixed(5)} vs ${c.toFixed(5)}, density ${dD.toFixed(2)} vs ${cD.toFixed(2)}`);
}

console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
