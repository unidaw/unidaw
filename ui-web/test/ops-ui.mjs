#!/usr/bin/env node
/**
 * EVERY ROW OP, TYPED WITH REAL KEYSTROKES, READ BACK OFF DISK.
 *
 * `ops.mjs` covers the ops thoroughly and writes almost all of them through the CONSOLE:
 * `run('ops ret5 p25')`, thirty-odd times. The keyboard appears there only to prove the buffer
 * opens seeded and that the grammar overlay is drawn. So the surface a person actually uses —
 * stand on the cell, press `@`, type, Enter — is exercised for two ops and asserted for none of
 * the rest.
 *
 * That matters here more than it usually would. `ops` and `op` are one code path and `@` is a
 * different one: the console parses a whole line and applies it, while the cell buffer opens
 * SEEDED with what the note already has, is edited in place, and is committed on Enter. A
 * console test passing says nothing about whether the seed survived the edit.
 *
 * WHAT IT ASSERTS, AND WHY IT IS THE SAVED PROJECT.
 *
 * `notes()` publishes `delay` and nothing else of the ops — no retrigger, probability, sound,
 * offset or condition — so there is no engine-truth read for most of them on this side.
 * `opsTextAtCursor()` is the UI's own model, and asserting the UI against the UI is how a cell
 * that draws the right text over an engine that never got the edit passes forever.
 *
 * The project file is written by the ENGINE from its own note records (project_file.cpp:341),
 * and it OMITS an op that is inert — so a dropped keystroke is an absent key, not a zero. That
 * is the reading that can tell "typed and stored" from "typed and drawn".
 *
 * The DRAWN CELL is checked too, because "every one of which is drawn" is a claim the demo
 * runbook makes out loud, and a value that reaches the engine but renders as nothing is a
 * feature nobody can see.
 */

import { chromium } from 'playwright';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const SONG = 'opsui';
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

console.log('\ntyping row ops into the grid\n');

await run(`new ${SONG}`);
await settle(1400);
await run('view tracker');
await settle(400);

/*
 * CLICK THE GRID. `goto` moves the cursor and does NOT hand over the keyboard — the single most
 * common way to make this app look broken, and a whole suite of silent no-ops if skipped here.
 */
const grid = await page.$('#tracker') || await page.$('.tk');
const box = grid ? await grid.boundingBox() : null;
check(!!box, 'the tracker grid is on screen');
if (box) await page.mouse.click(box.x + 60, box.y + Math.min(60, box.height / 2));
await settle(300);
check((await page.evaluate(() => window.__uni.state().focus)) === 'centre',
      'clicking it gives the grid the keyboard');

if (!(await page.evaluate(() => window.__uni.state().editMode))) {
  await page.keyboard.press('Meta+e');
  await settle(300);
}
check((await page.evaluate(() => window.__uni.state().editMode)) === true, 'edit mode is on');

/*
 * ONE NOTE PER OP, on its own row, so a failure names an op rather than a row. Written with the
 * PIANO KEYS — `z` at the default octave — because a note placed through the API would not prove
 * the cursor is where the ops are about to be typed.
 */
const OPS = [
  { token: 'ret3',   key: 'retrigger',      want: 3,      glyph: 'r' },
  // `v`, not `r`: `ret` already owns 'r' and two ops sharing a glyph makes a collapsed run
  // ambiguous, since the glyph is the only thing saying which op it is.
  { token: 'rv-60',  key: 'retrig_ramp',    want: -60,    glyph: 'v' },
  { token: 'p60',    key: 'probability',    want: 60,     glyph: 'p' },
  { token: 's5',     key: 'sound',          want: 5,      glyph: 's' },
  /*
   * `o80` MEANS 80/256 OF THE WAY IN, and the engine stores the fraction against 65535 — so the
   * stored value is 20480, not 80. I expected 80 and the app was right; rowops.js:106 spells the
   * contract out, including that a multiple of 256 spells back as the tracker form.
   */
  { token: 'o80',    key: 'sound_offset',   want: 20480,  glyph: 'o' },
  /*
   * `c1:2` packs as ((a-1) << 3 | (b-1)) + 1 so that 0 stays "no condition" — 1:2 is 2. Asserted
   * as the exact packed value rather than as "present": presence passes on any condition at all,
   * including one the parser mangled.
   */
  { token: 'c1:2',   key: 'trig_condition', want: 2,      glyph: 'c' },
  // `d` is authored as a FRACTION of a beat and stored as ticks, so the wanted value is derived
  // rather than typed: a sixth of 960000 is 160000. Asserting the token back would be asserting
  // the round-trip spelling, which is a different claim and already covered in ops.mjs.
  { token: 'd1/6',   key: 'delay',          want: 160000, glyph: 'd' },
];

