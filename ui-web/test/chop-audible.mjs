#!/usr/bin/env node
/**
 * DOES A CHOP MADE FROM THE UI ACTUALLY SOUND — and does slice 3 differ from slice 6?
 *
 * Every other check on the sampler asks the engine what it believes: the kit read-back says the
 * slots exist, name their slices, sit on consecutive keys. This one asks the speakers.
 *
 * It exists because tonight turned up three separate "reports healthy, produces silence" bugs,
 * none of which any structural check could have seen:
 *   - a patcher could not drive the sampler at all, because the note tee was only on the clip
 *     path. Rendered, the track was silent while the sampler reported a perfectly healthy render.
 *   - pan envelopes were started, released and never evaluated; every LFO in the file format was
 *     rendered by nothing. Two of ten modulation combinations reached the audio.
 *   - `modSet.filterType` is written by no site in the engine, so every cutoff modulator is inert
 *     by construction.
 *
 * Reasoning about a signal path is not evidence about a signal. The waveform is.
 *
 * WHAT IT CAN SAY THAT THE READ-BACK CANNOT. A chop whose slots all played the WHOLE file would
 * be structurally perfect — right count, right keys, right slice ids — and musically useless,
 * and the kit view could not tell you, because `lengthFrames` publishes the source's length for
 * every slot. Two different keys producing two different sounds is the claim, and it is only
 * answerable here.
 */

import { chromium } from 'playwright';
import { join, resolve } from 'node:path';
import { copyFileSync, existsSync, writeFileSync, readFileSync } from 'node:fs';
import { startStack } from './stack.mjs';
import { readWav, envelope, summarise } from './wav.mjs';

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
const WAV = join(process.cwd(), 'ui-web/test/out/chop-audible.wav');

// The engine must EXIT for the capture to be written, so it gets a bounded run rather than a
// kill — the first version of the audio test killed the engine and then looked for a file that
// was never going to exist.
const stack = await startStack({ keepDir: true, capture: WAV, captureSeconds: CAPTURE_SECONDS,
                                 runSeconds: CAPTURE_SECONDS + 12, numBlocks: 8 });
copyFileSync(resolve('presets/audio/waveform_probe.wav'), `${stack.dir}/break.wav`);
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
writeFileSync(`${stack.dir}/chop.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'chop', created_utc: 0, modified_utc: 0 },
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

await page.evaluate(() => window.__uni.loadProject('chop'));
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
await run(`load-sample ${T} 0 break.wav`);
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
check(Array.isArray(chopped) && chopped.length === 7, 'the chop is built from the UI',
      JSON.stringify(chopped));

/*
 * TWO NOTES, FAR APART IN THE BREAK, and a silence between them.
 *
 * Far apart because adjacent slices of a sustained probe tone may genuinely sound alike; the
 * first and the last of seven cannot be confused for the same audio unless the slot extents are
 * being ignored. The silence between is what makes the two windows separable in one capture
 * without guessing at timing.
 */
if (chopped && chopped.length === 7) {
  const early = chopped[0], late = chopped[6];
  await run('zoom 1');
  await run(`goto 0 ${T}`);
  await page.waitForTimeout(300);
  await run(`note ${early}`);
  await page.waitForTimeout(600);
  await run(`goto 16 ${T}`);
  await page.waitForTimeout(300);
  await run(`note ${late}`);
  await page.waitForTimeout(1200);
  /*
   * READ THE NOTES BACK. The first version asserted `check(true, ...)` here, which is a pass that
   * cannot fail — it reported "notes written" whether or not anything had been written, and a
   * silent capture then looked like a broken instrument rather than an empty track.
   */
  const placed = await page.evaluate((t) => {
    /*
     * THE PUBLISHED NOTE IS `tr`/`p`, NOT `track`/`pitch`.
     *
     * The first version filtered on `track`/`pitch` — the shape the piano FIXTURE builds — and
     * got zero from a read-back that held both notes. An unread key does not throw, it is
     * undefined, so the filter quietly matched nothing and the report was "the tracker wrote no
     * notes" about a tracker that had written them correctly. Read the names off the read-back.
     */
    const all = window.__uni.notes() || [];
    const ns = all.filter((n) => n.tr === t);
    return { n: ns.length, keys: ns.map((x) => x.p).sort((a, b) => a - b) };
  }, T);
  check(placed.n === 2 && placed.keys[0] === early && placed.keys[1] === late,
        `notes written for slice 1 (key ${early}) and slice 7 (key ${late})`,
        JSON.stringify(placed) + ' | ' + said.slice(-4).join(' ; '));

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
  check(kit.some((x) => x.keyLow <= early && x.keyHigh >= early) &&
        kit.some((x) => x.keyLow <= late && x.keyHigh >= late),
        'the two keys fall inside slot key ranges',
        kit.map((x) => `${x.keyLow}-${x.keyHigh}`).join(' '));

  const playedAt = (Date.now() - captureT0) / 1000;
  await page.evaluate(() => window.__uni.transport('play'));
  await page.waitForTimeout(6000);
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
for (let i = 0; i < 240 && !existsSync(WAV); i++) {
  await new Promise((r) => setTimeout(r, 250));
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
  check(s.peak > 0.01, 'the chop MAKES A SOUND when the transport runs',
        `peak ${s.peak.toFixed(4)} over ${s.seconds.toFixed(1)}s`);
  check(s.loud >= 1, 'and there is at least one loud stretch', `${s.loud} slices above the floor`);

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
  if (loudAt.length < 2) {
    check(false, 'two separate hits are visible in the capture', `${loudAt.length} loud windows`);
  } else {
    // The two hits, taken as the first and last loud windows with a gap between them.
    const first = loudAt[0], last = loudAt[loudAt.length - 1];
    check(last - first > 5, 'the two hits are separated in time',
          `windows ${first} and ${last}`);
    const grab = (at) => env.slice(at, at + 8);
    const a = grab(first), b = grab(last);
    const diff = a.reduce((acc, v, i) => acc + Math.abs(v - (b[i] || 0)), 0) / a.length;
    const scale = Math.max(...a, ...b, 1e-9);
    check(diff / scale > 0.05,
          'and slice 1 and slice 7 are DIFFERENT audio — a chop whose slots all played the '
          + 'whole file would be structurally perfect and musically useless',
          `mean envelope difference ${(diff / scale).toFixed(3)} of peak`);
  }
}

check(errors.length === 0, 'and nothing threw', errors.slice(0, 2).join(' | '));
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
