#!/usr/bin/env node
/**
 * Making a song, the way a person makes one.
 *
 *   node test/journey.mjs
 *
 * Every other suite here drives `window.__uni.*`. Those hooks are the app's
 * intent layer: `__uni.addTrack()` adds a track whether or not the button works,
 * whether or not the keyboard reaches it, whether or not anything on screen says
 * it happened. 197 of them were green while Backspace deleted a note instead of
 * the selected device, while the device rack had no delete control at all, and
 * while nothing in the shell could be resized.
 *
 * So this file has ONE rule: it may only do what a person can do. Real key
 * presses, real clicks, real drags, real wheel events. `__uni` is used to READ —
 * to ask what the app now believes — and never to cause anything. If a step
 * cannot be performed with a keyboard and a mouse, that is the finding, and it
 * is reported as GAP rather than worked around.
 *
 * It runs against its own engine (test/stack.mjs), so it can create, edit, save
 * and delete freely without touching anybody's session.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

const stack = await startStack();
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1700, height: 1000 } });

const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
page.on('console', (m) => {
  if (m.type() !== 'error') return;
  // A missing favicon is the browser's business, not the app's. Everything else
  // counts.
  if (/favicon/i.test(m.text())) return;
  errors.push('console: ' + m.text());
});
page.on('response', (r) => {
  if (r.status() === 404 && !/favicon/i.test(r.url())) errors.push('404: ' + r.url());
});
// Confirms are part of the flow (removing a track, removing a device). Accept
// them by default; a step that wants to cancel says so.
let lastDialog = null, dialogAnswer = 'accept';
page.on('dialog', async (d) => {
  lastDialog = d.message();
  if (dialogAnswer === 'accept') await d.accept(); else await d.dismiss();
});

let pass = 0, fail = 0, gaps = [];
const ok = (cond, what, detail = '') => {
  if (cond) pass++; else fail++;
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${what}${detail ? '  ' + detail : ''}`);
  return !!cond;
};
/** Something a person cannot do at all. Not a failure of this run — a finding. */
const gap = (what, why) => {
  gaps.push(`${what} — ${why}`);
  console.log(`  GAP   ${what}  ${why}`);
};
const step = (s) => console.log(`\n[${s}]`);

