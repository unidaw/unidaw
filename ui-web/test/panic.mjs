/**
 * Stop twice is panic, and panic means silence.
 *
 * Asserted with AUDIO, because the claim is about sound. Stop already halts the
 * transport and flushes held keyjazz notes, so a test that checked "the
 * transport stopped" would pass without panic existing at all. What panic adds
 * is the part Stop cannot do: a plugin's own ringing voices, a sustained
 * sequencer note whose note-off has not been reached, a generator mid-phrase.
 *
 * So the measurement is the A/B, on one run: how loud the room is after Stop,
 * and how loud after the second press. Measured before this was written:
 *
 *   playing        0.119
 *   after stop     0.0196     <- the tail Stop leaves behind
 *   after panic    0.00007
 *
 * `maximal` is the fixture for it: six Zebra2 tracks, two of them driven by
 * generators, so there are real sequencer notes AND a graph mid-phrase when the
 * key is pressed.
 */

import { existsSync } from 'node:fs';
import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { readWav, rmsBetween } from './wav.mjs';

const WAV = '/tmp/panic_check.wav';
const SECS = 18;
const RATE = 44100;

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

// numBlocks: 8 for the reason audible.mjs gives — a shallower pipeline starves
// on a machine also running Chrome and six plugin hosts, and a starved run is
// silent, which here is indistinguishable from the thing being tested.
const stack = await startStack({ capture: WAV, captureSeconds: SECS,
                                 runSeconds: SECS + 6, numBlocks: 8 });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });

/*
 * PAGE ERRORS ARE A RESULT, not noise.
 *
 * This suite drove a browser and ignored whatever it threw. That is how a readout shipped
 * raising `ticksPerBeat is not defined` on every frame — a missing import in index.html, which
 * no unit test can see because none of them run the draw path, and which the suites that DO
 * listen missed because none of them visited the field that used it.
 *
 * A listener is necessary and not sufficient: it only catches what this suite's own path
 * touches. That is still strictly more than nothing, and it costs two lines.
 */
const pageErrors = [];
page.on('pageerror', (e) => { if (!pageErrors.includes(e.message)) pageErrors.push(e.message); });

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1500);
await page.evaluate(() => window.__uni.loadProject('maximal'));
// A NAMED plugin, not merely a chain card: the engine puts a stand-in on a track
// while it resolves the real one, and a stand-in makes no sound — which would
// read as "panic worked" for the wrong reason.
await page.waitForFunction(
  () => JSON.stringify(window.__uni.chainProbe() || '').includes('Zebra'),
  null, { timeout: 40000 }).catch(() => {});
await page.waitForTimeout(1500);

await page.keyboard.press(' ');
// Stamped, so the windows below are anchored to what actually happened rather than to
// an assumption about how long the setup took.
const playedAt = Date.now();
await page.waitForTimeout(5000);

await page.evaluate(() => window.__uni.run('stop'));
await page.waitForTimeout(400);
const afterStop = await page.evaluate(() => ({
  transport: (window.__uni.engineState() || {}).transport,
  said: window.__uni.state().reject,
}));
check(afterStop.transport === 0, 'the first stop halts the transport',
      String(afterStop.transport));
check(!afterStop.said, 'and says nothing — it is an ordinary stop',
      String(afterStop.said));

// The second press. Through the console, which routes to the same function the
// transport button does — one meaning of "stop", whichever way you reach it.
await page.evaluate(() => window.__uni.run('stop'));
const panickedAt = Date.now();
await page.waitForTimeout(400);
const afterPanic = await page.evaluate(() => window.__uni.state().reject);
check(/panic/i.test(String(afterPanic)), 'the second press says it panicked',
      String(afterPanic));

await page.waitForTimeout((SECS + 7) * 1000);
check(pageErrors.length === 0, 'nothing threw in the browser while this ran',
      pageErrors.slice(0, 3).join(' | '));

await browser.close();
stack.stop();

if (!existsSync(WAV)) {
  check(false, 'a capture was produced', WAV);
} else {
  /*
   * THE WINDOWS COME FROM THE HARNESS, not from arithmetic about how long setup took.
   *
   * This file used to hand-roll its own WAV reader and place its windows at fixed
   * offsets — rms(2,5) for "playing", rms(9,13) for "after panic" — which only
   * lined up because plugin scanning happened to take about as long as it did on the
   * machine that wrote them. Setup time here is dominated by how long the audio
   * DEVICE takes to open: milliseconds on the built-in output, seconds on a Bluetooth
   * speaker. A window two seconds out reads the wrong phase.
   *
   * `stack.captureOffset` turns a wall-clock instant into an offset into the file,
   * because both ends of the ring are knowable: the last sample is `runSeconds` after
   * the device opened, and the first is `captureSeconds` before that. And `readWav`
   * is the shared reader, so a change to the engine's capture format does not have to
   * be found in two places.
   */
  const { mono, rate } = readWav(WAV);
  const at = (ms) => stack.captureOffset(ms);
  const playing = rmsBetween(mono, rate, at(playedAt + 1500), at(playedAt + 4500));
  const stopped = rmsBetween(mono, rate, at(panickedAt + 2000), at(panickedAt + 6000));
  console.log(`  playing ${playing.toFixed(5)}   after panic ${stopped.toFixed(5)}` +
              `   (capture ${(mono.length / rate).toFixed(1)}s,` +
              ` windows ${at(playedAt + 1500).toFixed(1)}s and` +
              ` ${at(panickedAt + 2000).toFixed(1)}s)`);
  /*
   * SOMETHING WAS SOUNDING FIRST. Without this the whole suite passes on a run where
   * the plugins never loaded — silence before and silence after, which is the failure
   * mode every audio test has to be built against.
   */
  check(playing > 0.01, 'the song was actually playing', playing.toFixed(5));
  check(stopped < 0.001, 'and panic left silence', stopped.toFixed(5));
  check(playing / Math.max(stopped, 1e-9) > 50,
        'by a margin no decay accounts for',
        `${(playing / Math.max(stopped, 1e-9)).toFixed(0)}x`);
}

console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
