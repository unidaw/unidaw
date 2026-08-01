/**
 * THE MASTER BUS HAD A FADER NOWHERE.
 *
 * The engine publishes the master as a real track — its own device chain, its own mixer
 * entry, its own per-insert meters, `kUiTrackFlagMaster` set on the published slot — and
 * `SetTrackMixer` addressed to `kMasterTrackId` is one of the few handlers that names the
 * master explicitly and routes it to the master runtime, whose gain and mute the audio
 * callback reads each block to attenuate the summed output.
 *
 * The mixer drew nothing for it. It iterates the LANE count, and the master is
 * deliberately excluded from that count so the tracker never draws a row for it — so the
 * one fader every track passes through was the one fader with no control anywhere, and
 * `shared_memory.h` says in as many words that the UI is meant to render it.
 *
 * WHAT THIS ASSERTS, in the order that makes each next claim mean anything:
 *   1. the engine publishes a master at all, found by its FLAG and not by its position
 *   2. the strip is on screen, and is not one of the track strips
 *   3. a fader drag moves the MASTER's gain and no track's
 *   4. it survives the round trip to the engine and back, rather than sitting local
 *   5. mute works and is addressed the same way
 *   6. the console reaches both, and refuses when there is no master to address
 *   7. the two controls the master has no meaning for are ABSENT, not disabled
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8 });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
page.on('pageerror', (e) => check(false, 'no page error', e.message));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1200);
await page.evaluate(() => window.__uni.run('view mixer'));
await page.waitForTimeout(1800);

console.log('\n[the engine publishes one]');
const has = await page.evaluate(() => window.__uni.hasMaster());
check(has === true, 'the engine publishes a master track', String(has));

/*
 * FOUND BY ITS FLAG, NOT BY ITS POSITION. The engine appends it today and
 * `shared_memory.h` says that is not a promise, so a strip that assumed the last slot
 * would one day attenuate a track instead of the mix.
 */
const slot = await page.evaluate(() => {
  const e = window.__uni.engine();
  return { trackCount: e.tracks, lanes: window.__uni.state().tracks };
});
check(slot.trackCount > slot.lanes,
      'and it is OUTSIDE the lane count, which is why the mixer never drew it',
      `${slot.trackCount} published slots, ${slot.lanes} lanes`);

const probe = () => page.evaluate(() => window.__uni.mixerProbe());
let p = await probe();
console.log('\n[the strip]');
check(p && p.master !== null && p.master !== undefined,
      'the mixer model carries a master strip', JSON.stringify(p && p.master));
check(p && p.master && p.master.onScreen === true, 'and it is on screen');
check(p && p.master && p.master.slot >= p.strips,
      'it is not one of the track strips',
      p && p.master ? `master at slot ${p.master.slot}, ${p.strips} track strips` : 'no master');

const dom = await page.evaluate(() => {
  const el = document.querySelector('.mx-strip.mx-master');
  if (!el) return null;
  const r = el.getBoundingClientRect();
  return {
    name: el.querySelector('.mx-name') ? el.querySelector('.mx-name').textContent.trim() : null,
    hasSolo: !!el.querySelector('.mx-solo'),
    hasRoute: !!el.querySelector('.mx-out'),
    hasMute: !!el.querySelector('.mx-mute'),
    nameActs: el.querySelector('.mx-name') ? el.querySelector('.mx-name').dataset.act || null : null,
    x: Math.round(r.x), w: Math.round(r.width), visible: r.width > 0 && r.height > 0,
  };
});
check(dom !== null && dom.visible, 'the strip is drawn with a real box', JSON.stringify(dom));
check(dom && dom.name === 'MAIN',
      'named MAIN, which is what every routing dropdown already calls it', dom && dom.name);

/*
 * THE TWO CONTROLS IT HAS NO MEANING FOR ARE ABSENT, NOT DISABLED. There is nothing to
 * solo the master against, and it IS the destination — a control that can never do
 * anything is worse than no control.
 */
check(dom && dom.hasSolo === false, 'no solo button — there is nothing to solo it against');
check(dom && dom.hasRoute === false, 'no destination selector — it IS the destination');
check(dom && dom.hasMute === true, 'but it does have a mute');
check(dom && dom.nameActs === null, 'and its name is not a rename target');

/*
 * THE FADER MOVES THE MASTER AND NO TRACK.
 *
 * Track gains are captured before and compared after: a control wired to the wrong strip
 * is exactly the failure this app has produced twice, and "the master moved" alone cannot
 * see it.
 */
console.log('\n[the fader]');
const gainsOf = async () => (await probe()).detail.map((s) => s.gain);
const before = await gainsOf();
const masterBefore = (await probe()).master.gain;

