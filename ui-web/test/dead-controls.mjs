#!/usr/bin/env node
/**
 * THE CONTROLS NO SUITE HAS EVER CLICKED.
 *
 * Found by audit rather than by guessing: every class the UI's own event handlers reach for
 * with `closest('.x')` or `querySelector('.x')`, compared against every class any suite in this
 * directory names. Twenty-four interactive classes; four that nothing touched. Two of those are
 * structure in the help panel, counted by its probe and clickable by nobody. The other two are
 * real controls, and BOTH of them were dead:
 *
 *   .dv-open   the plugin editor button on a rack card. It sent OpenPluginEditor with the
 *              device id written to the wrong offset of a same-sized struct, so the engine read
 *              0 every time and answered "device 0 not found" for every device on every track.
 *              It had never worked once, for anyone, and no test had ever clicked it.
 *
 *   .hm-row    a harmony row. `Harmony` registers its pointerdown listener only when given an
 *              `onSelect`, and index.html constructs it with `{}` — so the listener was never
 *              registered and the rows never even got the class that marks them clickable. A
 *              surface built to be interactive, wired to nothing.
 *
 * That correlation is the point and it is not a coincidence: a control nothing clicks is a
 * control nothing can notice is broken. The audit is cheap and it found the one bug in this
 * session that a person hit within ten minutes.
 *
 * WHAT THE EDITOR BUTTON CAN AND CANNOT ASSERT. The engine owns the plugin window, so nothing
 * in the page can observe one opening — which is exactly the excuse under which this stayed
 * broken. What CAN be observed is the engine's answer: `handleOpenPluginEditor` logs only on
 * failure, so silence is success. Silence alone proves nothing (a command that never arrived is
 * also silent), so the negative control below is load-bearing: a deliberately impossible device
 * id must make the engine name THAT id back. Before the fix it said "device 0" no matter what
 * was sent, which is how a wire mismatch between two 40-byte structs hides from both ends.
 */

import { chromium } from 'playwright';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0, blocked = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};
const block = (what, why) => { blocked++; console.log('  BLOCKED ', what, `— ${why}`); };

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
const ENGINE_LOG = join(stack.root, 'engine.log');
/** Every OpenPluginEditor refusal the engine has logged so far. */
const refusals = () => {
  let t = '';
  try { t = readFileSync(ENGINE_LOG, 'utf8'); } catch { return []; }
  return t.split('\n').filter((l) => /OpenPluginEditor failed/.test(l));
};

console.log('\ncontrols that nothing had ever clicked\n');

await run('new deadctl');
await settle(1200);

// ===========================================================================
// .dv-open — the plugin editor button
// ===========================================================================
/*
 * A REAL PLUGIN, because the button is only meaningful on one: the engine skips
 * OpenPluginEditor for non-VST kinds, which is the other way this card can look inert.
 */
const inserted = await page.evaluate(async () => {
  document.dispatchEvent(new KeyboardEvent('keydown', { key: 'b', metaKey: true, bubbles: true }));
  await new Promise((r) => setTimeout(r, 600));
  const chip = document.querySelector('.br-chip[data-cat="plug"]');
  if (!chip || chip.disabled) return 'no plugin catalogue on this machine';
  chip.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
  await new Promise((r) => setTimeout(r, 400));
  const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
  const row = rows.find((el) => /INST/.test(el.textContent || ''));
  if (!row) return 'no scanned instrument in the catalogue';
  row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
  return true;
});

if (inserted !== true) {
  block('the editor button opens a real plugin', String(inserted));
} else {
  await page.waitForFunction(() => {
    const p = window.__uni.chainProbe();
    return !!p && p.cards >= 1;
  }, null, { timeout: 30000 }).catch(() => {});
  await settle(2500);                 // the host has to come up before it can be asked

  const before = refusals().length;

  const clicked = await page.evaluate(() => {
    const cards = [...document.querySelectorAll('.dv-card')].filter((el) => el.style.display !== 'none');
    // The VST card, not the first card: a patcher's "open" means something else entirely.
    const card = cards.find((el) => /vst3/i.test(el.textContent || '')) || cards[0];
    if (!card) return 'no card';
    const btn = card.querySelector('.dv-open');
    if (!btn) return 'the card has no open button';
    btn.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    return card._devId;
  });
  check(typeof clicked === 'number', 'a rack card has an editor button to click', String(clicked));
  await settle(1500);

  /*
   * NO NEW REFUSAL. This handler logs only when it fails, so this is the whole of what the
   * engine will say about a success — and it is worth nothing without the control below.
   */
  const after = refusals();
  check(after.length === before,
        'clicking it produces no refusal from the engine',
        after.slice(before).join(' | ') || 'none');

  /*
   * THE CONTROL THAT MAKES THE ABOVE MEAN SOMETHING. An impossible device id must come back
   * NAMED. Before the payload fix the engine said "device 0" whatever was sent, so "no refusal"
   * and "a refusal about the wrong device" were the only two outcomes and neither was success.
   */
  await page.evaluate(() => window.__uni.openEditor(0, 4242));
  await settle(1500);
  const named = refusals().slice(after.length);
  check(named.some((l) => /device 4242 not found/.test(l)),
        'and a bogus device id comes back NAMED — the id really reaches the engine',
        named.join(' | ') || 'the engine said nothing at all');
}

// ===========================================================================
// .hm-row — a harmony row
// ===========================================================================
// `harmony <root> <scale> [tick]` — root is 0..11 and the scale is a NAME. The first
// version passed 'C major' as two arguments, which made the scale 'C' and the tick
// 'major', and the card drew no rows for a reason that had nothing to do with the card.
await run('harmony 7 major 3840000');
await settle(1200);

const rows = await page.evaluate(() => document.querySelectorAll('.hm-row').length);
check(rows > 0, 'the harmony card draws its rows', String(rows));

/*
 * CLICKABLE AT ALL. `Harmony` registers its pointerdown listener only if it is given an
 * `onSelect`, and marks the rows with `act` at the same time — so this class is both the
 * affordance and the proof that the handler exists. index.html constructed the card with `{}`.
 */
const act = await page.evaluate(() => {
  const el = document.querySelector('.hm-rows');
  return !!el && el.classList.contains('act');
});
check(act, 'the harmony rows are wired to something — the card was built with no callbacks');

if (act && rows > 0) {
  await run('goto 8 0');
  await settle(400);
  const moved = await page.evaluate(() => {
    const before = window.__uni.state().cursor.row;
    const row = document.querySelector('.hm-row');
    row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    return before;
  });
  await settle(600);
  const now = await page.evaluate(() => window.__uni.state().cursor.row);
  check(now !== moved, 'clicking a harmony row goes to that key change',
        `cursor ${moved} -> ${now}`);
}

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
const note = blocked ? ` · ${blocked} BLOCKED` : '';
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed${note}`
                      : `ALL PASS (${pass} checks)${note}`}\n`);
process.exit(fail ? 1 : 0);
