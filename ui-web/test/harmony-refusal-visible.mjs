#!/usr/bin/env node
/**
 * A HARMONY WRITE THE ENGINE REFUSES MUST BE VISIBLE, NOT SILENT.
 *
 * The reported bug was four key changes asked for and one arriving. The refusals themselves were
 * correct — the engine said `harmony.version_mismatch` three times in its log. What made it a bug
 * rather than an inconvenience is that NOTHING SAID SO: the console answered "key set" on the send,
 * the lane simply did not change, and the only way to find out was to read the engine's log.
 *
 * The wait-for-apply fix (5d04227) stops the refusals happening in the first place for the ordinary
 * sequential case. This file guards the OTHER half — that when a refusal does happen, a person sees
 * it. Those are different properties and fixing the first does not give you the second: a
 * concurrent writer (the agent editing while someone types, two windows on one engine) can still
 * make a base stale, and that path has to surface rather than evaporate.
 *
 * ── HOW A REFUSAL IS FORCED ─────────────────────────────────────────────────────────────────
 *
 * By sending a base the engine cannot match. `resolve_base` honours an EXPLICIT base — deliberately,
 * so that a caller who read a version, thought, and then wrote is not overridden — so a raw message
 * carrying `base: 999999` reproduces exactly the state a stale caller is in, without needing two
 * clients and a race to arrange it.
 *
 * ── THE CHAIN BEING ASSERTED, WHICH IS FOUR PIECES ─────────────────────────────────────────
 *
 *   engine    refuses and does not move harmonyVersion
 *   sidecar   send_harmony_and_await times out and answers {"error": ...} instead of {"ok":true}
 *   page      onAck's generic error branch puts it on `state.reject`
 *   person    sees the reject line
 *
 * Any one of those breaking returns the app to "it just did nothing", so the test drives the whole
 * chain rather than any single link. It also asserts the write really did NOT land — an error
 * message next to a successful edit would be its own kind of lie.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const Q = 960000;
const BAR = Q * 4;
const stack = await startStack({ keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await page.waitForTimeout(1500);

const run = (c) => page.evaluate((x) => window.__uni.run(x), c);
const settle = (ms) => page.waitForTimeout(ms);
const seen = () => page.evaluate(() => (window.__uni.harmony() || []).map((h) => h.root));
const reject = () => page.evaluate(() => window.__uni.state().reject || '');

console.log('\na refused key change says so\n');

await run('new harmref');
await settle(1200);

/* ── THE CONTROL FIRST ─────────────────────────────────────────────────────────────────────
 * An ordinary write must land AND leave the reject line clear. Without this, the assertions below
 * are satisfied by an app that reports an error for everything.
 */
await run(`harmony 7 minor ${BAR}`);
await settle(1500);
check((await seen()).includes(7), 'CONTROL: an ordinary key change lands', JSON.stringify(await seen()));
check((await reject()) === '', 'and says nothing about a refusal', `reject=${JSON.stringify(await reject())}`);

const before = await seen();

/* ── NOW ONE THE ENGINE CANNOT ACCEPT ─────────────────────────────────────────────────────── */
const sent = await page.evaluate((tick) => window.__uni.send(
  { type: 'harmony', root: 2, scale: 1, tick, base: 999999 }), 2 * BAR);
check(sent === true, 'the raw message was put on the wire', String(sent));

// Longer than the sidecar's wait, which is what has to elapse before it can answer at all.
await settle(2500);

const why = await reject();
check(why !== '',
      'THE REFUSAL REACHES THE PERSON — it is on the reject line, not only in the engine log',
      `reject is empty. The engine refused (the write is absent below), so silence here is the `
      + 'exact failure this file exists for: a command that reports nothing and does nothing');
console.log(`  the reject line says: ${JSON.stringify(why)}`);

const after = await seen();
check(JSON.stringify(after) === JSON.stringify(before),
      'and the refused write really did NOT land',
      `${JSON.stringify(before)} -> ${JSON.stringify(after)} — an error message beside a `
      + 'successful edit would be its own kind of lie');

/* ── AND THE APP KEEPS WORKING AFTERWARDS ──────────────────────────────────────────────────
 * A refusal must not wedge the path. This is the difference between reporting an error and
 * becoming one.
 */
await run(`harmony 9 minor ${3 * BAR}`);
await settle(1800);
check((await seen()).includes(9),
      'a good write still lands after a refused one', JSON.stringify(await seen()));

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
