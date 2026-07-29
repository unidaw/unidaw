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

import { readFileSync, existsSync } from 'node:fs';
import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

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
await page.waitForTimeout(400);
const afterPanic = await page.evaluate(() => window.__uni.state().reject);
check(/panic/i.test(String(afterPanic)), 'the second press says it panicked',
      String(afterPanic));

await page.waitForTimeout((SECS + 7) * 1000);
await browser.close();
stack.stop();

if (!existsSync(WAV)) {
  check(false, 'a capture was produced', WAV);
} else {
  const buf = readFileSync(WAV);
  const at = buf.indexOf('data');
  /** Loudness over a window of the capture, in seconds. */
  const rms = (from, to) => {
    let sum = 0, n = 0;
    const a = at + 8 + Math.round(from * RATE) * 4;
    const b = Math.min(buf.length - 1, at + 8 + Math.round(to * RATE) * 4);
    for (let i = a; i < b; i += 2) { const v = buf.readInt16LE(i) / 32768; sum += v * v; n++; }
    return n ? Math.sqrt(sum / n) : 0;
  };
  const playing = rms(2, 5);
  const stopped = rms(9, 13);
  console.log(`  playing ${playing.toFixed(5)}   after panic ${stopped.toFixed(5)}`);
  /*
   * SOMETHING WAS SOUNDING FIRST. Without this the whole suite passes on a run
   * where the plugins never loaded — silence before and silence after, which is
   * the failure mode every audio test has to be built against.
   */
  check(playing > 0.01, 'the song was actually playing', playing.toFixed(5));
  check(stopped < 0.001, 'and panic left silence', stopped.toFixed(5));
  check(playing / Math.max(stopped, 1e-9) > 50,
        'by a margin no decay accounts for',
        `${(playing / Math.max(stopped, 1e-9)).toFixed(0)}x`);
}

console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