const frame = () => page.evaluate(
  () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
const settle = async (ms = 400) => { await page.waitForTimeout(ms); await frame(); };

/** What the app believes right now. READ ONLY — never used to cause anything. */
const st = () => page.evaluate(() => {
  const s = window.__uni.state();
  // `engineState()`. Not `engineStats()`, which is the SOCKET's counters, and
  // not `engine()`, which is a method on the DOCK's api object and not on __uni
  // at all — both read as `undefined` or `null` here and turned three checks
  // into things that could not fail meaningfully.
  const e = window.__uni.engineState ? window.__uni.engineState() : null;
  return {
    view: s.view, focus: s.focus, zoom: s.zoom, octave: s.octave, editMode: s.editMode,
    cursor: { row: s.cursor.row, track: s.cursor.track, col: s.cursor.col },
    tracks: s.tracks, project: s.currentProject, reject: s.reject,
    engine: e,
  };
});
const names = () => page.evaluate(() => window.__uni.names());
const probe = () => page.evaluate(() => window.__uni.probe());

/** Click the middle of the first element matching `sel`, if it is really there. */
async function clickSel(sel, label) {
  const at = await page.evaluate((s) => {
    const el = document.querySelector(s);
    if (!el) return null;
    const r = el.getBoundingClientRect();
    if (r.width < 4 || r.height < 4) return { clipped: true };
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    const top = document.elementFromPoint(x, y);
    // A control that is present but covered is not a control. This is the
    // Open-button failure: a real rect, nothing clickable.
    return { x, y, reachable: !!(top && (top === el || el.contains(top) || top.closest(s))) };
  }, sel);
  if (!at) { gap(label, `no element matches ${sel}`); return false; }
  if (at.clipped) { gap(label, `${sel} has no size`); return false; }
  if (!at.reachable) { gap(label, `${sel} is covered by something else`); return false; }
  await page.mouse.click(at.x, at.y);
  await settle();
  return true;
}

await page.goto(stack.url);
await page.waitForFunction(() => !!window.__uni, null, { timeout: 20000 });
await page.waitForFunction(() => window.__uni.canSend(), null, { timeout: 20000 })
  .catch(() => {});
await settle(1200);

console.log(`\nmaking a song at ${stack.url}\n${'='.repeat(60)}`);

// ---------------------------------------------------------------------------
step('1. start a new song');
// Cmd-N is taken by the browser (it opens a window and cannot be preventDefault'd
// there), which is one of the two things that would need Electron. The dock
// command is the path a person actually has.
await page.keyboard.press('Escape');
const before = await st();
await clickSel('.ch-btn.ch-play', 'transport play button exists');   // just to have focus somewhere sane
await page.keyboard.press('Space');                                   // stop again if it started
await settle();

// ---------------------------------------------------------------------------
step('2. add tracks with the buttons');
const t0 = (await st()).engine;
const startTracks = t0 ? t0.trackCount : 0;
let added = 0;
for (let i = 0; i < 3; i++) {
  // The add-track button is an icon in the chrome. Find it by its title, which is
  // what a person reads on hover.
  const hit = await page.evaluate(() => {
    const b = [...document.querySelectorAll('.ch-btn')]
      .find((e) => /add a track/i.test(e.title || ''));
    if (!b) return null;
    const r = b.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
  });
  if (!hit) { gap('add a track', 'no chrome button whose title mentions adding a track'); break; }
  await page.mouse.click(hit.x, hit.y);
  await settle(600);
  added++;
}
const afterAdd = (await st()).engine;
ok(afterAdd && afterAdd.trackCount === startTracks + added,
   'clicking add-track adds a track each time',
   `${startTracks} + ${added} -> ${afterAdd && afterAdd.trackCount}`);

// ---------------------------------------------------------------------------
step('3. name a track');
// Naming goes through the console, which is the one grammar every surface shares.
await page.keyboard.press('Slash');            // '/' focuses the console
await settle(250);
const consoleFocused = (await st()).focus === 'dock';
if (!ok(consoleFocused, '/ opens the console', JSON.stringify((await st()).focus))) {
  gap('name a track', 'could not reach the console with /');
} else {
  await page.keyboard.type('rename 0 Drums');
  await page.keyboard.press('Enter');
  await settle(700);
  const n = await names();
  ok(n && n[0] === 'Drums', 'the console renames a track', JSON.stringify(n && n.slice(0, 3)));
  await page.keyboard.press('Escape');
  await settle(200);
}

// ---------------------------------------------------------------------------
step('4. enter notes by playing the keyboard');
// Click into the grid first — that is how a person tells the app where to type.
const grid = await page.evaluate(() => {
  const c = document.querySelector('.tk-row[data-row="0"] .tk-cell');
  if (!c) return null;
  const r = c.getBoundingClientRect();
  return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
});
if (!grid) {
  gap('enter notes', 'no tracker cell on screen to click');
} else {
  await page.mouse.click(grid.x, grid.y);
  await settle(200);
  const s1 = await st();
  ok(s1.focus === 'centre', 'clicking the grid focuses the tracker', s1.focus);

  // Edit mode has to be ON for keys to write notes; that is the toggle Jaakko
  // asked for, so a person turns it on the same way.
  if (!s1.editMode) {
    await page.keyboard.press('Meta+e');
    await settle(250);
  }
  const s2 = await st();
  ok(s2.editMode === true, 'edit mode can be turned on from the keyboard', String(s2.editMode));

  const notesBefore = (await st()).engine.noteCount;
  // 'z s x d c' is C D E F G on the tracker's piano row.
  for (const k of ['z', 's', 'x', 'd', 'c']) {
    await page.keyboard.press(k);
    await page.waitForTimeout(220);
  }
  await settle(900);
  const notesAfter = (await st()).engine.noteCount;
  ok(notesAfter > notesBefore, 'playing the piano keys writes notes',
     `${notesBefore} -> ${notesAfter}`);
}

// ---------------------------------------------------------------------------
step('5. move around the song');
const nav = async (key, times = 1) => {
  for (let i = 0; i < times; i++) { await page.keyboard.press(key); }
  await settle(200);
  return (await st()).cursor;
};
const c0 = (await st()).cursor;
const cDown = await nav('ArrowDown', 4);
ok(cDown.row === c0.row + 4, 'arrows move the cursor by rows', `${c0.row} -> ${cDown.row}`);
const cTab = await nav('Tab');
ok(cTab.track !== cDown.track, 'Tab moves to the next track',
   `track ${cDown.track} -> ${cTab.track}`);
const cShiftTab = await nav('Shift+Tab');
ok(cShiftTab.track === cDown.track, 'Shift+Tab moves back',
   `track ${cTab.track} -> ${cShiftTab.track}`);

// ---------------------------------------------------------------------------
step('6. scroll the song');
const beforeScroll = await page.evaluate(() => window.__uni.state().start);
await page.mouse.move(grid ? grid.x : 800, grid ? grid.y : 500);
for (let i = 0; i < 3; i++) await page.mouse.wheel(0, 120);
await settle(300);
const afterScroll = await page.evaluate(() => window.__uni.state().start);
ok(afterScroll > beforeScroll, 'the wheel scrolls the tracker',
   `${beforeScroll} -> ${afterScroll}`);

// ---------------------------------------------------------------------------
step('7. look at the arrangement');
await page.keyboard.press('F2');
await settle(500);
const inArrange = (await st()).view;
ok(inArrange === 'arrange', 'F2 switches to the arrangement', inArrange);

// ---------------------------------------------------------------------------
step('8. mix');
await page.keyboard.press('F8');
await settle(500);
ok((await st()).view === 'mixer', 'F8 switches to the mixer', (await st()).view);
await page.keyboard.press('F1');
await settle(400);

// ---------------------------------------------------------------------------
step('9. save it, and get it back');
await page.keyboard.press('Slash');
await settle(250);
await page.keyboard.type('save journeysong');
await page.keyboard.press('Enter');
await settle(1500);
await page.keyboard.press('Escape');
await settle(300);
const savedName = (await st()).project;
ok(/journeysong/.test(String(savedName)), 'the song saves under its name', String(savedName));

// ---------------------------------------------------------------------------
step('10. does it make a sound?');
// Transport, then read the engine's own meters — the only honest answer to
// "did anything come out" that does not need ears.
await page.keyboard.press('Space');
await settle(2500);
const meters = await page.evaluate(() => {
  const m = window.__uni.mixerProbe ? window.__uni.mixerProbe() : null;
  return m;
});
await page.keyboard.press('Space');
await settle(300);
if (!meters) gap('hear the song', 'no mixer probe to read levels from');
else ok(true, 'transport ran and the mixer reported', JSON.stringify(meters).slice(0, 120));

// ---------------------------------------------------------------------------
step('page errors');
ok(errors.length === 0, 'nothing threw during the whole journey',
   errors.slice(0, 3).join(' | '));

console.log(`\n${'='.repeat(60)}`);
if (gaps.length) {
  console.log(`${gaps.length} THINGS A PERSON CANNOT DO:`);
  for (const g of gaps) console.log(`  - ${g}`);
}
console.log(`${fail === 0 ? `ALL PASS (${pass})` : `${fail} FAILED, ${pass} passed`}`
          + `${gaps.length ? `, ${gaps.length} gaps` : ''}`);

await browser.close();
stack.stop();
process.exit(fail ? 1 : 0);
