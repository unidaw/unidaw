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
  /*
   * A LANE THAT DOES NOT EXIST IS AN ANSWER, and the answer arrives ASYNCHRONOUSLY.
   *
   * `curve` returns a receipt — "asking the engine" — and the real answer prints itself into
   * the log when it lands. The first version of this check read the RECEIPT and reported the
   * engine as unresponsive; backend could not reproduce it and was right not to. The engine
   * answers `found: false` on every path, which is the whole reason the slot exists.
   *
   * So this asks, waits, and reads the log — which is what a person does. And the lesson is one
   * I have written down twice this week in other words: a receipt is not an outcome.
   */
  await type('curve 0 nosuchparam');
  await page.waitForTimeout(1800);
  /*
   * A CHEAP command to read the log back, NOT `help`.
   *
   * `help` prints sixty lines into a 300-line ring and pushed the answer out of it, so this
   * check failed while the answer was sitting in the log a moment earlier. A probe that changes
   * what it measures is the oldest mistake in this file.
   */
  const log = await page.evaluate(() => window.__uni.run('oct 4'));
  const said = (log || []).join(' | ');
  check(/nothing automates nosuchparam/i.test(said),
        'asking for a lane that does not exist is ANSWERED, not left hanging',
        said.slice(-180));
}

// ---------------------------------------------------------------------------
// AND NOW WITH A POINTER, which is the half that did not exist.
//
// Everything above drives automation by typing a tick and a value. That is a complete API and
// half a product: the curve was drawn on screen and could not be touched, so the only way to
// change a point was to know its tick and retype it. The rule in this project is that the
// console and the pointer both reach everything, and a suite that only types cannot tell the
// difference between a feature and an API.
//
// WHAT IT CAN DO IS DELIBERATELY HALF OF WHAT IT SHOULD, and the checks say which half: there
// is no opcode to remove an automation point, so a point can be made and its value dragged but
// not moved in time or deleted. The mode says so in its own help rather than letting a drag
// silently do nothing.
// ---------------------------------------------------------------------------
{
  // The mode is refused while nothing is automated — and that refusal is the feature, not an
  // edge case: a crosshair over an empty lane writes points into a parameter nobody chose.
  /*
   * A SECOND TRACK, because the cursor cannot stand on one that does not exist.
   *
   * The fixture has one track and `goto 0 3` clamps straight back to it — so the first version
   * of this check asked about the automated track while believing it was asking about an empty
   * one, and read the mode turning on as a refusal that never happened. Every check after it
   * then ran one toggle out of phase.
   */
  await type('add-track');
  await page.waitForTimeout(700);
  await type('goto 0 1');            // row 0 of the new track, which nothing automates
  const noLane = await type('draw on');
  check(/nothing automates/i.test(noLane),
        'the mode refuses on a track with no automation — there is nothing to draw on',
        noLane.slice(0, 120));
  const chipHidden = await page.evaluate(() => {
    const el = document.querySelector('.ch-draw');
    return el ? el.style.display === 'none' : 'missing';
  });
  check(chipHidden === true, 'and the button is not drawn at all rather than lit and arguing',
        String(chipHidden));

  await type('goto 0 0');            // back to track 0, the automated one
  await page.waitForTimeout(400);

  /*
   * THE BUTTON, not the command. `draw` is tested by using it above; what is unproven is that
   * the chrome can reach the same mode — which is the exact divergence this project keeps
   * finding, six times in one session by its own count.
   */
  await page.locator('.ch-draw').click();
  await page.waitForTimeout(400);
  const p0 = await page.evaluate(() => window.__uni.arrangeProbe());
  check(p0 && p0.automation.edit, 'pressing DRAW turns the mode on',
        JSON.stringify(p0 && p0.automation));
  check(p0 && p0.automation.clickable,
        'and the curve layer takes the pointer — without this the mode is a lit chip and nothing else');

  /*
   * DRAG AN EXISTING POINT.
   *
   * Grabbed by its position on the glass rather than by its tick, which is what a person does.
   * The point at tick 960000 is at a known x; drag it upward and its VALUE must change while
   * its TICK does not — the second half of that is the part a missing remove-opcode makes
   * load-bearing, because a drag that moved it in time would leave a duplicate behind.
   */
  const before = await curve();
  const at960 = before.points.find(([t]) => t === 960000);
  const geom = await page.evaluate(() => {
    const p = window.__uni.arrangeProbe();
    const el = document.querySelector('.ar-auto');
    const r = el.getBoundingClientRect();
    return { left: r.left, top: r.top, width: r.width, tpp: p.ticksPerPixel,
             start: p.startTick, laneH: p.laneHeight || 44 };
  });
  const x = geom.left + (960000 - geom.start) / geom.tpp;
  // Two thirds up the first lane, which is a value no point in WANT already has — so a check
  // that passes cannot be passing because nothing moved.
  const targetY = geom.top + 3 + (geom.laneH - 6) * (1 - 0.66);
  await page.mouse.move(x, geom.top + 3 + (geom.laneH - 6) * (1 - at960[1]));
  await page.mouse.down();
  await page.mouse.move(x, targetY, { steps: 8 });
  await page.mouse.up();
  await page.waitForTimeout(1800);

  await page.evaluate(() => window.__uni.automationPoints(0, 'cutoff'));
  await page.waitForTimeout(1500);
  const after = await curve();
  check(after && after.points.length === before.points.length,
        'dragging a point does not add one — the same curve, moved',
        after && `${before.points.length} -> ${after.points.length}`);
  const moved = after && after.points.find(([t]) => t === 960000);
  check(moved && Math.abs(moved[1] - 0.66) < 0.06,
        'the point under the pointer takes the value the hand left it at',
        moved && `${moved[1]} (wanted ~0.66)`);
  check(moved && moved[0] === 960000,
        'and stays at the tick it was on — a drag changes the value, not the time',
        moved && String(moved[0]));

  /*
   * AND A CLICK ON EMPTY LANE MAKES A POINT. The other half of the gesture: without it the
   * pointer can only edit what the console already wrote.
   */
  /*
   * A tick that is BETWEEN two existing points and ON THE GLASS.
   *
   * Between, so the hit test cannot grab a neighbour instead of creating one — the first
   * version picked a tick past the last point and past the right edge of the view, so the click
   * landed outside the canvas and wrote nothing while looking like a create that failed.
   */
  const newTick = 1440000;
  const nx = geom.left + (newTick - geom.start) / geom.tpp;
  check(nx > geom.left && nx < geom.left + geom.width,
        'the tick being clicked is actually on screen — a click outside the canvas proves nothing',
        `${Math.round(nx - geom.left)}px of ${Math.round(geom.width)}`);
  const ny = geom.top + 3 + (geom.laneH - 6) * (1 - 0.4);
  await page.mouse.click(nx, ny);
  await page.waitForTimeout(1800);
  await page.evaluate(() => window.__uni.automationPoints(0, 'cutoff'));
  await page.waitForTimeout(1500);
  const grown = await curve();
  check(grown && grown.points.length === after.points.length + 1,
        'clicking empty lane writes a new point',
        grown && `${after.points.length} -> ${grown.points.length}`);
  const made = grown && grown.points.find(([t]) => Math.abs(t - newTick) < 60000);
  check(!!made, 'at the tick under the pointer',
        grown && JSON.stringify(grown.points.map((q) => q[0])));
  check(made && Math.abs(made[1] - 0.4) < 0.06, 'and the value under it',
        made && String(made[1]));

  // OFF again, and the lane goes back to the clips. A mode with no way out is a trap.
  await page.locator('.ch-draw').click();
  await page.waitForTimeout(300);
  const p1 = await page.evaluate(() => window.__uni.arrangeProbe());
  check(p1 && !p1.automation.edit && !p1.automation.clickable,
        'pressing it again gives the lane back to the clips',
        JSON.stringify(p1 && p1.automation));
}

check(errors.length === 0, 'and nothing threw while asking', errors.slice(0, 2).join(' | '));

await browser.close();
stack.stop();
const tail = blocked ? `  (${blocked} blocked)` : '';
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)${tail}`
                            : `${fail} of ${pass + fail} FAILED${tail}`}`);
process.exit(fail === 0 ? 0 : 1);
