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

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