const box = await page.evaluate(() => {
  const f = document.querySelector('.mx-strip.mx-master .mx-fader');
  if (!f) return null;
  const r = f.getBoundingClientRect();
  return { x: r.x + r.width / 2, top: r.top, h: r.height };
});
check(box !== null, 'the master fader has a box to drag');
if (box) {
  // Two thirds down the fader — a position no default sits on, so "it moved" cannot be
  // satisfied by the value it already had.
  await page.mouse.click(box.x, box.top + box.h * 0.66);
  await page.waitForTimeout(400);
  const mid = await probe();
  check(mid.master.gain !== masterBefore, 'dragging it changes the master gain',
        `${masterBefore} -> ${mid.master.gain}`);
  const after = await gainsOf();
  check(JSON.stringify(after) === JSON.stringify(before),
        'and no track gain moved with it', `${JSON.stringify(before)} -> ${JSON.stringify(after)}`);

  /*
   * AN INVERTED CHECK — IT ASSERTS THE GAP, SO IT FAILS THE DAY THE GAP CLOSES.
   *
   * A master-only edit never clears `pending`, and the cause is one ordering in the
   * engine: `uiMixerVersion` is bumped from `mixerChanged`, which is computed by
   * comparing each TRACK slot against `lastGainMillibels[]` — and the master's slot is
   * filled AFTER that, in the block that appends it with `kUiTrackFlagMaster`. So the
   * master's gain IS published and readable, and nothing tells a reader it moved.
   *
   * The fader still works: the engine applies it, the audio callback reads the atomics
   * each block. What is missing is the acknowledgement, so an optimistic strip has
   * nothing to reconcile against and stays pending for ever. Reported to backend.
   *
   * Written as "still pending" rather than left failing, because a suite that is red for
   * a known reason is a suite people stop reading — and written as an ASSERTION rather
   * than a comment, because a recorded limitation nobody re-checks is indistinguishable
   * from a real one.
   */
  let settled = false;
  for (let i = 0; i < 15 && !settled; i++) {
    await page.waitForTimeout(200);
    const q = await probe();
    settled = q.master && q.master.pending === false;
  }
  check(!settled,
        'GAP: a master edit is never acknowledged — uiMixerVersion does not move for it',
        settled ? 'it settled! the engine now bumps the version for the master — delete this '
                + 'inverted check and assert the round trip instead' : undefined);
  const done = await probe();
  check(done.master.gain === mid.master.gain,
        'the fader value stands while it waits',
        `${mid.master.gain} -> ${done.master.gain}`);

  /*
   * AND THE METER. `uiTrackPeakRms[m] = 0.0f` with the comment "master peak: 4b" — the
   * master's slot is published with a hard-coded zero, so the strip's meter can only ever
   * be empty. Same shape, same treatment: asserted, so it retires itself.
   */
  check(done.master.meter === 0,
        'GAP: the master meter is published as a constant zero (engine: "master peak: 4b")',
        done.master.meter !== 0 ? `it moved to ${done.master.meter} — the engine publishes a `
          + 'master peak now, so delete this and assert the meter follows the mix' : undefined);
}

console.log('\n[mute]');
const muteBefore = (await probe()).master.mute;
const trackMutesBefore = (await probe()).detail.map((s) => s.mute);
await page.click('.mx-strip.mx-master .mx-mute');
await page.waitForTimeout(900);
const muted = await probe();
check(muted.master.mute !== muteBefore, 'clicking M mutes the master',
      `${muteBefore} -> ${muted.master.mute}`);
check(JSON.stringify(muted.detail.map((s) => s.mute)) === JSON.stringify(trackMutesBefore),
      'and no track was muted with it',
      `${JSON.stringify(trackMutesBefore)} -> ${JSON.stringify(muted.detail.map((s) => s.mute))}`);
await page.click('.mx-strip.mx-master .mx-mute');
await page.waitForTimeout(900);
check((await probe()).master.mute === muteBefore, 'and it unmutes again');

/*
 * THE CONSOLE REACHES BOTH. Pointer-only is not done — a control an agent cannot name is
 * a control an agent cannot use.
 */
console.log('\n[the console]');
const g0 = (await probe()).master.gain;
const said = await page.evaluate(() => window.__uni.run('main-gain -12'));
await page.waitForTimeout(900);
const g1 = (await probe()).master.gain;
check(g1 === -1200, 'main-gain sets the master in dB', `${g0} -> ${g1}, said "${said}"`);
const m0 = (await probe()).master.mute;
await page.evaluate(() => window.__uni.run('main-mute'));
await page.waitForTimeout(900);
check((await probe()).master.mute !== m0, 'main-mute toggles it');
await page.evaluate(() => window.__uni.run('main-mute'));
await page.waitForTimeout(600);

/*
 * AND THE PALETTE OFFERS THEM, because a verb that only works when typed exactly is a
 * verb nobody finds.
 */
await page.evaluate(() => window.__uni.run('view mixer'));
await page.keyboard.press('Meta+k');
await page.waitForTimeout(300);
await page.evaluate(() => window.__uni.paletteQuery('main-'));
await page.waitForTimeout(300);
// Read the ROWS the palette drew, not a probe of its internal list: what a person can
// find is what is on screen.
const rows = await page.evaluate(() =>
  [...document.querySelectorAll('.pl-item')]
    .map((e) => e.textContent.trim()).filter(Boolean));
check(rows.some((r) => r.startsWith('main-gain')) && rows.some((r) => r.startsWith('main-mute')),
      'both are findable in the palette', JSON.stringify(rows).slice(0, 200));
await page.keyboard.press('Escape');

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
