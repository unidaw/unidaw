#!/usr/bin/env node
/**
 * DELETE CLEARS THE SELECTION.
 *
 * Owner's ruling, 2026-08-07. Until now the tracker's Delete called `deleteAtCursor` and never
 * consulted the selection, so selecting a range and pressing Delete removed the ONE note under the
 * cursor and left the rest. `cut` (x) was the only way to clear a range.
 *
 * That is a silent no-op four times out of five: you watch a single note vanish out of five
 * selected and conclude the key did not take. Renoise clears the selection with Delete, which is
 * where everyone arrives from.
 *
 * ── WHAT IT ASSERTS AND WHY THAT SHAPE ──────────────────────────────────────────────────────
 *
 * FIVE notes, and the count going to ZERO. Not "fewer than before": the old behaviour also left
 * fewer than before — exactly one fewer — so a check for "something was deleted" passes on the bug
 * it exists for. The number that distinguishes them is ALL of them.
 *
 * And the SINGLE-CELL case still works, because the ruling is "when there is a selection", not
 * "instead of". A Delete with no selection must still take the note under the cursor and step.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

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
const notes = () => page.evaluate(() => {
  const e = window.__uni.engineState();
  return e ? e.noteCount : -1;
});

console.log('\ndelete clears the selection\n');

await run('new delsel');
await settle(1200);
await run('view tracker');
await settle(400);
await page.evaluate(() => { const g = document.querySelector('#tracker'); if (g) g.click(); });
await settle(300);

/* ── THE SINGLE-CELL CASE FIRST, while no selection has ever existed ──────────────────────
 *
 * ORDER MATTERS AND MY FIRST ATTEMPT GOT IT WRONG. I ran this second and cleared the selection
 * with `window.__uni.state().selection = null` — which does NOTHING, because `state()` returns a
 * DEEP COPY. The write went to the copy, the selection was still live, and Delete correctly
 * cleared the range while the check called that a failure of the single-cell path.
 *
 * There is no console verb for "deselect", so rather than invent one for a test, this simply runs
 * before anything is selected. The ruling is "when there IS a selection", so the no-selection
 * branch is the one that has to keep working.
 */
{
  await run('goto 0'); await settle(150); await run('note 72'); await settle(400);
  await run('goto 4'); await settle(150); await run('note 74'); await settle(500);
  const two = await notes();
  check(two === 2, 'two notes, and nothing selected yet', String(two));

  await run('goto 0');
  await settle(250);
  const rowBefore = await page.evaluate(() => window.__uni.state().cursor.row);
  await page.keyboard.press('Delete');
  await settle(1200);
  const left = await notes();
  check(left === 1,
        'with NO selection, Delete takes just the note at the cursor',
        `${two} -> ${left}`);
  const rowAfter = await page.evaluate(() => window.__uni.state().cursor.row);
  check(rowAfter > rowBefore,
        'and steps the cursor, which is the note-entry idiom',
        `row ${rowBefore} -> ${rowAfter}`);
}

/* ── THEN THE SELECTION, which is the ruling ──────────────────────────────────────────────── */
await run('new delsel2');
await settle(1200);
await page.evaluate(() => { const g = document.querySelector('#tracker'); if (g) g.click(); });
await settle(300);
for (const [row, pitch] of [[0, 60], [2, 62], [4, 64], [6, 65], [8, 67]]) {
  await run(`goto ${row}`); await settle(150);
  await run(`note ${pitch}`); await settle(300);
}
await settle(600);
const before = await notes();
check(before === 5, 'five notes to select', String(before));

const sel = await run('select 0 11');
check(/5 note/.test(String(sel)), 'all five are selected', String(sel));

await page.keyboard.press('Delete');
await settle(1400);
const after = await notes();
check(after === 0,
      'DELETE REMOVES EVERY NOTE IN THE SELECTION',
      `${before} -> ${after}. The old behaviour deleted the ONE note under the cursor, so "fewer `
      + 'than before" is true on the bug too — only ALL of them tells the two apart');

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
