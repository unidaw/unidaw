/**
 * WHERE YOU CAN TYPE MUST BE WHERE THE GRID IS.
 *
 * A lane's rows come from three sources in strict order — the CLIP's own grid,
 * then the track's lines-per-beat, then the zoom's. `viewmodel.js` resolves that
 * chain exactly, with integer arithmetic, and every row it lands on is drawn as
 * an ordinary cell; the rest are drawn `offgrid`.
 *
 * `onLaneGrid()` in index.html is a SECOND copy of the same rule, and it is the
 * one that decides whether a keystroke is allowed to become a note. Two copies of
 * a rule is one copy and one guess; this suite makes them answer out loud.
 *
 * The two ways they diverged:
 *
 *   1. ROUNDING. The copy computed `row % Math.round(zoomLpb / laneLpb)`, and
 *      `Math.round(4 / 3)` is 1 — so `row % 1`, which is 0 for every row. A
 *      triplet lane declared EVERY row writable at every zoom but the finest,
 *      while the renderer greyed out two rows in three. viewmodel.js carries a
 *      comment about this exact expression; the second copy never got the fix.
 *
 *   2. THE CLIP. The copy never looked at the extent at all, so a clip carrying
 *      its own subdivision had no effect on where you could type.
 *
 * WHY IT SURVIVED: every clip in the browser fixture is authored at
 * `linesPerBeat: 4`. The fixture varies the METER and never the SUBDIVISION, so
 * the clip step of the chain was never exercised against a differing value. So
 * this suite drives the REAL engine and the `meter` project, whose lanes
 * genuinely disagree.
 *
 * THE DRAWN SIDE IS READ FROM THE DOM, not from a probe added for the purpose.
 * `data-kind="offgrid"` is what the user's eye is reading; a probe would be a
 * third copy of the rule and could agree with the gate while both were wrong.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8 });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
page.on('pageerror', (e) => check(false, 'no page error', e.message));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1200);
await page.evaluate(() => window.__uni.run('view tracker'));
await page.evaluate(() => window.__uni.loadProject('meter'));
await page.waitForTimeout(2500);

const lpb = await page.evaluate(() => (window.__uni.engine() || {}).lpb || []);
console.log('\n[the project]');
check(lpb.length > 0, 'the project publishes a per-track subdivision', JSON.stringify(lpb));

// A lane that is NOT a power-of-two fraction of the zoom is the whole point: it is
// the case `Math.round` destroys. Without one in the project there is nothing to
// test and a green run would mean nothing.
const triplet = lpb.findIndex((n) => n === 3 || n === 6 || n === 12);
check(triplet >= 0, 'and at least one lane is a triplet subdivision',
      triplet < 0 ? `none of ${JSON.stringify(lpb)} is 3, 6 or 12` : `track ${triplet} at ${lpb[triplet]}/beat`);

if (triplet < 0) {
  console.log('\nCannot test the rule without a triplet lane in the project.');
  await browser.close();
  stack.stop();
  process.exit(1);
}

/**
 * The two answers for the same rows.
 *
 * `writable` is the WRITE gate — the rows `onLaneGrid()` accepts a note into.
 * The drawn side comes from `data-kind`, and ONLY the two kinds the grid branch
 * emits can be read as an answer: `offgrid` and `empty`. A cell holding a note
 * is drawn `note` whatever the grid says, so it reports nothing about the grid —
 * reading it as on-grid is how the first version of this suite accused a correct
 * fix of being wrong, because rows 1 and 5 of the triplet lane have notes on
 * them. Ambiguous rows are counted and excluded, never guessed at.
 */
const answers = async (track) => page.evaluate((t) => {
  const writable = window.__uni.laneRows(t);
  const off = [], on = [], ambiguous = [];
  for (const rowEl of document.querySelectorAll('[data-row]')) {
    const r = Number(rowEl.dataset.row);
    if (!Number.isFinite(r) || r < 0 || r >= 12) continue;
    const cell = rowEl.querySelector(`.tk-cell[data-track="${t}"]`);
    if (!cell) continue;
    if (cell.dataset.kind === 'offgrid') off.push(r);
    else if (cell.dataset.kind === 'empty') on.push(r);
    else ambiguous.push(r);
  }
  const s = (a) => a.sort((x, y) => x - y);
  return { writable, off: s(off), on: s(on), ambiguous: s(ambiguous) };
}, track);

