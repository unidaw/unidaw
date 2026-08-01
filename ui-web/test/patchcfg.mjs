/**
 * Turning a patcher node's knob changes the SOUND.
 *
 * Reported twice — "I'm editing T1 and T2's patchers Euclidean settings, nothing
 * seems to change", then "I was editing T1's patcher's Euclidean and I don't
 * think it had any effect". Part of that was the unscoped patcher view, which is
 * fixed. Part of it was that FOUR OF THE SEVEN KNOBS ON THAT NODE DID NOTHING,
 * which this file found and which backend has since fixed.
 *
 * The reason it went unnoticed is the reason this file asserts on audio: the only
 * evidence anyone had was a read-back — the engine publishing the number back —
 * which proves the command was parsed and stored and proves nothing about whether
 * the audio thread ever reads it. degree, oct, vel and base round-tripped
 * perfectly through the wire, the UI and the read-back, and
 * patcher_process_euclidean emitted a payload with all four hard-zeroed. A UI
 * mirror that round-trips and never reaches the DSP is invisible to every check
 * that is not a microphone.
 *
 * Five phases against one capture, chosen so that no single mechanism carries the
 * file — two different nodes, four different fields, and both a LEVEL claim and a
 * PITCH claim, which fail in different ways:
 *
 *   A  the preset as authored      -> sound                    (non-vacuity)
 *   B  random_degree vel 1         -> nearly silent            level
 *   C  random_degree vel 127       -> loud again               level, and RETURNS
 *   D  euclidean base 9            -> a much higher register   pitch
 *   E  euclidean hits 0            -> silence                  the sentinel
 *
 * C is what makes B an argument rather than a way of breaking the engine: sound
 * has to COME BACK when the knob comes back, which no crash, no stuck voice and
 * no dead plugin can fake. D is the one that was dead — base is euclidean's own
 * octave and nine octaves is not a subtle claim. E pins a semantic that changed
 * underneath this file: `hits 0` used to be an UNSET sentinel that made the
 * engine substitute the default and play five, so the box said 0 while the
 * generator ran. Backend made 0 mean zero, which is also the natural spelling of
 * "this generator is in the graph and emitting nothing" — a state we had no way
 * to express.
 *
 * LEVEL AND PITCH, NOT ONSETS. A long release smears every note into the next, so
 * density read 0.96 at five notes a bar and 1.00 at sixteen — a change you can
 * SEE in the envelope and cannot measure by counting attacks. Twelve parameter
 * pairs of onset detector never beat 1.6x separation on a 3x density change.
 * Velocity moves the level 10x and does not care about the tail; the octave moves
 * the zero-crossing rate and does not care about the level.
 *
 * `generator` is the fixture because it is ONE track — euclidean -> random_degree
 * -> event_out -> Zebralette, one written note. On `maximal` five other Zebras
 * would be playing over the answer.
 *
 * The windows come from stack.captureOffset rather than from guessing how long
 * setup took: plugin scanning and opening the audio device dominate that and vary
 * by seconds between machines, and a window two seconds out reads the wrong phase
 * and reports a working feature broken.
 */

import { chromium } from 'playwright';
import { startStack, soundGate } from './stack.mjs';
import { readWav, rmsBetween, zeroCrossingRate, loudFraction } from './wav.mjs';

const WAV = '/tmp/patchcfg_check.wav';
/** The engine's whole life. Everything below is placed inside it. */
const RUN = 80;
const KEEP = 64;
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

/*
 * PAGE ERRORS ARE A RESULT, not noise.
 *
 * This suite drove a browser and ignored whatever it threw. That is how a readout shipped
 * raising `ticksPerBeat is not defined` on every frame — a missing import in index.html, which
 * no unit test can see because none of them run the draw path, and which the suites that DO
 * listen missed because none of them visited the field that used it.
 *
 * A listener is necessary and not sufficient: it only catches what this suite's own path
 * touches. That is still strictly more than nothing, and it costs two lines.
 */
const pageErrors = [];
page.on('pageerror', (e) => { if (!pageErrors.includes(e.message)) pageErrors.push(e.message); });

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
  return e && r ? { euclidean: e.id, random: r.id, base: e.cfg[6] } : null;
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

// ---- D: euclidean's own octave — one of the four that used to be dead. ---
await waitUntil(cAt + PHASE);
const dAt = cAt + PHASE;
check(await set(graph.euclidean, 6, 9) === 9, 'and the euclidean at base octave 9');

// ---- E: no hits. ---------------------------------------------------------
await waitUntil(dAt + PHASE);
const eAt = dAt + PHASE;
check(await set(graph.euclidean, 1, 0) === 0, 'and at zero hits');

