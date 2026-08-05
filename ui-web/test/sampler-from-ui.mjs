#!/usr/bin/env node
/**
 * CAN A PERSON USE THE SAMPLER WITHOUT TYPING A COMMAND?
 *
 * Asked, having added one: "the sampler device has no UI?" It has one — a kit list, a
 * waveform with the slice boundaries marked, a filter button and a gate button, all on its
 * rack card (chain.js). It had nothing to SHOW, because none of that draws until a file is
 * loaded, and loading a file was reachable only from the console.
 *
 * The chain was broken at every joint:
 *
 *   add a sampler        the rack's "+" sent a fixed `patcher event`, and the rail listed
 *                        only projects and plugins             -> DEVICES category
 *   give it a file       `loadSample` was a console verb; the rail's SAMPLES chip had been
 *                        drawn unavailable since it was drawn  -> `samples` feed + category
 *   see the result       draws only once a sample is loaded, so it followed from the above
 *
 * Each link was individually invisible: every suite drives the console, and the console
 * could do all three. This walks the POINTER's path end to end, which is the only way the
 * gap shows up — the same reason the console's own focus bug survived every suite that
 * "types" by calling __uni.run().
 *
 * ADDRESSED TO THE SAMPLER, ASSERTED. The load names a device id, and the sampler is not
 * necessarily first in a chain. A patcher is put in front of it here deliberately, so that
 * a load which used device 0 would go to the patcher and the kit would stay empty — which
 * is exactly the "reports healthy, shows nothing" failure this file exists to catch.
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

console.log('\nusing the sampler with the pointer only\n');

await page.evaluate(() => window.__uni.run('new samplerui'));
await page.waitForTimeout(1200);

/**
 * Open the rail on a category and click the first row whose text contains `want`.
 *
 * ENSURES the rail is open rather than TOGGLING it. ⌘B is a toggle, so calling this with
 * the rail already up closed it, the rows went invisible, the row was "not found" and the
 * failure read as "a sample cannot be loaded by clicking it" — a true sentence about a
 * test that had shut the window it was looking through.
 */
const pick = async (cat, want) => {
  const open = await page.evaluate(() => {
    const r = document.querySelector('.br');
    return !!r && r.offsetParent !== null && getComputedStyle(r).display !== 'none';
  });
  if (!open) await page.keyboard.press('Meta+b');
  await page.waitForTimeout(500);
  const ok = await page.evaluate((c) => {
    const chip = document.querySelector(`.br-chip[data-cat="${c}"]`);
    if (!chip || chip.disabled) return false;
    chip.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    return true;
  }, cat);
  if (!ok) return false;
  await page.waitForTimeout(400);
  return page.evaluate((w) => {
    const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
    const row = rows.find((el) => (el.textContent || '').toLowerCase().includes(w));
    if (!row) return false;
    row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    return true;
  }, want);
};

// A patcher FIRST, so the sampler is not device 0. See the header.
check(await pick('devs', 'patcher event'), 'a patcher can be added from the rail');
await page.waitForTimeout(900);
check(await pick('devs', 'sampler'), 'a sampler can be added from the rail');
await page.waitForFunction(() => {
  const p = window.__uni.chainProbe();
  return !!p && p.cards >= 2;
}, null, { timeout: 15000 }).catch(() => {});

const chain = await page.evaluate(() => {
  const p = window.__uni.chainProbe();
  return p ? { cards: p.cards, titles: (p.titles || []).join(',') } : null;
});
check(!!chain && chain.cards === 2 && /sampler/i.test(chain.titles),
      'the chain is patcher then sampler', JSON.stringify(chain));

// ---------------------------------------------------------------------------
// The SAMPLES category, which was drawn unavailable until the sidecar grew a feed.
// ---------------------------------------------------------------------------
await page.keyboard.press('Meta+b');
await page.waitForTimeout(500);
const chip = await page.evaluate(() => {
  const c = document.querySelector('.br-chip[data-cat="smpl"]');
  return c ? { there: true, off: !!c.disabled } : { there: false };
});
check(chip.there && !chip.off, 'the SAMPLES category is available', JSON.stringify(chip));

/*
 * ASSERT THE CATEGORY ACTUALLY CHANGED, and not merely that rows are on screen.
 *
 * The first version of this clicked a DISABLED chip, which does nothing, counted whatever
 * rows were already showing — the four DEVICES rows from the step above — and reported
 * "the rail lists audio files" in green. A count of visible rows is satisfied by any
 * category at all, which makes it a check that cannot fail for the reason it names.
 */
const cat = await page.evaluate(() => {
  const c = document.querySelector('.br-chip[data-cat="smpl"]');
  c.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
  return null;
});
await page.waitForTimeout(500);
const onSmpl = await page.evaluate(() => {
  const c = document.querySelector('.br-chip[data-cat="smpl"]');
  return !!c && c.classList.contains('on');
});
check(onSmpl, 'the SAMPLES chip takes the click and becomes the selected category');
const names = await page.evaluate(() => [...document.querySelectorAll('.br-item')]
  .filter((el) => el.offsetParent !== null).map((el) => (el.textContent || '').toLowerCase()));
check(names.length > 0 && !names.some((n) => n.includes('patcher graph')),
      'the rail lists audio files and NOT the device rows',
      `listed ${names.length}: ${JSON.stringify(names.slice(0, 4))}`);
/*
 * The repo ships presets/audio/waveform_probe.wav, and startStack copies presets into the
 * run's root — so this name is a fact about the tree, not about the machine.
 */
check(names.some((n) => n.includes('waveform_probe')),
      'including the one the repo ships', JSON.stringify(names.slice(0, 6)));

// ---------------------------------------------------------------------------
// Loading it, and the kit that proves it landed in the SAMPLER and not the patcher.
// ---------------------------------------------------------------------------
check(await pick('smpl', 'waveform_probe'), 'a sample can be loaded by clicking it');

// The engine answers with a kit; wait for the ANSWER, not for a timer.
const kit = await page.waitForFunction(() => {
  const p = window.__uni.chainProbe();
  if (!p) return null;
  const k = window.__uni.samplerKitCached
    ? window.__uni.samplerKitCached(window.__uni.state().cursor.track, 2) : null;
  return k && k.slots && k.slots.length ? k : null;
}, null, { timeout: 20000 }).then((h) => h.jsonValue()).catch(() => null);

check(!!kit, 'the sampler answers with a kit — the file reached the SAMPLER, not the patcher',
      'no kit arrived; a load addressed to device 0 would go to the patcher and look like this');
if (kit) {
  check(kit.slots.length > 0, 'the kit has at least one slot', JSON.stringify(kit.slots.length));
}

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
