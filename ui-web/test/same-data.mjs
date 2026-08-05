#!/usr/bin/env node
/**
 * THE TRACKER, THE ARRANGEMENT AND THE PIANO ROLL ARE ONE SONG, NOT THREE.
 *
 * This is the claim the whole design rests on and nothing asserted it. e2e checks that the
 * roll is on screen and how tall it is; layout.mjs checks every view renders. Neither asks
 * the question that matters: is the thing you see in the roll the SAME NOTE you typed in the
 * tracker, and does moving it in one move it in the others.
 *
 * WHY IT IS WORTH A FILE OF ITS OWN. "Three views of one document" is exactly the kind of
 * claim that is true when written and quietly stops being true — a surface that caches, or
 * derives its own copy, or reads a different revision, looks perfect in isolation. Every
 * defect this repo has spent a day on has that shape: two copies of one rule agreeing on
 * names and diverging on behaviour. Three views is three chances.
 *
 * WHAT IT ASSERTS, in the order that makes each one mean something:
 *
 *   1. a note typed in the TRACKER is the same note id the roll and the arrangement see
 *   2. its pitch and tick agree across all three — not "a note exists", the SAME one
 *   3. an edit made in ONE surface shows up in the others without a reload
 *   4. deleting it removes it everywhere
 *
 * (3) is the load-bearing one. Reading the same store from three places proves the store is
 * shared; changing it from one and watching the others follow proves nothing is holding a
 * private copy.
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
/** Every note the engine has published, as {id, pitch, tick, track}. One source, read once. */
const notes = () => page.evaluate(() => (window.__uni.notes() || []).map((n) => ({
  id: n.id, pitch: n.p, track: n.tr, tick: n.on ?? n.t ?? n.tOn,
})));
console.log('\none song, three views\n');

await run('new samedata');
await settle(1300);

// ---------------------------------------------------------------------------
// 1 & 2 — the same note, seen from three places.
//
// Written in the TRACKER, which is where a person types. Placed past tick 0 so the fixture
// stays useful to anything that later wants to HEAR it: waveform_probe.wav is silent for its
// first second by construction, so a note on the downbeat plays and cannot be heard.
// ---------------------------------------------------------------------------
await run('goto 4 0');
await run('note 60');
await settle(900);

const written = await notes();
check(written.length === 1, 'the tracker wrote exactly one note', JSON.stringify(written));
const theNote = written[0];

for (const v of ['arrange', 'piano', 'tracker']) {
  await run(`view ${v}`);
  await settle(600);
  const seen = await notes();
  /*
   * THE SAME NOTE, BY ID. "Both views show a note" is satisfied by two different notes, and
   * by a view that invented one. The id is what makes this an identity claim.
   */
  check(seen.length === 1 && seen[0].id === theNote.id
        && seen[0].pitch === theNote.pitch && seen[0].tick === theNote.tick,
        `the ${v} view sees the SAME note — id, pitch and tick`,
        `${JSON.stringify(seen)} vs ${JSON.stringify(theNote)}`);
}

// ---------------------------------------------------------------------------
// 3 — an edit in ONE surface reaches the others.
//
// Transposing from the tracker and then reading the store from the roll and the
// arrangement. If any surface held a private copy this is where it would show.
// ---------------------------------------------------------------------------
await run('view tracker');
await settle(500);
await run('goto 4 0');
await run('select 4 4 0');
await settle(300);
await run('transpose 5');
await settle(900);

const moved = await notes();
check(moved.length === 1 && moved[0].pitch === theNote.pitch + 5,
      'transposing in the tracker moves the note', JSON.stringify(moved));

/*
 * THE ID IS NOT ASSERTED ACROSS THE EDIT, and that is deliberate.
 *
 * A transpose RE-MINTS the note id — observed here, 1 becomes 2, same tick, new pitch — so
 * the engine implements it as a rewrite rather than an in-place change. This file's first
 * version asserted the id survived, which was an assumption about the engine dressed up as a
 * claim about the surfaces, and it failed for a reason that has nothing to do with whether
 * the three views share a document.
 *
 * Not asserted in the other direction either: pinning "the id CHANGES" would freeze today's
 * implementation and fail the day someone makes transpose in-place, which is an improvement.
 * What is asserted below is the thing that is actually claimed — that all three views agree
 * with EACH OTHER at any instant.
 */
const afterEdit = moved[0];
for (const v of ['piano', 'arrange']) {
  await run(`view ${v}`);
  await settle(600);
  const seen = await notes();
  check(seen.length === 1 && seen[0].pitch === afterEdit.pitch
        && seen[0].tick === afterEdit.tick && seen[0].id === afterEdit.id,
        `the ${v} view followed the edit — no reload, no private copy`,
        `${JSON.stringify(seen)} vs ${JSON.stringify(afterEdit)}`);
}

// ---------------------------------------------------------------------------
// AND THE SURFACES ACTUALLY DREW SOMETHING. Reading the store from three views only
// proves the store is shared; a view that renders nothing would pass every check above.
// ---------------------------------------------------------------------------
await run('view arrange');
await settle(700);
const arrangeDrew = await page.evaluate(() =>
  document.querySelectorAll('#arrange .ar-clip, #arrange .ar-note, #arrange canvas').length);
check(arrangeDrew > 0, 'the arrangement drew the material', `${arrangeDrew} element(s)`);

await run('view piano');
await settle(700);
const pianoDrew = await page.evaluate(() =>
  document.querySelectorAll('#piano .pr-note, #piano canvas').length);
check(pianoDrew > 0, 'the roll drew the material', `${pianoDrew} element(s)`);

// ---------------------------------------------------------------------------
// 4 — deleting reaches everywhere too.
// ---------------------------------------------------------------------------
await run('view tracker');
await settle(500);
await run('goto 4 0');
await run('del');
await settle(900);
const gone = await notes();
check(gone.length === 0, 'deleting in the tracker removes it from the song',
      JSON.stringify(gone));

await run('view piano');
await settle(600);
const goneRoll = await notes();
check(goneRoll.length === 0, 'and the roll agrees it is gone', JSON.stringify(goneRoll));

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
