#!/usr/bin/env node
/**
 * CAN YOU GET TEXT OUT OF THE AGENT PANEL?
 *
 * Asked for in these words, from live use: "make it possible to copy from the ai
 * panel". The transcript is the one surface in this application whose content is
 * PROSE — a refusal worth pasting into a bug report, a plan the model just wrote —
 * and none of it could be taken out of the window.
 *
 * Three separate things had to be true, and none of them implies the others.
 * They are asserted separately because a suite that checked one and reported "you
 * can copy from the panel" would be a green line for a feature that still does
 * not work:
 *
 *   1. the text can be SELECTED with a pointer — shell.css listed `.dk-log` as an
 *      exception to the application-wide `user-select: none` and the exception was
 *      OUTRANKED by the very rule it escaped (`#app *` scores higher than a bare
 *      class), so the declaration was there and did nothing;
 *   2. ⌘C over that selection reaches the BROWSER — the keymap binds ⌘C to the
 *      tracker's note copy and preventDefaults, so a highlighted paragraph was
 *      answered with "nothing to copy" about notes nobody had selected;
 *   3. and a selection made in the panel does not then follow you around the
 *      application, silently eating the ⌘C that belongs to the song.
 *
 * Plus the copy control, which is the answer to the case selection serves badly:
 * taking the WHOLE exchange out of a log that auto-scrolls under the pointer.
 *
 * EVERY ONE OF THESE IS DRIVEN AS A GESTURE — a real drag, a real keypress, a real
 * click — and read back through `document.getSelection()` and the real system
 * clipboard. Asserting `getComputedStyle(...).userSelect === 'text'` would have
 * passed against the broken build for the first two of the three, because the
 * property was never the thing that was wrong.
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
const origin = `http://127.0.0.1:${srv.address().port}`;

const browser = await chromium.launch({ channel: 'chrome' });
/*
 * The REAL clipboard, not a stub. "Did ⌘C reach the browser" is the whole
 * question for check 2, and the only thing that can answer it is the place the
 * browser would have put the text.
 */
