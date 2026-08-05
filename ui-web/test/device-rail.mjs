#!/usr/bin/env node
/**
 * CAN YOU ADD A BUILT-IN DEVICE WITHOUT TYPING A COMMAND?
 *
 * Asked with the app open: "how do I add the built-in Sampler to a track's device chain?"
 * The answer was that you could not. Six device kinds exist in the engine's enum; the rack's
 * "+" card sent a hard-coded `patcher event`, and the browser rail carried exactly two row
 * kinds — projects and plugins. So five of the six, the sampler among them, were reachable
 * from the console and from nowhere on screen.
 *
 * That is the standing rule broken in the most basic place in the app: the console and the
 * pointer must be able to do the same things. `dock.js` even carried a comment claiming the
 * "+" card and the console's `sampler` verb "call the same function, so the console and the
 * pointer cannot come to mean different things by 'add a device'". They called it with
 * different fixed kinds, so they meant different things anyway — a comment asserting a
 * property nothing checked, which is how it stayed wrong.
 *
 * WHAT THIS ASSERTS, and the part that is not obvious:
 *
 * The rail offers FOUR kinds, not six. `vst instrument` and `vst effect` are deliberately
 * absent, because adding one without naming a plugin produces a device with an empty vstRef
 * — which is exactly the card asked about ten minutes earlier: "what's the VST
 * instrument on track 1/Bass that doesn't have anything loaded". The engine keeps it in the
 * chain, nothing can ever load into it, and the rack draws a box with a kind name and no
 * plugin. A control whose only possible outcome is a broken device is worse than no control,
 * so the absence is a FEATURE and is asserted as one — otherwise someone restores it as a
 * kindness.
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
await page.waitForTimeout(1200);

console.log('\nadding a built-in device from the rail\n');

// A project with empty chains, so what the rail adds is the only thing in them.
await page.evaluate(() => window.__uni.run('new devrail'));
await page.waitForTimeout(1200);

// ---------------------------------------------------------------------------
// The category exists and is USABLE — a chip that is present but disabled is a
// category you still cannot reach.
// ---------------------------------------------------------------------------
await page.keyboard.press('Meta+b');
await page.waitForTimeout(600);

const chip = await page.evaluate(() => {
  const c = document.querySelector('.br-chip[data-cat="devs"]');
  return c ? { there: true, off: !!c.disabled, label: (c.textContent || '').trim() } : { there: false };
});
check(chip.there && !chip.off, 'the rail has a DEVICES category and it is available',
      JSON.stringify(chip));

await page.evaluate(() => document.querySelector('.br-chip[data-cat="devs"]').dispatchEvent(
  new PointerEvent('pointerdown', { bubbles: true })));
await page.waitForTimeout(500);

const listed = await page.evaluate(() => [...document.querySelectorAll('.br-item')]
  .filter((el) => el.offsetParent !== null)
  .map((el) => (el.textContent || '').toLowerCase()));

check(listed.some((t) => t.includes('sampler')), 'the sampler is one of them',
      JSON.stringify(listed));
check(listed.length === 4, 'four kinds are offered', `${listed.length}: ${JSON.stringify(listed)}`);
/*
 * THE ABSENCE IS THE ASSERTION. See the header: a bare `vst instrument` row would make the
 * unloadable card the rail exists to stop people making.
 */
check(!listed.some((t) => t.includes('vst')),
      'the two VST kinds are NOT offered — a plugin device is made by naming the plugin',
      JSON.stringify(listed));

// ---------------------------------------------------------------------------
// Clicking it actually makes one. The rail's own click path, not addDevice().
// ---------------------------------------------------------------------------
const before = await page.evaluate(() => {
  const p = window.__uni.chainProbe();
  return p ? p.cards : -1;
});

await page.evaluate(() => {
  const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
  const row = rows.find((el) => (el.textContent || '').toLowerCase().includes('sampler'));
  row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
});

// Wait for the ENGINE to answer, not for a timer: the chain republishes on change.
await page.waitForFunction((n) => {
  const p = window.__uni.chainProbe();
  return !!p && p.cards > n;
}, before, { timeout: 15000 }).catch(() => {});

