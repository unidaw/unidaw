/**
 * AUTOMATION: written, published, read back, DRAWN, and still there after a save.
 *
 * Five links in one chain, and the suite follows it in that order because each one can be
 * correct while the next is broken and they fail in completely different ways. A point that is
 * written and not published is invisible. One that is published and not drawn is a curve nobody
 * can see. One that is drawn and not saved is gone tomorrow.
 *
 * WHY THE WRITE IS HERE AT ALL, when this started as a read. Automation had no writer anywhere:
 * `WriteAutomationPoint` existed as an opcode, daw-cli had no verb for it, and this app had no
 * surface — so there was NOTHING TO READ and the read path could not be tested at all. Building
 * the write to test the read is not a testing trick; the app needs it either way, and a
 * capability built to set up a test is a capability. (Which is the same conclusion as the last
 * time this came up, and the note in my memory says exactly this.)
 *
 * THE VALUE BETWEEN POINTS IS NEVER ASSERTED, and the renderer never computes one either. The
 * engine interpolates when it plays; a second implementation here could disagree with it, and
 * "the picture and the sound differ" is the precise failure this read-back exists to prevent.
 * What is asserted is the POINTS — which is what both sides agree on.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

let pass = 0, fail = 0, blocked = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};
/** A claim this suite cannot make yet, with the reason and whose it is. Never silent. */
const block = (what, why) => { blocked++; console.log('  BLOCKED', what, `— ${why}`); };

const stack = await startStack({ numBlocks: 8 });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1200);

const type = async (line) => {
  const log = await page.evaluate((c) => window.__uni.run(c), line);
  await page.waitForTimeout(600);
  const at = (log || []).lastIndexOf(`in: > ${line}`);
  const mine = at >= 0 ? log.slice(at + 1) : (log || []);
  return mine.filter((l) => String(l).startsWith('out:'))
             .map((l) => String(l).slice(5)).join('\n');
};
const lanes = () => page.evaluate(() => window.__uni.automation());
const curve = () => page.evaluate(() => window.__uni.automationPoints(0, 'cutoff'));

await page.evaluate(() => window.__uni.run('view arrange'));
await page.waitForTimeout(500);

// ---------------------------------------------------------------------------
// NOTHING IS AUTOMATED, and the app says so rather than showing an empty lane.
// ---------------------------------------------------------------------------
{
  const l = await lanes();
  check(l && l.list.length === 0, 'a new project has no automation',
        JSON.stringify(l && l.list));
  check(/nothing/i.test(await type('automation')),
        'and the console says so rather than printing an empty list');
}

// ---------------------------------------------------------------------------
// WRITE. Five points, a shape with a peak, a dip and a peak — so a curve drawn upside down,
// flattened or reversed is visibly not this one.
// ---------------------------------------------------------------------------
const WANT = [[0, 0.1], [960000, 0.9], [1920000, 0.3], [2880000, 0.95], [3840000, 0.2]];
for (const [tick, v] of WANT) await type(`autopoint 0 cutoff ${tick} ${v}`);
await page.waitForTimeout(1500);

// ---------------------------------------------------------------------------
// PUBLISHED: the lane appears in the standing list, with its count and its shape.
// ---------------------------------------------------------------------------
{
  const l = await lanes();
  const lane = (l && l.list || []).find((x) => x.param === 'cutoff');
  check(!!lane, 'the lane is published', JSON.stringify(l && l.list));
  check(lane && lane.points === WANT.length, 'with every point counted',
        lane && `${lane.points} of ${WANT.length}`);
  /*
   * RAMPED, not stepped — and this is not cosmetic. A discrete clip STEPS between points and a
   * ramped one interpolates, so drawing the wrong one says the parameter passes through values
   * it never takes. The engine publishes which, per lane, and it is fixed when the clip is
   * created.
   */
  check(lane && lane.discrete === false, 'and says the curve ramps rather than steps',
        lane && String(lane.discrete));
  check(/cutoff/.test(await type('automation')), 'the console lists it');
}

// ---------------------------------------------------------------------------
// READ BACK: the points, exactly, in order.
// ---------------------------------------------------------------------------
{
  await page.evaluate(() => window.__uni.automationPoints(0, 'cutoff'));
  await page.waitForTimeout(1800);
  const c = await curve();
  check(c && c.found, 'the curve comes back', JSON.stringify(c));
  if (c && c.found) {
    // EXACTLY, and as a whole: a per-point loop that stopped at the first mismatch would report
    // one wrong number where the interesting failure is usually the SHAPE — reversed, off by
    // one, or every value the same.
    const got = c.points.map(([t, v]) => [t, Math.round(v * 1000) / 1000]);
    const want = WANT.map(([t, v]) => [t, Math.round(v * 1000) / 1000]);
    check(JSON.stringify(got) === JSON.stringify(want),
          'with every point at the tick and the value it was written to',
          `${JSON.stringify(got)} vs ${JSON.stringify(want)}`);
  }
  // Writing the same tick REPLACES rather than stacking — which is what makes a pointer drag
  // over a curve possible instead of a way to accumulate duplicates.
  await type('autopoint 0 cutoff 960000 0.5');
  await page.waitForTimeout(1600);
  const after = await curve();
  check(after && after.points.length === WANT.length,
        'writing the same tick again replaces that point rather than adding one',
        after && `${after.points.length} points`);
  const at960 = after && after.points.find(([t]) => t === 960000);
  check(at960 && Math.abs(at960[1] - 0.5) < 0.002, 'and it holds the new value',
        at960 && String(at960[1]));
}