const ctx = await browser.newContext({ viewport: { width: 1500, height: 1200 } });
await ctx.grantPermissions(['clipboard-read', 'clipboard-write'], { origin });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(`${origin}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
await page.evaluate(() => document.fonts.ready);
await page.waitForTimeout(300);

console.log('\ntaking text out of the agent panel\n');

/*
 * Something to copy, put there through the console's own grammar so the lines are
 * the ones a person would actually be looking at. `tempo` with no argument
 * refuses, and its refusal is a sentence — which is exactly the kind of text
 * somebody wants out of this panel.
 */
await page.evaluate(() => {
  window.__uni.dock(true);
  window.__uni.run('tempo');
  window.__uni.run('tempo');
});
await page.waitForTimeout(300);

const clipboard = () => page.evaluate(() => navigator.clipboard.readText());
const selection = () => page.evaluate(() => String(document.getSelection()));
const setClipboard = (t) => page.evaluate((x) => navigator.clipboard.writeText(x), t);

/**
 * The transcript lines that are actually on screen and actually have text.
 *
 * Measured fresh every time rather than cached: the log is a pool inside a
 * scroller inside a resizable cell, and a stale rectangle aims a drag at the
 * splitter handle that overlaps the log's top-left corner. That is not a
 * hypothetical — it is what made three drafts of this drag select nothing at all
 * and look like the feature was still broken.
 */
const lines = () => page.evaluate(() => {
  const log = document.querySelector('.dk-log');
  if (!log) return [];
  const lr = log.getBoundingClientRect();
  return [...log.querySelectorAll('.dk-line')]
    .filter((e) => e.style.display !== 'none' && e.textContent.trim())
    .map((e) => { const r = e.getBoundingClientRect();
                  return { t: e.textContent, x: r.x, y: r.y, w: r.width, h: r.height }; })
    .filter((r) => r.y >= lr.top && r.y + r.h <= lr.bottom);
});

/** Drag across the transcript, the way a hand does. */
async function dragAcross(rows) {
  const a = rows[0], b = rows[rows.length - 1];
  await page.mouse.move(a.x + 8, a.y + a.h / 2);
  await page.mouse.down();
  // In two moves with a pause: a press and a release at two points is a click as
  // far as the browser is concerned, and never extends a selection.
  await page.mouse.move(a.x + 60, a.y + a.h / 2, { steps: 5 });
  await page.waitForTimeout(40);
  await page.mouse.move(b.x + b.w - 20, b.y + b.h / 2, { steps: 15 });
  await page.waitForTimeout(40);
  await page.mouse.up();
  await page.waitForTimeout(150);
}

// ---------------------------------------------------------------------------
// 1. THE TEXT SELECTS UNDER A POINTER.
// ---------------------------------------------------------------------------
const rows = await lines();
check(rows.length >= 2, 'the transcript has visible lines to select',
      `${rows.length} on screen`);

await dragAcross(rows);
const sel = await selection();
check(sel.length > 0, 'dragging across the transcript selects text',
      JSON.stringify(sel));
check(sel.includes('tempo'),
      'and what is selected is the transcript, not something behind it',
      JSON.stringify(sel.slice(0, 80)));
check(sel.split('\n').length >= 2,
      'a drag spanning several lines selects several lines',
      `${sel.split('\n').length} line(s)`);

/*
 * AND THE APPLICATION AGREES that this is copyable text. `selectedText()` is the
 * one function the ⌘C binding asks, so if it disagrees with the document the key
 * will too — and it decides by reading the computed `user-select` back rather
 * than by holding its own list of panel names.
 */
const seen = await page.evaluate(() => window.__uni.selectedText());
check(seen === sel, 'the keymap sees the same selection the document has',
      `keymap ${JSON.stringify(seen.slice(0, 40))} vs document ${JSON.stringify(sel.slice(0, 40))}`);

// ---------------------------------------------------------------------------
// 2. ⌘C OVER IT REACHES THE BROWSER.
//
// The clipboard is primed with a sentinel first: reading back the selected text
// is only evidence if the clipboard did not already contain it.
// ---------------------------------------------------------------------------
await setClipboard('SENTINEL-nothing-was-copied');
await page.keyboard.press('Meta+c');
await page.waitForTimeout(300);
const copied = await clipboard();
check(copied !== 'SENTINEL-nothing-was-copied',
      '⌘C over the selection is not swallowed by the keymap',
      `clipboard still ${JSON.stringify(copied)}`);
check(copied.includes('tempo'),
      'the selected transcript text is what landed on the clipboard',
      JSON.stringify(copied.slice(0, 80)));
check((await page.evaluate(() => window.__uni.state().reject)) !== 'nothing to copy',
      'and the tracker\'s note copy did not fire instead',
      'the reject line says "nothing to copy", so ⌘C ran the wrong copy');

// ---------------------------------------------------------------------------
// 3. THE SELECTION DOES NOT FOLLOW YOU OUT OF THE PANEL.
//
// `user-select: none` also suppresses the press that would normally dismiss a
// highlight, so without help a selection made here survives every click in the
// song and goes on eating ⌘C. Silently: the copy succeeds, against a paragraph in
// a panel nobody is looking at.
// ---------------------------------------------------------------------------
await page.evaluate(() => window.__uni.setView('tracker'));
await page.waitForTimeout(250);
const tb = await page.evaluate(() => {
  const r = document.getElementById('tracker').getBoundingClientRect();
  return { x: r.x, y: r.y };
});
await page.mouse.click(tb.x + 150, tb.y + 150);
await page.waitForTimeout(200);
check((await selection()) === '',
      'clicking into the song dismisses the transcript highlight',
      JSON.stringify(await selection()));

await setClipboard('SENTINEL-2');
await page.keyboard.press('Meta+c');
await page.waitForTimeout(250);
check((await clipboard()) === 'SENTINEL-2',
      '…so ⌘C in the song belongs to the song again',
      'the transcript text was copied from a panel that is not even showing');

// ---------------------------------------------------------------------------
// 4. THE COPY CONTROL.
// ---------------------------------------------------------------------------
await page.evaluate(() => { window.__uni.dock(true); window.__uni.setView('tracker'); });
await page.waitForTimeout(250);
const copyBox = await page.evaluate(() => {
  const e = document.querySelector('.dk-copy');
  if (!e) return null;
  const r = e.getBoundingClientRect();
  return { x: r.x, y: r.y, w: r.width, h: r.height };
});
check(!!copyBox && copyBox.w > 0, 'the panel offers a copy control',
      JSON.stringify(copyBox));

if (copyBox) {
  const whole = await page.evaluate(() => window.__uni.dockProbe().lines);
  await setClipboard('SENTINEL-3');
  await page.mouse.click(copyBox.x + copyBox.w / 2, copyBox.y + copyBox.h / 2);
  await page.waitForTimeout(400);
  const all = await clipboard();
  check(all !== 'SENTINEL-3' && all.includes('tempo'),
        'with nothing selected it copies the whole transcript',
        `${whole} lines in the log, clipboard has ${JSON.stringify(all.slice(0, 60))}`);
  check(all.split('\n').length >= 4,
        'and the whole transcript means all of it, not the last line',
        `${all.split('\n').length} line(s) copied`);

  /*
   * WITH A SELECTION IT COPIES THE SELECTION — which is the case two separate
   * mechanisms would have broken. A press collapses a selection, and the
   * selection-dismissing listener from check 3 would have cleared it as well,
   * since the control itself is not selectable text. Both are exempted, and if
   * either exemption goes this check reports the whole log instead.
   */
  const rows2 = await lines();
  if (rows2.length >= 2) {
    await dragAcross(rows2.slice(0, 2));
    const picked = await selection();
    await setClipboard('SENTINEL-4');
    await page.mouse.click(copyBox.x + copyBox.w / 2, copyBox.y + copyBox.h / 2);
    await page.waitForTimeout(400);
    const got = await clipboard();
    check(got === picked && picked.length > 0,
          'with a selection it copies exactly that',
          `selected ${JSON.stringify(picked.slice(0, 60))}, copied ${JSON.stringify(got.slice(0, 60))}`);
  } else {
    check(false, 'with a selection it copies exactly that',
          'could not measure two transcript lines to select');
  }
}

// ---------------------------------------------------------------------------
// 5. NONE OF THIS COST THE KEYBOARD.
//
// This application routes keystrokes aggressively and the whole change is about
// letting one gesture through, so the two ways that could go wrong are checked
// from both ends: the app must still own the keys it owns, and the console's own
// field must still be able to type.
// ---------------------------------------------------------------------------
await page.evaluate(() => { document.getSelection().removeAllRanges();
                            window.__uni.setView('tracker'); window.__uni.focus('centre'); });
await page.waitForTimeout(200);
await page.keyboard.press('F2');
await page.waitForTimeout(250);
check((await page.evaluate(() => window.__uni.state().view)) === 'arrange',
      'the app still owns its own keys — F2 switches surface',
      await page.evaluate(() => window.__uni.state().view));
await page.keyboard.press('F1');
await page.waitForTimeout(250);

/*
 * And a selection standing in the transcript must not stop the song's keys
 * either: the ⌘C rule is the only thing a highlight is allowed to change.
 */
const rows3 = await lines();
if (rows3.length >= 2) await dragAcross(rows3.slice(0, 2));
await page.evaluate(() => window.__uni.focus('centre'));
await page.keyboard.press('F2');
await page.waitForTimeout(250);
check((await page.evaluate(() => window.__uni.state().view)) === 'arrange',
      'and still owns them with a selection standing in the transcript',
      await page.evaluate(() => window.__uni.state().view));
await page.keyboard.press('F1');
await page.waitForTimeout(200);

// The console's field, typed into with real keys — the path that a capturing
// preventDefault kills and that no `__uni.run()` can exercise.
const input = await page.evaluate(() => {
  const el = document.querySelector('.dk-input');
  const r = el.getBoundingClientRect();
  return { x: r.x, y: r.y, w: r.width, h: r.height };
});
await page.mouse.click(input.x + input.w / 2, input.y + input.h / 2);
await page.keyboard.type('tempo 100');
await page.waitForTimeout(200);
check((await page.evaluate(() => document.querySelector('.dk-input').value)) === 'tempo 100',
      'and the console field still types',
      JSON.stringify(await page.evaluate(() => document.querySelector('.dk-input').value)));

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
srv.close();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
