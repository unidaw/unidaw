#!/usr/bin/env node
/**
 * CAN YOU TELL WHAT IS IN A CELL — and specifically, is this chord a strum?
 *
 * The question that prompted this, asked with the app open: "how do I edit/inspect a chord?
 * how do I know whether it's a strum?" You could not, and the reason was in three places at
 * once, which is why it survived:
 *
 *   the engine     publishes spreadNanoticks, humanizeTiming, humanizeVelocity on every
 *                  frame, and has since UiClipChord had them
 *   the sidecar    can WRITE all three — `build_chord` parses "spread"/"ht"/"hv" and has
 *                  unit tests for them
 *   wire.js        decoded none of them, and dockApi.chord passed three hardcoded zeroes
 *
 * So the capability existed at both ends with nothing joining them, in BOTH directions at
 * once: every chord the app could write was a block chord, and no chord it could read could
 * be told apart from one. That is the defect this repo keeps finding, and it is invisible
 * from either end alone — the engine's tests pass, the sidecar's tests pass, and the thing
 * does not work.
 *
 * WHAT THIS ASSERTS, in the order that matters:
 *
 *   1. a strum can be WRITTEN — the console verb carries spread/ht/hv
 *   2. it comes BACK — the decoder keeps all three across the wire
 *   3. the INSPECTOR says so in words, for the cursor's cell and for a hovered one
 *   4. a block chord is stated POSITIVELY, not by the absence of a line
 *
 * (4) is the one worth defending. "No strum line" and "the panel does not know about
 * strums" look identical on screen, and the second was the state of the world an hour
 * before this file existed. A panel that says "no — a block chord" cannot be confused with
 * a panel that has never heard of the question.
 *
 * THE PROBE IS THE PANEL'S OWN STRINGS. `__uni.inspect()` returns what the view last wrote
 * into its nodes, not a re-derivation from the model — a test that recomputed the expected
 * text would pass while the panel drew something else entirely.
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
const insp = () => page.evaluate(() => window.__uni.inspect());
const settle = (ms) => page.waitForTimeout(ms);
/** One inspector line's value, by label, or undefined. */
const line = (vm, label) => {
  const r = (vm.rows || []).find((x) => x.label === label);
  return r ? r.value : undefined;
};

console.log('\nthe cell inspector\n');

await run('new inspect');
await settle(1200);

// ---------------------------------------------------------------------------
// An empty cell says so — the panel is never blank, because a blank panel and a
// broken panel are the same thing to look at.
// ---------------------------------------------------------------------------
await run('goto 0 0');
await settle(400);
const empty = await insp();
check(empty.title === 'empty', 'an empty cell is described as empty', JSON.stringify(empty.title));
check(line(empty, 'cell') === 'nothing here', 'and says what it means', JSON.stringify(empty));

// ---------------------------------------------------------------------------
// A BLOCK CHORD. Written with no spread, and reported as a block chord IN WORDS.
// ---------------------------------------------------------------------------
await run('goto 0 0');
await run('chord 1 triad');
await settle(1200);

const block = await insp();
check(/chord/.test(block.title), 'a chord cell is described as a chord', block.title);
check(line(block, 'degree') !== undefined, 'the degree is named', JSON.stringify(block.rows));
check(line(block, 'quality') === 'triad', 'the quality is named', line(block, 'quality'));
/*
 * THE POSITIVE STATEMENT. See the header: the absence of a strum line would be
 * indistinguishable from a panel that does not know what a strum is.
 */
check(/^no —/.test(String(line(block, 'strum'))),
      'a block chord SAYS it is a block chord, rather than omitting the line',
      JSON.stringify(line(block, 'strum')));

// ---------------------------------------------------------------------------
// A STRUM. The whole point.
// ---------------------------------------------------------------------------
await run('goto 4 0');
await settle(300);
const wrote = await run('chord 4 seventh 1 4 240000 20 15');
await settle(1500);

const strum = await insp();
check(!/refus|unknown|expects/i.test(String(wrote)),
      'the console verb accepts a spread and two humanize amounts', JSON.stringify(wrote));
check(/^yes —/.test(String(line(strum, 'strum'))),
      'a strummed chord is reported as a strum',
      `the panel says ${JSON.stringify(line(strum, 'strum'))}`);
check(/240000nt/.test(String(line(strum, 'strum'))),
      'and says HOW MUCH, in the units a command takes',
      JSON.stringify(line(strum, 'strum')));
check(/timing 20/.test(String(line(strum, 'humanize')))
      && /velocity 15/.test(String(line(strum, 'humanize'))),
      'the humanize amounts survive the round trip',
      JSON.stringify(line(strum, 'humanize')));
check(line(strum, 'quality') === 'seventh', 'the seventh survives too', line(strum, 'quality'));
check(String(line(strum, 'inversion')).includes('1'), 'and the inversion',
      line(strum, 'inversion'));

/*
 * THE ROUND TRIP IS THE CLAIM, so it is worth being explicit: these values are not what the
 * page remembers sending. They came back from the engine through the shared-memory frame and
 * the decoder. Proven by the block chord above reading `no` from the same panel — if the
 * inspector were echoing the last command, both cells would say the same thing.
 */
check(String(line(block, 'strum')) !== String(line(strum, 'strum')),
      'the two cells disagree — the panel is reading the chord, not the last command');

// ---------------------------------------------------------------------------
// HOVER. The same panel, describing a cell the cursor is not on.
// ---------------------------------------------------------------------------
await run('goto 0 0');                       // cursor on the BLOCK chord
await settle(400);
await page.evaluate(() => window.__uni.hover(4, 0, 0));   // pointer on the STRUM
await settle(400);

const hovered = await insp();
check(/hovered/.test(String(hovered.subtitle)),
      'the panel says it is describing a hovered cell, not the cursor', hovered.subtitle);
check(/^yes —/.test(String(line(hovered, 'strum'))),
      'hovering a different cell describes THAT cell',
      `cursor is on the block chord; the panel says ${JSON.stringify(line(hovered, 'strum'))}`);

await page.evaluate(() => window.__uni.hover(null));
await settle(400);
const backToCursor = await insp();
check(!/hovered/.test(String(backToCursor.subtitle)),
      'and leaving the grid returns it to the cursor', backToCursor.subtitle);
check(/^no —/.test(String(line(backToCursor, 'strum'))),
      'which is the block chord again', JSON.stringify(line(backToCursor, 'strum')));

// ---------------------------------------------------------------------------
// A NOTE, with its row ops spelled out. The cell shows `C-4` and a glyph run;
// the panel is where the seven ops have room to be named.
// ---------------------------------------------------------------------------
await run('goto 8 0');
await settle(300);
await run('note 48');
await settle(900);
const note = await insp();
check(/note/.test(String(note.title)), 'a note cell is described as a note', note.title);
check(/48/.test(String(line(note, 'pitch'))), 'the pitch is named and numbered',
      line(note, 'pitch'));
check(line(note, 'velocity') !== undefined, 'the velocity is shown', JSON.stringify(note.rows));
check(line(note, 'length') !== undefined, 'and the length, which the cell cannot show',
      line(note, 'length'));
check(String(line(note, 'row ops')) === 'none' || line(note, 'as typed') !== undefined,
      'row ops are listed, or stated absent', JSON.stringify(note.rows));

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