// ---------------------------------------------------------------------------
// DRAWN. The model holding a curve and the arrangement drawing one are different claims, and
// the second is the one a person sees.
// ---------------------------------------------------------------------------
{
  const p = await page.evaluate(() => window.__uni.arrangeProbe());
  check(p && p.automation && p.automation.bound,
        'the arrangement has an automation reader bound',
        JSON.stringify(p && p.automation));
  check(p && p.automation.drawn >= 1, 'and draws a curve for the automated track',
        JSON.stringify(p && p.automation));
  /*
   * AND IT DOES NOT REPAINT WHEN NOTHING CHANGED. A canvas is the most expensive thing on this
   * surface and the guard is what makes it affordable — a repaint per frame would be invisible
   * in every screenshot and would show up as a hot laptop.
   */
  const before = p.automation.repaints;
  await page.waitForTimeout(1200);
  const p2 = await page.evaluate(() => window.__uni.arrangeProbe());
  check(p2.automation.repaints === before,
        'and does not repaint while nothing changes',
        `${before} -> ${p2.automation.repaints}`);
  // ...but DOES when a point moves. A guard that never releases is the other failure.
  await type('autopoint 0 cutoff 2880000 0.15');
  await page.waitForTimeout(1800);
  const p3 = await page.evaluate(() => window.__uni.arrangeProbe());
  check(p3.automation.repaints > before, 'and does repaint when a point moves',
        `${before} -> ${p3.automation.repaints}`);
}

// ---------------------------------------------------------------------------
// SAVED. The last thing a person has is the file.
// ---------------------------------------------------------------------------
{
  await type('save autocheck');
  await page.waitForTimeout(2500);
  let doc = null;
  try { doc = JSON.parse(readFileSync(join(stack.dir, 'autocheck.uniproj.json'), 'utf8')); }
  catch (e) { check(false, 'the project was written', e.message); }
  if (doc) {
    const t0 = (doc.tracks || []).find((t) => t.track_id === 0) || {};
    const auto = t0.automation || [];
    const lane = auto.find((a) => a.param_id === 'cutoff');
    check(!!lane, 'the file holds the automation', JSON.stringify(Object.keys(t0)));
    check(lane && lane.points && lane.points.length === WANT.length,
          'with every point', lane && String(lane.points && lane.points.length));
    // ...and the SHAPE, not just the count. A file with the right number of wrong points is the
    // failure a count cannot see.
    const ticks = (lane && lane.points || []).map((p) => p.nanotick);
    check(JSON.stringify(ticks) === JSON.stringify(WANT.map((w) => w[0])),
          'at the ticks they were written to', JSON.stringify(ticks));
  }
}

// ---------------------------------------------------------------------------
// A LANE THAT DOES NOT EXIST. Recorded as BLOCKED, with the mechanism, because it is verifiable
// and it is not mine.
// ---------------------------------------------------------------------------
{
  const said = await type('curve 0 nosuchparam');
  if (/nothing automates/i.test(said)) {
    check(true, 'asking for a lane that does not exist is ANSWERED, not left hanging');
  } else {
    /*
     * The engine does not answer a RequestAutomationLane for a parameter with no automation. It
     * is documented as answering `found: 0` — "nothing automates that" is an ANSWER — and
     * instead nothing is written to the slot at all, so every caller waits out its timeout.
     *
     * NOT MINE, and verified with backend's own tool rather than inferred from my wire:
     *     daw-cli get automation-points --track 0 --param cutoff
     *     daw-cli: no automation answer for track 0 param "cutoff" (slot 0)
     *
     * Blocked rather than deleted, so it turns green on their side instead of being
     * rediscovered on mine. The symptom here is a console line that says "asking the engine…"
     * for ever; the app does not hang, because the request times out and says so.
     */
    block('asking for a lane that does not exist is ANSWERED, not left hanging',
          'the engine writes no slot for a param with no automation, so the request times out. '
          + 'Documented as found:0. Reproduced with daw-cli get automation-points.');
  }
}

check(errors.length === 0, 'and nothing threw while asking', errors.slice(0, 2).join(' | '));

await browser.close();
stack.stop();
const tail = blocked ? `  (${blocked} blocked)` : '';
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)${tail}`
                            : `${fail} of ${pass + fail} FAILED${tail}`}`);
process.exit(fail === 0 ? 0 : 1);