const after = await page.evaluate(() => {
  const p = window.__uni.chainProbe();
  return p ? { cards: p.cards, titles: (p.titles || []).join(',') } : null;
});
check(!!after && after.cards === before + 1, 'clicking it puts a device in the chain',
      `cards ${before} -> ${after && after.cards}`);
check(!!after && /sampler/i.test(after.titles), 'and the device is a sampler',
      after && after.titles);

// The rail gets out of the way of the rack it just changed — same as insertPlugin.
const railShut = await page.evaluate(() => {
  const r = document.querySelector('.br');
  return !r || r.offsetParent === null || getComputedStyle(r).display === 'none';
});
check(railShut, 'the rail closes, so the result of the click is visible');

// ---------------------------------------------------------------------------
// A chain holds ONE instrument, and the refusal says so in words.
//
// A SAMPLER IS AN INSTRUMENT, and this comment used to say it was not.
//
// It read: "the engine does not count a sampler as an instrument kind: device_chain.cpp's
// isInstrumentKind lists VstInstrument and PatcherInstrument only", and called the omission a
// bug for backend to answer. It was, they answered it, and isInstrumentKind lists three kinds
// now. Neither this note nor the UI's mirror followed — so both went on stating the old rule
// while naming the changed file as their authority.
//
// The track below ALREADY HOLDS A SAMPLER by the time these clicks happen, which is why the
// first one is refused and not the second: with the sampler counted, the very first instrument
// added on top of it is one too many. That is also the ordinary case — a sampler, then a plugin
// — and until now it produced "chain error on track 0 (code 1)".
//
// unit.mjs now reads isInstrumentKind out of device_chain.cpp and fails if the mirror drifts,
// because a comment cannot notice that its subject changed.
// ---------------------------------------------------------------------------
const addTwice = async (want) => {
  /*
   * ENSURE OPEN, DO NOT TOGGLE. ⌘B is a toggle and the refusal path deliberately leaves the rail
   * OPEN — only a successful insert closes it, so that the rack it just changed is visible. So
   * the second call used to press ⌘B on an already-open rail, SHUT it, find no rows, and report
   * "clicked = false", which reads exactly like a missing row.
   *
   * The other suites' `pick` helper checks first for this reason. This one did not, and the
   * difference only surfaced once a refusal started firing on the FIRST click instead of the
   * second — the helper was correct precisely as long as the bug it was testing for existed.
   */
  const open = await page.evaluate(() => {
    const r = document.querySelector('.br');
    return !!r && r.offsetParent !== null && getComputedStyle(r).display !== 'none';
  });
  if (!open) await page.keyboard.press('Meta+b');
  await page.waitForTimeout(500);
  await page.evaluate(() => {
    document.querySelector('.br-chip[data-cat="devs"]').dispatchEvent(
      new PointerEvent('pointerdown', { bubbles: true }));
  });
  await page.waitForTimeout(400);
  const clicked = await page.evaluate((w) => {
    const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
    // `includes`, not startsWith: a row's text is badge + name + meta, so it begins
    // with "DEV". Matching the front of it silently found nothing and clicked nothing,
    // which reads exactly like a guard that failed to fire.
    const row = rows.find((el) => (el.textContent || '').toLowerCase().includes(w));
    if (row) { row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); return true; }
    return false;
  }, want);
  await page.waitForTimeout(1000);
  return clicked;
};

// Assert the CLICKS LANDED. Without this, a matcher that finds no row reports the same
// empty `reject` as a guard that never fired — the two failures are indistinguishable,
// and the first one wasted a run here already.
const hit1 = await addTwice('patcher instrument');  // refused: the sampler IS an instrument
const hit2 = await addTwice('patcher instrument');  // and still refused
check(hit1 && hit2, 'the patcher-instrument row was found and clicked both times',
      `first=${hit1} second=${hit2}`);

const refusal = await page.evaluate(() => window.__uni.state().reject || '');
check(/already has an instrument/i.test(refusal),
      'a second instrument is refused in words, not as a numeric chain error',
      `reject was ${JSON.stringify(refusal)}`);

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
