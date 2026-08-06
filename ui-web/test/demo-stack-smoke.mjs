#!/usr/bin/env node
/**
 * IS THE DEMO'S OWN STACK ALIVE? — run this against a RUNNING `tools/webstack.sh`.
 *
 * Every other suite here boots its own stack through `stack.mjs`: a temp copy of presets/, its own
 * ports, its own SHM name. That is right for tests and it means NOTHING in the sweep ever touches
 * `tools/webstack.sh`, which is the first line of the demo runbook. The two share the engine and
 * the sidecar and differ in every wire around them — ports, project directory, the key file, the
 * decision to keep the engine alive after the last tab closes.
 *
 * So a green sweep says nothing about whether the demo starts. This is the missing check, and it
 * is deliberately NOT in the sweep: it needs a stack somebody else started, and starting a second
 * one while `all.mjs` runs collides on the engine.
 *
 *     DAW_ENV_FILE=$PWD/.env KEEP_ENGINE=1 tools/webstack.sh
 *     node ui-web/test/demo-stack-smoke.mjs
 *
 * WHAT IT ASKS, in the order the demo depends on them:
 *   - the page loads and CONNECTS (canSend) — the sidecar found the engine's segment
 *   - a project loads and brings tracks and notes with it
 *   - the SCALE REGISTRY arrived, which the chord numerals' casing needs and which is published
 *     once per client rather than polled; a stack that never sends it draws every chord upper
 *     case and nothing else looks wrong
 *   - nothing threw
 */

import { chromium } from 'playwright';

const URL = process.env.DEMO_URL || 'http://127.0.0.1:8173/index.html';
let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

console.log(`\nsmoking the demo stack at ${URL}\n`);
let reached = true;
try {
  await page.goto(URL, { waitUntil: 'load', timeout: 15000 });
} catch (e) {
  reached = false;
  check(false, 'the page is being served', `${String(e).slice(0, 120)} — is tools/webstack.sh running?`);
}

if (reached) {
  const up = await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                                        { timeout: 25000 }).then(() => true).catch(() => false);
  check(up, 'the page connects to the engine',
        'canSend() never became true — the sidecar is up but has not attached to the segment');

  if (up) {
    await page.waitForTimeout(1500);
    await page.evaluate(() => window.__uni.run('load demo'));
    await page.waitForTimeout(2500);
    const st = await page.evaluate(() => {
      const e = window.__uni.engineState() || {};
      return { tracks: e.trackCount, notes: e.noteCount };
    });
    check(st.tracks > 0 && st.notes > 0, 'a project loads, with tracks and notes',
          JSON.stringify(st));

    /*
     * SENT ONCE PER CLIENT, not polled — so this is the reading most likely to be silently absent
     * on a stack that is otherwise working. Without it every chord numeral draws upper case
     * (see nameChord: no scale, no claim about quality) and nothing else looks wrong.
     */
    const scales = await page.evaluate(() => (window.__uni.scaleNames() || []).length);
    check(scales > 0, 'the scale registry arrived — chord numerals can be cased',
          `${scales} scales`);
  }
}

/*
 * DOES SOUND ACTUALLY LEAVE THE MACHINE?
 *
 * Nothing else in this repo asks. Every audio assertion we have goes through the OFFLINE RENDER,
 * which is the right oracle precisely because it does not touch a device — byte-exact, no
 * hardware, no CoreAudio. Which means the entire suite can be green on a machine where the demo
 * would be silent, and that is the one failure a runbook cannot absorb: it happens in front of
 * people, in the first ten seconds, with no error on screen.
 *
 * The engine already answers it, and answers it the strong way. It does not trust `start()`
 * returning true, and it does not trust the device's own `isPlaying()` — both report success on a
 * machine where CoreAudio opens the device, reports its name and rate, and never runs the IO
 * proc. It COUNTS REAL CALLBACKS and prints one of two lines. Both agents have lost time to "the
 * app makes no sound" against the weaker version of this check.
 *
 * So this reads the line rather than re-deriving it. `webstack.sh` writes the engine log to
 * /tmp/eng<SEG>.log, where SEG is the shm name with non-alphanumerics turned into underscores.
 */
{
  const { readFileSync, existsSync, readdirSync, statSync } = await import('node:fs');
  /** mtime, or 0 — a log we cannot stat must not throw and take the whole check with it. */
  const mtime = (f) => { try { return statSync(f).mtimeMs; } catch { return 0; } };
  const shm = process.env.DAW_UI_SHM_NAME || '/daw_web_ui';
  const seg = shm.replace(/[^A-Za-z0-9]/g, '_');
  let path = `/tmp/eng${seg}.log`;
  if (!existsSync(path)) {
    // The stack may have been started with a different segment. Take the newest engine log
    // rather than reporting a missing file as a silent device — being wrong about WHICH engine
    // is recoverable; reporting nothing at all is what this check exists to prevent.
    const candidates = readdirSync('/tmp').filter((f) => /^eng.*\.log$/.test(f))
      .map((f) => `/tmp/${f}`)
      .sort((a, b) => mtime(b) - mtime(a));
    if (candidates.length) path = candidates[0];
  }
  const log = existsSync(path) ? readFileSync(path, 'utf8') : '';
  const started = /Audio output started/.test(log);
  const opened = /OPENED BUT NEVER STARTED/.test(log);
  check(started && !opened,
        'AUDIO IS ACTUALLY RUNNING — the device ran a real callback, not just opened',
        opened
          ? `the engine says the device OPENED BUT NEVER STARTED: ${
              (log.match(/OPENED BUT NEVER STARTED[^\n]*/) || [''])[0].slice(0, 160)}`
          : (log ? `no "Audio output started" in ${path} — every render will still be perfect`
                 : `no engine log found at ${path}; is tools/webstack.sh running?`));
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
