#!/usr/bin/env node
/**
 * CAN YOU ADD A BUILT-IN DEVICE WITHOUT TYPING A COMMAND?
 *
 * Jaakko, with the app open: "how do I add the built-in Sampler to a track's device chain?"
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
 * — which is exactly the card Jaakko asked about ten minutes earlier: "what's the VST
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
// `patcher instrument` and NOT a second sampler, because the engine does not count a
// sampler as an instrument kind: device_chain.cpp's isInstrumentKind lists VstInstrument
// and PatcherInstrument only. This side mirrors that predicate, so testing with a sampler
// would be asserting a rule neither end has.
//
// THAT OMISSION LOOKS LIKE A BUG, and it is the engine's to answer: TrackRuntime holds
// `samplerDeviceId` — ONE atomic, documented "0 = this track has no sampler" — so a chain
// the engine happily accepts with two samplers has a second one the runtime can never
// address. Same shape as the empty VST card: a device you can see and cannot reach.
// Reported to backend rather than papered over here; a mirror that invents a stricter rule
// than the thing it mirrors is the divergence this repo keeps paying for.
// ---------------------------------------------------------------------------
const addTwice = async (want) => {
  await page.keyboard.press('Meta+b');
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
const hit1 = await addTwice('patcher instrument');  // allowed: only a sampler so far
const hit2 = await addTwice('patcher instrument');  // refused: now there IS an instrument
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
