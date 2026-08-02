#!/usr/bin/env node
/**
 * A CHORD MAKES NO SOUND, AND NOTHING IN THIS REPO COULD HAVE TOLD US.
 *
 * Backend measured it on 2026-08-01: in one offline render, one sampler, one track, a plain
 * note peaks at 9263 and a chord in the same fixture peaks at 0. It is not dropped early —
 * it is in the model, the scheduler reaches it, it resolves its pitches against the scale in
 * force, and it emits note-ons byte-identical to a note's. The failure is downstream of the
 * emission and upstream of the sampler.
 *
 * It survived because **no fixture in this repo has ever contained a chord**. Every chord
 * test there is checks what a chord WRITES — the token parses, the cell draws, the clip
 * round-trips — and not one asks what comes out.
 *
 * This is that question, from the UI, through the OFFLINE RENDER. The render needs no audio
 * device, which matters here: this machine's device opens and never calls a callback, so a
 * live capture cannot answer anything at all.
 *
 * THE NOTE IS THE CONTROL AND IT IS NOT OPTIONAL. "The chord window is silent" and "the whole
 * render is silent" are the same observation, and only a note that DOES sound in the same
 * render tells them apart. Without it this suite would have passed against a broken engine,
 * a broken sampler, a missing sample and an empty project alike — which is exactly the shape
 * of the hole it exists to close.
 *
 * THE CHORD CHECK IS INVERTED, FOR NOW. It asserts the current, broken state, so it goes red
 * the day the engine is fixed and says what to write instead. That is only safe while its two
 * outcomes stay distinguishable, which the note control is what guarantees — the caveat
 * learned the same day from a master-meter check that kept passing after its gap closed,
 * because a silent machine and a constant zero look identical.
 */

import { chromium } from 'playwright';
import { execFileSync } from 'node:child_process';
import { writeFileSync, readFileSync, existsSync, unlinkSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startStack } from './stack.mjs';
import { readWav, rmsBetween } from './wav.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const RATE = 44100;

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8 });

/*
 * ONE SQUARE-WAVE SAMPLE, built from integer arithmetic so the bytes are identical on any
 * machine that runs this file. Long enough that a note and a chord both have room to sound
 * inside their own window.
 */
{
  const n = Math.round(RATE * 0.8);
  const data = Buffer.alloc(n * 2);
  const period = Math.round(RATE / 220);
  for (let i = 0; i < n; i++) {
    data.writeInt16LE((i % period) < (period >> 1) ? 12000 : -12000, i * 2);
  }
  const head = Buffer.alloc(44);
  head.write('RIFF', 0); head.writeUInt32LE(36 + data.length, 4); head.write('WAVE', 8);
  head.write('fmt ', 12); head.writeUInt32LE(16, 16); head.writeUInt16LE(1, 20);
  head.writeUInt16LE(1, 22); head.writeUInt32LE(RATE, 24);
  head.writeUInt32LE(RATE * 2, 28); head.writeUInt16LE(2, 32); head.writeUInt16LE(16, 34);
  head.write('data', 36); head.writeUInt32LE(data.length, 40);
  writeFileSync(join(stack.dir, 'tone.wav'), Buffer.concat([head, data]));
}

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
const errors = [];
page.on('pageerror', (e) => { if (!errors.includes(e.message)) errors.push(e.message); });
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1500);

const said = [];
const run = async (c) => {
  const r = await page.evaluate((x) => window.__uni.run(x), c);
  said.push(c);
  return r;
};

