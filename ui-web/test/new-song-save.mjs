#!/usr/bin/env node
/**
 * `new NAME` THEN SAVE MUST WRITE TO **NAME** — it wrote to the previous song.
 *
 * What happened, in full, because the shape of it matters more than the line that fixed it:
 *
 *   1. The owner opened a preset (`rack`) and worked on it.
 *   2. `new kala` — a new, empty document, and a file `kala.uniproj.json` written to hold it.
 *   3. Half an hour of work: six tracks, five clips, seventy-four notes, five devices.
 *   4. Save.
 *   5. All of it landed in `rack.uniproj.json`, a SHIPPED PRESET, overwriting it.
 *      `kala.uniproj.json` still held the empty document from step 2 — which is the most
 *      convincing possible evidence that nothing had been saved at all.
 *
 * `save` and `load` both set `state.currentProject`. `newSong` did not. So the app went on
 * believing the current project was `rack`, and every save that defaults to "the current one" —
 * the browser's ⌘S, the console's bare `save` — wrote there. Nothing warned, because from the
 * app's point of view nothing was wrong: it saved the current project, and the current project
 * was stale.
 *
 * ── WHAT THIS ASSERTS ───────────────────────────────────────────────────────────────────────
 *
 * BOTH FILES. That the new name got the work, and that THE OLD ONE DID NOT CHANGE. Only the
 * second one catches this class of bug: a check that the new file has notes passes on a build
 * that writes to both, and a build that writes to the old one only is caught by nothing else.
 *
 * The old project is a real one with content, not an empty scratch file, because the damage here
 * is overwriting something that already mattered.
 */

import { chromium } from 'playwright';
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
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
const notesIn = (name) => {
  const p = join(stack.dir, `${name}.uniproj.json`);
  if (!existsSync(p)) return -1;
  try {
    const doc = JSON.parse(readFileSync(p, 'utf8'));
    return (doc.clips || []).reduce((n, c) => n + ((c.notes || []).length), 0);
  } catch { return -2; }
};

console.log('\na new song saves to its own name\n');

// THE FIRST SONG, with content worth losing.
await run('new firstsong');
await settle(1000);
for (const [row, pitch] of [[0, 60], [4, 62], [8, 64]]) {
  await run(`goto ${row}`); await settle(150);
  await run(`note ${pitch}`); await settle(250);
}
await run('save firstsong');
await settle(1500);
const firstBefore = notesIn('firstsong');
check(firstBefore === 3, 'the first song is saved with its three notes', String(firstBefore));

// NOW A NEW ONE, and work in it.
await run('new secondsong');
await settle(1200);
const cur = await page.evaluate(() => window.__uni.state().currentProject);
check(cur === 'secondsong',
      '`new` makes the new song the CURRENT project',
      `currentProject=${JSON.stringify(cur)} — if this is the old name, every save that defaults `
      + 'to "the current one" goes to the old file');

for (const [row, pitch] of [[0, 72], [4, 74]]) {
  await run(`goto ${row}`); await settle(150);
  await run(`note ${pitch}`); await settle(250);
}

// A BARE SAVE — the gesture that did the damage. Not `save secondsong`, which names the target
// and would pass on the broken build too.
await page.evaluate(() => {
  const n = window.__uni.state().currentProject;
  return window.__uni.saveProject ? window.__uni.saveProject(n) : window.__uni.run('save ' + n);
});
await settle(1800);

check(notesIn('secondsong') === 2,
      'THE NEW SONG GETS THE WORK',
      `${notesIn('secondsong')} notes in secondsong, expected 2`);
check(notesIn('firstsong') === firstBefore,
      'AND THE FIRST SONG IS UNTOUCHED — this is the check that catches it',
      `firstsong went ${firstBefore} -> ${notesIn('firstsong')}; the reported failure is that the `
      + 'previous project silently receives the new work and is overwritten');

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
