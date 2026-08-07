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
import { detectNotes, noteName } from './notes.mjs';

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
/*
 * WAIT FOR THE KIT, AND SAY SO IF IT NEVER COMES — this slept 1200ms and then GUESSED.
 *
 * `samplerKitCached` is a cache filled by the reply to `samplerKit`, so reading it after a fixed
 * sleep is a race. The old code scanned six devices and, finding nothing, fell back to `return 1`.
 * That guess is what made the failure unreadable: every later `slot 0 <dev> ...` addressed a
 * device that might not be the sampler, and the first visible symptom was `zone` coming back null
 * about thirty lines further down — "the sample answers a RANGE of keys — null", which is what
 * sweep 21 printed and which says nothing about the actual cause.
 *
 * ASKS ON EVERY POLL, like sampler-device-id.mjs does, because a request can be answered before
 * this page was listening; polling at 500ms rather than every frame keeps that from becoming a
 * request storm. A guess is replaced by a check that fails where the problem is.
 */
const kitFound = await page.waitForFunction(() => {
  for (let d = 0; d < 6; d++) {
    window.__uni.samplerKit(0, d);
    const k = window.__uni.samplerKitCached(0, d);
    /*
     * `k.found`, NOT just `k`. Asking about a device that holds no sampler still gets an ANSWER
     * — an empty kit — and the page caches it, so a truthy kit means "the engine replied", not
     * "there is a sampler here". The original code tested `if (k)` and this rewrite copied it;
     * a control that scanned devices 90-95 selected device 90 quite happily and only fell over
     * two checks later. `found` is the field that distinguishes the two.
     */
    if (k && k.found) return { dev: d };
  }
  return null;
}, null, { timeout: 20000, polling: 500 }).then((h) => h.jsonValue()).catch(() => null);
check(kitFound !== null,
      'the sampler answers with a kit, so the device id below is read and not guessed',
      'no kit from any of devices 0-5 in 20s — everything after this would have been aimed at a '
      + 'guessed device, which is how this failed as an unrelated null further down');
const kitDev = kitFound ? kitFound.dev : 1;
/*
 * A PITCHED SAMPLE, because this suite now asserts WHICH NOTES SOUND.
 *
 * It used `waveform_probe.wav`, which is the peak-pyramid probe asset: stepped level regions with
 * no pitch in them at all. Against it the detector reported "A-4" for every note in both renders
 * — a periodicity in the asset, not the note being played — and TWO of the three pitch checks
 * passed vacuously on that: "the C# is gone" is trivially true when nothing was ever C#, and
 * "everything is in C major" is trivially true of an A.
 *
 * `demo_pluck_c4.wav` is middle C with real harmonics, so a note played at 61 comes back as 61.
 */
await run(`load-sample 0 ${kitDev} demo_pluck_c4.wav`);
/*
 * AND THE SLOT, for the same reason: this also slept and then fell back to `1`. A wrong slot id
 * makes every `slot 0 <dev> <slot> ...` below land on nothing, and the suite goes on to render
 * and compare two files that were never configured.
 */
const slotFound = await page.waitForFunction((d) => {
  window.__uni.samplerKit(0, d);
  const k = window.__uni.samplerKitCached(0, d);
  return (k && k.slots && k.slots.length) ? { slot: k.slots[0].slot } : null;
}, kitDev, { timeout: 20000, polling: 500 }).then((h) => h.jsonValue()).catch(() => null);
check(slotFound !== null,
      'and the loaded sample appears as a slot in that kit',
      `no slot on device ${kitDev} in 20s — the slot id below would have been a guess`);
const slot = slotFound ? slotFound.slot : 1;
// A wide zone, NOT fixed pitch: the sample must transpose with the note, or a snapped
// pitch and an unsnapped one play the identical sample and the renders match.
await run(`slot 0 ${kitDev} ${slot} keylow 36`);
await run(`slot 0 ${kitDev} ${slot} keyhigh 96`);
await run(`slot 0 ${kitDev} ${slot} root 60`);
/*
 * `pitchtrack 1000`, IN MILLI-UNITS — and it was `1` here, which is 0.001.
 *
 * The field is `pitch_track_milli`: 1000 is "transpose fully with the note", 0 is "play the
 * sample at its own pitch whatever key triggers it". Setting 1 asked for a thousandth of a
 * semitone per semitone, so the slot played every note at middle C — and the comment right above
 * it said the opposite in capitals.
 *
 * That is what made the whole suite nearly vacuous. Its claim is that harmony quantize CHANGES
 * WHAT YOU HEAR, and with the slot not transposing, re-pitching a note from C# to C moved the
 * playback rate by 0.001 semitones — about six hundredths of a cent. The two renders differed by
 * a handful of bytes, `Buffer.compare !== 0` passed, and nothing audible had happened at all.
 *
 * Found by asserting PITCHES instead of difference: the detector reported C-4 for a note the clip
 * stores as C#, in both renders.
 */
await run(`slot 0 ${kitDev} ${slot} pitchtrack 1000`);
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

/*
 * WHAT PITCHES ACTUALLY SOUNDED — the assertion this suite could not make until there was a note
 * detector.
 *
 * "The two renders differ" is satisfied by ANY difference: a gain change, a dropped note, a click.
 * It says the flag did something and not that it did the right thing, and the whole claim of the
 * harmony system is a claim about WHICH NOTES YOU HEAR.
 *
 * The song holds C# (61), which C major does not contain. So:
 *
 *   quantize OFF  ->  C# sounds, because the note is played as typed
 *   quantize ON   ->  C or D sounds, because it is re-pitched into the key
 *
 * and the stored note stays 61 either way, which is asserted above. That is the feature stated in
 * full: non-destructive on disk, different in the air.
 */
const pitchesIn = (name) => {
  const path = join(stack.dir, `${name}.wav`);
  if (!existsSync(path)) return [];
  const w = readWav(path);
  // Confident detections only: a half-heard tail is not evidence about a pitch.
  return detectNotes(w.mono, w.rate, { minConf: 0.6 })
    .filter((n) => n.level > 0.01)
    .map((n) => n.midi);
};
const plainPitches = pitchesIn('plain');
const snappedPitches = pitchesIn('snapped');
console.log(`  unquantized sounded: ${JSON.stringify(plainPitches.map(noteName))}`);
console.log(`  quantized sounded:   ${JSON.stringify(snappedPitches.map(noteName))}`);

check(plainPitches.includes(61),
      'UNQUANTIZED, the C# you typed is the C# you hear',
      `sounded ${JSON.stringify(plainPitches.map(noteName))} — no C# means this render is not `
      + `playing the note the clip holds, and the comparison below is against the wrong thing`);
check(snappedPitches.length > 0 && !snappedPitches.includes(61),
      'QUANTIZED, the C# is gone from the audio — the harmony system is applied',
      `sounded ${JSON.stringify(snappedPitches.map(noteName))}; a C# still here means the flag `
      + `reads back on and changes nothing you can hear`);
check(snappedPitches.every((p) => [0, 2, 4, 5, 7, 9, 11].includes(((p % 12) + 12) % 12)),
      'and everything that sounds is IN C major — re-pitched, not merely moved',
      `sounded ${JSON.stringify(snappedPitches.map(noteName))}`);

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
