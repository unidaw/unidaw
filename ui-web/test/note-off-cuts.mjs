#!/usr/bin/env node
/**
 * DOES A NOTE-OFF CUT A SAMPLED NOTE SHORT?
 *
 * Jaakko's ruling, after I recorded the opposite as defensible: "should be able to cut it off
 * with note off". The engine agrees and always has — `sampler_engine.h`'s noteOff() releases a
 * voice unless its slot has `gate == 0`, and the comment there calls that "the difference
 * between a drum and a pad". What was missing is that NO SURFACE COULD SET IT: gate is field 2
 * of SamplerSetSlot, and until now the only way to change it was to hand-edit a project file.
 *
 * So a half-second note on an eight-second sample sounded for eight seconds, and it took three
 * guesses in another test before I stopped reading that as a stuck voice.
 *
 * This measures the LENGTH of the sound rather than its content: one short note, and the loud
 * stretch it produces must end when the note does rather than when the sample does. The control
 * is the same run with gate left at its default, where the stretch is the whole sample — an
 * order of magnitude apart, so the threshold is not a judgement call.
 */

import { chromium } from 'playwright';
import { join, resolve } from 'node:path';
import { existsSync, statSync, unlinkSync, writeFileSync, readFileSync } from 'node:fs';
import { startStack } from './stack.mjs';
import { readWav, envelope, summarise, zeroCrossingRate } from './wav.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

/*
 * LONG ENOUGH TO BUILD THE CHOP AND THEN PLAY IT.
 *
 * The engine is given a BOUNDED run so it exits and writes the capture, and the first version
 * budgeted fourteen seconds — most of which went on launching a browser, adding a track, making
 * a sampler, loading a sample and waiting for the kit. The slice command landed near the end and
 * the poll for its result ran past the engine's exit, so the chop simply never happened and the
 * failure read as "the chop is built from the UI — null".
 *
 * Setup is not the measurement. The window has to cover both.
 */
const CAPTURE_SECONDS = 34;
const WAV = join(process.cwd(), 'ui-web/test/out/note-off-cuts.wav');

// The engine must EXIT for the capture to be written, so it gets a bounded run rather than a
// kill — the first version of the audio test killed the engine and then looked for a file that
// was never going to exist.
/*
 * DELETE LAST RUN'S CAPTURE FIRST.
 *
 * The wait below is "poll until the file exists", and a file left behind by an earlier run
 * satisfies that instantly. Every run then analysed the PREVIOUS run's audio — which is how a
 * 34-second capture came to be read by a test configured for 50, and why a control track running
 * a known-good plugin looked silent: the silence was real, and it was three runs old.
 *
 * A guard that is satisfied by its own leftovers is not a guard.
 */
try { unlinkSync(WAV); } catch { /* nothing to remove is the normal case */ }

const stack = await startStack({ keepDir: true, capture: WAV, captureSeconds: CAPTURE_SECONDS,
                                 runSeconds: CAPTURE_SECONDS + 12, numBlocks: 8 });