await waitUntil(eAt + PHASE - 500);
check(pageErrors.length === 0, 'nothing threw in the browser while this ran',
      pageErrors.slice(0, 3).join(' | '));

await browser.close();

// Let the engine reach its own end and write the file: the WAV appears when the
// capture window CLOSES, and a SIGTERM on the way out skips that entirely.
await new Promise((r) => setTimeout(r, Math.max(0, t0 + RUN * 1000 + 2500 - Date.now())));
stack.stop();

const at = (ms) => stack.captureOffset(t0 + ms);
/* See stack.mjs's soundGate: with no device there is no capture, and every question below
   is unanswerable rather than answered "no". This suite read a 39-hour-old capture and
   reported ALL PASS off it until the stack started deleting the file. */
const { soundCheck, banner } = soundGate(stack, check);
let wav = null;
if (!stack.audioRunning) soundCheck(false, 'a capture was produced');
else try { wav = readWav(WAV); } catch (e) { check(false, 'a capture was produced', e.message); }

if (wav) {
  const { mono, rate } = wav;
  // Each window sits inside its phase, 4s clear of the front for whatever was
  // already sounding when the knob moved and 0.5s clear of the back.
  const win = (start) => [at(start + 4000), at(start + PHASE - 500)];
  const level = (start) => rmsBetween(mono, rate, ...win(start));
  const pitch = (start) => zeroCrossingRate(mono, rate, ...win(start));
  const dens = (start) => loudFraction(mono, rate, ...win(start));
  const [a, b, c, d, e] = [aAt, bAt, cAt, dAt, eAt].map(level);
  console.log(`  preset ${a.toFixed(5)}   vel 1 ${b.toFixed(5)}   vel 127 ${c.toFixed(5)}` +
              `   base 9 ${d.toFixed(5)}   hits 0 ${e.toFixed(5)}`);
  console.log(`  density: base 9 ${dens(dAt).toFixed(2)} -> hits 0 ${dens(eAt).toFixed(2)}`);
  console.log(`  zero crossings/s: at base ${graph.base} ${pitch(cAt).toFixed(0)}` +
              ` -> at base 9 ${pitch(dAt).toFixed(0)}` +
              `   (capture ${(mono.length / rate).toFixed(1)}s, phase A at ${win(aAt)[0].toFixed(1)}s)`);

  soundCheck(a > 0.005, 'the generator was sounding to begin with', a.toFixed(5));
  soundCheck(b < a * 0.5, 'the quietest velocity made it quiet',
        `${b.toFixed(5)} vs ${a.toFixed(5)}`);
  soundCheck(c > b * 3, 'and the loudest brought it back',
        `${(c / Math.max(b, 1e-9)).toFixed(1)}x`);
  /*
   * FIVE OCTAVES UP, so the crossing rate must MULTIPLY, not merely differ.
   * The number this replaces was 1430/s against 1685/s — an 18% wobble that is
   * the random degrees wandering, on a field that claimed to move the octave.
   * A ratio, not a difference, because the absolute rate depends on the patch.
   */
  soundCheck(pitch(dAt) > pitch(cAt) * 3, 'base octave 9 plays in a far higher register',
        `${pitch(cAt).toFixed(0)}/s -> ${pitch(dAt).toFixed(0)}/s`);
  /*
   * ZERO HITS IS ZERO HITS — measured as DENSITY, not as silence, and the
   * difference is the whole content of this check.
   *
   * `generator` has one WRITTEN note in its clip, and the transport loops the
   * 7-second placement, so a note fires roughly every 7s no matter what the
   * generator does. That is correct: it is the song, not the euclidean. Asserting
   * silence here failed at rms 0.0045 and I nearly reported a working fix as
   * broken — the capture's envelope showed isolated bursts on a ~7s period with
   * true silence between them, which is a looping clip and could not be anything
   * else.
   *
   * Density tells the two apart where level cannot: five notes a bar is
   * continuous sound (0.99) and one note per loop is not (0.11), while their
   * average levels are within a factor of six. The previous phase is asserted
   * loud in the same breath, so "both are quiet because the run was broken"
   * cannot pass.
   */
  soundCheck(dens(dAt) > 0.9, 'the generator was running continuously before',
        dens(dAt).toFixed(2));
  soundCheck(dens(eAt) < 0.3, 'and at zero hits it stops — 0 is not "use the default"',
        `density ${dens(eAt).toFixed(2)}, rms ${e.toFixed(5)}`);
}

console.log(banner(fail, pass));
process.exit(fail === 0 ? 0 : 1);