/** Where the gate and the drawing disagree, on the rows that can answer. */
const disagreement = (a) => {
  const bad = [];
  for (const r of a.off) if (a.writable.includes(r)) bad.push(`row ${r} drawn off-grid but writable`);
  for (const r of a.on) if (!a.writable.includes(r)) bad.push(`row ${r} drawn on-grid but refused`);
  return bad;
};

console.log('\n[a triplet lane]');
const a = await answers(triplet);
check(a.off.length + a.on.length > 0, 'the lane is on screen with rows that can answer',
      `${a.on.length} drawn on-grid, ${a.off.length} off-grid, ${a.ambiguous.length} carrying content`);
check(disagreement(a).length === 0,
      'every row you can type into is a row the grid actually has',
      disagreement(a).join('; '));

/**
 * THE SAME QUESTION AT EVERY ZOOM. The rounding bug is zoom-dependent — it
 * disappears at the zoom where the division comes out whole, which is how a spot
 * check at one zoom passes on broken code.
 */
console.log('\n[at every zoom]');
let discriminated = null;
for (let z = 0; z < 5; z++) {
  await page.evaluate((zz) => window.__uni.setZoom(zz), z);
  await page.waitForTimeout(150);
  const r = await answers(triplet);
  const bad = disagreement(r);
  check(bad.length === 0, `zoom ${z}: the write gate and the drawn grid agree`, bad.join('; '));
  if (r.off.length > 0) discriminated = { z, ...r };
}

/**
 * THE AGREEMENT MUST HAVE BEEN NON-VACUOUS SOMEWHERE. A lane FINER than the axis
 * genuinely owns every row, so "writable === drawn" is also true of two rules
 * that both say yes to everything. At least one zoom has to have produced a lane
 * that refuses a row, or this suite proved nothing.
 */
check(discriminated !== null,
      'and at least one zoom put the lane on a coarser grid than the axis',
      discriminated ? `zoom ${discriminated.z} drew rows ${JSON.stringify(discriminated.off)} off-grid`
                    : 'no zoom drew a single off-grid row — nothing discriminating was tested');

/**
 * AND THE REFUSAL IS REAL. The gate only matters because it stops a note; a
 * refusal that does not refuse is the same bug wearing a different layer. Put
 * the cursor on a row the lane does NOT have, type on the KEYBOARD, and confirm
 * nothing was written.
 */
console.log('\n[the refusal is real]');
// At the zoom the sweep found discriminating — not a fixed one. A zoom where the
// lane owns every row has no refusal to test and would pass by having nothing to do.
await page.evaluate((zz) => window.__uni.setZoom(zz), discriminated ? discriminated.z : 0);
await page.waitForTimeout(250);
const at0 = await answers(triplet);
const missing = [];
for (let r = 0; r < 12; r++) if (!at0.writable.includes(r)) missing.push(r);
check(missing.length > 0, 'the lane has a row it does not own',
      `writable ${JSON.stringify(at0.writable)}`);

if (missing.length > 0) {
  const before = (await page.evaluate(() => window.__uni.engine())).notes;
  await page.evaluate(({ t, r }) => window.__uni.goto(r, t), { t: triplet, r: missing[0] });
  await page.waitForTimeout(150);
  await page.keyboard.press('q');           // C in the tracker's note keyboard
  await page.waitForTimeout(900);
  const after = (await page.evaluate(() => window.__uni.engine())).notes;
  const reject = await page.evaluate(() => window.__uni.state().reject);
  check(after === before, 'a note typed onto a row the lane does not have is not written',
        `${before} -> ${after} at row ${missing[0]}`);
  check(!!reject, 'and the refusal says so on screen', JSON.stringify(reject));

  // The positive control: the SAME keystroke on a row the lane does own must
  // write. Without it, "nothing was written" is also true of a broken keyboard.
  const owned = at0.writable[1] !== undefined ? at0.writable[1] : at0.writable[0];
  await page.evaluate(({ t, r }) => window.__uni.goto(r, t), { t: triplet, r: owned });
  await page.waitForTimeout(150);
  await page.keyboard.press('q');
  await page.waitForTimeout(900);
  const wrote = (await page.evaluate(() => window.__uni.engine())).notes;
  check(wrote > after, 'and the same keystroke on a row it does own writes one',
        `${after} -> ${wrote} at row ${owned}`);
}

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
