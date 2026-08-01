#!/usr/bin/env node
/**
 * TWO SAMPLERS, DIFFERENT FILES, THE SAME LOCAL SOURCE ID.
 *
 * A sampler's `sourceLocalId` is a per-DEVICE counter, so the first source of every sampler is
 * id 1. The waveform answer echoes the id it was asked about — which means a cache keyed on that
 * id alone puts two different files in one entry, and the second pad draws the first one's audio.
 *
 * THAT IS A WRONG PICTURE, WHICH IS WORSE THAN A MISSING ONE, and it is invisible from every
 * surface: both pads show a waveform. Neither this repo's fixtures nor the engine's had two
 * samplers, so it survived a design review on both sides — backend shipped the colliding version,
 * I predicted the collision from the key rather than from a failure, and they landed
 * `kUiWaveformFlagSamplerSource` + `samplerAddr` so the answer names which sampler it is for.
 *
 * This is the fixture that can tell the two apart, and it is the whole reason the file exists:
 * ONE sampler proves nothing here. It is the same lesson as the one-device patcher publishing
 * owner 0 and the one-track kit read-back returning another track's answer — a fixture with one
 * of something cannot tell "correct" from "always".
 */

import { chromium } from 'playwright';
import { copyFileSync, writeFileSync } from 'node:fs';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8 });

/*
 * TWO AUDIBLY DIFFERENT FILES, synthesised with integer arithmetic so the bytes cannot move
 * under the check. The fingerprint the probe takes is a sum of magnitudes, so a square wave and
 * a much quieter one differ in it by a lot — which is what makes "these two entries hold
 * different audio" a claim rather than a hope.
 */
const RATE = 44100;
const mkWav = (path, amp, periodDiv) => {
  const n = RATE;                                  // one second
  const data = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) {
    const period = Math.round(RATE / periodDiv);
    data.writeInt16LE((i % period) < (period >> 1) ? amp : -amp, i * 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20);
  h.writeUInt16LE(1, 22); h.writeUInt32LE(RATE, 24);
  h.writeUInt32LE(RATE * 2, 28); h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34);
  h.write('data', 36); h.writeUInt32LE(data.length, 40);
  writeFileSync(path, Buffer.concat([h, data]));
};
mkWav(`${stack.dir}/loud.wav`, 14000, 220);
mkWav(`${stack.dir}/quiet.wav`, 1200, 660);

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => { if (!errors.includes(e.message)) errors.push(e.message); });
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1200);
const run = (c) => page.evaluate((x) => window.__uni.run(x), c);

await run('new wavecollide');
await page.waitForTimeout(2500);
await run('add-track');
await page.waitForTimeout(1200);

// A sampler on each of two tracks, one file each. Both files become LOCAL id 1 on their own
// device, which is the collision this exists to rule out.
for (const [track, file] of [[0, 'loud.wav'], [1, 'quiet.wav']]) {
  await run(`sampler ${track}`);
  await page.waitForTimeout(1200);
  await run(`load-sample ${track} 0 ${file}`);
  await page.waitForTimeout(2000);
}

const locals = await page.evaluate(async () => {
  const out = [];
  for (const t of [0, 1]) {
    window.__uni.samplerKit(t, 0);
    await new Promise((r) => setTimeout(r, 800));
    const k = window.__uni.samplerKitCached(t, 0);
    out.push(k && k.slots && k.slots[0] ? k.slots[0].source : -1);
  }
  return out;
});
check(locals[0] === locals[1] && locals[0] > 0,
      'both samplers call their first source by the SAME local id — the precondition for the '
      + 'collision, and the reason one sampler cannot find it', JSON.stringify(locals));

// Open the sample view on each in turn, so both windows are asked for and cached.
for (const t of [0, 1]) {
  await run(`goto 0 ${t}`);
  await page.waitForTimeout(500);
  await run(`bank ${t} 0 default-view 1`);
  await page.waitForTimeout(3000);
}

const cache = await page.evaluate(() => window.__uni.waveProbe());
console.log(`  cache: ${JSON.stringify(cache.map((e) => [e.key, e.addr, e.cols]))}`);
const sampler = cache.filter((e) => e.addr !== 0);
check(sampler.length >= 2,
      'the cache holds an entry per sampler, not one shared between them',
      JSON.stringify(sampler.map((e) => e.key)));
check(new Set(sampler.map((e) => e.addr)).size >= 2,
      'and they are told apart by the ADDRESS the answer echoes, not by the source id — which '
      + 'is identical for both', JSON.stringify(sampler.map((e) => [e.source, e.addr])));
/*
 * AND THEY HOLD DIFFERENT AUDIO. The two checks above would both pass on two entries that
 * happened to contain the same file — which is precisely what the old key produced once the
 * second request overwrote the first. The fingerprint is what makes this a statement about the
 * SAMPLES rather than about the bookkeeping.
 */
const inks = sampler.map((e) => e.ink);
check(new Set(inks).size >= 2,
      'and the two entries hold DIFFERENT audio — the loud file and the quiet one, which is the '
      + 'claim the keys alone cannot make', JSON.stringify(inks));

check(errors.length === 0, 'and nothing threw', errors.slice(0, 3).join(' | '));
await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
