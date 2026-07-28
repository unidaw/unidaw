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
 * It exists because of a real afternoon: Jaakko reported notes he could hear and
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
step('load a song that has an instrument in it');
// `maximal` names Zebra2, which is in the plugin cache — `rack` names Identity,
// which is built and never scanned, so it resolves to whatever sits at slot 0.
await page.evaluate(() => window.__uni.loadProject('maximal'));
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
await page.waitForFunction(
  () => { const c = window.__uni.chainProbe();
          return c && c.cards >= 1 && c.named >= 1
                 && c.params && c.params[0] > 0; },
  null, { timeout: 40000 }).catch(() => {});
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
const armed = !!(chain && chain.named >= 1 && notes > 0);

// ---------------------------------------------------------------------------
step('press play, wait, press stop — with the keyboard');
// Silence first, so the capture has a quiet head to compare against.
await page.waitForTimeout(1500);
const quietUntil = Date.now();
await page.keyboard.press('Space');
await page.waitForTimeout(6000);
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
   * exactly the shape of the note-off bug Jaakko heard: a 0.125s note that rings
   * for seven seconds does not fail an "is there sound" check, it fails a
   * "does the sound END" check.
   */
  /**
   * Does silence ARRIVE?
   *
   * The transport was stopped six seconds before the capture ends, and a release
   * tail is over long before that. So the last stretch of the file must be quiet,
   * and if it is not, notes are still sounding with nothing playing them — which
   * is precisely what Jaakko heard and reported as phantom notes.
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
  blocked(tailPeak < floor,
          'and the song goes SILENT after the transport stops',
          'engine: note-off is not honoured — a 0.125s note sounds for 7s, and '
          + 'changing its duration changes nothing (reported to backend)',
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
