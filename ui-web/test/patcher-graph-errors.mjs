#!/usr/bin/env node
/**
 * THE PATCHER REFUSES SIX WAYS AND FIVE OF THEM HAD NEVER BEEN DRIVEN.
 *
 * `GRAPH_ERRORS` in index.html words six: no such node type, no such node, that would make a
 * cycle, the engine could not add it, that connection is not allowed, those ports do not fit.
 * Only "no such node" appears in any suite. The rest are wired to a table nobody has watched
 * render.
 *
 * ── WHY THE PATCHER ESPECIALLY ──────────────────────────────────────────────────────────────
 *
 * Because a refused patch is INVISIBLE BY CONSTRUCTION. A refused note leaves a cell empty where
 * you expected a note; a refused link leaves a graph that looks exactly like a graph you have not
 * finished wiring. `patcher-connect.mjs` says as much about the drag gesture — a link that does
 * nothing "would read exactly like the gesture doing nothing" — and its own fixture avoids
 * euclidean -> euclidean specifically because the engine refuses it.
 *
 * So the refusal message is not a nicety here, it is the only difference between "the engine said
 * no" and "you have not connected it yet".
 *
 * ── WHAT IS DRIVEN ──────────────────────────────────────────────────────────────────────────
 *
 *   a CYCLE            two passthru nodes wired A->B then B->A
 *   PORTS THAT DO NOT FIT   an lfo (control out only) into an out (event in only)
 *
 * Both are ordinary mistakes rather than contrived ones: a cycle is what you get by wiring a
 * feedback path on purpose and finding out the engine will not have it, and the port mismatch is
 * what you get by aiming at the wrong box.
 *
 * A VALID LINK IS MADE FIRST, and it must produce NO message. Without it every assertion below is
 * satisfied by an app that refuses everything, which is the failure this kind of test invites.
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
const nodeIds = () => page.evaluate(() =>
  [...document.querySelectorAll('.pt-node')]
    .filter((el) => el.offsetParent !== null)
    .map((el) => Number(el.dataset.id)));

console.log('\nthe patcher says WHY it refused a link\n');

await run('new ptgraph');
await settle(1200);
await page.evaluate(() => window.__uni.addDevice(0, 'patcher event'));
await settle(1500);
await page.evaluate(() => window.__uni.setView('patcher'));
await settle(800);

/**
 * Run a link and return the reject line IT produced, waiting for the line to CHANGE.
 *
 * `state.reject` persists until replaced, so a fixed sleep reads the PREVIOUS case's message
 * whenever this one is slower — a sibling of this file passed and failed on unchanged code that
 * way. Empty string on timeout, so a case that never answered fails with its own story.
 *
 * The console verb THROWS on a client-side refusal and sets state.reject on an engine-side one;
 * both are answers, so the throw is caught and folded in rather than taking the suite down.
 */
const linkAndHear = async (a, b) => {
  const before = await reject();
  await page.evaluate(async (ids) => {
    try { window.__uni.run(`link ${ids[0]} ${ids[1]}`); } catch (e) { /* an answer too */ }
  }, [a, b]);
  try {
    await page.waitForFunction(
      (prev) => String((window.__uni.state() || {}).reject || '') !== prev,
      before, { timeout: 8000 });
  } catch { return ''; }
  return reject();
};

/* ── THE CONTROL: a legal link, which must say nothing ─────────────────────────────────────
 * euclidean sources events, out sinks them. This is the pairing patcher-connect.mjs uses for the
 * same reason — it is the one that is allowed.
 */
/**
 * Add a node and return ITS id, by diffing the visible set.
 *
 * NOT an index into nodeIds(). A patcher device does not start empty — a fresh "patcher event"
 * already shows several nodes — so `ids[0]` and `ids[1]` are somebody else's, and the first
 * version of this file wired two of those together and called it the control. It passed, which is
 * the whole problem: a control that links two arbitrary nodes proves nothing about the pair the
 * test claims to be about.
 */
const addNode = async (type) => {
  const before = await nodeIds();
  await run(`addnode ${type}`);
  await settle(900);
  const after = await nodeIds();
  const fresh = after.filter((id) => !before.includes(id));
  return fresh.length === 1 ? fresh[0] : null;
};

const euclid = await addNode('euclidean');
const evOut = await addNode('out');
check(euclid !== null && evOut !== null,
      'a euclidean and an out, each identified by its own id',
      `euclidean=${euclid} out=${evOut}`);
{
  const before = await reject();
  await page.evaluate((x) => window.__uni.run(`link ${x[0]} ${x[1]}`), [euclid, evOut]);
  await settle(1500);
  check((await reject()) === before,
        'CONTROL: a legal link (euclidean -> out) produces NO message',
        `reject moved to ${JSON.stringify(await reject())} — if a good link is refused, the two `
        + 'checks below prove nothing about bad ones');
}

/* ── PORTS THAT DO NOT FIT: control out into an event in ───────────────────────────────────
 *
 * ANSWERED BY THE SIDECAR, NOT THE ENGINE, which is worth writing down because it surprised this
 * test. The sidecar knows the port shapes and refuses with "those two node types have no
 * compatible ports" before the command is ever queued, so the engine's own wordings for this
 * condition — GRAPH_ERRORS 5 "that connection is not allowed" and 6 "those ports do not fit" —
 * are NOT reachable from the web UI at all. They would only fire for a client that skips the
 * sidecar's check.
 *
 * So this asserts the behaviour that exists rather than the table: the refusal is explained. The
 * two engine strings are left uncovered deliberately and this comment is why — a check written
 * against them here could only ever pass by accident.
 */
{
  const lfo = await addNode('lfo');
  check(lfo !== null, 'an lfo to aim wrongly', String(lfo));
  const said = await linkAndHear(lfo, evOut);
  console.log(`  lfo -> out said: ${JSON.stringify(said)}`);
  check(/compatible ports|ports do not fit|not allowed/.test(said),
        'A PORT MISMATCH IS EXPLAINED',
        `${JSON.stringify(said)} — an lfo has only a control out and an EventOut only an event `
        + 'in; refused in silence this is indistinguishable from a graph you have not finished');
}

/* ── A CYCLE: two passthru nodes, wired both ways ─────────────────────────────────────────── */
{
  const a = await addNode('passthru');
  const b = await addNode('passthru');
  check(a !== null && b !== null && a !== b,
        'two passthru nodes, distinct', `a=${a} b=${b}`);

  // Forward first. This one is legal and is the setup, not an assertion.
  await page.evaluate((x) => window.__uni.run(`link ${x[0]} ${x[1]}`), [a, b]);
  await settle(1500);

  const said = await linkAndHear(b, a);
  console.log(`  passthru ${b} -> ${a} (back again) said: ${JSON.stringify(said)}`);
  check(/cycle/.test(said),
        'A CYCLE IS NAMED AS A CYCLE',
        `${JSON.stringify(said)} — the engine will not run a feedback path, and if it declines `
        + 'without saying so the graph simply looks half-wired');
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
