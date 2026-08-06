#!/usr/bin/env node
/**
 * DRAGGING A NOTE'S RIGHT EDGE IN THE ROLL CHANGES ITS LENGTH.
 *
 * The owner asked for this as a missing feature — "make it possible to edit note length in the
 * scale roll" — and the code is already there: `piano.js` picks `resize` over `move` when the
 * press lands within `EDGE` pixels of the note's right side. So the question is which of two
 * things is true, and they need different answers:
 *
 *   - it works and is undiscoverable  -> a documentation and affordance problem
 *   - it is broken                    -> a bug
 *
 * Nothing asserted it either way, which is why nobody could say.
 *
 * ── WHY THIS MATTERS BEYOND CONVENIENCE ─────────────────────────────────────────────────────
 *
 * Note length is how LEGATO is authored. A mono synth glides between two notes when the second
 * note-on arrives while the first is still held, and in the tracker a duration of 0 means "until
 * the next event" — which cannot express an overlap. Dragging a note's end past the next note's
 * start is the only way to say it, so this gesture is the 303-slide feature in disguise.
 *
 * ── WHAT IT ASSERTS ─────────────────────────────────────────────────────────────────────────
 *
 * The DURATION IN THE SAVED PROJECT, and that the note's START did not move. A resize that
 * dragged the whole note would change the duration too if only the end were measured, and that
 * is the most likely way for this to be subtly wrong.
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

/** Every note in the saved project, as {tick, duration, pitch}. */
const notes = async (name) => {
  await run(`save ${name}`);
  for (let i = 0; i < 40; i++) {
    const p = join(stack.dir, `${name}.uniproj.json`);
    if (existsSync(p)) {
      try {
        const doc = JSON.parse(readFileSync(p, 'utf8'));
        return (doc.clips || []).flatMap((c) => (c.notes || []).map((n) => ({
          tick: n.nanotick, duration: n.duration, pitch: n.pitch,
        }))).sort((a, b) => a.tick - b.tick);
      } catch { /* mid-write */ }
    }
    await settle(150);
  }
  return null;
};

console.log('\nresizing a note in the roll\n');

await run('new rollresize');
await settle(1200);
// ONE note, so there is no ambiguity about which one moved.
await run('goto 0');
await settle(200);
await run('note 60');
await settle(900);

await run('view piano');
await settle(1200);

const before = await notes('rr_before');
check(before && before.length === 1, 'one note to resize',
      JSON.stringify(before));

const box = await page.evaluate(() => {
  const el = document.querySelector('.pr-notes .pr-note');
  if (!el) return null;
  const r = el.getBoundingClientRect();
  return { left: r.left, right: r.right, mid: r.top + r.height / 2, w: r.width };
});
check(!!box, 'the note is drawn in the roll', JSON.stringify(box));

if (before && before.length === 1 && box) {
  /*
   * GRAB THE RIGHT EDGE. `piano.js` picks `resize` when the press is within EDGE pixels of the
   * note's right side, so one pixel inside it is the gesture — grabbing the middle would move
   * the note instead, which is a different feature and would still change the file.
   */
  await page.mouse.move(box.right - 2, box.mid);
  await page.mouse.down();
  await page.mouse.move(box.right + 120, box.mid, { steps: 10 });
  await page.mouse.up();
  await settle(1200);

  const after = await notes('rr_after');
  check(after && after.length === 1, 'still one note afterwards — a resize is not a new note',
        JSON.stringify(after));

  if (after && after.length === 1) {
    check(after[0].duration > before[0].duration,
          'DRAGGING THE RIGHT EDGE MAKES THE NOTE LONGER',
          `duration ${before[0].duration} -> ${after[0].duration}`);
    // The START must not move. A drag that moved the whole note would also change where it ends,
    // so measuring only the end cannot tell a resize from a move.
    check(after[0].tick === before[0].tick,
          'and its START stays put — this is a resize, not a move',
          `tick ${before[0].tick} -> ${after[0].tick}`);
    check(after[0].pitch === before[0].pitch,
          'and its pitch is unchanged',
          `pitch ${before[0].pitch} -> ${after[0].pitch}`);
  }
}

/*
 * AND THE EDGE SAYS SO BEFORE YOU DRAG IT.
 *
 * The gesture worked before this suite existed; what it lacked was any sign that it did, which is
 * why it was reported as a missing feature. The cursor is the sign. Asserted as a CONTRAST — over
 * the edge versus over the middle — because a build that set `ew-resize` on the whole note would
 * pass a single check while telling you the middle resizes too.
 */
{
  // RE-MEASURED: the note was just made longer, so the box captured before the drag has its old
  // right edge — which is now the middle. The same stale-coordinate mistake as aiming at a card
  // after a reload, one file over.
  const now = await page.evaluate(() => {
    const el = document.querySelector('.pr-notes .pr-note');
    if (!el) return null;
    const r = el.getBoundingClientRect();
    return { left: r.left, right: r.right, mid: r.top + r.height / 2, w: r.width };
  });
  check(!!now, 'the note is still on screen to hover');
  const box2 = now || box;
  const cursorAt = async (x) => {
    await page.mouse.move(x, box2.mid);
    await settle(120);
    return page.evaluate(() => {
      const b = document.querySelector('.pr-band');
      return b ? (b.style.cursor || getComputedStyle(b).cursor) : 'no band';
    });
  };
  const onEdge = await cursorAt(box2.right - 2);
  const onBody = await cursorAt(box2.left + Math.max(3, box2.w * 0.3));
  check(onEdge === 'ew-resize',
        'THE CURSOR OVER THE RIGHT EDGE SAYS RESIZE', `got ${JSON.stringify(onEdge)}`);
  check(onBody !== 'ew-resize',
        'and over the body it does not — the middle moves the note',
        `got ${JSON.stringify(onBody)}; a cursor on the whole note promises a resize the app `
        + 'will not perform');
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