// ---------------------------------------------------------------------------
// BUILD IT THROUGH THE UI. A hand-written project would test the format, not the surface —
// and the surface is where a person types a chord and hears nothing.
// ---------------------------------------------------------------------------
await run('new chordsong');
await page.waitForTimeout(2500);
await run('sampler 0');
await page.waitForTimeout(1500);
// One sample across a wide key range, so every pitch a chord resolves to has something to
// play. A chord that resolved to keys with no slot would be silent for a reason that has
// nothing to do with the bug under test.
await run('load-sample 0 0 tone.wav 36');
await page.waitForTimeout(2000);
await run('slot 0 0 1 keylow 0');
await run('slot 0 0 1 keyhigh 127');
await page.waitForTimeout(800);
// An amp envelope, or the voice has no level to render at all.
await run('env 0 0 1000 200000 1000 200000');
await page.waitForTimeout(1200);

await page.evaluate(() => window.__uni.samplerKit(0, 0));
await page.waitForTimeout(1500);
const kit = await page.evaluate(() => window.__uni.samplerKitCached(0, 0));
check(kit && kit.slots && kit.slots.length > 0,
      'the sampler has a slot to play — without one the render is silent for a reason that '
      + 'has nothing to do with chords',
      JSON.stringify(kit && kit.slots && kit.slots.length));

// ---------------------------------------------------------------------------
// A NOTE NEAR THE START, A CHORD WELL AFTER IT.
// ---------------------------------------------------------------------------
await run('view tracker');
await run('goto 0 0');
await run('note 60 960000 100');
await page.waitForTimeout(900);

await run('goto 32 0');
const chordSaid = await run('chord 1 triad');
await page.waitForTimeout(1200);

/*
 * WHERE THEY ACTUALLY LANDED, ASKED RATHER THAN COMPUTED.
 *
 * `goto 32` is thirty-two rows in the CURRENT zoom's row space, and a row is a different
 * number of ticks at every zoom — so computing "row 32 is eight seconds" bakes in a zoom
 * this file never set. Ask the model where the material is, then measure there.
 *
 * AND A CHORD IS NOT A SET OF NOTES. It is stored as a degree, a quality and an inversion,
 * resolved against the harmony timeline when it plays — so `notes()` cannot see one and the
 * first version of this file concluded the chord had not been written at all. It is its own
 * event list, and asking the wrong one is how you accuse a feature of not existing.
 */
const placed = await page.evaluate(() => {
  // `__uni.notes()` names its fields short: `t` is the on-tick, `p` the pitch.
  const notes = (window.__uni.notes && window.__uni.notes()) || [];
  const chords = (window.__uni.chords && window.__uni.chords()) || [];
  return { notes: notes.map((n) => ({ tick: n.t })), chords };
});
console.log(`  notes at ${JSON.stringify(placed.notes.map((n) => n.tick))}` +
            `, chords at ${JSON.stringify(placed.chords.map((c) => c.tick))}`);

check(placed.notes.length >= 1, 'the note is in the clip',
      JSON.stringify(placed.notes));
check(placed.chords.length >= 1, 'and the chord is in the clip as a chord EVENT, not as notes',
      `${JSON.stringify(placed.chords)} — said ${JSON.stringify(chordSaid).slice(-70)}`);
const noteTick = placed.notes.length ? placed.notes[0].tick : 0;
const chordTick = placed.chords.length ? placed.chords[0].tick : -1;
check(chordTick > noteTick, 'and they are at different ticks, so one window cannot catch both',
      `note ${noteTick}, chord ${chordTick}`);

await run('save chordsong');
await page.waitForTimeout(2000);
check(existsSync(join(stack.dir, 'chordsong.uniproj.json')), 'and the song is on disk');

await browser.close();
stack.stop();

