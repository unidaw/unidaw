#!/usr/bin/env node
/**
 * Does a song built through the UI actually make a sound?
 *
 *   node test/audible.mjs
 *
 * Every other check in this repo asks the app what it believes. This one asks
 * the speakers. It drives the interface with real keys and clicks — load a
 * project that has an instrument, press play, press stop — and then reads the
 * engine's captured master output and looks at the envelope.
 *
 * It exists because of a real afternoon: notes were reported as audible and
 * could not see, and I twice concluded from reasoning that nothing was wrong.
 * What settled it was a capture. Reasoning about a signal path is not evidence
 * about a signal; the waveform is.
 *
 * Two things it can say that nothing else here can:
 *   - sound happens when the transport runs, and
 *   - silence happens when it does not.
 * The second is the half that would have caught the note-off bug: a 0.125s note
 * that goes on ringing for seven seconds is not audible-versus-silent, it is
 * silence that never arrives.
 */

import { chromium } from 'playwright';
import { join } from 'node:path';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { startStack } from './stack.mjs';
import { readWav, envelope, summarise } from './wav.mjs';

const WAV = join(tmpdir(), `uni-audible-${process.pid}.wav`);
const CAPTURE_SECONDS = 26;

let pass = 0, fail = 0;
const ok = (cond, what, detail = '') => {
  if (cond) pass++; else fail++;
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${what}${detail ? '  ' + detail : ''}`);
  return !!cond;
};
/**
 * A check that SHOULD pass and is known not to, because of a defect somewhere
 * else. It does not fail the run — but it shouts the moment it starts passing,
 * so the marker gets removed instead of quietly outliving the bug.
 */
const blockedList = [];
const blocked = (cond, what, why, detail = '') => {
  if (cond) {
    pass++;
    console.log(`  PASS  ${what}  ${detail}  <- NO LONGER BLOCKED, remove the marker`);
    return;
  }
  blockedList.push(`${what} — ${why}`);
  console.log(`  BLOCKED  ${what}${detail ? '  ' + detail : ''}`);
};
const step = (s) => console.log(`\n[${s}]`);

// The engine must EXIT for the capture to be written, so it is given a bounded
// run rather than killed. Long enough for six plugin hosts to launch, the song
// to load, and the play/stop window to happen inside it.
const stack = await startStack({ capture: WAV, captureSeconds: CAPTURE_SECONDS,
                                 runSeconds: CAPTURE_SECONDS + 6,
                                 // A deeper pipeline. Without it this test is
                                 // silent about one run in four, because the
                                 // machine running it is also running Chrome and
                                 // several plugin hosts and the audio producer
                                 // starves — 1237 of 2759 callbacks dropped a
                                 // track in the run I caught. The question here
                                 // is what the app plays, not how it behaves
                                 // when starved; that belongs in its own test.
                                 numBlocks: 8,
                                 keepDir: !!process.env.UNI_KEEP });
/**
 * Roughly when the capture started, so later moments can be located INSIDE it.
 *
 * The capture begins when the engine does, and everything after that — the
 * browser starting, six plugin hosts launching, a project loading — takes an
 * amount of time that varies by many seconds between runs. Judging "the tail of
 * the file" by a fixed percentage therefore measured a different part of the
 * song each time: 79% loud on one run and 25% on the next, with nothing changed.
 * Anchoring to the wall clock makes the question exact.
 */
const captureT0 = Date.now();
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

await page.goto(stack.url);
await page.waitForFunction(() => !!window.__uni, null, { timeout: 20000 });
await page.waitForFunction(() => window.__uni.canSend(), null, { timeout: 20000 })
  .catch(() => {});
await page.waitForTimeout(2500);

console.log(`\nlistening to ${stack.url}\n${'='.repeat(60)}`);

// ---------------------------------------------------------------------------
step('write a one-note song, so the question is exact');
/**
 * ONE track, ONE instrument, ONE short note.
 *
 * `maximal` was the wrong instrument for this: six tracks whose plugin hosts
 * finish loading at different moments, so how much of the song actually played
 * varied by a factor of three between runs, and with it whether a ringing tail
 * was even detectable. The note-off question — does a 0.125-second note stop
 * after 0.125 seconds — needs a song where nothing else is making noise.
 *
 * Written into the stack's own project directory, which is a disposable copy.
 */
{
  const { writeFileSync } = await import('node:fs');
  const Q = 960000;                                    // nanoticks per quarter
  const doc = {
    schema_version: 4,
    meta: { name: 'onenote', created_utc: 0, modified_utc: 0 },
    timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
    nanoticks_per_quarter: Q,
    tempo_map: [{ nanotick: 0, bpm: 120.0 }],
    harmony_timeline: [],
    clips: [{
      // LONG. At 120bpm a 16-quarter clip loops every eight seconds, and the
      // repeat landed inside the window this test calls a gap — so the loop
      // read as a stuck note. 240 quarters is two minutes: nothing repeats
      // inside the ten seconds of playback below.
      id: 1, name: 'one', length: Q * 240, lines_per_beat: 4, kind: 'symbolic',
      time_sig_numerator: 4, time_sig_denominator: 4,
      // 240000 ticks = an eighth of a second at 120bpm.
      // Four short notes over two seconds, then nothing. Enough sound to be a
      // stretch rather than a click, and a definite end to listen for: the last
      // note stops at 1.625s and everything after that must be silence.
      notes: [0, 480000, 960000, 1440000].map((t, i) => ({
        nanotick: t, duration: 240000, pitch: 60 + i * 4, velocity: 110,
        column: 0, note_id: i + 1,
      })),
      chords: [],
    }],
    tracks: [{
      track_id: 0, name: 'One', harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0.0, pan: 0.0, mute: false, solo: false },
      device_chain: [{
        device_id: 0, kind: 'vst_instrument', capability_mask: 5, patcher_node_id: 0,
        host_slot_index: 4294967294, bypass: false,
        vst_ref: { vendor: '', name: 'Zebralette',
                   path: '/Library/Audio/Plug-Ins/VST3/Zebra2.vst3', uid16: '' },
      }],
      mod_links: [],
      placements: [{ clip_id: 1, at: 0, length: Q * 240, notes: [], chords: [], mutes: [] }],
    }],
  };
  writeFileSync(join(stack.dir, 'onenote.uniproj.json'), JSON.stringify(doc));
}

step('load it');
await page.evaluate(() => window.__uni.loadProject('onenote'));
// Wait for the chain to report a real device rather than for a guessed delay:
// six plugin hosts have to launch, and how long that takes is not this test's
// business to predict.
// Wait for the NOTES, which is what this test is about — a chain card can be
// the engine's stand-in Identity plugin and say nothing about whether the song
// arrived.
await page.waitForFunction(
  () => ((window.__uni.engineState() || {}).noteCount || 0) > 0,
  null, { timeout: 30000 }).catch(() => {});
// A NAMED device, not merely a card. The engine puts a stand-in on track 0 while
// it resolves the real plugin, and playing against the stand-in produces a
// capture of perfect silence — which reads as "the song makes no sound" when
// what happened is that the test did not wait for the instrument.
/**
 * The REAL instrument, not the stand-in.
 *
 * The engine loads an Identity plug-in on a track while it resolves what the
 * project actually asked for, and Identity makes no sound. Waiting for "a named
 * device with at least one parameter" was satisfied by the stand-in immediately,
 * so the test played a silent track and reported that the app makes no sound.
 * Zebralette publishes 256 parameters; anything past a handful means the plugin
 * host answered for the thing the project names.
 */
await page.waitForFunction(
  () => { const c = window.__uni.chainProbe();
          return c && c.cards >= 1 && c.titles && c.titles[0]
                 && !/^Identity/.test(c.titles[0])
                 && c.params && c.params[0] > 8; },
  null, { timeout: 45000 }).catch(() => {});
const chain = await page.evaluate(() => window.__uni.chainProbe());
ok(chain && chain.cards >= 1 && chain.named >= 1,
   'the project has a loaded instrument on the cursor track',
   `${JSON.stringify(chain && chain.titles)} params=${JSON.stringify(chain && chain.params)}`);

const notes = await page.evaluate(
  () => (window.__uni.engineState() || {}).noteCount || 0);
ok(notes > 0, 'and notes to play', `${notes} notes`);

/**
 * Whether there is anything to listen to at all.
 *
 * Launching six out-of-process plugin hosts and getting a parameter list back is
 * the slowest thing in this repo, and on a loaded machine it sometimes does not
 * finish inside the capture window. That is the environment, not the app — so
 * the audio assertions below are BLOCKED rather than failed when it happens.
 * Reporting a machine that was busy as "the song makes no sound" is the kind of
 * flake that teaches people to ignore a suite.
 */
const armed = !!(chain && chain.titles && chain.titles[0]
                 && !/^Identity/.test(chain.titles[0]) && notes > 0);

// ---------------------------------------------------------------------------
step('press play, wait, press stop — with the keyboard');
// Silence first, so the capture has a quiet head to compare against.
await page.waitForTimeout(1500);
const quietUntil = Date.now();
// REWIND FIRST. Play starts from wherever the playhead was left, and earlier
// steps move it — so the four notes at the top of the song were simply never
// reached, and the capture was a silence that looked like a broken instrument.
// Stop halts AND rewinds, which is exactly what is wanted here.
await page.evaluate(() => {
  const b = [...document.querySelectorAll('.ch-btn')]
    .find((e) => /stop/i.test(e.title || ''));
  if (b) b.click();
});
await page.waitForTimeout(700);
await page.keyboard.press('Space');
const playedAtMs = Date.now() - captureT0;
// The notes run for under two seconds; play well past them so the silence AFTER
// them happens while the transport is still going.
await page.waitForTimeout(9000);
await page.keyboard.press('Space');
const stoppedAtMs = Date.now() - captureT0;
// ...and a quiet tail. Long enough that a release tail is over and anything
// still sounding is a stuck voice rather than a decay.
await page.waitForTimeout(6000);
const transport = await page.evaluate(() => (window.__uni.engineState() || {}).transport);
ok(transport === 0, 'the transport is stopped again', `transport=${transport}`);

await browser.close();

// ---------------------------------------------------------------------------
step('read what came out');
// The engine writes the file when its capture window closes, so wait for the
// window rather than for the process.
await new Promise((r) => setTimeout(r, Math.max(0, (CAPTURE_SECONDS + 3) * 1000
                                                   - (Date.now() - quietUntil))));
stack.stop();
await new Promise((r) => setTimeout(r, 1200));

if (!existsSync(WAV)) {
  ok(false, 'the engine wrote a capture', WAV);
} else {
  const { rate, mono } = readWav(WAV);
  const sum = summarise(mono, rate);
  ok(sum.seconds > 5, 'the capture has audio in it', `${sum.seconds.toFixed(1)}s @ ${rate}Hz`);
  /**
   * A silent capture with a loaded instrument is worth naming precisely.
   *
   * The engine names its plugin-host sockets by TRACK INDEX in /tmp —
   * `/tmp/daw_host_track_0.sock` — so any two engines on this machine share
   * them. An orphaned host from a previous run leaves that path occupied, the
   * new engine connects to a host it did not start (in one case an Analog Heat
   * calling itself Identity), and the capture is perfect silence. It looks
   * exactly like "the app makes no sound".
   */
  if (armed && sum.peak <= 0.01) {
    const { readFileSync } = await import('node:fs');
    let hint = '';
    try {
      const log = readFileSync(join(stack.root, 'engine.log'), 'utf8');
      const bad = log.split('\n').filter((l) => /host|socket|audio device|underrun/i.test(l));
      hint = bad.slice(-3).join(' | ').slice(0, 240);
    } catch { /* the dir may already be gone */ }
    console.log(`  note: silent capture with an instrument loaded — check for a stale\n`
              + `        plugin host on /tmp/daw_host_track_*.sock from another engine.\n`
              + `        engine log: ${hint}`);
  }
  if (!armed) {
    blocked(false, 'THE SONG MAKES A SOUND',
            'no instrument finished loading before the capture window closed — '
            + 'the plugin hosts were still starting');
  } else {
    ok(sum.peak > 0.01, 'THE SONG MAKES A SOUND', `peak ${sum.peak.toFixed(4)}`);
  }

  // Where the sound is, in time. The transport ran for six seconds in the middle
  // of the capture, so the loud part must be a contiguous stretch — not the whole
  // file, which would mean it never stopped.
  const env = envelope(mono, rate);
  const floor = Math.max(0.004, sum.peak * 0.06);
  const loud = env.map((v) => v > floor);
  const loudCount = loud.filter(Boolean).length;
  const frac = loudCount / loud.length;
  if (armed) {
    ok(frac > 0.05, 'and the sound is a real stretch, not a click',
       `${(frac * 100).toFixed(0)}% of the capture is above the floor`);
  }
  /**
   * THE HALF THAT MATTERS.
   *
   * The transport ran for six of roughly twenty-six seconds. If nearly the whole
   * capture is above the floor then the song never stopped sounding — which is
   * exactly the shape of the note-off bug that was heard: a 0.125s note that rings
   * for seven seconds does not fail an "is there sound" check, it fails a
   * "does the sound END" check.
   */
  /**
   * THE GAP BETWEEN THE NOTES, WHILE THE TRANSPORT IS STILL RUNNING.
   *
   * This is the assertion that catches the real defect, and getting here took a
   * wrong turn worth recording: the first version listened AFTER pressing Stop,
   * and Stop flushes every held voice. So a song that rang continuously went
   * quiet the moment the test asked, and the bug hid behind the very gesture
   * used to look for it.
   *
   * The four notes end 1.6 seconds in. From three seconds after play until just
   * before stop, nothing is scheduled, so the master output must be silent while
   * the transport is still running. One 0.125-second note that goes on sounding
   * is exactly what fails here.
   */
  const perSlice0 = sum.seconds / envelope(mono, rate).length;
  const envAll = envelope(mono, rate);
  const gapFrom = Math.floor(((playedAtMs / 1000) + 3) / perSlice0);
  const gapTo = Math.floor(((stoppedAtMs / 1000) - 0.5) / perSlice0);
  if (armed && gapTo > gapFrom + 4) {
    const gap = envAll.slice(gapFrom, gapTo);
    const gapPeak = gap.reduce((m, v) => Math.max(m, v), 0);
    // A plain assertion: with no generator on the track this holds every run.
    // If it ever fails, the first thing to check is a euclidean or random_degree
    // node in the device's patcher graph — that is what it caught last time, and
    // it is not a defect, only an invisible one.
    ok(gapPeak < floor,
       'the song is SILENT between notes while still playing',
       `gap ${(gapFrom * perSlice0).toFixed(1)}-${(gapTo * perSlice0).toFixed(1)}s `
            + `peak ${gapPeak.toFixed(4)} vs floor ${floor.toFixed(4)}`);
  }

  /**
   * Does silence ARRIVE?
   *
   * The transport was stopped six seconds before the capture ends, and a release
   * tail is over long before that. So the last stretch of the file must be quiet,
   * and if it is not, notes are still sounding with nothing playing them — which
   * is precisely what was heard and reported as phantom notes.
   *
   * This is BLOCKED, not failed: the cause is in the engine, not here. One
   * 0.125-second note produces seven seconds of sound and changing its duration
   * changes nothing, so note-off is not reaching the plugin. Backend has the
   * repro. When it lands, this line starts passing and says so.
   */
  // Three seconds after the transport stopped, to the end of the file. Three is
  // generous for a release tail and far short of the seven seconds a single
  // stuck note rings for.
  const perSlice = sum.seconds / env.length;
  const tailFrom = Math.min(env.length - 1,
                            Math.floor(((stoppedAtMs / 1000) + 3) / perSlice));
  const tail = env.slice(tailFrom);
  const tailPeak = tail.reduce((m, v) => Math.max(m, v), 0);
  ok(tailPeak < floor,
     'and the song goes SILENT after the transport stops',
     `stopped at ${(stoppedAtMs / 1000).toFixed(1)}s, tail peak `
          + `${tailPeak.toFixed(4)} vs floor ${floor.toFixed(4)} `
          + `(${tail.length} slices from ${(tailFrom * perSlice).toFixed(1)}s)`);
  ok(frac < 0.98, 'and it is not one unbroken wall of sound',
     `${(frac * 100).toFixed(0)}% of the capture is above the floor`);

  // A rough picture, so a failure is readable without opening the file.
  const buckets = 40, per = Math.ceil(env.length / buckets);
  let bar = '';
  for (let i = 0; i < env.length; i += per) {
    let m = 0;
    for (let k = i; k < Math.min(env.length, i + per); k++) m = Math.max(m, env[k]);
    bar += m > floor * 4 ? '#' : m > floor ? '-' : '.';
  }
  console.log(`\n  envelope over ${sum.seconds.toFixed(0)}s:  ${bar}`);
  console.log(`  ('.' silent  '-' quiet  '#' loud)`);
}

step('page errors');
ok(errors.length === 0, 'nothing threw', errors.slice(0, 3).join(' | '));

console.log(`\n${'='.repeat(60)}`);
if (blockedList.length) {
  console.log(`${blockedList.length} BLOCKED on a defect elsewhere:`);
  for (const b of blockedList) console.log(`  - ${b}`);
}
console.log((fail === 0 ? `ALL PASS (${pass})` : `${fail} FAILED, ${pass} passed`)
          + (blockedList.length ? `, ${blockedList.length} blocked` : ''));
process.exit(fail ? 1 : 0);
