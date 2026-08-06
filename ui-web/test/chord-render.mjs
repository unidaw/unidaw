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
import { envelope, readWav, rmsBetween } from './wav.mjs';

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
 * AND A STRUMMED ONE, WELL AFTER THAT.
 *
 * The strum is the headline of the runbook's chord section and nothing measured it. `spread` was
 * decoded away below the UI until this week, so "the model asked for a strum" and "the song
 * contains one" were separate claims — and even once it crossed the wire, that it CHANGES WHAT
 * YOU HEAR was still unasserted.
 *
 * 480000 nanoticks is a quarter of a second at 120 BPM: long enough that the voices arriving one
 * after another is a shape in the envelope rather than a rounding difference, and short enough
 * to still read as one chord.
 *
 * `chord <degree> <quality> <inv> <oct> <spread> <ht> <hv>` — the same verb, one argument more.
 */
// Row 40, not 64. A row is half a second here, so 64 put the strum at 32s in a 24s render and
// its window read as silence — the same "window past the end" trap the block chord already has a
// guard for, which is why that guard is now extended to cover this one too.
await run('goto 40 0');
const strumSaid = await run('chord 1 triad 0 4 480000 0 0');
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
/*
 * SORTED, AND THE STRUM IDENTIFIED BY ITS SPREAD rather than by its position in the list. The
 * order `chords()` returns is the engine's and not a promise; picking [0] and [1] would silently
 * compare the block chord against itself the day that changes, and the check would pass.
 */
const sorted = [...placed.chords].sort((a, b) => a.tick - b.tick);
const blockChord = sorted.find((c) => !c.spread);
const strumChord = sorted.find((c) => c.spread > 0);
const chordTick = blockChord ? blockChord.tick : -1;
const strumTick = strumChord ? strumChord.tick : -1;
check(chordTick > noteTick, 'and they are at different ticks, so one window cannot catch both',
      `note ${noteTick}, chord ${chordTick}`);
check(!!strumChord && strumTick > chordTick,
      'the strummed chord is there too, later, and carries its spread',
      `${JSON.stringify(sorted.map((c) => ({ tick: c.tick, spread: c.spread })))} — said `
      + `${JSON.stringify(strumSaid).slice(-70)}`);

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
  const strumAt = secOf(strumTick);
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

/*
 * DOES A STRUM SOUND DIFFERENT FROM A BLOCK CHORD?
 *
 * The two chords are the same degree, quality, inversion and octave. The ONLY difference is
 * `spread`, so anything that separates them in the audio is the strum and nothing else.
 *
 * MEASURED AS RISE TIME, not as level. A block chord's voices all start together, so it reaches
 * its peak within a bucket or two; a strummed one arrives voice by voice across `spread` and
 * climbs over that time. Comparing PEAKS would prove nothing — both are the same three notes and
 * end up equally loud, which is exactly why "the strum is on the wire" was as far as the earlier
 * checks could go.
 */
if (rendered) {
  const { mono, rate } = rendered;
  const per = 0.01;
  const env = envelope(mono, rate, per);
  /**
   * How long the window SOUNDS for, in seconds.
   *
   * DURATION, not rise time. My first metric was time-to-peak, on the theory that a strum climbs
   * as its voices accumulate — and the envelopes say otherwise. Both chords are flat at 0.144
   * and the difference is how LONG that lasts:
   *
   *     block   0.144 x15 buckets then silence      0.15s
   *     strum   0.144 x40 buckets then silence      0.40s
   *
   * Which is the strum stated plainly: the last voice starts `spread` after the first, so the
   * chord goes on sounding for `spread` longer. Rise time saw 0.100 against 0.120 and called it
   * no difference, and it was measuring the wrong feature of the right audio.
   */
  const soundsFor = (at0, span) => {
    const lo = Math.max(0, Math.round(at0 / per));
    const hi = Math.min(env.length, Math.round((at0 + span) / per));
    let n = 0;
    for (let i = lo; i < hi; i++) if (env[i] > 0.005) n++;
    return n * per;
  };
  const at = (tick) => (tick / 960000) * 0.5;
  const dur = mono.length / rate;
  check(at(strumTick) + 1.5 < dur,
        'the strummed chord is inside the rendered take too',
        `its window ends at ${(at(strumTick) + 1.5).toFixed(2)}s of a ${dur.toFixed(2)}s render — `
        + `past the end reads as silence and blames the strum`);

  const blockFor = soundsFor(at(chordTick), 1.5);
  const strumFor = soundsFor(at(strumTick), 1.5);
  const SPREAD_S = 480000 / 960000 * 0.5;            // 480000 nanoticks at 120 BPM = 0.25s
  console.log(`  sounds for: block ${blockFor.toFixed(2)}s   strummed ${strumFor.toFixed(2)}s ` +
              `(spread is ${SPREAD_S.toFixed(2)}s)`);
  /*
   * The two chords are the same degree, quality, inversion and octave; `spread` is the only
   * difference, so the extra duration is the strum and nothing else. Compared against the spread
   * rather than "is it bigger" — bigger by any amount would also pass on a chord that simply
   * rang longer for an unrelated reason.
   */
  check(Math.abs((strumFor - blockFor) - SPREAD_S) < 0.08,
        'A STRUM IS AUDIBLY A STRUM — it sounds for exactly `spread` longer than the block chord',
        `block ${blockFor.toFixed(2)}s, strummed ${strumFor.toFixed(2)}s, difference `
        + `${(strumFor - blockFor).toFixed(2)}s against a spread of ${SPREAD_S.toFixed(2)}s`);
}

check(errors.length === 0, 'and nothing threw in the browser', errors.slice(0, 2).join(' | '));

console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
