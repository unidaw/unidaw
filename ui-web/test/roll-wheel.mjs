#!/usr/bin/env node
/**
 * CAN YOU SCROLL THE SCALE ROLL WITH THE WHEEL?
 *
 * Asked for in these words, from live use: "scale roll: I should be able to
 * scroll up/down with mousewheel, left/right with shift-mousewheel". The
 * arrangement and the tracker have had a wheel since they were written; the roll
 * had none, so the one surface whose vertical axis is 128 keys tall could only be
 * moved an octave at a time with the arrow keys.
 *
 * IT DRIVES A REAL WHEEL, not `__uni.pianoTo()`. GUIDELINES 2.15 is about exactly
 * this shape and it has already cost this project once: the arrangement's wheel
 * gestures were written, correct, and unreachable for months because `onNav` was
 * never passed to the constructor — `_wheel` returns on its first line without it,
 * so every gesture was inert while the code that implemented them read fine. A
 * test that calls the handler cannot tell those two worlds apart. `page.mouse.wheel`
 * can.
 *
 * AND IT ASSERTS THE PICTURE MOVED, not only that a number changed. A state field
 * nothing draws from is the same failure one layer down, so each axis is checked
 * twice: the viewport number the model reads, and the DOM the renderer wrote —
 * the octave labels down the key ladder for pitch, the scrolling strip's
 * transform for time.
 *
 * No engine: the gesture is pure front-end and `?engine=off` is the branch the
 * goldens use (GUIDELINES 2.17), so this cannot disturb a stack somebody is using.
 */

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { join, extname, normalize, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
               '.css': 'text/css', '.json': 'application/json', '.woff2': 'font/woff2' };
