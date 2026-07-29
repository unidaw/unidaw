/**
 * Lane quantize: the setting lands, it comes back, and NOTHING ON DISK MOVES.
 *
 * The last of those is the whole promise. A destructive quantize throws the
 * performance away on the first pass and there is no way back; this one applies
 * to a separate scheduling copy, so the authored tick stays what was played and
 * can be tightened or loosened afterwards. If that is not true the feature is a
 * slower destructive quantize with extra steps.
 *
 * SO IT IS CHECKED AGAINST WHAT A SAVE WRITES, not against a published region.
 * Backend hit exactly that trap on this feature: a check comparing the published
 * clip before and after a change that deliberately does not bump the version
 * gating that region's rebuild read the same stale snapshot twice, and a
 * DESTRUCTIVE quantize passed as non-destructive. The file is the claim.
 *
 * AND THE READ-BACK IS WHAT MAKES THE FILE CHECK MEAN ANYTHING. "The file did not
 * change" is also true of a command that was dropped — which is not a
 * hypothetical here, since a stale base version silently dropped every clip edit
 * on this project a few hours ago. So the setting has to be observed arriving
 * before its non-effect on disk proves anything.
 *
 * THE SWING BIAS is the third thing. It travels +500 biased through an unsigned
 * payload field and reads back plain signed, so a leg applied twice or not at all
 * is an off-by-500 — and a wrong swing is not an error, it is a groove. -100 must
 * come back as -100.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

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

/**
 * Every authored note tick in the SAVED file.
 *
 * THREE shapes, because a note can be written in three places and which one a
 * project uses is a property of the project: a track's own `notes` for loose
 * material, a placement's `notes` for an override, and a shared CLIP's notes for
 * everything a placement points at. `meter` uses the third.
 *
 * Reading only the first found nothing, and the check that no tick moved PASSED —
 * on a file with no ticks in it. That is the vacuous green this suite exists to
 * refuse, and it is why the count below is asserted before the comparison is
 * trusted.
 */
const savedTicks = (name) => {
  const d = JSON.parse(readFileSync(join(stack.dir, name + '.uniproj.json'), 'utf8'));
  const out = [];
  for (const c of d.clips || []) for (const n of c.notes || []) out.push(c.id + ':' + n.nanotick);
  for (const t of d.tracks) {
    for (const n of t.notes || []) out.push('t:' + n.nanotick);
    for (const p of t.placements || []) {
      for (const n of p.notes || []) out.push('p:' + n.nanotick);
    }
  }
  return out.join(',');
};

const lanes = () => page.evaluate(() => window.__uni.quantized());

const before = await lanes();
check(before.length === 0, 'no lane starts quantized', JSON.stringify(before));

await page.evaluate(() => window.__uni.run('save qbefore'));
await page.waitForTimeout(1500);
const ticksBefore = savedTicks('qbefore');
// Non-vacuity: "no tick moved" is trivially true of a file with no ticks.
check(ticksBefore.split(',').filter(Boolean).length >= 8,
      'the project has notes that could have moved',
      `${ticksBefore.split(',').filter(Boolean).length} notes`);

// Through the console, in the units a person types: percent, and a named grid.
const said = await page.evaluate(() => window.__uni.run('quantize 1 1/16 60 -10'));
await page.waitForTimeout(1200);

const after = await lanes();
const lane = after.find((q) => q.track === 1);
check(!!lane, 'the lane reports a quantize setting back', JSON.stringify(after));
check(lane && lane.grid === 240000, 'at a 1/16 grid in nanoticks',
      lane && String(lane.grid));
check(lane && lane.strength === 600, 'strength 60% arrives as 600 thousandths',
      lane && String(lane.strength));
/*
 * THE BIAS, EXACTLY ONCE. -10% is -100 thousandths, sent as 400 through an
 * unsigned field and read back plain. Applied twice it would come back as -600
 * (clamped to -500); applied never, as +400. Both are grooves, not errors.
 */
check(lane && lane.swing === -100, 'and swing -10% survives the +500 bias intact',
      lane && String(lane.swing));
check(after.length === 1, 'and no other lane was touched', JSON.stringify(after));
check(/1\/16/.test(String(said)), 'the console says what it did', String(said));

// The lane header shows it, or the setting is invisible — which for a change that
// deliberately moves nothing on screen means indistinguishable from no setting.
const badges = await page.evaluate(() => [...document.querySelectorAll('.hquant')]
  .filter((e) => e.offsetParent).map((e) => e.textContent).filter(Boolean));
check(badges.length === 1 && /1\/16/.test(badges[0]),
      'and the lane header wears it', JSON.stringify(badges));
check(badges.length === 1 && /60%/.test(badges[0]) && /-10/.test(badges[0]),
      'with the strength and the swing on it', JSON.stringify(badges));

await page.evaluate(() => window.__uni.run('save qafter'));
await page.waitForTimeout(1500);
check(savedTicks('qafter') === ticksBefore,
      'and NOT ONE AUTHORED TICK MOVED on disk');

/*
 * AND THE BAR AGREES WITH THE ENGINE'S OWN NUMBER.
 *
 * This is the check that justifies publishing the deviation at all. A test that
 * asserted "a mark is drawn" would pass a mark computed independently and wrongly
 * — which is exactly the outcome the field exists to prevent, since a JavaScript
 * port of quantizeTick is off by one tick on the notes played late and looks
 * completely right while doing it.
 *
 * So: take a note the ENGINE says it moves, work out where that lands in its row
 * from the engine's own numbers, and compare against the pixel position the cell
 * is drawing. One number, checked at both ends.
 */
{
  // A zoom where a row is a POSITION, not a summary — at "1 bar" per row the cell
  // holds a count and a mark inside it would point at a note it is not showing.
  await page.evaluate(() => window.__uni.setZoom(1));
  await page.waitForTimeout(600);

  const moved = await page.evaluate(() => {
    const n = (window.__uni.notes() || []).filter((x) => x.tr === 1 && x.dev);
    return n.length ? { row: n[0].row, dev: n[0].dev, delay: n[0].delay, p: n[0].p } : null;
  });
  check(!!moved, 'the engine reports a note its lane moves', JSON.stringify(moved));

  if (moved) {
    // 1/16 rows at this zoom: 240000 nanoticks each. The percentage the renderer
    // draws is (delay + dev) / rowTicks, clamped into the cell.
    const ROW_TICKS = 240000;
    const want = Math.max(0, Math.min(99,
      Math.round(((moved.delay + moved.dev) / ROW_TICKS) * 100)));
    const drawn = await page.evaluate((row) => {
      for (const el of document.querySelectorAll('.tk-cell.dev')) {
        if (!el.offsetParent) continue;
        const r = el.closest('.tk-row');
        if (r && Number(r.dataset.row) === row) return el.style.getPropertyValue('--dev');
      }
      return null;
    }, moved.row);
    check(drawn !== null, 'and the cell holding it draws a deviation mark',
          `row ${moved.row}, dev ${moved.dev}`);
    check(drawn !== null && parseInt(drawn, 10) === want,
          'at exactly the position the engine\'s own deviation puts it',
          `drawn ${drawn}, engine says ${want}% (dev ${moved.dev} + delay ${moved.delay})`);
  }
}

// Off again, so the setting is not one-way.
await page.evaluate(() => window.__uni.run('quantize 1 off'));
await page.waitForTimeout(1200);
check((await lanes()).length === 0, 'and a lane can be unquantized again',
      JSON.stringify(await lanes()));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
