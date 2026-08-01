/**
 * Bypass makes the device stop.
 *
 * The e2e suite proves the button and the `b` key put the state into the ENGINE's
 * chain snapshot, and that is where a check like this normally stops. It should
 * not: the snapshot is the engine agreeing it wrote the flag down, and the claim
 * is about sound. Every failure this project has had in the rack has been of
 * exactly that shape — a value that round-trips perfectly and changes nothing you
 * can hear. patchcfg.mjs found four knobs like that on one node.
 *
 * So, three phases against one capture, bypassing the INSTRUMENT:
 *
 *   A  playing            -> sound       (non-vacuity)
 *   B  instrument bypassed -> silence
 *   C  un-bypassed         -> sound again
 *
 * The instrument, because bypass has a specific meaning the host implements —
 * pass the input through instead of processing — and an instrument has no audio
 * input, so a correctly bypassed one is silence rather than "quieter". C is what
 * separates "bypass works" from "something broke": the sound has to come back.
 *
 * NOT level-matched, and that is worth knowing before reading the numbers.
 * Backend's 15c scales a bypassed insert's passthrough by the gain the insert was
 * measured to apply, so an EFFECT's bypass compares tone rather than loudness.
 * There is nothing to scale here — the passthrough is silence either way.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { readWav, rmsBetween } from './wav.mjs';

const WAV = '/tmp/bypass_check.wav';
const RUN = 62;
const KEEP = 48;
const PHASE = 9_000;
const PLAY_AT = 24_000;

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ capture: WAV, captureSeconds: KEEP, runSeconds: RUN,
                                 numBlocks: 8 });
const t0 = stack.audioStartedAt;
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
await page.waitForTimeout(1000);
await page.evaluate(() => window.__uni.loadProject('generator'));
// A NAMED plugin: a stand-in makes no sound, which would read as "bypass worked"
// in all three phases.
await page.waitForFunction(
  () => JSON.stringify(window.__uni.chainProbe() || '').includes('Zebra'),
  null, { timeout: 30000 }).catch(() => {});

/** The instrument on track 0, by KIND — device ids are not positions. */
const instrument = await page.evaluate(() => {
  const c = window.__uni.chains()['0'];
  const d = (c && c.devices || []).find((x) => x.kind === 3);
  return d ? d.id : null;
});
check(instrument !== null, 'the track has an instrument to switch off',
      String(instrument));
if (instrument === null) { await browser.close(); stack.stop(); process.exit(1); }

/** Set bypass and wait for the ENGINE to publish it back. */
const setBypass = (device, on) => page.evaluate(({ device, on }) => {
  window.__uni.bypass(0, device, on);
  const at = () => {
    const c = window.__uni.chains()['0'];
    const d = (c && c.devices || []).find((x) => x.id === device);
    return d ? !!d.bypass : null;
  };
  return new Promise((resolve) => {
    const started = Date.now();
    const poll = () => {
      if (at() === on || Date.now() - started > 4000) return resolve(at());
      setTimeout(poll, 40);
    };
    poll();
  });
}, { device, on });

const waitUntil = async (ms) => {
  const left = t0 + ms - Date.now();
  if (left > 0) await page.waitForTimeout(left);
};

const ready = Date.now() - t0;
if (ready > PLAY_AT - 2000) {
  check(false, 'setup finished in time to place the phases', `${(ready / 1000).toFixed(1)}s`);
}

await waitUntil(PLAY_AT - 1500);
await page.keyboard.press(' ');
const aAt = PLAY_AT;

await waitUntil(aAt + PHASE);
const bAt = aAt + PHASE;
check(await setBypass(instrument, true) === true,
      'the engine has the instrument bypassed');

await waitUntil(bAt + PHASE);
const cAt = bAt + PHASE;
check(await setBypass(instrument, false) === false, 'and back on');

await waitUntil(cAt + PHASE - 500);
check(pageErrors.length === 0, 'nothing threw in the browser while this ran',
      pageErrors.slice(0, 3).join(' | '));

await browser.close();
await new Promise((r) => setTimeout(r, Math.max(0, t0 + RUN * 1000 + 2500 - Date.now())));
stack.stop();

const at = (ms) => stack.captureOffset(t0 + ms);

/*
 * THE SOUND HALF IS ONLY ANSWERABLE IF THE DEVICE RAN.
 *
 * `stack.audioRunning` is the engine's own verdict at the device boundary: it is true only
 * once a callback has actually landed. A device that opens and never pulls a block produces
 * no capture, and every question below is then unanswerable rather than answered "no".
 *
 * That signal is INDEPENDENT of what these checks test — it says nothing about whether
 * bypass works, only whether this run could observe it — which is the whole condition for
 * an excuse being safe rather than a way of not looking.
 *
 * This suite reported ALL PASS for 38 hours without it, on a capture file from a previous
 * run that nothing deleted. The stack deletes it now, so the choice is a real answer or an
 * honest blank.
 */
let blocked = 0;
const soundCheck = (ok, what, detail) => {
  if (stack.audioRunning) return check(ok, what, detail);
  blocked++;
  console.log('  BLOCK', what, '— the audio device never started, so this run cannot answer it');
};

let wav = null;
if (!stack.audioRunning) {
  soundCheck(false, 'a capture was produced');
} else {
  try { wav = readWav(WAV); } catch (e) { check(false, 'a capture was produced', e.message); }
}

if (wav) {
  const { mono, rate } = wav;
  // 4s of guard at the front of each window: a synth's release is not the synth
  // still playing, and cutting it fine would fail on a patch with a long tail.
  const level = (start) => rmsBetween(mono, rate, at(start + 4000), at(start + PHASE - 500));
  const [a, b, c] = [aAt, bAt, cAt].map(level);
  console.log(`  playing ${a.toFixed(5)}   bypassed ${b.toFixed(5)}` +
              `   back ${c.toFixed(5)}   (capture ${(mono.length / rate).toFixed(1)}s)`);

  soundCheck(a > 0.005, 'the instrument was sounding to begin with', a.toFixed(5));
  soundCheck(b < 0.0005, 'bypassing it left silence', b.toFixed(5));
  soundCheck(c > 0.005, 'and switching it back on brought it back', c.toFixed(5));
  soundCheck(c > b * 20, 'by a margin no decay accounts for',
        `${(c / Math.max(b, 1e-9)).toFixed(0)}x`);
}

/*
 * A BLOCKED RUN IS NOT A PASS, and it says so in the banner rather than in a line above it
 * that scrolls away. "ALL PASS" on a run that could not hear anything is how a suite stops
 * being read.
 */
const note = blocked ? ` · ${blocked} BLOCKED (the audio device never started — see daw_audio_probe)` : '';
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)${note}`
                            : `${fail} of ${pass + fail} FAILED${note}`}`);
process.exit(fail === 0 ? 0 : 1);
