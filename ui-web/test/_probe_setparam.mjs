/**
 * MEASUREMENT PROBE (temporary) — does a rack parameter write reach the plugin?
 *
 * (a) the write never reached the plugin -> a live re-read and the saved manifest both
 *     still show the old value.
 * (b) the write landed and only the mirror is stale -> a live re-read shows the new
 *     value, and so does the saved params manifest.
 */
import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { writeFileSync, readFileSync, existsSync, readdirSync } from 'node:fs';
import { resolve, join } from 'node:path';

const Q = 960000;
const IDENTITY = resolve('build/identity_plugin_artefacts/RelWithDebInfo/VST3/Identity.vst3');

const stack = await startStack({ numBlocks: 8, keepDir: true });
console.log('stack dir', stack.dir);
writeFileSync(`${stack.dir}/spprobe.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'spprobe', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [], clips: [
    { clip_id: 1, name: 'c', length_nanoticks: Q * 4, events: [
      { type: 'note', nanotick_offset: 0, note: { pitch: 60, velocity: 100, duration_nanoticks: Q } },
      { type: 'note', nanotick_offset: Q, note: { pitch: 60, velocity: 100, duration_nanoticks: Q } },
    ] },
  ],
  tracks: [{
    track_id: 0, name: 'P', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
    device_chain: [
      { device_id: 6, kind: 'vst_instrument', capability_mask: 5, patcher_node_id: 0,
        host_slot_index: 4294967294, bypass: false,
        vst_ref: { vendor: '', name: 'Identity', path: IDENTITY, uid16: '' } }],
    mod_links: [], placements: [{ placement_id: 1, clip_id: 1, nanotick: 0, length_nanoticks: Q * 4 }],
  }],
}));

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
page.on('pageerror', (e) => console.log('PAGEERROR', e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 12000 });
await page.evaluate(() => window.__uni.loadProject('spprobe'));
await page.waitForFunction(
  () => { const c = window.__uni.chainProbe(); return c && c.params && c.params[0] > 0; },
  null, { timeout: 45000 });
await page.waitForTimeout(1500);

const mirror = () => page.evaluate(() => {
  const all = window.__uni.deviceParams();
  const k = Object.keys(all).find((x) => all[x].params && all[x].params.length);
  if (!k) return null;
  return { key: Number(k), track: Math.floor(Number(k) / 65536), device: Number(k) % 65536,
           name: all[k].name, params: all[k].params };
});

const before = await mirror();
console.log('device', before.device, 'name', before.name);
const gain = before.params.find((p) => p.name === 'Gain');
console.log('BEFORE gain', JSON.stringify(gain));

const TARGET = 400;
const sent = await page.evaluate(([t, d, i, u, v]) => window.__uni.setParam(t, d, i, u, v),
  [before.track, before.device, gain.index, gain.uid, TARGET]);
console.log('setParam returned', sent);

// 1. what the card shows after the optimistic hold expires (EDIT_HOLD_MS = 1200)
await page.waitForTimeout(2200);
const afterMirror = await mirror();
console.log('MIRROR after 2.2s  gain =',
  afterMirror.params.find((p) => p.name === 'Gain').valueMilli,
  '(published mirror, no re-request)');

// 2. a FRESH read-back straight from the plugin
await page.evaluate(([t, d]) => window.__uni.reqParams(t, d), [before.track, before.device]);
await page.waitForTimeout(1500);
const afterFresh = await mirror();
const fg = afterFresh.params.find((p) => p.name === 'Gain');
console.log('FRESH read-back    gain =', fg.valueMilli, 'display', JSON.stringify(fg.display));

// 3. with transport rolling, in case the plugin has to process a block to adopt it
await page.evaluate(() => window.__uni.transport('play'));
await page.waitForTimeout(1200);
await page.evaluate(([t, d, i, u, v]) => window.__uni.setParam(t, d, i, u, v),
  [before.track, before.device, gain.index, gain.uid, 250]);
await page.waitForTimeout(1200);
await page.evaluate(([t, d]) => window.__uni.reqParams(t, d), [before.track, before.device]);
await page.waitForTimeout(1500);
const playing = await mirror();
console.log('WHILE PLAYING      gain =',
  playing.params.find((p) => p.name === 'Gain').valueMilli, '(asked for 250)');

// 4. THE SAVED PROJECT
await page.evaluate(() => window.__uni.saveProject('spprobe'));
await page.waitForTimeout(3000);
for (const n of readdirSync(stack.dir)) console.log('  projdir:', n);
const stateDir = join(stack.dir, 'spprobe.uniproj.state');
console.log('state dir exists', existsSync(stateDir), existsSync(stateDir) ? readdirSync(stateDir) : '');
if (existsSync(stateDir)) {
  for (const f of readdirSync(stateDir)) {
    if (!f.endsWith('.params.json')) continue;
    const txt = readFileSync(join(stateDir, f), 'utf8');
    console.log('MANIFEST', f, txt.slice(0, 600));
  }
}

await browser.close();
await stack.stop();
console.log('probe done');
