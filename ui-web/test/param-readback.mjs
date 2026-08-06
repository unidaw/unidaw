/**
 * A PARAMETER WRITE IS READ BACK — the bar stays where you put it.
 *
 * THE BUG, reported from live use: "editing VST instrument parameters on the device card face
 * APPEARS to take effect but doesn't — tweak the Size up, it takes effect for a second then
 * reverts."
 *
 * IT IS THE DISPLAY THAT IS WRONG, NOT THE PARAMETER, and that distinction is the whole reason
 * this file asserts the SAVED PROJECT as well as the card. The two candidate causes look
 * identical from the rack:
 *
 *   (a) the write never reached the plugin, so the read-back is CORRECT and the moment of
 *       movement was an optimistic lie;
 *   (b) the write landed and the published mirror is stale, so the parameter is right and the
 *       display is wrong.
 *
 * The discriminator is whether the new value is in the saved project — which is not the card's
 * opinion of the plugin but a live query of it (saveProject asks the host for each plugin's
 * parameters and writes them beside the document as `<track>_<device>.params.json`). MEASURED:
 * it is (b). The value written by a drag is in that manifest, at the value dragged to, whether
 * or not the rack ever shows it.
 *
 * What was broken is that nothing asked the plugin again. The parameter mirror is
 * REQUEST-DRIVEN — the engine fills UiDeviceParamsRegion only in answer to a `reqparams`, and
 * the rack asks once per device — so after the first answer the mirror never moved. chain.js
 * holds a dragged value optimistically for EDIT_HOLD_MS (1200 ms) waiting for the engine to
 * agree, the engine was never asked, the hold expired, and the bar fell back to the answer from
 * before the edit. "A second, then it reverts" was that timeout, exactly on time.
 *
 * So the checks below are in two halves and BOTH are needed:
 *   - the saved manifest carries the dragged value: names the cause as (b) and would catch a
 *     regression that made the rack lie convincingly while the plugin heard nothing;
 *   - the PUBLISHED mirror and the card carry it too, more than a second after the drag ends:
 *     that is the bug the owner reported, and it is asserted after the optimistic hold has
 *     expired (probe().pending empty) so nothing on screen can be an unconfirmed guess.
 *
 * A REAL PLUGIN, and a real pointer drag on the bar, because every layer between them is where
 * this lived: a stub answering from a table, or a call to `__uni.setParam`, would leave the
 * optimistic hold — the thing that expires — out of the test entirely.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { writeFileSync, readFileSync, existsSync, readdirSync } from 'node:fs';
import { resolve, join } from 'node:path';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const Q = 960000;
const IDENTITY = resolve('build/identity_plugin_artefacts/RelWithDebInfo/VST3/Identity.vst3');
/** Where the drag lands, as a fraction of the bar. Far from the default (1.0) so a bar that
 *  never moved and a bar that moved back are the same failure, and both are loud. */
const TO = 0.40;
/** The plugin may quantise to its own grid; the rack settles on ±1 milli, so this is generous
 *  about the value and strict about the direction. */
const TOL = 40;

const stack = await startStack({ numBlocks: 8 });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

