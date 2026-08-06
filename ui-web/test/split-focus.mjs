#!/usr/bin/env node
/**
 * TWO PANES, ONE SELECTION, AND THE KEY WENT TO THE OTHER PANE.
 *
 * Reported from live use: "if I'm split (tracker above, scale roll below), and in the scale roll
 * and I select a note and hit delete, the keyb focus is still in the tracker, so I'm unable to
 * delete the note via the scale roll."
 *
 * `state.pane` decides which pane the keys go to, and clicking a note in the roll never changed
 * it. So Delete ran the TRACKER's branch: it acted on the tracker's cursor, or refused for want
 * of a selection there, while a note sat visibly selected in the pane below.
 *
 * The rule is already the app's own, written one surface over: "clicking a row must select it
 * whether or not you then drag, and the keyboard operates on whatever the last click selected."
 * The chain card does it for the patcher. The roll simply never claimed its pane — so the rule
 * held everywhere except the one surface where selecting things with the mouse IS the point.
 *
 * ── WHY THIS ASSERTS THE NOTE COUNT AND NOT THE FOCUS FLAG ──────────────────────────────────
 *
 * `state.pane` moving to 1 is the mechanism, not the symptom. A test on the flag would pass the
 * day somebody changed how panes are numbered while the note still refused to die. What was
 * reported is that the note does not go away, so that is what is measured — and separately, that
 * the TRACKER's note survives, because "delete removed a note" is also true when it removed the
 * wrong one from the wrong pane.
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
const st = () => page.evaluate(() => {
  const s = window.__uni.state();
  return { view: s.view, view2: s.view2, pane: s.pane, selectedNote: s.selectedNote };
});
const notes = () => page.evaluate(() => {
  const e = window.__uni.engineState();
  return e ? e.noteCount : -1;
});

console.log('\ndelete acts on the pane you clicked in\n');

await run('new splitfocus');
await settle(1200);

// Notes on ONE track, so a delete that lands on the wrong pane still has something to hit —
// which is exactly how the bug hid: something always happened, just not the thing asked for.
// `note` writes at the cursor, so each one needs its own row.
for (const [row, pitch] of [[0, 60], [4, 62], [8, 64], [12, 65]]) {
  await run(`goto ${row}`);
  await settle(150);
  await run(`note ${pitch}`);
  await settle(250);
}
await settle(800);
const before = await notes();
check(before >= 4, 'a phrase to select from', String(before));

// Tracker on top, roll below — the reported layout.
await run('view tracker');
await settle(400);
// SHIFT + the view's key opens it in the SECOND pane — one key per view, the modifier for
// "the other pane". There is no console verb for it; this is the gesture a person makes.
await page.keyboard.press('Shift+F4');
await settle(900);
const layout = await st();
check(layout.view === 'tracker' && layout.view2 === 'piano',
      'split: tracker above, roll below', JSON.stringify(layout));

/*
 * THE TRACKER TAKES THE KEYS FIRST, and this is the whole setup.
 *
 * Shift+F4 sets `pane = 1` itself, so a test that split and then clicked the roll would find
 * pane 1 and call the bug fixed — a check passing for a reason unrelated to the fix. The
 * reported situation is the one AFTER you have been working in the tracker: click up there, then
 * select something below.
 */
const tracker = await page.evaluate(() => {
  const el = document.getElementById('paneTop');
  if (!el) return null;
  const r = el.getBoundingClientRect();
  return { x: r.left + r.width * 0.5, y: r.top + r.height * 0.5 };
});
check(!!tracker, 'the tracker pane is on screen');
if (tracker) await page.mouse.click(tracker.x, tracker.y);
await settle(400);
const armed = await st();
check(armed.pane === 0,
      'the keys are in the TRACKER — the state the report starts from',
      `pane=${armed.pane}; starting at pane 1 would pass with the bug present`);