// ---------------------------------------------------------------------------
// RENDER IT OFFLINE. No audio device, no wall clock: the pump waits for every host to finish
// each block, so this is the one path on this machine that can answer a question about sound.
// ---------------------------------------------------------------------------
const OUT = join(stack.dir, 'chordtake.wav');
try { unlinkSync(OUT); } catch { /* absent is the normal case */ }
let rendered = null;
try {
  execFileSync(join(ROOT, 'build', 'daw_engine'),
               ['--project', 'chordsong', '--render', 'chordtake', '--run-seconds', '24'],
               { cwd: join(ROOT, 'build'),
                 env: { ...process.env,
                        DAW_PROJECT_DIR: stack.dir,
                        DAW_HOST_BINARY: join(ROOT, 'build', 'juce_host_process'),
                        DAW_UI_SHM_NAME: `/chordrender_${process.pid}` },
                 stdio: ['ignore', 'pipe', 'pipe'], timeout: 180000 });
  rendered = existsSync(OUT) ? readWav(OUT) : null;
} catch (e) {
  check(false, 'the offline render runs', String(e).slice(0, 200));
}
check(!!rendered, 'the offline render writes a WAV',
      rendered ? `${(rendered.mono.length / rendered.rate).toFixed(1)}s` : 'nothing');

if (rendered) {
  const { mono, rate } = rendered;
  /*
   * Windows derived from where the material actually IS. 960000 nanoticks is a quarter note
   * and the song is 120 BPM, so a quarter is half a second. Guarded at both ends — this asks
   * "did anything sound here", it does not measure a level.
   */
  const secOf = (tick) => (tick / 960000) * 0.5;
  const noteAt = secOf(noteTick);
  const chordAt = secOf(chordTick);
  console.log(`  note at ${noteAt.toFixed(2)}s, chord at ${chordAt.toFixed(2)}s`);
  /*
   * THE WINDOW HAS TO BE INSIDE THE FILE.
   *
   * The first run of this rendered twelve seconds and put the chord at sixteen, so the chord
   * window was past the end and read 0.00000 — and the inverted check PASSED, reporting
   * silence it had not measured. A window off the end of a file and a silence in it are the
   * same number, and only this tells them apart.
   */
  const dur = mono.length / rate;
  check(chordAt + 0.45 < dur,
        'the chord is inside the rendered take — a window past the end reads as silence',
        `chord window ends at ${(chordAt + 0.45).toFixed(2)}s of a ${dur.toFixed(2)}s render`);

  const noteLevel = rmsBetween(mono, rate, noteAt + 0.05, noteAt + 0.45);
  const chordLevel = chordAt + 0.45 < dur
    ? rmsBetween(mono, rate, chordAt + 0.05, chordAt + 0.45) : NaN;
  console.log(`  note window ${noteLevel.toFixed(5)}   chord window ${chordLevel.toFixed(5)}`);

  /*
   * THE CONTROL, FIRST AND LOUDEST. Everything below is meaningless without it: a silent
   * render makes the chord check pass for a reason that has nothing to do with chords.
   */
  check(noteLevel > 0.005, 'the NOTE sounds — so the render, the sampler and the sample all work',
        noteLevel.toFixed(5));

  if (noteLevel > 0.005 && Number.isFinite(chordLevel)) {
    /*
     * THE CLAIM. This was an INVERTED check for one afternoon — it asserted the silence, so
     * that it would go red the day the engine was fixed and say what to write instead. It did
     * exactly that on its second run, and this is what it said to write.
     *
     * The fix was engine-side: the chord path was a SECOND COPY of emitNoteOnWithOff and the
     * copy was missing the sampler tee on the note-on, so chords were silent through the
     * built-in sampler and correct through a hosted VST. Backend measured it through daw-cli;
     * this measures it through the sidecar, which is the half their check does not cover.
     *
     * WHAT THIS DOES NOT CLAIM: that all three voices sound. RMS over a window cannot count
     * them — three copies of one sample at different pitches can sum to more or less than one
     * copy depending on phase and on how far each is shifted. It says the chord is audible,
     * which is the thing that was false.
     */
    check(chordLevel > 0.005, 'and the CHORD sounds too',
          `${chordLevel.toFixed(5)} against the note's ${noteLevel.toFixed(5)}`);
  }
}

check(errors.length === 0, 'and nothing threw in the browser', errors.slice(0, 2).join(' | '));

console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