/*
 * AN AUTHORED SAMPLE, BECAUSE THE FIXTURE DECIDES WHAT IS FINDABLE.
 *
 * `presets/audio/waveform_probe.wav` cannot answer this question: it is steady enough that two
 * different eighths of it have the SAME envelope shape, so a shape comparison scored 0.0073 with
 * the op and 0.0076 without — the check could not fail, which is the only thing a control is for.
 * The content differs; the envelope does not, and an envelope is what a capture can compare
 * cheaply and robustly.
 *
 * So the eighths are made obviously different from each other: eighth i is a burst that occupies
 * the first (i+1)/8 of its segment and is silent after. Slice 0 is a click and a long gap; slice
 * 7 is a continuous tone. Those are different shapes by construction, and two plays of the SAME
 * slice are identical by construction — which is what makes both directions of this test
 * meaningful.
 */
{
  const RATE = 44100, SECONDS = 8, N = RATE * SECONDS, SEG = Math.floor(N / 8);
  const pcm = Buffer.alloc(44 + N * 2);
  pcm.write('RIFF', 0); pcm.writeUInt32LE(36 + N * 2, 4); pcm.write('WAVE', 8);
  pcm.write('fmt ', 12); pcm.writeUInt32LE(16, 16); pcm.writeUInt16LE(1, 20);
  pcm.writeUInt16LE(1, 22); pcm.writeUInt32LE(RATE, 24); pcm.writeUInt32LE(RATE * 2, 28);
  pcm.writeUInt16LE(2, 32); pcm.writeUInt16LE(16, 34);
  pcm.write('data', 36); pcm.writeUInt32LE(N * 2, 40);
  /*
   * CONTINUOUS, AND EACH EIGHTH A DIFFERENT PITCH.
   *
   * The first authored version made each eighth a burst followed by silence, which is different
   * per slice and useless here: a two-second note then produces INTERMITTENT audio, the
   * loud-window runs are bursts rather than notes, and the two "hits" being compared were parts
   * of the same note. The control scored HIGHER than the treatment, which is a measurement
   * reporting its own fixture.
   *
   * Continuous tone means one run per note, and pitch means the difference is in the CONTENT
   * rather than the envelope — which is what `s` changes and what a zero-crossing count can see
   * without a Fourier transform. 220 Hz to 1760 Hz across the eight, an octave and a half apart
   * at the ends.
   */
  for (let i = 0; i < N; i++) {
    const seg = Math.min(7, Math.floor(i / SEG));
    const hz = 220 * (seg + 1);
    const v = Math.sin(2 * Math.PI * hz * (i / RATE)) * 0.8;
    pcm.writeInt16LE(Math.round(v * 32767), 44 + i * 2);
  }
  writeFileSync(`${stack.dir}/break.wav`, pcm);
}
/*
 * A PROJECT MUST BE LOADED BEFORE A BARE SAMPLE NAME MEANS ANYTHING.
 *
 * `resolveSourcePath` joins a relative name with `loadedProjectDir`, and with no project loaded
 * that is empty — so `break.wav` resolved against nothing, the source decoded to zero frames,
 * and `divideEqually(0, parts)` returned no boundaries. The chop had nothing to cut and the
 * failure read as "the slice command did not work", which it did, on an empty file.
 *
 * The load reported found:true with one slot throughout, which is exactly the shape that made
 * this hard to see: every structural fact was correct and the audio was absent.
 */
