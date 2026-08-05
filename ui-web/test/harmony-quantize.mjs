#!/usr/bin/env node
/**
 * THE HARMONY LANE, AND WHETHER IT ACTUALLY QUANTIZES ANYTHING.
 *
 * A demo feature with, until this file, ZERO coverage: `harmony_quantize` appeared in this
 * directory only as the word `false` inside project fixtures. Nothing set it, nothing read it
 * back, and nothing checked that turning it on changed a single sample of audio.
 *
 * WHAT IT CLAIMS. A key change on the harmony timeline says what scale is in force from that
 * tick. A track with harmony-quantize ON snaps its notes to that scale — so an out-of-key note
 * SOUNDS in key while the note you authored stays what you typed. That last part is the whole
 * design: like timing quantize, it is non-destructive, so the stored pitch is still yours.
 *
 * WHICH MAKES IT HARD TO TEST, AND IS WHY THIS USES THE RENDER. The read-back cannot show it:
 * the clip still holds the pitch that was typed, by design, so a check that compared the note
 * store before and after would compare two identical numbers and pass whatever the engine did
 * with the sound. The only place the difference exists is the audio.
 *
 * So: build one song, render it twice — once with the track quantized to the harmony and once
 * not — and require the two to DIFFER. Then the claim is about what came out of the engine
 * rather than about a flag this side set on itself.
 *
 * THE NOTE IS CHOSEN TO BE OUT OF KEY. C# against C major: the one semitone that cannot be
 * mistaken for anything in the scale, so a snap has somewhere to move it to and a render that
 * ignored the flag has nowhere to hide.
 */

import { chromium } from 'playwright';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startStack } from './stack.mjs';
import { readWav, envelope } from './wav.mjs';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));
let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const SONG = 'harmq';
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

console.log('\nthe harmony lane and its quantization\n');

await run(`new ${SONG}`);
await settle(1300);

// ---------------------------------------------------------------------------
// A KEY. Without one the flag has nothing to snap to, and the card says so rather
// than pretending — which is itself worth asserting, because "no key" and "the
// feature does nothing" look identical from outside.
// ---------------------------------------------------------------------------
// `harmony()` answers an ARRAY of events, not `{list}` — reading `.list` off it gave 0
// and reported a working key change as a missing one.
const before = await page.evaluate(() => (window.__uni.harmony() || []).length);
await run('harmony 0 major 0');
await settle(1200);
const after = await page.evaluate(() => (window.__uni.harmony() || []).length);
check(after > before || after > 0, 'a key change lands on the harmony timeline',
      `${before} -> ${after} events`);

// ---------------------------------------------------------------------------
// A SAMPLER THAT SOUNDS, and mapped ACROSS the keys rather than pinned to one.
//
// A slot is minted at root 36 fixed-pitch answering exactly C-2, so a snap that
// moved a note by a semitone would produce the same silence either way and the
// comparison below would be between two identical empty renders.
// ---------------------------------------------------------------------------
await run('sampler 0');
await settle(1200);
const kitDev = await page.evaluate(() => {
  for (let d = 0; d < 6; d++) {
    const k = window.__uni.samplerKitCached(0, d);
    if (k) return d;
  }
  return 1;
});
await run(`load-sample 0 ${kitDev} waveform_probe.wav`);
await settle(2500);
const slot = await page.evaluate((d) => {
  const k = window.__uni.samplerKitCached(0, d);
  return k && k.slots && k.slots.length ? k.slots[0].slot : 1;
}, kitDev);
// A wide zone, NOT fixed pitch: the sample must transpose with the note, or a snapped
// pitch and an unsnapped one play the identical sample and the renders match.
await run(`slot 0 ${kitDev} ${slot} keylow 36`);
await run(`slot 0 ${kitDev} ${slot} keyhigh 96`);
await run(`slot 0 ${kitDev} ${slot} root 60`);
// `pitchtrack 1` — the slot TRANSPOSES with the note. A fixed-pitch slot plays the
// same sample whatever key triggers it, which would make a snapped pitch and an
// unsnapped one produce identical audio and the comparison below vacuous.
await run(`slot 0 ${kitDev} ${slot} pitchtrack 1`);
await settle(1200);

const zone = await page.evaluate(([d, s]) => {
  const k = window.__uni.samplerKitCached(0, d);
  const sl = k && k.slots && k.slots.find((x) => x.slot === s);
  return sl ? { lo: sl.keyLow, hi: sl.keyHigh, root: sl.root } : null;
}, [kitDev, slot]);
check(!!zone && zone.hi > zone.lo,
      'the sample answers a RANGE of keys, so a snapped pitch sounds different',
      JSON.stringify(zone));