writeFileSync(`${stack.dir}/pwback.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'pwback', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [], clips: [],
  tracks: [{
    track_id: 0, name: 'P', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
    device_chain: [
      { device_id: 6, kind: 'vst_instrument', capability_mask: 5, patcher_node_id: 0,
        host_slot_index: 4294967294, bypass: false,
        vst_ref: { vendor: '', name: 'Identity', path: IDENTITY, uid16: '' } }],
    mod_links: [], placements: [],
  }],
}));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 12000 });
await page.evaluate(() => window.__uni.loadProject('pwback'));
const loaded = await page.waitForFunction(
  () => { const c = window.__uni.chainProbe(); return c && c.params && c.params[0] > 0; },
  null, { timeout: 45000 }).then(() => true).catch(() => false);
check(loaded, 'the plugin loads and publishes its parameters');
if (!loaded) { await browser.close(); await stack.stop(); process.exit(1); }
await page.waitForTimeout(1500);

/** The published mirror — written ONLY by the sidecar's `deviceParams` push, so a value read
 *  here is the engine's answer and can never be this side's optimism. */
const mirror = () => page.evaluate(() => {
  const all = window.__uni.deviceParams();
  const k = Object.keys(all).find((x) => all[x].params && all[x].params.length);
  if (!k) return null;
  return { track: Math.floor(Number(k) / 65536), device: Number(k) % 65536,
           name: all[k].name, params: all[k].params };
});
/** The Gain row as a PERSON sees it: its name, its displayed value, and where its fill sits. */
const face = () => page.evaluate(() => {
  const r = [...document.querySelectorAll('.dv-p')]
    .filter((x) => x.style.display !== 'none')
    .find((x) => { const n = x.querySelector('.dv-p-n'); return n && n.textContent === 'Gain'; });
  if (!r) return null;
  const bar = r.querySelector('.dv-p-bar'), fill = r.querySelector('.dv-p-fill');
  const b = bar.getBoundingClientRect(), f = fill.getBoundingClientRect();
  return { value: r.querySelector('.dv-p-v').textContent,
           fill: b.width > 0 ? f.width / b.width : -1 };
});

const before = await mirror();
const gain0 = before && before.params.find((p) => p.name === 'Gain');
check(!!gain0 && Math.abs(gain0.value - 1) < 0.01,
      'Gain starts at the plugin default', JSON.stringify(gain0));
const face0 = await face();
check(!!face0, 'the rack draws a Gain row', JSON.stringify(face0));

// ---------------------------------------------------------------------------
// THE DRAG. On the bar, with the pointer, exactly as a person does it.
// ---------------------------------------------------------------------------
const box = await page.evaluate(() => {
  const r = [...document.querySelectorAll('.dv-p')]
    .filter((x) => x.style.display !== 'none')
    .find((x) => { const n = x.querySelector('.dv-p-n'); return n && n.textContent === 'Gain'; });
  const b = r.querySelector('.dv-p-bar').getBoundingClientRect();
  return { x: b.left, y: b.top, w: b.width, h: b.height };
});
const at = (f) => ({ x: box.x + box.w * f, y: box.y + box.h / 2 });
await page.mouse.move(at(0.9).x, at(0.9).y);
await page.mouse.down();
// Through a couple of intermediate positions: the strip sends one message per milli-unit, and a
// single jump would never exercise the coalescing a real drag runs into.
for (const f of [0.75, 0.6, 0.5, TO]) { await page.mouse.move(at(f).x, at(f).y); await page.waitForTimeout(30); }
await page.mouse.up();

// The bar moves AT ONCE — optimistically, on this side. That much always worked, and it is what
// made the bug look like a working control for one second.
const held = await face();
check(held && Math.abs(held.fill - TO) < 0.08,
      'the bar follows the drag immediately', JSON.stringify(held));

// ---------------------------------------------------------------------------
// AND IT IS STILL THERE AFTER THE OPTIMISTIC HOLD EXPIRES.
//
// EDIT_HOLD_MS is 1200; two seconds is past it with room to spare. `pending: []` is the load-
// bearing part of this section — it says the value on screen is the engine's answer and not a
// guess this side is still holding, which is the difference between the fix and a longer lie.
// ---------------------------------------------------------------------------
await page.waitForTimeout(2000);
const pending = await page.evaluate(() => window.__uni.chainProbe().pending);
check(Array.isArray(pending) && pending.length === 0,
      'the optimistic hold has expired, so nothing on screen is unconfirmed',
      JSON.stringify(pending));

const after = await mirror();
const gain1 = after && after.params.find((p) => p.name === 'Gain');
const milli1 = gain1 ? Math.round(gain1.value * 1000) : -1;
check(Math.abs(milli1 - TO * 1000) <= TOL,
      'THE PUBLISHED MIRROR carries the dragged value — the app read the plugin back',
      `mirror says ${milli1}, dragged to ${TO * 1000}`);

const face1 = await face();
check(!!face1 && Math.abs(face1.fill - TO) < 0.08,
      'and the card face still shows it a second later — the reported bug',
      JSON.stringify(face1));
check(!!face1 && face1.value !== face0.value,
      'the row reads out the plugin\'s new value, not the one from before the drag',
      `${JSON.stringify(face0 && face0.value)} -> ${JSON.stringify(face1 && face1.value)}`);

// ---------------------------------------------------------------------------
// THE SAVED PROJECT, which is what tells the two causes apart.
//
// `<name>.state/t<track>_d<device>.params.json` is written by asking the plugin host for that
// plugin's parameters at save time — the plugin's own answer, not the rack's copy of it. A value
// here is proof the write reached the plugin.
// ---------------------------------------------------------------------------
await page.evaluate(() => window.__uni.saveProject('pwback'));
const stateDir = join(stack.dir, 'pwback.uniproj.state');
let manifest = null;
for (let i = 0; i < 40 && !manifest; i++) {
  await page.waitForTimeout(250);
  if (!existsSync(stateDir)) continue;
  const f = readdirSync(stateDir).find((n) => n.endsWith('.params.json'));
  if (f) { try { manifest = JSON.parse(readFileSync(join(stateDir, f), 'utf8')); } catch { manifest = null; } }
}
const saved = manifest && (manifest.params || []).find((p) => p.name === 'Gain');
check(!!saved, 'the saved project carries a parameter manifest for the device',
      manifest ? JSON.stringify(manifest).slice(0, 200) : 'no manifest written');
check(!!saved && Math.abs(Math.round(saved.value * 1000) - TO * 1000) <= TOL,
      'THE SAVED PROJECT has the dragged value — the write reached the PLUGIN, so a revert on '
      + 'the card is the display being wrong and not the parameter',
      saved ? `saved ${saved.value} (${saved.display}), dragged to ${TO}` : '');

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n  ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