/**
 * Move the cursor onto the ops field of the given row, by keystroke, and CHECK IT LANDED.
 *
 * Two rights from the note field: `fieldOfCol` is `col % FIELDS_PER_NOTE` and the ops field is
 * 2 (index.html:349, :5246). ops.mjs navigates the same way and does not verify it, which is
 * fine there because it writes through the console — here the keystrokes ARE the test, and
 * typing a token into the wrong field would be refused silently and read as a broken op.
 *
 * So the landing is asserted once rather than assumed seven times.
 */
let fieldChecked = false;
/**
 * Back to the NOTE field of a row.
 *
 * `goto <row> <track>` moves the row and the track and leaves the cursor on whatever FIELD it
 * was already in — so after the first op is typed the cursor is sitting on the ops field, and
 * every `z` after that goes into a field that does not take note keys and is silently refused.
 *
 * That is exactly what happened: seven notes were asked for, one was written, and the six
 * missing ones read as six broken ops rather than as one navigation mistake. The count check
 * below is what told them apart.
 */
const toNoteField = async (row) => {
  await run(`goto ${row} 0`);
  await settle(150);
  for (let i = 0; i < 4; i++) {
    const f = await page.evaluate(() => {
      const c = window.__uni.state().cursor;
      return c && c.col !== undefined ? c.col % 3 : -1;
    });
    if (f === 0) return true;
    await page.keyboard.press('ArrowLeft');
    await settle(80);
  }
  return false;
};

const toOpsField = async (row) => {
  await run(`goto ${row} 0`);
  await settle(150);
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('ArrowRight');
  await settle(200);
  if (!fieldChecked) {
    fieldChecked = true;
    const field = await page.evaluate(() => {
      const c = window.__uni.state().cursor;
      return c && c.col !== undefined ? c.col % 3 : -1;
    });
    check(field === 2, 'two cursor-rights from the note lands on the OPS field',
          `field ${field} — if the tracker gained or lost a field, every token below goes `
          + `somewhere else and is refused without saying so`);
  }
};

for (let i = 0; i < OPS.length; i++) {
  const { token } = OPS[i];
  // CONSECUTIVE, so all seven are in view at once when the ops column is read below. Spread
  // four rows apart they ran off the bottom and the last two never appeared — which read as two
  // undrawn ops rather than as a scrolled window.
  const row = 4 + i;
  const onNote = await toNoteField(row);
  if (!onNote) { check(false, `row ${row}: could not get back to the note field`); continue; }
  await page.keyboard.press('z');            // a note, so the op has something to be on
  await settle(250);

  await toOpsField(row);
  /*
   * `@` OPENS THE BUFFER, then the token is TYPED.
   *
   * `keyboard.type()` and not a press() per character. press() takes a KEY, and for a character
   * that needs shift on this layout it does not produce the character at all — `rv-60` arrived as
   * retrig_ramp 60 with the sign dropped, and `c1:2` arrived as nothing, while the digits either
   * side landed perfectly. Two ops that looked broken and were mistyped by the test.
   *
   * The punctuation is the reason this suite exists: `-`, `:` and `/` are all part of the token
   * grammar, and a suite that only managed digits would assert the easy half of it.
   */
  await page.keyboard.press('@');
  await settle(200);
  await page.keyboard.type(token, { delay: 30 });
  await settle(200);
  await page.keyboard.press('Enter');
  await settle(350);
}

/*
 * THE DRAWN CELLS, before saving. From the DOM rather than the view-model, because the claim is
 * that a person can SEE the op — a model holding the right value and a cell rendering it are two
 * different things, and only one of them is on stage.
 *
 * `.tk-cell.ops`, NOT `.tk-cell`. The first version of this scanned EVERY cell for a substring
 * and was worthless in both directions: `/3/` and `/6/` matched row numbers and passed while the
 * ops were silently wrong, and the rest failed for reasons that had nothing to do with the ops
 * column. tracker.js:744 gives the ops cells their own class; using it means a match can only
 * have come from an op.
 */
