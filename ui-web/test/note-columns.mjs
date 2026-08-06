#!/usr/bin/env node
/**
 * A SECOND NOTE COLUMN, FROM THE KEYBOARD — and two notes that overlap because of it.
 *
 * The engine has supported several note columns per track since the store was written, and the
 * count was reachable only by typing `columns N` at the console. A capability nobody clicking or
 * typing normally can find is one nobody has.
 *
 * ── WHY THIS IS THE LEGATO FEATURE ──────────────────────────────────────────────────────────
 *
 * A monophonic synth glides between two notes instead of retriggering when the second note-on
 * arrives while the first is still held. Producing that means two notes that OVERLAP IN TIME.
 *
 * Inside one column you cannot say it. A tracker column is monophonic by construction — the next
 * note-on IS the previous note's end — and the OFF row is a setter with one implicit target, so
 * with two notes sounding it cannot name which one it ends. Between two columns there is no
 * ambiguity at all: each column still has at most one thing sounding, each keeps its own OFF, and
 * the instrument receives both note-ons on one channel. Same MIDI, nothing unaddressable.
 *
 * So this suite does not stop at "the column count went up". It writes a note in each column at
 * ticks that OVERLAP and asserts both survive in the saved project with their own lengths —
 * which is the condition a mono synth reads as legato.
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
const cols = () => page.evaluate(() => {
  const s = window.__uni.state();
  return { shown: s.noteColumns, wanted: s.noteColumnsWanted };
});
const notes = async (name) => {
  await run(`save ${name}`);
  for (let i = 0; i < 40; i++) {
    const p = join(stack.dir, `${name}.uniproj.json`);
    if (existsSync(p)) {
      try {
        const doc = JSON.parse(readFileSync(p, 'utf8'));
        return (doc.clips || []).flatMap((c) => (c.notes || []).map((n) => ({
          tick: n.nanotick, dur: n.duration, pitch: n.pitch, col: n.column || 0,
        }))).sort((a, b) => a.col - b.col || a.tick - b.tick);
      } catch { /* mid-write */ }
    }
    await settle(150);
  }
  return null;
};

console.log('\na second note column, and the overlap it makes possible\n');

await run('new notecols');
await settle(1200);
await run('view tracker');
await settle(500);
// The grid has to own the keyboard, or the keystroke goes nowhere — which is itself a documented
// trap in this app ("goto moves the cursor but does not hand over the keys").
await page.evaluate(() => { const g = document.querySelector('#tracker'); if (g) g.click(); });
await settle(400);

const start = await cols();
check(start.shown === 1, 'one note column to begin with', JSON.stringify(start));

// ── THE KEY ─────────────────────────────────────────────────────────────────────────────────
await page.keyboard.press('Shift+BracketRight');
await settle(600);
const after = await cols();
check(after.shown === 2,
      'SHIFT+] ADDS A NOTE COLUMN',
      `${JSON.stringify(start)} -> ${JSON.stringify(after)} — the count was reachable only by `
      + 'typing `columns 2` at the console, which is a capability nobody can find');

await page.keyboard.press('Shift+BracketLeft');
await settle(600);
check((await cols()).shown === 1, 'and SHIFT+[ takes it away again',
      JSON.stringify(await cols()));

await page.keyboard.press('Shift+BracketRight');
await settle(600);
check((await cols()).shown === 2, 'back to two for the rest of this');

// ── AND THE OVERLAP THE SECOND COLUMN EXISTS FOR ────────────────────────────────────────────
// Column 0 gets a long note; column 1 gets one that starts while the first is still sounding.
// Written through the console's own verb so this exercises the document, not the key handler.
/*
 * THE CURSOR CARRIES THE COLUMN. `note <pitch> [dur] [vel]` has no column argument — it writes
 * wherever the cursor is, and `state.cursor.col` is a CELL index, three cells per note column.
 * So column 1's note field is three ArrowRights away, and that is the gesture a person makes.
 */
await run('goto 0');
await settle(200);
// A LONG first note: the default ends where the next event does, which cannot overlap anything.
await run('note 60 3840000');
await settle(600);
await run('goto 2');
await settle(200);
for (let i = 0; i < 3; i++) { await page.keyboard.press('ArrowRight'); await settle(120); }
const atCol1 = await page.evaluate(() => window.__uni.state().cursor.col);
// WHICH COLUMN, derived — not which cell. `cursor.col` is a cell index and where it starts
// depends on what the cursor was on beforehand, so asserting a literal 3 was asserting my
// arithmetic rather than the thing that matters: three rights move you one note column over.
check(Math.floor(atCol1 / 3) === 1,
      'three cursor-rights move into the SECOND note column',
      `cursor.col=${atCol1} -> note column ${Math.floor(atCol1 / 3)}`);
await run('note 67 3840000');
await settle(800);

const doc = await notes('nc_out');
check(!!doc && doc.length >= 2, 'two notes on the track', JSON.stringify(doc));

if (doc && doc.length >= 2) {
  const c0 = doc.filter((n) => n.col === 0);
  const c1 = doc.filter((n) => n.col === 1);
  check(c0.length >= 1 && c1.length >= 1,
        'ONE IN EACH COLUMN — which is what a second column is for',
        `column 0: ${JSON.stringify(c0)}  column 1: ${JSON.stringify(c1)}`);

  if (c0.length && c1.length) {
    const a = c0[0];
    const b = c1[0];
    // The condition a mono synth reads as legato: A still sounding when B starts. Asserted on
    // the DOCUMENT, because what the plugin then does with it is the plugin's portamento setting
    // and not this engine's business.
    check(a.tick + a.dur > b.tick,
          'AND THEY OVERLAP — the first is still sounding when the second starts',
          `column 0 runs ${a.tick}..${a.tick + a.dur}, column 1 starts at ${b.tick}. Inside ONE `
          + 'column this cannot be expressed at all: the next note-on is the previous note\'s end');
  }
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
