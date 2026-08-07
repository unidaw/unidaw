#!/usr/bin/env node
/**
 * A REFUSED CHAIN EDIT SAID "chain error on track 0 (code 2)".
 *
 * Three error families reach `describeEngineEvent` — chain-error, routing-error and mod-error —
 * and none of them was worded. They fell through to the generic last line, which prints the kind
 * and a raw number. A number is not an explanation: "code 2" and "there is no such device to
 * remove" are the same fact, and only one of them tells you what to do.
 *
 * ── WHY THIS FILE EXISTS AS WELL AS THE WORDING ─────────────────────────────────────────────
 *
 * Because wording that nobody drives is how this project got ten sampler reasons and four clip
 * reasons that had never once been rendered — two of which turned out to be WRONG when finally
 * exercised (d1b9fb1: a device that was not there and a device that was not a sampler both said
 * "re-read the kit, it has moved"). Writing a nicer sentence and not running it would repeat
 * exactly that.
 *
 * So this drives the one code that can be provoked without a fixture — removing a device that is
 * not there — and asserts the sentence, not the number.
 *
 * ── THE CODES, AND WHERE THEY COME FROM ─────────────────────────────────────────────────────
 *
 * They are positional, not documented on the wire: `engine_chain_commands.cpp` sets 1..4 at the
 * four points where an operation reports it changed nothing.
 *
 *   1  daw::addDevice returned false
 *   2  daw::removeDeviceById returned false   <- driven here
 *   3  daw::moveDeviceById returned false
 *   4  none of setDeviceBypass / setDevicePatcherNodeId / setDeviceHostSlotIndex changed anything
 *
 * 1 and 3 need a chain in a state where the engine's own guards refuse, which is a fixture apiece
 * and is left for when something touches that code. One driven beats four described.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

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
const reject = () => page.evaluate(() => String(window.__uni.state().reject || ''));

console.log('\na refused chain edit explains itself\n');

await run('new chainerr');
await settle(1300);

/* ── CONTROL: a legal chain edit must produce NO message ───────────────────────────────────
 * Without it, every assertion below is satisfied by an app that complains about everything.
 */
{
  const before = await reject();
  await page.evaluate(() => window.__uni.addDevice(0, 'patcher event'));
  await settle(1800);
  check((await reject()) === before,
        'CONTROL: adding a device produces no error message',
        `reject moved to ${JSON.stringify(await reject())} — if a good edit is refused, the check `
        + 'below proves nothing about bad ones');
}

/* ── REMOVING A DEVICE THAT IS NOT THERE ───────────────────────────────────────────────────
 * Sent raw, past the app's own send path: the rack only offers you devices that exist, so this
 * is the state a stale client or a script gets into, not one a person can click their way to.
 */
{
  const before = await reject();
  await page.evaluate(() => window.__uni.send({ type: 'deldevice', track: 0, device: 999 }));
  const arrived = await page.waitForFunction(
    (prev) => String((window.__uni.state() || {}).reject || '') !== prev,
    before, { timeout: 8000 }).then(() => true).catch(() => false);
  const said = arrived ? await reject() : '';
  console.log(`  the reject line says: ${JSON.stringify(said)}`);

  check(arrived, 'REMOVING A DEVICE THAT IS NOT THERE IS REPORTED AT ALL',
        'nothing reached the reject line in 8s — the engine refused (there is no device 999) and '
        + 'the app said nothing, which is the failure this file is about');
  check(/no such device to remove/.test(said),
        'and it SAYS SO, rather than printing a code',
        `${JSON.stringify(said)} — "chain error on track 0 (code 2)" is the same fact and tells `
        + 'the reader nothing they can act on');
  check(!/code 2/.test(said),
        'the raw code is gone from the sentence', JSON.stringify(said));
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