// Click a note IN THE ROLL. Through the roll's own hit-testing rather than a synthetic call, so
// this exercises the path a person takes.
/*
 * Click the NOTE itself, at the pixel the app drew it at.
 *
 * The roll is DIVs, not a canvas — `.pr-keys` and `.pr-band` under `#piano`, which is reparented
 * into `#paneBot` when it is the second pane. Asking the DOM where a note is beats guessing a
 * fraction of the pane: a guess that lands on empty grid selects nothing, and the test would then
 * report a focus bug that is really a bad click.
 */
const at = await page.evaluate(() => {
  const host = document.getElementById('piano');
  if (!host) return null;
  // The CHILDREN of the note layer, not the layer itself: `.pr-notes` is the container and its
  // class matches any "note" pattern, so clicking it lands on empty grid and selects nothing.
  const layer = host.querySelector('.pr-notes');
  const notes = layer ? [...layer.children].filter((el) => {
    const r = el.getBoundingClientRect();
    return r.width > 2 && r.height > 2;
  }) : [];
  if (!notes.length) {
    return { none: true,
             classes: [...new Set([...host.querySelectorAll('*')]
               .map((e) => String(e.className)).filter(Boolean))].slice(0, 12) };
  }
  const r = notes[0].getBoundingClientRect();
  return { x: r.left + r.width / 2, y: r.top + r.height / 2, n: notes.length,
           cls: String(notes[0].className), tag: notes[0].tagName };
});
check(at && !at.none, 'the roll is drawing notes to click',
      at && at.none ? `no note-ish elements; classes seen: ${JSON.stringify(at.classes)}`
                    : `${at.n} note element(s), clicking ${at.tag}.${at.cls}`);
if (at && !at.none) console.log(`  (clicking ${at.n} candidates, first is ${at.tag}.${at.cls})`);
if (at && !at.none) { await page.mouse.click(at.x, at.y); }
await settle(700);

const afterClick = await st();
check(afterClick.pane === 1,
      'CLICKING IN THE ROLL GIVES THE ROLL THE KEYBOARD',
      `pane=${afterClick.pane} — with pane 0 the next keystroke runs the tracker's branch while `
      + 'the selection everyone can see is in the pane below');

if (afterClick.selectedNote >= 0) {
  const n0 = await notes();
  await page.keyboard.press('Delete');
  await settle(1200);
  const n1 = await notes();
  check(n1 === n0 - 1,
        'and Delete removes the note that was SELECTED IN THE ROLL',
        `${n0} -> ${n1}; the reported bug is that this stays the same because the key went to `
        + 'the tracker');
} else {
  check(false, 'clicking the roll selected a note', JSON.stringify(afterClick));
}

/*
 * AND IT FOLLOWS THE CLICKS, BOTH WAYS.
 *
 * The checks above can be satisfied by a pane that simply happens to be 1 — Shift+F4 sets it, so
 * a test that splits and then clicks the roll finds what it wanted without the feature existing.
 * That is this file's own setup supplying its own precondition, and it made the first negative
 * control report one failure where it should have reported three.
 *
 * A pane that ALTERNATES cannot be a constant. Clicking up, then down, then up again, and
 * asserting the value each time, fails on any build where clicks do not move focus — whatever
 * the starting value was.
 */
const paneAfterClick = async (id) => {
  const at = await page.evaluate((paneId) => {
    const el = document.getElementById(paneId);
    if (!el) return null;
    const r = el.getBoundingClientRect();
    return { x: r.left + r.width * 0.5, y: r.top + 8 };
  }, id);
  if (!at) return -1;
  await page.mouse.click(at.x, at.y);
  await settle(300);
  return (await st()).pane;
};
const seq = [await paneAfterClick('paneTop'), await paneAfterClick('paneBot'),
             await paneAfterClick('paneTop')];
check(JSON.stringify(seq) === '[0,1,0]',
      'FOCUS FOLLOWS THE CLICKS, top then bottom then top',
      `${JSON.stringify(seq)} — a constant here is a build where clicking does not move the `
      + 'keyboard, whichever pane it happened to start in');

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