const srv = createServer((q, r) => {
  const p = join(root, normalize(decodeURI(q.url.split('?')[0])));
  let b; try { b = readFileSync(p); } catch { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': MIME[extname(p)] || 'application/octet-stream',
                     'cache-control': 'no-store' });
  r.end(b);
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1500, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(`http://127.0.0.1:${srv.address().port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
await page.evaluate(() => document.fonts.ready);
await page.waitForTimeout(300);

console.log('\nthe scale roll under a real wheel\n');

await page.evaluate(() => window.__uni.setView('piano'));
await page.waitForTimeout(250);

/**
 * The roll's viewport, from BOTH sides.
 *
 * `low`/`start` are the two numbers the view model is built from. `octaves` and
 * `shift` are what the renderer actually put on screen — the ladder labels only
 * the Cs, so the list of them names the octave window exactly, and the strip's
 * translate names the time window. A change in the first pair with no change in
 * the second is a scroll that happened in a variable and nowhere else.
 *
 * Hidden pool elements are filtered out: surplus keys keep the label they last
 * held (GUIDELINES 3.7 — hide, never remove), so reading them back would report a
 * window from several frames ago.
 */
const view = () => page.evaluate(() => {
  const s = window.__uni.state();
  const grid = document.querySelector('.pr-grid');
  const octaves = [...document.querySelectorAll('.pr-key')]
    .filter((e) => e.style.display !== 'none' && e.textContent)
    .map((e) => e.textContent).join(' ');
  return {
    low: s.pianoLow, start: s.pianoStart, follow: s.followPlayhead,
    octaves, shift: grid ? grid.style.transform : null,
  };
});

const box = await page.evaluate(() => {
  const r = document.getElementById('piano').getBoundingClientRect();
  return { x: r.x, y: r.y, w: r.width, h: r.height };
});
check(box.w > 100 && box.h > 100, 'the scale roll is on screen', JSON.stringify(box));

// Over the middle of the roll, and left there: every wheel below lands here.
await page.mouse.move(box.x + box.w / 2, box.y + box.h / 2);

const wheel = async (dy, shift) => {
  if (shift) await page.keyboard.down('Shift');
  // One notch is ~100px; 240 is a couple of firm turns, comfortably more than
  // the one-key threshold the accumulator holds back sub-key travel with.
  await page.mouse.wheel(0, dy);
  if (shift) await page.keyboard.up('Shift');
  await page.waitForTimeout(200);
  return view();
};

// ---------------------------------------------------------------------------
// 1. PLAIN WHEEL MOVES PITCH, AND ONLY PITCH.
// ---------------------------------------------------------------------------
const v0 = await view();
const down = await wheel(240, false);
check(down.low < v0.low,
      'wheel down walks the window down the keyboard',
      `lowPitch ${v0.low} -> ${down.low}`);
check(down.start === v0.start,
      'and leaves the time axis exactly where it was',
      `startTick ${v0.start} -> ${down.start}`);
check(!!down.octaves && down.octaves !== v0.octaves,
      'the key ladder relabelled, so the picture moved and not just a number',
      `octaves "${v0.octaves}" -> "${down.octaves}"`);

const up = await wheel(-240, false);
check(up.low > down.low,
      'wheel up walks it back up',
      `lowPitch ${down.low} -> ${up.low}`);
check(up.start === down.start,
      'still without touching the time axis',
      `startTick ${down.start} -> ${up.start}`);

/*
 * A WHEEL IS A REQUEST TO LOOK SOMEWHERE, so it must take the view off the
 * playhead — otherwise follow drags it back before the frame you scrolled to is
 * on screen, and the surface reads as one that refuses to scroll. The tracker and
 * the arrangement both make this bargain; asserted here so the roll cannot quietly
 * stop making it.
 */
check(v0.follow === true && up.follow === false,
      'scrolling takes the view off the playhead',
      `followPlayhead ${v0.follow} -> ${up.follow}`);

// ---------------------------------------------------------------------------
// 2. SHIFT MOVES THE OTHER AXIS — the half of the request that says "left/right".
// ---------------------------------------------------------------------------
const before = await view();
const right = await wheel(240, true);
check(right.start > before.start,
      'shift-wheel pans the time axis forward',
      `startTick ${before.start} -> ${right.start}`);
check(right.low === before.low,
      'and leaves the pitch window exactly where it was',
      `lowPitch ${before.low} -> ${right.low}`);
check(right.shift !== before.shift,
      'the scrolling strip carries the pan, so time moved on screen too',
      `transform ${before.shift} -> ${right.shift}`);

const left = await wheel(-240, true);
check(left.start < right.start,
      'shift-wheel the other way pans back',
      `startTick ${right.start} -> ${left.start}`);
check(left.low === right.low,
      'still without touching the pitch window',
      `lowPitch ${right.low} -> ${left.low}`);

// ---------------------------------------------------------------------------
// 3. THE TWO AXES ARE GENUINELY DIFFERENT AXES.
//
// The cheapest way to pass everything above with a bug in it is to move ONE
// number and call it both names, so the two gestures are compared against each
// other rather than only against standing still.
// ---------------------------------------------------------------------------
check(down.low !== v0.low && right.start !== before.start
      && down.start === v0.start && right.low === before.low,
      'shift changes the OTHER axis, not the same one harder',
      `plain moved pitch by ${down.low - v0.low} and time by ${down.start - v0.start}; `
    + `shift moved pitch by ${right.low - before.low} and time by ${right.start - before.start}`);

// ---------------------------------------------------------------------------
// 4. THE ROLL DOES NOT SCROLL THE PAGE OR ANY OTHER SURFACE.
//
// A wheel handler that forgets preventDefault leaves the browser free to scroll
// something behind it, which on a fixed shell looks like nothing happening until
// the day the shell is not fixed.
// ---------------------------------------------------------------------------
const scrolled = await page.evaluate(() => ({ x: window.scrollX, y: window.scrollY }));
check(scrolled.x === 0 && scrolled.y === 0,
      'the page itself never scrolled', JSON.stringify(scrolled));

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
srv.close();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