// ---------------------------------------------------------------------------
// AN OUT-OF-KEY NOTE. C# against C major.
// ---------------------------------------------------------------------------
/*
 * THE FIXTURE'S SAMPLE IS SILENT FOR ITS FIRST SECOND, and that is not a bug.
 *
 * `waveform_probe.wav` is the WAVEFORM PEAKS probe asset — stepped level regions for testing
 * the peak pyramid, not a musical sample — and its first second is digital silence by
 * construction. So a note that starts at tick 0 is inaudible until a second after it starts,
 * and a window measured over the opening of a render reads 0 for a note that played perfectly.
 *
 * I reported this as "a note at tick 0 is dropped from the render" after measuring five notes
 * at 0/1/2/3/4s and finding onsets at 1/2/3/4. Backend rendered the same file and took it
 * apart: deleting the tick-0 note made the note at 1.0s vanish instead, and moving every note
 * +0.25s moved the silence with them. It was never tick 0 and never the clip length — it was
 * always the first note, because the first second of every note is silent.
 *
 * The notes here are therefore spaced far enough apart to clear that second. Not a workaround
 * for an engine defect: a fixture whose asset is silent at the start needs its measurements
 * placed where the asset makes sound.
 */
for (const row of [2, 4, 6]) {
  await run(`goto ${row} 0`);
  await run('note 61');          // C#, the semitone C major does not contain
  await settle(250);
}
await settle(900);
const stored = await page.evaluate(() =>
  (window.__uni.notes() || []).filter((n) => n.tr === 0).map((n) => n.p));
check(stored.filter((x) => x === 61).length >= 3,
      'the out-of-key notes are written as typed', JSON.stringify(stored));

const render = (name) => {
  const out = join(stack.dir, `${name}.wav`);
  try { unlinkSync(out); } catch { /* absent is normal */ }
  execFileSync(join(ROOT, 'build', 'daw_engine'),
               ['--project', SONG, '--render', name, '--run-seconds', '8'],
               { cwd: join(ROOT, 'build'),
                 env: { ...process.env, DAW_PROJECT_DIR: stack.dir,
                        DAW_HOST_BINARY: join(ROOT, 'build', 'juce_host_process'),
                        DAW_UI_SHM_NAME: `/harmq_${process.pid}_${name}` },
                 stdio: ['ignore', 'pipe', 'pipe'], timeout: 120000 });
  return existsSync(out) ? readFileSync(out) : null;
};

// OFF first, so the comparison is against the engine's own untouched output.
await run('harmony-quantize 0 off');
await settle(800);
await run(`save ${SONG}`);
await settle(1800);
let plain = null;
try { plain = render('plain'); } catch (e) { check(false, 'the render runs', String(e).slice(0, 180)); }
check(plain && plain.length > 44, 'the unquantized song renders',
      plain ? `${plain.length} bytes` : 'nothing');

if (plain) {
  const w = readWav(join(stack.dir, 'plain.wav'));
  const pk = envelope(w.mono, w.rate, 0.05).reduce((m, v) => Math.max(m, v), 0);
  /*
   * NOT SILENT, FIRST. Two silent renders are byte-identical, so without this the
   * comparison below would report "quantizing changed nothing" for a song that never
   * made a sound — the check passing for the wrong reason, in the direction that reads
   * as a working feature being broken.
   */
  check(pk > 0.001, 'and it is not silent — the comparison below can mean something',
        `peak ${pk.toFixed(4)}`);
}

// ---------------------------------------------------------------------------
// ON. Same song, same note, same everything else.
// ---------------------------------------------------------------------------
const said = await run('harmony-quantize 0 on');
await settle(1000);
// `harmonyQuantized()` answers an ARRAY, one entry per track — it takes no argument, and
// calling it with one returns the whole array, which is truthy for every track including
// the ones that are off.
const flags = await page.evaluate(() => window.__uni.harmonyQuantized());
check(Array.isArray(flags) && flags[0] === 1, 'the flag reads back on, from the engine',
      `${JSON.stringify(said)} · harmonyQuantized() = ${JSON.stringify(flags)}`);

await run(`save ${SONG}`);
await settle(1800);
let snapped = null;
try { snapped = render('snapped'); } catch (e) { check(false, 'the quantized render runs', String(e).slice(0, 180)); }
check(snapped && snapped.length > 44, 'the quantized song renders',
      snapped ? `${snapped.length} bytes` : 'nothing');

/*
 * THE CLAIM. The stored note did not move — asserted above — so any difference in the
 * audio is the harmony timeline acting on what SOUNDS, which is exactly what the feature
 * says it does.
 */
if (plain && snapped) {
  check(Buffer.compare(plain, snapped) !== 0,
        'quantizing the track to the harmony CHANGES THE SOUND of an out-of-key note',
        'the two renders were byte-identical, so the flag reached nothing');
}

const stillStored = await page.evaluate(() =>
  (window.__uni.notes() || []).filter((n) => n.tr === 0).map((n) => n.p));
check(stillStored.includes(61),
      'and the note you typed is still the note that is stored — non-destructive',
      JSON.stringify(stillStored));

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
