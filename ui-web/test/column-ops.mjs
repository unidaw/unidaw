#!/usr/bin/env node
/**
 * EVERY SELECTION OP FORGOT WHICH NOTE COLUMN IT WAS ON.
 *
 * Four ops — select, delete, copy/paste, transpose — each built its own message, and all four
 * dropped the same field. The sidecar reads `"column"` and DEFAULTS IT TO ZERO when it is absent,
 * so a missing column is not a refusal; it is a confident edit to the wrong cell.
 *
 *   select     `n.track * state.columns` under a comment reading "notes are column 0", so a
 *              selection over column 0 swept up every other column's notes too, and a selection
 *              over column 1 alone matched nothing.
 *   delete     removed COLUMN 0's note at that tick — a different note — and left the selected
 *              one alive. The sidecar's own comment on that arm predicts this exactly.
 *   copy+paste collapsed a two-column phrase onto column 0, where the second note replaces the
 *              first, so half of it silently disappears.
 *   transpose  wrote the new pitch at column 0 and left the original at its old pitch: one note
 *              became two, only one of them transposed.
 *
 * ── WHY THIS MATTERS RIGHT NOW ──────────────────────────────────────────────────────────────
 *
 * The owner asked for note columns last night, for one reason: two overlapping note-ons on one
 * channel is how you write a 303 slide, and inside a single column it cannot be expressed at all.
 * So the second column is not a power feature here — it IS the legato feature, and every editing
 * op that touches it was quietly wrong.
 *
 * ── AND WHY THE SUITE THAT ALREADY "COVERS" TRANSPOSE DOES NOT ──────────────────────────────
 *
 * `journey.mjs` selects a row, runs `transpose 12`, and asserts the pitch SET at that row and
 * track changed. It never filters by column — so under the bug, where the transposed copy lands
 * in column 0 and the original stays put, the set goes from {67} to {67, 79}. It CHANGED. The
 * check passes, and has been passing, with the defect present the whole time.
 *
 * That is the recurring trap in this project rather than a slight on that suite: a green test
 * that would be green either way. It is also why "transpose is covered" was not a reason to skip
 * this file.
 *
 * ── WHY IT ASSERTS THE SAVED DOCUMENT ───────────────────────────────────────────────────────
 *
 * A note written to the wrong column still draws a note on screen, one cell over, in a grid that
 * is mostly empty. Nothing about the display distinguishes "transposed" from "duplicated into the
 * next column". The file does.
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

/** Every note in the saved project as {tick, dur, pitch, col}, sorted for stable comparison. */
const notes = async (name) => {
  await run(`save ${name}`);
  for (let i = 0; i < 40; i++) {
    const p = join(stack.dir, `${name}.uniproj.json`);
    if (existsSync(p)) {
      try {
        const doc = JSON.parse(readFileSync(p, 'utf8'));
        return (doc.clips || []).flatMap((c) => (c.notes || []).map((n) => ({
          tick: n.nanotick, dur: n.duration, pitch: n.pitch, col: n.column || 0,
        }))).sort((a, b) => a.col - b.col || a.tick - b.tick || a.pitch - b.pitch);
      } catch { /* mid-write */ }
    }
    await settle(150);
  }
  return null;
};

/** Put a note at the cursor in a given note column, via the cursor the way a person would. */
const writeAt = async (row, noteColumn, pitch, dur) => {
  await run(`goto ${row}`);
  await settle(150);
  // `note` writes wherever the cursor is, and `cursor.col` is a CELL index — three cells per
  // note column. Derived rather than assumed: where the cursor starts depends on what it was on.
  const at = await page.evaluate(() => window.__uni.state().cursor.col);
  const want = noteColumn * 3;
  for (let i = at; i < want; i++) { await page.keyboard.press('ArrowRight'); await settle(80); }
  for (let i = at; i > want; i--) { await page.keyboard.press('ArrowLeft'); await settle(80); }
  const now = await page.evaluate(() => window.__uni.state().cursor.col);
  check(Math.floor(now / 3) === noteColumn,
        `the cursor is in note column ${noteColumn}`, `cursor.col=${now}`);
  await run(dur === undefined ? `note ${pitch}` : `note ${pitch} ${dur}`);
  await settle(500);
};

console.log('\nselection ops and the note column they were dropping\n');

await run('new colops');
await settle(1200);
await run('view tracker');
await settle(400);
await page.evaluate(() => { const g = document.querySelector('#tracker'); if (g) g.click(); });
await settle(300);
// Two columns to work in.
await page.keyboard.press('Shift+BracketRight');
await settle(600);
check((await page.evaluate(() => window.__uni.state().noteColumns)) === 2,
      'two note columns to work in');

/*
 * THE FIXTURE: one note in each column, at DIFFERENT ROWS.
 *
 * Different rows on purpose. With both at row 0, an op that wrote to the wrong column would
 * collide with the other note and the wreckage would be ambiguous — "did it move or did it
 * overwrite?". Apart, each note's fate is readable on its own.
 */