const Q = 960000;
writeFileSync(`${stack.dir}/gatecut.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'gatecut', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [],
  /*
   * AN EMPTY CLIP, PLACED. Not a decoration: notes written at the cursor land in the clip under
   * it, and a track with no placement accepts every `note` command, reports `note 36`, and keeps
   * nothing. That is what happened — the tracker said it wrote two notes, the read-back held
   * none, and the silent capture looked like a broken sampler.
   *
   * Long, for the reason audible.mjs found: at 120bpm a short clip loops inside the window this
   * test calls a gap, and the repeat reads as a second hit. 240 quarters is two minutes.
   */
  clips: [{ id: 1, name: 'chop', length: Q * 240, lines_per_beat: 4, kind: 'symbolic',
            time_sig_numerator: 4, time_sig_denominator: 4, notes: [], chords: [] }],
  // EMPTY device_chain, so the sampler this test adds is device 0 — the id every command below
  // passes. The default project's track 0 already carries a chain and the sampler landed behind
  // it, which is why the first version asked an empty device for its kit.
  tracks: [{ track_id: 0, name: 'Chop', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
    device_chain: [], mod_links: [],
    placements: [{ clip_id: 1, at: 0, length: Q * 240, notes: [], chords: [], mutes: [] }] }],
}));

/*
 * When the capture WINDOW opened, which is when the engine started — not when this script did.
 * The file is written when that window closes, and killing the engine before it does skips the
 * write entirely, which reads as "the song makes no sound" and is really "nobody asked".
 */
const captureT0 = Date.now();
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 20000 }).catch(() => {});
await page.waitForTimeout(1500);

/*
 * KEEP WHAT THE COMMANDS SAY. A dock verb reports refusals by RETURNING a message, so discarding
 * the return value turns "that track does not exist" into an invisible no-op — which is how a
 * capture of silence came to be blamed on the sampler when nothing had been written at all.
 */
const said = [];
const run = async (c) => { const r = await page.evaluate((x) => window.__uni.run(x), c);
                           said.push(`${c} -> ${r}`); return r; };

await page.evaluate(() => window.__uni.loadProject('gatecut'));
await page.waitForTimeout(2500);

// BUILD THE CHOP THROUGH THE UI, exactly as the kit suite does — no daw-cli anywhere.
//
// On a FRESH track, so the sampler is device 0. The default project's track 0 already carries a
// chain, and the sampler would land at whatever id followed — which the first version assumed
// was 0 and then asked an empty device for its kit.
// Track 0 of the fixture: empty chain, one placed clip. Both matter (see the fixture above).
const T = 0;
await run(`sampler ${T}`);
await page.waitForTimeout(1500);
/*
 * LOADED ONTO KEY 60, away from where the chop will put its slices.
 *
 * Every slot this chop makes is ONE key wide — the read-back says `36-36 36-36 37-37 ... 43-43`
 * — so with the default root the whole-file slot shares key 36 with the first slice, and a note
 * there is two slots answering at once. Two such notes are not the same sound as each other, so
 * the control could not fail and the check could not mean anything.
 *
 * On 60 the keymap has exactly one answer, and the only thing that can make the second note
 * differ is its `s` op.
 */
/*
 * THE BANK DEFAULT GOES FIRST, because it SEEDS A SLOT AT MINT and does nothing afterwards.
 *
 * This is the gesture Jaakko asked for — "could that be a setting per bank, ignore note-offs" —
 * and setting it before the load is not a trick to make the test pass, it is the only order in
 * which it means anything: `load-sample` stamps the slot it creates, and a default set later
 * leaves that slot exactly as it was. Which is the property that keeps this ONE fact rather than
 * a device flag the voice re-consults on every note.
 *
 * The alternative — a loop setting `gate` on every slot the kit happens to have — is what this
 * replaces. It is also racy in a way a default is not: the kit can change between reading the
 * ids and writing them.
 */
await run(`bank ${T} 0 default-gate ${process.env.UNI_GATE === '0' ? 0 : 1}`);
await page.waitForTimeout(1200);
await page.evaluate((t) => window.__uni.loadSample(t, 0, 'break.wav', 60, true), T);
await page.waitForTimeout(2000);
await run(`slice ${T} 0 8`);
const chopped = await page.evaluate(async (t) => {
  for (let i = 0; i < 60; i++) {
    window.__uni.samplerKit(t, 0);
    const k = window.__uni.samplerKitCached(t, 0);
    if (k && k.slots && k.slots.length >= 8) return k.slots.filter((s) => s.slice > 0)
      .sort((a, b) => a.keyLow - b.keyLow).map((s) => s.keyLow);
    await new Promise((r) => setTimeout(r, 150));
  }
  return null;
}, T);
/*
 * EIGHT SLICES FROM EIGHT PARTS. It was seven for a night: `divideEqually` returned only the
 * INTERIOR boundaries, so the region from frame 0 to the first marker had no index, no id and no
 * way to be played — the DOWNBEAT of every equal division. Frame 0 is a legal marker now.
 */
check(Array.isArray(chopped) && chopped.length === 8, 'the chop is built from the UI',
      JSON.stringify(chopped));

/*
 * AN AMP ENVELOPE, WITHOUT WHICH NONE OF THIS MAKES A SOUND.
 *
 * A freshly loaded slot's default envelope produces no level, so the whole chop — right slots,
 * right keys, right slice ids, every source resolved, voices running — is mute. Measured in
 * sampler-device-id.mjs: 0.0000 across three tracks without this, 0.28-0.40 with it.
 *
 * Through the app's OWN verb — proving that shelling out to daw-cli to establish this, which is
 * how it was found, is no longer necessary. Marked clearly because the command still does not
 * belong here: loading a sample should produce something audible without one. Reported; when the
 * engine's default becomes audible this block goes and the test should still pass.
 */
{
  const said = await run(`env ${T} 0 1000 200000 1000 200000`);
  check(!/refus|not |error/i.test(String(said).split('out:').pop() || ''),
        'the chop gets an amp envelope, which it needs to sound at all',
        String(said).slice(-70));
  await page.waitForTimeout(1500);
}

/*
 * TWO NOTES, FAR APART IN THE BREAK, and a silence between them.
 *
 * Far apart because adjacent slices of a sustained probe tone may genuinely sound alike; the
 * first and the last of seven cannot be confused for the same audio unless the slot extents are
 * being ignored. The silence between is what makes the two windows separable in one capture
 * without guessing at timing.
 */
if (chopped && chopped.length === 8) {
  const early = chopped[0];
  await run('zoom 1');
  /*
   * TWO NOTES ON ONE KEY, differing only in their `s` op.
   *
   * `early` is the key the first slice sits on, so the keymap resolves BOTH notes to that slice.
   * The ops then say otherwise: the second note asks for the last slice by number. Any difference
   * in the capture is `s` doing its job, because nothing else about the two notes differs.
   */
  /*
   * GATE EVERY SLOT, then write ONE short note on the whole-file slot's key.
   *
   * Slot 0 means all of them. The note is half a second against an eight-second sample, so the
   * two outcomes are an order of magnitude apart and nothing hinges on where a threshold sits.
   */
  const KEY = 60;
  /*
   * EVERY SLOT BY ITS OWN ID. Slot 0 is not a wildcard for this command — the engine answers
   * `sampler.set_slot_rejected ... no_such_slot`, which is a log event, so the command "worked"
   * and the sound ran the full eight seconds. deviceId and modSetId ARE wildcards at 0 on the
   * neighbouring commands, which is exactly why assuming it here was easy.
   */
  /*
   * NOTHING IS SET PER SLOT HERE. The bank default above did it at mint, and that is the whole
   * claim: a person says "this kit ignores note-offs" once and every slot it makes obeys.
   *
   * So a failure below is the SEED failing, not a slot command failing — there is no slot
   * command in this run.
   */
  const DUR = 960000;                        // one quarter: half a second at 120bpm
  await run(`goto 0 ${T}`);
  await page.waitForTimeout(300);
  await run(`note ${KEY} ${DUR}`);
  await page.waitForTimeout(1200);

  const placed = await page.evaluate((t) => {
    const ns = (window.__uni.notes() || []).filter((n) => n.tr === t);
    return { n: ns.length, keys: ns.map((x) => x.p) };
  }, T);
  check(placed.n === 1 && placed.keys[0] === KEY, 'one half-second note is written',
        JSON.stringify(placed));

  /*
   * RULE OUT THE TEST BEFORE BLAMING THE PRODUCT.
   *
   * A silent capture has three cheap explanations that are not the sampler, and all three have
   * caught me before: the playback happened OUTSIDE the capture window, the transport never
   * actually advanced, or the slots exist but their source never resolved (`flags` bit 2). Each
   * would report exactly what a broken instrument reports. So measure them here, in the run that
   * makes the claim, rather than reasoning about them afterwards.
   */
  const kit = await page.evaluate((t) => (window.__uni.samplerKitCached(t, 0) || {}).slots || [], T);
  const missing = kit.filter((x) => (x.flags & 4) !== 0).length;
  check(kit.length > 0 && missing === 0, 'every slot resolved its source',
        `${missing} of ${kit.length} slots have the source-missing flag`);
  check(kit.some((x) => x.keyLow <= KEY && x.keyHigh >= KEY),
        'the key both notes share falls inside a slot key range',
        kit.map((x) => `${x.keyLow}-${x.keyHigh}`).join(' '));

  /*
   * REWIND FIRST. THE TRANSPORT STARTS WHERE THE CURSOR IS.
   *
   * The notes are written by moving the cursor to each row, so when the last one is written the
   * cursor is sitting on the LAST row of the song — and play from there skips every note before it.
   * The capture proved it: one note sounded, 0.25s after play, for exactly the 2 seconds of the
   * note that happened to be under the cursor, and the other three never played at all. Read as
   * "the sampler makes no sound" for hours.
   */
  /*
   * REWIND, THEN LET THE CAPTURE ACTUALLY START.
   *
   * Two separate things, both learned the hard way. `goto` moves the CURSOR; the playhead is a
   * different thing and stop is what rewinds it — play from the last row written skips the whole
   * song. And the tap does not begin recording when the engine starts, it begins when the AUDIO
   * DEVICE does, after the plugin hosts have launched: playing before that records nothing, and
   * the run that finally sounded differed from the silent one by this wait alone.
   */
  await page.evaluate(() => {
    const b = [...document.querySelectorAll('.ch-btn')].find((e) => /stop/i.test(e.title || ''));
    if (b) b.click();
  });
  await page.waitForTimeout(8000);

  const playedAt = (Date.now() - captureT0) / 1000;
  await page.evaluate(() => window.__uni.transport('play'));
  await page.waitForTimeout(17000);   // the second note is twelve seconds in
  const ran = await page.evaluate(() => {
    const e = window.__uni.engineState() || {};
    return { transport: e.transport, pos: e.positionTicks ?? e.position ?? null };
  });
  await page.evaluate(() => window.__uni.transport('stop'));
  const stoppedAt = (Date.now() - captureT0) / 1000;
  check(playedAt + 6 < CAPTURE_SECONDS, 'the playback happens INSIDE the capture window',
        `played at ${playedAt.toFixed(1)}s, stopped at ${stoppedAt.toFixed(1)}s, ` +
        `window is ${CAPTURE_SECONDS}s`);
  check(ran.transport === 1, 'the transport was actually running', JSON.stringify(ran));
  await page.waitForTimeout(1000);
}

await browser.close();
/*
 * WAIT FOR THE WINDOW, NOT FOR THE WORK.
 *
 * The engine writes the file when its capture window CLOSES. Stopping it first skips the write —
 * which the first run of this did, and which reads as "the song makes no sound" when it really
 * means nobody waited. So: sleep until the window is over, THEN stop, then let the write land.
 */
const elapsed = Date.now() - captureT0;
await new Promise((r) => setTimeout(r, Math.max(0, (CAPTURE_SECONDS + 3) * 1000 - elapsed)));
/*
 * THE FILE IS WRITTEN ON THE WAY OUT, AND ONLY IF THE ENGINE GETS THERE.
 *
 * `daw_engine_main.cpp` writes the WAV after the audio device has stopped — "Audio is stopped,
 * so the capture buffer is quiescent and safe to write" — which is engine SHUTDOWN, not the
 * moment the capture buffer fills. Two runs were lost to that distinction:
 *
 *   1. the check ran immediately after `stack.stop()`, before the write could land; and then
 *   2. with a wait added, `stack.stop()` was still SIGTERMing the engine at ~37s when its own
 *      `--run-seconds` was 46 — so it died before the shutdown path ever ran. The log proved
 *      it: `audio.capture_armed` with the right path and the right 34 seconds, no matching
 *      `audio.capture_written`, and a tail of "Failed to receive control header".
 *
 * So do not stop the engine — let `--run-seconds` expire and wait for the file it writes on the
 * way out. Poll rather than sleep a guessed interval, and only tear the stack down afterwards.
 */
/*
 * WAIT FOR THE FILE TO STOP GROWING, NOT FOR IT TO APPEAR.
 *
 * The engine creates the WAV and then writes it, so "the file exists" is true from the first
 * byte. Stopping the stack there kills the engine mid-write and leaves a valid, TRUNCATED
 * capture — one run held 4.7 seconds of a 50-second take, and the missing 45 seconds were where
 * all the playback was. It reads exactly like an app that made no sound, and nothing about the
 * file says it is short: the header is fine, the samples are real, there are just not many.
 *
 * So poll the SIZE until it holds still, then stop.
 */
let lastSize = -1, stable = 0;
for (let i = 0; i < 400 && stable < 3; i++) {
  await new Promise((r) => setTimeout(r, 250));
  let size = -1;
  try { size = statSync(WAV).size; } catch { size = -1; }
  if (size > 0 && size === lastSize) stable++; else stable = 0;
  lastSize = size;
}
stack.stop();

/*
 * WAS THE MACHINE KEEPING UP? ASK BEFORE BELIEVING THE SILENCE.
 *
 * A starved producer hands the device nothing, the device outputs zeros, and the capture records
 * them — so "the chop makes no sound" is what a broken sampler AND an overloaded laptop both look
 * like. The engine says which, in as many words: "Engine: audio underrun — 184 dropout
 * callback(s) in the last ~2s". At 86 callbacks a second that is every one of them.
 *
 * This cost most of a night. A run with 79 stray engine processes on the machine reported every
 * audio check as a failure of the app, including a control track running a plugin that is known
 * to work. Until this line is absent, nothing below is a statement about the app.
 */
const engineLog = (() => { try { return readFileSync(join(stack.root, 'engine.log'), 'utf8'); }
                           catch { return ''; } })();
const underruns = (engineLog.match(/audio underrun/g) || []).length;
const conclusive = underruns === 0;
/*
 * A CHECK THAT CANNOT BE ANSWERED IS NOT A CHECK THAT FAILED.
 *
 * When the producer underran, the device output silence and the capture recorded it, so every
 * question about what the app played is unanswerable on this run — not answered "no". Reporting a
 * busy machine as "the app makes no sound" is the kind of flake that teaches people to ignore a
 * suite, so those checks are BLOCKED instead, and the run still fails on the underrun itself.
 */
let blocked = 0;
const soundCheck = (ok, what, detail) => {
  if (conclusive) return check(ok, what, detail);
  blocked++;
  console.log('  BLOCK', what, '— the producer underran, so this run cannot answer it');
};

check(underruns === 0, 'the producer kept up (no underruns)',
      `${underruns} underrun report(s) — a starved producer outputs silence, so the sound checks ` +
      `below are INCONCLUSIVE rather than failing`);

if (!existsSync(WAV)) {
  check(false, 'the engine wrote a capture', `no file at ${WAV}`);
} else {
  const w = readWav(WAV);
  const s = summarise(w.mono, w.rate);
  /*
   * SOUND HAPPENED. The half that catches "the note tee was missing" — a track that renders
   * healthily and emits nothing. Peak, not average: two short one-shots in a fourteen-second
   * capture barely move a mean.
   */
  soundCheck(s.peak > 0.01, 'the chop MAKES A SOUND when the transport runs',
        `peak ${s.peak.toFixed(4)} over ${s.seconds.toFixed(1)}s`);
  soundCheck(s.loud >= 1, 'and there is at least one loud stretch', `${s.loud} slices above the floor`);

  /*
   * ...AND THE TWO SLICES ARE DIFFERENT AUDIO.
   *
   * This is the claim no read-back can make. A chop whose slots all played the WHOLE file would
   * be structurally perfect — right count, right keys, right slice ids — and musically useless.
   * Compared as ENVELOPES rather than samples: two renderings of the same region differ in
   * phase and level run to run, and an envelope difference is the coarse, robust question
   * ("is this a different part of the file") rather than the brittle one.
   */
  const env = envelope(w.mono, w.rate, 0.02);
  const loudAt = env.map((v, i) => [v, i]).filter(([v]) => v > 0.01).map(([, i]) => i);
  /*
   * HOW LONG THE SOUND LASTS, from its first loud window to its last.
   *
   * The note is half a second and the sample is eight, so a gated slot gives roughly the former
   * plus the envelope's release and an un-gated one gives the latter. Measured as a span rather
   * than as a count of runs: a release tail can dip below the floor and come back, and a check
   * that counted runs would call that two notes.
   */
  if (!loudAt.length) {
    soundCheck(false, 'the note sounds at all', 'no loud windows in the capture');
  } else {
    const seconds = (loudAt[loudAt.length - 1] - loudAt[0] + 1) * 0.02;
    console.log(`  the sound lasts ${seconds.toFixed(2)}s for a 0.50s note on an 8s sample`);
    soundCheck(seconds > 0.2, 'the note sounds at all', `${seconds.toFixed(2)}s`);
    /*
     * THE CLAIM. Two seconds is four times the note and a quarter of the sample, so it separates
     * "cut when the note ended" from "played to the end of the file" without sitting near either.
     */
    soundCheck(seconds < 2,
          'and a NOTE-OFF cuts it short — the gate is respected',
          `the sound lasts ${seconds.toFixed(2)}s for a 0.50s note on an 8-second sample; ` +
          `un-gated it runs the whole sample, which is what gate 0 means and what every slot ` +
          `made by load or slice still defaults to`);
  }
}

check(errors.length === 0, 'and nothing threw', errors.slice(0, 2).join(' | '));
const note = blocked ? ` · ${blocked} BLOCKED (the producer underran, so those questions are unanswerable on this run)` : '';
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}${note}`);
process.exit(fail === 0 ? 0 : 1);
