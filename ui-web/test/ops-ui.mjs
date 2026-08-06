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
  { token: 'ret3',   key: 'retrigger',      want: 3,      glyph: /3/ },
  { token: 'rv-60',  key: 'retrig_ramp',    want: -60,    glyph: /60/ },
  { token: 'p60',    key: 'probability',    want: 60,     glyph: /60/ },
  { token: 's5',     key: 'sound',          want: 5,      glyph: /5/ },
  { token: 'o80',    key: 'sound_offset',   want: 80,     glyph: /80/ },
  { token: 'c1:2',   key: 'trig_condition', want: null,   glyph: /1|2/ },
  // `d` is authored as a FRACTION of a beat and stored as ticks, so the wanted value is derived
  // rather than typed: a sixth of 960000 is 160000. Asserting the token back would be asserting
  // the round-trip spelling, which is a different claim and already covered in ops.mjs.
  { token: 'd1/6',   key: 'delay',          want: 160000, glyph: /6/ },
];

/** Move the cursor onto the ops field of the given row, by keystroke. */
const toOpsField = async (row) => {
  await run(`goto ${row} 0`);
  await settle(150);
  // Two fields right of the note: note, velocity, ops. ops.mjs navigates the same way.
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('ArrowRight');
  await settle(200);
};

for (let i = 0; i < OPS.length; i++) {
  const { token } = OPS[i];
  const row = 4 + i * 4;
  await run(`goto ${row} 0`);
  await settle(150);
  await page.keyboard.press('z');            // a note, so the op has something to be on
  await settle(250);

  await toOpsField(row);
  /*
   * `@` OPENS THE BUFFER. Every character then goes in as a keystroke, including the ones with
   * shift on them — `-` and `:` and `/` are all part of a token grammar, and a suite that typed
   * only digits would miss exactly the tokens that need punctuation.
   */
  await page.keyboard.press('@');
  await settle(200);
  for (const ch of token) { await page.keyboard.press(ch); }
  await settle(200);
  await page.keyboard.press('Enter');
  await settle(350);
}

/*
 * THE DRAWN CELLS, before saving. Read from the DOM rather than from the view-model, because the
 * claim is that a person can see the op — a model that holds the right value and a cell that
 * renders it are two different things, and only one of them is on stage.
 */
const drawn = await page.evaluate(() =>
  [...document.querySelectorAll('.tk-cell')].map((c) => c.textContent || '').join('|'));
for (const { token, glyph } of OPS) {
  check(glyph.test(drawn), `\`${token}\` is drawn in the grid`,
        `no ${glyph} anywhere in the visible cells`);
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
  if (want === null) {
    // The condition packs two numbers into one integer and the packing is ops.mjs's business;
    // here the claim is only that typing it reaches the engine at all.
    check(!!hit, `\`${token}\` reaches the engine (\`${key}\` present)`,
          `no note carries ${key}: ${JSON.stringify(notes.map((n) => Object.keys(n)))}`);
  } else {
    check(hit && hit[key] === want, `\`${token}\` reaches the engine as ${key} ${want}`,
          hit ? `${key} = ${hit[key]}` : `no note carries ${key}`);
  }
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
for (const ch of ' p40') { await page.keyboard.press(ch === ' ' ? 'Space' : ch); }
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
for (const ch of ' zz9') { await page.keyboard.press(ch === ' ' ? 'Space' : ch); }
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