await writeAt(0, 0, 60);
await writeAt(4, 1, 67);

const start = await notes('co_start');
check(start && start.length === 2, 'one note in each column to begin with', JSON.stringify(start));
check(start && start.some((n) => n.col === 0 && n.pitch === 60)
            && start.some((n) => n.col === 1 && n.pitch === 67),
      'and they are where they were typed', JSON.stringify(start));

/* ── MEMBERSHIP, over a PARTIAL range of cells ─────────────────────────────────────────────
 *
 * The console's `select` spans a whole track — every note column of it — so it cannot express
 * "column 1 only" and the membership bug is invisible from there. A MOUSE DRAG can, and
 * `selectRows(r0, r1, f0, f1)` is the same cell range the drag builds, which is why it is driven
 * here rather than faked with a hand-set state object.
 */
{
  const cols = await page.evaluate(() => window.__uni.state().columns);
  check(cols === 6, 'six cells across: two note columns of three fields', String(cols));

  // Column 1's three cells only.
  await page.evaluate((c) => window.__uni.selectRows(0, 7, 3, 5), cols);
  await settle(250);
  const onlyC1 = await page.evaluate(() => window.__uni.selected());
  check(onlyC1.length === 1 && onlyC1[0].pitch === 67,
        'A SELECTION OVER COLUMN 1 ALONE PICKS COLUMN 1\'S NOTE',
        `${JSON.stringify(onlyC1)} — the membership test placed every note at column 0's cell, so `
        + 'this matched nothing');

  // ...and column 0's three cells only. The CONTRAST is the point: a rule that returned every
  // note regardless would pass the check above and fail this one.
  await page.evaluate(() => window.__uni.selectRows(0, 7, 0, 2));
  await settle(250);
  const onlyC0 = await page.evaluate(() => window.__uni.selected());
  check(onlyC0.length === 1 && onlyC0[0].pitch === 60,
        'and a selection over column 0 alone picks ONLY column 0\'s',
        `${JSON.stringify(onlyC0)} — under the old rule this swept up every column's notes`);
}

/* ── TRANSPOSE ────────────────────────────────────────────────────────────────────────────── */
{
  // The WHOLE TRACK, through the console's own verb — both notes, across both columns. This is
  // the ordinary gesture, and it is enough to expose the write-side bug on its own.
  const n = await run('select 0 7');
  check(/2 note/.test(String(n)), 'selecting rows 0-7 picks both notes', String(n));

  await run('transpose 2');
  await settle(1200);
  const after = await notes('co_transposed');
  check(after && after.length === 2,
        'TRANSPOSE DOES NOT DUPLICATE THE NOTE',
        `${JSON.stringify(after)} — writing the new pitch without a column put column 1's note `
        + 'into column 0 while the original stayed put at its old pitch: two notes became three');
  const c1 = (after || []).filter((x) => x.col === 1);
  const c0 = (after || []).filter((x) => x.col === 0);
  check(c1.length === 1 && c1[0].pitch === 69,
        'column 1\'s note transposes IN PLACE, 67 -> 69', JSON.stringify(after));
  check(c0.length === 1 && c0[0].pitch === 62,
        'and column 0\'s does too, 60 -> 62', JSON.stringify(after));
}

/* ── DELETE, ONE NOTE AT A TIME ────────────────────────────────────────────────────────────
 *
 * ONE note, deliberately, and the reason is a second bug this file is NOT about.
 *
 * Deleting a selection of SEVERAL notes removes only the LAST one. Measured on a single-column
 * project, so it has nothing to do with columns: two notes selected leaves one, three leaves two.
 * Measured again on the UNPATCHED page, so it is not something this change introduced. It is
 * filed separately; asserting it here would make this suite fail for a reason it does not
 * describe, and a check that fails for two reasons at once tells you neither.
 *
 * So the column question is asked in isolation: select the row that ONLY column 1's note is on,
 * delete it, and see which note goes. `select` spans the whole track, which is fine — the row
 * narrows it to one note.
 */
{
  const before = await notes('co_predelete');
  check(before && before.length === 2, 'two notes going in', JSON.stringify(before));

  // Row 4 holds column 1's note alone; column 0's is on row 0.
  const n = await run('select 4 4');
  check(/1 note/.test(String(n)), 'selecting row 4 picks exactly one note', String(n));

  await page.keyboard.press('Delete');
  await settle(1200);
  const after = await notes('co_deleted');
  check(after && after.length === 1, 'one note removed, one left', JSON.stringify(after));
  check(after && after.every((x) => x.col !== 1),
        'AND IT IS COLUMN 1\'S NOTE THAT IS GONE',
        `${JSON.stringify(after)} — the op carried no column, so the wire defaulted it to 0 and `
        + 'the delete addressed a different cell entirely');
  check(after && after.some((x) => x.col === 0 && x.pitch === 62),
        'COLUMN 0\'S NOTE SURVIVES — the check that catches a delete aimed at the wrong column',
        JSON.stringify(after));
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
