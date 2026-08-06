#!/usr/bin/env node
/**
 * `I-V-vi-IV`, NOT `I-V-VI-IV` — the numerals carry the chord quality.
 *
 * Roman-numeral notation says major or minor in its CASE. Every numeral this app drew was upper
 * case, so a I-V-vi-IV progression read as though all four chords were major — the one thing the
 * notation exists to carry, thrown away. Asked for by Jaakko in exactly those terms.
 *
 * WHY THIS SUITE EXISTS AND NOT JUST THE UNIT TEST. The unit test passed while the app was
 * wrong, and it passed *because* it was wrong in the same direction.
 *
 * `stepCents` is CUMULATIVE — one offset from the root per degree, Major being
 * `[0,200,400,500,700,900,1100]` — not the per-step deltas the name suggests. The first version
 * of the code read deltas, the first version of the unit test supplied deltas, and the two agreed
 * with each other perfectly. Only the running app, drawing `VI` where `vi` belonged, disagreed.
 *
 * So this asserts against the ENGINE'S OWN scale registry, over the real wire, rendered by the
 * real tracker. A fixture cannot lie about the shape of data it did not invent.
 *
 * IT CHECKS BOTH SURFACES. The grid cell and the CELL panel used to spell the numeral from two
 * private tables; they call one speller now, and the failure this guards is them disagreeing
 * about the same chord on screen at the same time.
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

console.log('\nchord numerals carry their quality\n');

await run('new casechk');
await settle(1400);

/*
 * A KEY FIRST. Without one the numerals stay upper case ON PURPOSE — that is the honest
 * rendering of "the quality is not established here", and guessing major would be a claim the
 * document does not make. Asserted below, because a feature that is quietly always-on is
 * indistinguishable from one that ignores its input.
 */
await run('goto 0 0');
await run('chord 6 triad 0 4 0 0 0');
await settle(900);
const beforeKey = await page.evaluate(() =>
  [...document.querySelectorAll('.tk-cell')].map((c) => (c.textContent || '').trim())
    .filter((t) => /^[ivxIVX]+$/.test(t)));
check(beforeKey.includes('VI'),
      'with no key set, the sixth is drawn VI — no claim about its quality',
      JSON.stringify(beforeKey));

await run('harmony 0 major 0');
await settle(1000);

// I - V - vi - IV, one per bar, entered the way a person says it: 1-based degrees.
for (const [i, deg] of [1, 5, 6, 4].entries()) {
  await run(`goto ${i * 4} 0`);
  await run(`chord ${deg} triad 0 4 0 0 0`);
  await settle(250);
}
await settle(1200);

const cells = await page.evaluate(() =>
  [...document.querySelectorAll('.tk-cell')].map((c) => (c.textContent || '').trim())
    .filter((t) => /^[ivxIVX]+°?$/.test(t)));
console.log(`  chord cells: ${JSON.stringify(cells)}`);
check(cells.join('-') === 'I-V-vi-IV',
      'in C major the progression reads I-V-vi-IV',
      `got ${JSON.stringify(cells)} — upper-case throughout means the scale never reached the speller`);

/*
 * THE CELL PANEL AGREES WITH THE GRID. Two spellers is the failure mode: the panel had its own
 * numeral table and would have kept saying VI while the cell beside it said vi.
 */
await run('goto 8 0');
await settle(700);
const insp = await page.evaluate(() => window.__uni.inspect());
check(/^vi\b/.test(String(insp.title)),
      'and the CELL panel names it the same way the grid does', JSON.stringify(insp.title));
const degRow = (insp.rows || []).find((r) => r.label === 'degree');
check(degRow && /^vi\b/.test(String(degRow.value)),
      'including its degree row, which keeps the number beside the cased numeral',
      JSON.stringify(degRow));

/*
 * A MINOR KEY CASES THE SAME DEGREE THE OTHER WAY, which is the whole claim: the numeral is
 * about the chord's place in THIS key, not a property of the degree. It is also what proves the
 * name cache is keyed on the scale — keyed only on the degree it would hand back `vi` here.
 */
await run('harmony 0 minor 0');
await settle(1200);
const minorCells = await page.evaluate(() =>
  [...document.querySelectorAll('.tk-cell')].map((c) => (c.textContent || '').trim())
    .filter((t) => /^[ivxIVX]+°?$/.test(t)));
console.log(`  the same chords in minor: ${JSON.stringify(minorCells)}`);
/*
 * READ WITHOUT TOUCHING ANYTHING FIRST. The point is that the CHANGE ITSELF repaints — an
 * assertion taken after a cursor move or another edit would pass on a screen that only caught up
 * because of the nudge, which is exactly the bug this found:
 *
 * the tracker does a full repaint only when `contentRevision` moves, and that revision tracked
 * notes, aggregates, the grid and extents — not the harmony. Nothing in the grid used to depend
 * on the key, so omitting it was right; casing the numerals made it wrong, and the numerals sat
 * a key behind until an unrelated clip edit happened to bump the revision. Data right, screen
 * stale, and no error anywhere.
 */
check(minorCells.join('-') === 'i-v-VI-iv',
      'changing the key re-cases them immediately, with nothing else touched',
      `got ${JSON.stringify(minorCells)} — the major casing means the repaint never happened`);

check(errors.length === 0, 'nothing threw', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