const opsCells = await page.evaluate(() =>
  [...document.querySelectorAll('.tk-cell.ops')].map((c) => (c.textContent || '').trim())
    .filter(Boolean));
console.log(`  ops cells drawn: ${JSON.stringify(opsCells)}`);
check(opsCells.length > 0, 'the ops column is drawn at all', 'no .tk-cell.ops has any text');
/*
 * THE GLYPH IS WHAT IS DRAWN, not the value. A cell shows a run of one letter per op — `opGlyph`
 * is `op.glyph || op.prefix[0]` — and the runbook's claim is exactly that: "a set of named
 * per-note ops, every one of which is drawn". Asserting the VALUE here would be asserting
 * something the column does not attempt, and asserting a bare digit matched row numbers.
 */
for (const { token, glyph } of OPS) {
  check(opsCells.some((t) => t.includes(glyph)), `\`${token}\` draws its \`${glyph}\` glyph`,
        `no ${glyph} in ${JSON.stringify(opsCells)}`);
}

await run(`save ${SONG}`);
await settle(2000);

/*
 * AND WHAT THE ENGINE STORED. project_file.cpp omits an inert op entirely, so a key that is
 * absent means the keystrokes never reached the note — which is precisely the failure a
 * UI-against-UI assertion cannot see.
 */
let notes = [];
try {
  const doc = JSON.parse(readFileSync(join(stack.dir, `${SONG}.uniproj.json`), 'utf8'));
  notes = (doc.clips || []).flatMap((c) => c.notes || []);
} catch (e) {
  check(false, 'the saved project is readable', String(e).slice(0, 160));
}
check(notes.length === OPS.length, `all ${OPS.length} notes were written by the keyboard`,
      `${notes.length} note(s) in the saved clips`);

for (const { token, key, want } of OPS) {
  const hit = notes.find((n) => n[key] !== undefined);
  check(hit && hit[key] === want, `\`${token}\` reaches the engine as ${key} ${want}`,
        hit ? `${key} = ${hit[key]}` : `no note carries ${key}`);
}

/*
 * THE SEED SURVIVES AN EDIT, which is the whole reason the buffer opens with the existing text in
 * it rather than empty. Add a second op to a note that already has one and BOTH must remain — a
 * buffer that opened selected, or one that replaced rather than merged, loses the first.
 */
await toOpsField(4);                                   // the `ret3` note
await page.keyboard.press('@');
await settle(200);
const seeded = await page.evaluate(() => {
  const c = document.querySelector('.tk-cell[data-kind="editing"]');
  return c ? c.textContent : null;
});
check(seeded && seeded.startsWith('ret3'), 'the buffer opens SEEDED with what the note has',
      JSON.stringify(seeded));
await page.keyboard.type(' p40', { delay: 30 });
await settle(200);
await page.keyboard.press('Enter');
await settle(400);
await run(`save ${SONG}2`);
await settle(2000);

try {
  const doc = JSON.parse(readFileSync(join(stack.dir, `${SONG}2.uniproj.json`), 'utf8'));
  const all = (doc.clips || []).flatMap((c) => c.notes || []);
  const both = all.find((n) => n.retrigger === 3 && n.probability === 40);
  check(!!both, 'editing the cell ADDS an op and keeps the one already there',
        `notes: ${JSON.stringify(all.map((n) => ({ r: n.retrigger, p: n.probability })))}`);
} catch (e) {
  check(false, 'the second save is readable', String(e).slice(0, 160));
}

/*
 * A REFUSED TOKEN CHANGES NOTHING. `p0` is out of range (1-100) and the buffer must reject it
 * rather than clamp — a clamp would write 1 and read back as a successful edit.
 */
await toOpsField(4);
await page.keyboard.press('@');
await settle(200);
await page.keyboard.type(' zz9', { delay: 30 });
await page.keyboard.press('Enter');
await settle(400);
const after = await page.evaluate(() => window.__uni.opsTextAtCursor());
check(/ret3/.test(String(after)) && /p40/.test(String(after)),
      'a nonsense token is refused and the existing ops survive', JSON.stringify(after));

check(errors.length === 0, 'nothing threw', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
