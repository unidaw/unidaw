#!/usr/bin/env node
/**
 * FOUR REASONS AN EDIT CAN BE REFUSED, AND EACH MUST SAY ITS OWN ONE.
 *
 * `UiClipRejectReason` is StaleBase 1, UnknownTrack 2, UnknownNote 3, ValueOutOfRange 4. The page
 * words all four. Only ONE of them has ever been driven: `e2e.mjs` sends a note with base 999999
 * and checks the stale-base sentence appears. Reasons 2, 3 and 4 are wired and unexercised.
 *
 * ── WHY THAT IS WORTH A FILE ────────────────────────────────────────────────────────────────
 *
 * Because this exact thing has already been wrong once, and index.html says so in a comment:
 * everything that was not UnknownTrack came out as "refused an edit composed against version N",
 * so a value the engine considered out of range was reported as a CONCURRENCY problem — with two
 * version numbers that were equal sitting in the message to prove it was not one. Somebody read
 * those numbers for a while before looking at the value.
 *
 * The recovery differs per reason, which is the entire point of the codes being distinct:
 *
 *   StaleBase        retry with the version the engine handed back
 *   UnknownTrack     never going to work; the track is not there
 *   UnknownNote      the client is holding an id from before a reload
 *   ValueOutOfRange  retrying is pointless, the NUMBER is wrong
 *
 * A message that names the wrong one sends the reader to the wrong fix. So each case here asserts
 * both halves: that its own wording appears, AND that the stale-base wording does NOT — the latter
 * being the specific regression that happened before, and the one a looser check would miss.
 *
 * ── THE THREE ARE ALL ROW-OP REFUSALS ───────────────────────────────────────────────────────
 *
 * UnknownTrack, UnknownNote and ValueOutOfRange are emitted from the row-ops path
 * (`engine_clip_edit.cpp`, all three logging `rowops.rejected`), so all three are driven with
 * `setrowops` messages sent raw — past the app's own send path, whose job is to never produce one.
 * `probability > 100` is REFUSED rather than clamped, deliberately, which is what makes reason 4
 * reachable at all.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const Q = 960000;
const MASK_PROBABILITY = 2;            // kRowOpMaskProbability = 1u << 1
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
const clearReject = () => page.evaluate(() => { window.__uni.run('view tracker'); });

console.log('\nevery refusal reason says its own thing\n');

await run('new rejreason');
await settle(1200);
await run('goto 0');
await settle(200);
await run('note 60');
await settle(1200);

const note = await page.evaluate(() => (window.__uni.notes() || [])[0]);
check(note && note.id, 'a real note to aim at', JSON.stringify(note));
const clips = await page.evaluate(() => window.__uni.clips() || []);
const clipId = clips[0] && clips[0].clip;
check(clipId !== undefined, 'and the clip it lives in', JSON.stringify(clips[0]));

/**
 * Send one raw setrowops and return the reject line IT produced.
 *
 * WAITS FOR THE LINE TO CHANGE, rather than sleeping and hoping. `state.reject` persists until
 * something replaces it, so a fixed sleep reads the PREVIOUS case's message whenever this one is
 * slower than the sleep — which made the first version of this file pass and fail on the same
 * unchanged code, three checks apart, depending on machine load. Waiting for the transition is
 * both deterministic and faster.
 *
 * Returns the empty string if nothing arrived in time, which fails the caller's assertion with the
 * right story rather than silently reporting the last case's answer as this one's.
 */
const refuse = async (patch) => {
  const before = await reject();
  await page.evaluate((p) => window.__uni.send(Object.assign(
    { type: 'setrowops', track: 0, clip: p.clip, note: p.note, mask: 2, prob: 50 }, p.over)), patch);
  try {
    await page.waitForFunction(
      (prev) => String((window.__uni.state() || {}).reject || '') !== prev,
    // 8s was too tight. In sweep 34 this wait expired on the first run of all three
    // reject-reason suites at once, each reporting an EMPTY reject line, and each passed on a
    // retry that took a fifth of the time. The sweep's own machine line said why: it started at
    // load 6,71 against sweep 33's 4,75, with Spotlight indexing throughout. The refusal was
    // real and on its way; this side simply stopped listening first.
    //
    // Raised rather than slept on: the assertion is unchanged and a passing run still returns as
    // soon as the reject line moves, so the only cost is how long a GENUINE silence takes to be
    // reported. 25s buys the loaded case without turning a real regression into a slow one.
      before, { timeout: 25000 });
  } catch { return ''; }
  return reject();
};

const STALE = /refused an edit composed against version/;

/* ── REASON 4: a value the engine will never accept ────────────────────────────────────────
 * probability is a percentage; 200 is not "a lot", it is nonsense, and the engine refuses rather
 * than clamping so the caller finds out it sent nonsense.
 */
{
  const said = await refuse({ clip: clipId, note: note.id, over: { prob: 200 } });
  console.log(`  reason 4 said: ${JSON.stringify(said)}`);
  check(/out of range/.test(said),
        'AN OUT-OF-RANGE VALUE IS REPORTED AS ONE',
        `${JSON.stringify(said)} — a number the engine will never take`);
  check(!STALE.test(said),
        'and NOT as a concurrency problem — the regression this file is named for',
        `${JSON.stringify(said)} — reporting it as a stale base sends the reader to retry, which `
        + 'cannot ever work: the value is wrong, not the version');
}

/* ── REASON 3: a note id that is not there ────────────────────────────────────────────────── */
{
  const said = await refuse({ clip: clipId, note: 999999, over: {} });
  console.log(`  reason 3 said: ${JSON.stringify(said)}`);
  check(/no note with that id/.test(said),
        'A MISSING NOTE IS REPORTED AS A MISSING NOTE', JSON.stringify(said));
  check(!STALE.test(said), 'and not as a stale base', JSON.stringify(said));
}

/* ── REASON 2: a track that is not there ──────────────────────────────────────────────────── */
{
  const said = await refuse({ clip: clipId, note: note.id, over: { track: 60 } });
  console.log(`  reason 2 said: ${JSON.stringify(said)}`);
  check(/does not exist/.test(said),
        'A MISSING TRACK IS REPORTED AS A MISSING TRACK', JSON.stringify(said));
  check(!STALE.test(said), 'and not as a stale base', JSON.stringify(said));
}

/* ── AND THE APP STILL WORKS ──────────────────────────────────────────────────────────────
 * Three refusals in a row must not wedge the edit path — the difference between reporting an
 * error and becoming one.
 */
await run('goto 8');
await settle(200);
await run('note 67');
await settle(1200);
const after = await page.evaluate(() => (window.__uni.notes() || []).length);
check(after === 2, 'a good edit still lands after three refusals', String(after));

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
