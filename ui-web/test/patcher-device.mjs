#!/usr/bin/env node
/**
 * DOES A PATCHER EDIT LAND IN THE DEVICE'S GRAPH, OR IN THE POOL?
 *
 * This exists because a suite in this directory got it wrong and said so in green.
 *
 * `full-song.mjs` added a `patcher event` device to track 2, ran `addnode euclidean`,
 * `addnode out` and `link`, asserted that the patcher had two nodes and that they linked, and
 * passed. The saved project held `patcher_state: {nodes: 0, edges: 0}` on that device. The
 * nodes were real and they were in the POOL — the shared graph owned by no device — and since
 * patcher-is-a-device the pool is not what a project renders or saves.
 *
 * EVERY ASSERTION WAS TRUE AND THE CONCLUSION WAS FALSE. `__uni.patcher()` reports the
 * ASSEMBLED POOL, which is every device's graph unioned with re-id'd nodes plus the ownerless
 * ones, so "the graph gained two nodes" is a fact about the pool and says nothing about the
 * device you thought you were editing. The engine's own header warns about this in as many
 * words: without `kUiPatcherFlagHasDeviceId` "EVERY patcher command from a UI is pool-scoped,
 * which since patcher-is-a-device is not the graph a project renders".
 *
 * THE CAUSE was that `addPatcherNode` and `linkPatcherNode` hardcoded `track: 0` and named no
 * device at all, and `onOpenPatcher` was handed the track and the device id and stored neither.
 * So the app drew one device's graph and sent every edit somewhere else.
 *
 * THE ORACLE IS THE SAVED PROJECT, and it has to be. Nothing published can distinguish these
 * two outcomes — that is the entire defect — so a check that reads the published graph is a
 * check that cannot fail for the reason it names. The file is where a device's own graph is
 * written down.
 */

import { chromium } from 'playwright';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
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

console.log('\na patcher edit and the graph it lands in\n');

await run('new patchdev');
await settle(1200);

// A patcher device on a track that is NOT track 0, because track 0 is where the old bug sent
// everything and a test on track 0 would pass either way.
await run('add-track');
await settle(700);
await run('goto 0 1');
await settle(300);
await page.evaluate(() => window.__uni.addDevice(1, 'patcher event'));
await settle(1500);

const chain = await page.evaluate(() => window.__uni.state() && window.__uni.chainProbe());
check(!!chain, 'the track has a chain', JSON.stringify(chain));

// ---------------------------------------------------------------------------
// Default is POOL-SCOPED, and that is deliberate — the patcher suites drive it and it is the
// engine's legacy path. Asserted so a later change cannot make it device-scoped by accident.
// ---------------------------------------------------------------------------
const before = await page.evaluate(() => window.__uni.patchTarget());
check(before.device === -1, 'with no patcher opened, edits are pool-scoped',
      JSON.stringify(before));

// ---------------------------------------------------------------------------
// Open the device's patcher the way the rack does, then edit.
// ---------------------------------------------------------------------------
const devId = await page.evaluate(() => {
  const cards = [...document.querySelectorAll('.dv-card')].filter((el) => el.style.display !== 'none');
  const card = cards.find((el) => /patcher/i.test(el.textContent || ''));
  if (!card) return -1;
  // A double-click on a patcher card is what opens its graph — the app's own gesture, not a
  // synthesised call, so this exercises the path a person takes.
  card.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
  return card._devId;
});
check(devId >= 0, 'the rack has a patcher card to open', String(devId));
await settle(900);

const target = await page.evaluate(() => window.__uni.patchTarget());
check(target.track === 1 && target.device === devId,
      'opening it addresses that track AND that device',
      `${JSON.stringify(target)} vs device ${devId}`);

await run('addnode euclidean');
await settle(700);
await run('addnode out');
await settle(700);

const ids = await page.evaluate(() => (window.__uni.nodes() || []).map((n) => n.id));
if (ids.length >= 2) {
  await run(`link ${ids[ids.length - 2]} ${ids[ids.length - 1]}`);
  await settle(700);
}

// ---------------------------------------------------------------------------
// THE CLAIM. Read from the file, because nothing published can tell the two apart.
// ---------------------------------------------------------------------------
await run('save patchdev');
await settle(2000);

let saved = null;
try {
  saved = JSON.parse(readFileSync(join(stack.dir, 'patchdev.uniproj.json'), 'utf8'));
} catch (e) { /* the checks below report it */ }
check(!!saved, 'the project saved');

if (saved) {
  const track1 = (saved.tracks || []).find((t) => t.track_id === 1);
  const patcher = ((track1 && track1.device_chain) || [])
    .find((d) => String(d.kind || '').includes('patcher'));
  check(!!patcher, 'the saved project has the patcher device on track 1',
        JSON.stringify(track1 && track1.device_chain));

  const st = (patcher && (patcher.patcher_state || patcher.patcher)) || {};
  const nodes = st.nodes || [];
  const edges = st.edges || [];
  /*
   * THE CHECK THAT WOULD HAVE CAUGHT IT. `nodes: 0` here with a green "the patcher has two
   * nodes" from the published graph is exactly what happened, and only this side of it is
   * load-bearing.
   */
  check(nodes.length >= 2,
        "the nodes are in the DEVICE'S OWN graph, not the pool",
        `the saved device holds ${nodes.length} node(s) — pool-scoped edits save as 0`);
  check(edges.length >= 1, 'and so is the link between them',
        `the saved device holds ${edges.length} edge(s)`);
  const types = nodes.map((n) => n.type);
  check(types.some((t) => /euclid/i.test(String(t))),
        'the euclidean is one of them', JSON.stringify(types));
}

/*
 * AND THE PUBLISHED NODES SAY WHOSE THEY ARE. Without an owner on each node a UI cannot
 * address an edit at all — this is the fact that makes the fix above possible, so it is worth
 * asserting that it survives the wire rather than assuming it.
 */
const owners = await page.evaluate(() => (window.__uni.patcher().nodes || []).map((n) => n.owner));
check(owners.some((o) => o > 0),
      'the published nodes carry their owning device id',
      `owners: ${JSON.stringify(owners)} — all zero means the pool, which is the old behaviour`);

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
