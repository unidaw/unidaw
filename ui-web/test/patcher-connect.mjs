#!/usr/bin/env node
/**
 * CONNECTING TWO PATCHER NODES — BY MOUSE, AND BY KEYBOARD.
 *
 * Nothing in this repository had ever connected two nodes through the UI by any route. Not one
 * check, on either gesture, in any suite. That was found the way these things always are: by a
 * person trying to build a patcher and failing, then asking whether a test existed.
 *
 * What they hit, and what it turned out to be:
 *
 *   "I can't click on node edges to create connections"
 *       -> connecting was KEYBOARD-ONLY: select a node, press `c`, select another, press `c`.
 *          The status line says so while a link is armed, which is not the same as being
 *          discoverable. There is now a pointer path, and this asserts both.
 *
 *   "the AI wasn't able to do it either: it did add the nodes but not the connections"
 *       -> the agent has `patcher_node` and NOTHING to connect with. Worse, OP_REGISTRY claimed
 *          it did: `link: { cli: 'patcher-connect', agent: 'patcher_node' }`. `patcher_node`
 *          ADDS a node — a different opcode entirely. The parity ratchet only checks that the
 *          named tool EXISTS, so it passed while the claim was false.
 *
 * ── WHAT THIS ASSERTS, AND WHY IT IS THE SAVED GRAPH ────────────────────────────────────────
 *
 * The edge, in the DEVICE's own saved graph. Not the rendered cable, and not that a command was
 * accepted. A patcher edit without a device id goes to the shared pool, which is not what a
 * project renders and never reaches disk — the graph then reloads with the nodes present and
 * nothing joining them, after a gesture that looked like it worked. That exact shape was a real
 * bug in daw-cli's patcher-connect, found earlier today.
 *
 * euclidean -> event-out, because two euclideans CANNOT connect: a euclidean emits gates and has
 * no event input, so the engine answers `invalid_port` and the graph saves with two nodes and no
 * edge — indistinguishable, from the outside, from the feature being broken.
 */

import { chromium } from 'playwright';
import { readFileSync, existsSync } from 'node:fs';
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

/**
 * Select a node the way a person does: click its BODY.
 *
 * The CENTRE, not the bottom edge. Ports sit on the left and right edges and the head is the
 * link-drag grip, so a click near any border starts a gesture instead of selecting — which is
 * why an earlier version of this left the same node selected twice and the app answered "a node
 * cannot connect to itself". Returns whether the selection actually moved, so a click that
 * misses is reported as a click that missed rather than as a broken feature.
 */
const clickNode = async (id) => {
  const at = await page.evaluate((n) => {
    const el = document.querySelector(`.pt-node[data-id="${n}"]`);
    if (!el) return null;
    const r = el.getBoundingClientRect();
    // THE HEAD. `onSelect` fires on pointerdown for it, and a link drag that ends on the node it
    // started from is discarded — so a plain click selects and does nothing else. The body is
    // not usable here: an `out` node is 64px with a port on each edge, and the centre of what is
    // left lands on nothing. An earlier version clicked there, selected nothing, and the app
    // answered "a node cannot connect to itself" because both `c` presses saw the same node.
    const head = el.querySelector('.pt-head');
    const h = head ? head.getBoundingClientRect() : r;
    return { x: h.left + h.width / 2, y: h.top + h.height / 2 };
  }, id);
  if (!at) return false;
  await page.mouse.click(at.x, at.y);
  await settle(200);
  const sel = await page.evaluate(() => window.__uni.state().selectedNode);
  return sel === id;
};

console.log('\nconnecting patcher nodes, by mouse and by keyboard\n');

await run('new ptconn');
await settle(1200);
await page.evaluate(() => window.__uni.addDevice(0, 'patcher event'));
await settle(1500);
await page.evaluate(() => window.__uni.setView('patcher'));
await settle(800);

/** Every edge in the saved project's device graphs, so a pool-scoped edit cannot count. */
const savedEdges = async (name) => {
  await run(`save ${name}`);
  for (let i = 0; i < 40; i++) {
    const p = join(stack.dir, `${name}.uniproj.json`);
    if (existsSync(p)) {
      try {
        const doc = JSON.parse(readFileSync(p, 'utf8'));
        return (doc.tracks || []).flatMap((t) => (t.device_chain || [])
          .flatMap((d) => (d.patcher && d.patcher.edges) || (d.graph && d.graph.edges) || []));
      } catch { /* mid-write */ }
    }
    await settle(150);
  }
  return null;
};
// Ids off the DOM, because the probe reports COUNTS not a node list — and the DOM is what a
// person can actually click, which is the thing under test.
const nodeIds = () => page.evaluate(() =>
  [...document.querySelectorAll('.pt-node')]
    .filter((el) => el.offsetParent !== null)
    .map((el) => Number(el.dataset.id)));

/** How many visible nodes say "no config published". */
const unpublished = () => page.evaluate(() =>
  [...document.querySelectorAll('.pt-node')]
    .filter((el) => el.offsetParent !== null)
    .filter((el) => {
      const e = el.querySelector('.pt-empty');
      return e && e.style.display !== 'none';
    }).length);

// Two nodes that CAN legally join. A euclidean has no event input, so euclidean -> euclidean is
// refused by the engine and would read exactly like the gesture doing nothing.
await run('addnode euclidean');
await settle(900);
await run('addnode out');
await settle(900);

const ids = await nodeIds();
check(ids.length >= 2, 'two nodes to connect', JSON.stringify(ids));

/*
 * A NODE YOU JUST ADDED HAS ITS SETTINGS.
 *
 * Reported from live use: "I added an Euclidean node, but it has no parameters. no config
 * published". A euclidean has eight settings — steps, hits, offset, degree, oct, vel, base, dur —
 * and the box says so when the ENGINE has not published them (`hasConfig` is a byte per node in
 * the published graph, set only when the node carries a config struct).
 *
 * Nodes loaded from a project file have one. That is why every existing patcher check passes:
 * they all start from a preset. A node ADDED through the app is the untested path, and it is also
 * the only path for building a patch from scratch — which is the thing that could not be done.
 */
check(await unpublished() === 0,
      'a node added through the app arrives WITH its settings, not "no config published"',
      `${await unpublished()} of ${ids.length} node(s) have no published config — a euclidean has `
      + 'eight settings and none of them can be edited until the engine publishes them');
/*
 * THE EDGE SET BEFORE THE GESTURE, not a count of zero.
 *
 * This asserted there were NO edges, and then that there was AT LEAST ONE afterwards. Both were
 * wrong in the same way: the `patcher event` preset SHIPS WITH AN EDGE, so the first check failed
 * and — far worse — the second was already true before the drag happened. My headline check for
 * the feature could not fail. It would have passed with the mouse path deleted.
 *
 * Found by another agent reading this file, not by the file failing. A count is a comparison
 * against a number somebody assumed; a diff is a comparison against what was actually there.
 */
const edgeKey = (e) => `${e.src_node_id}:${e.src_port_id}->${e.dst_node_id}:${e.dst_port_id}`;
const edgesBefore = new Set(((await savedEdges('ptconn_a')) || []).map(edgeKey));
console.log(`  (the graph starts with ${edgesBefore.size} edge(s) — the preset ships one)`);

// ── THE POINTER PATH ────────────────────────────────────────────────────────────────────────
// Drag the FIRST node's head onto the second. The head, because the rows below it own the
// pointer for field drags.
if (ids.length >= 2) {
  const box = async (id) => page.evaluate((n) => {
    const el = document.querySelector(`.pt-node[data-id="${n}"]`);
    if (!el) return null;
    const r = el.getBoundingClientRect();
    const h = el.querySelector('.pt-head').getBoundingClientRect();
    return { headX: h.left + h.width / 2, headY: h.top + h.height / 2,
             cx: r.left + r.width / 2, cy: r.top + r.height / 2 };
  }, id);

  /*
   * THE TWO I JUST ADDED, not the first two on screen.
   *
   * The `patcher event` preset ships nodes of its own, so `ids[0]`/`ids[1]` were a preset node
   * and one of mine — a pair with no port in common, which `orientLink` correctly refuses. The
   * drag then did nothing for a real reason and the test read it as the gesture being broken.
   * euclidean -> out are the last two added and are known to connect.
   */
  const pair = ids.slice(-2);
  const a = await box(pair[0]);
  const b = await box(pair[1]);
  check(!!a && !!b, 'both nodes are on screen with a grabbable head',
        JSON.stringify({ a, b }));

  if (a && b) {
    await page.mouse.move(a.headX, a.headY);
    await page.mouse.down();
    // Through an intermediate point, so a handler that only reads the final position is not
    // accidentally satisfied by a click.
    await page.mouse.move((a.headX + b.cx) / 2, (a.headY + b.cy) / 2, { steps: 8 });
    const banding = await page.evaluate(() =>
      !!document.querySelector('.pt-linkline') &&
      document.querySelector('.pt-linkline').getAttribute('d') !== '');
    check(banding, 'a rubber band follows the pointer while dragging',
          'no .pt-linkline with a path — the drag gives no feedback that it is doing anything');
    await page.mouse.move(b.cx, b.cy, { steps: 8 });
    await page.mouse.up();
    await settle(1500);

    // WHERE IS IT LOST? The live graph and the saved file answer different questions: a link
    // that reaches the pool shows up live and never reaches disk.
    const live = await page.evaluate(() => {
      const p = window.__uni.patcherProbe ? window.__uni.patcherProbe() : null;
      return p ? { nodes: p.nodes, edges: p.edges, scoped: p.scoped, pool: p.poolCount,
                   notice: p.notice } : null;
    });
    console.log(`  (live graph after the drag: ${JSON.stringify(live)})`);
    const rejected = await page.evaluate(() => window.__uni.state().reject);
    if (rejected) console.log(`  (the app refused it: ${JSON.stringify(rejected)})`);
    const edges = (await savedEdges('ptconn_b')) || [];
    const added = edges.map(edgeKey).filter((k) => !edgesBefore.has(k));
    check(added.length >= 1,
          'DRAGGING ONE NODE ONTO ANOTHER CONNECTS THEM, in the device\'s saved graph',
          `edges went ${edgesBefore.size} -> ${edges.length}, NEW: ${JSON.stringify(added)} — `
          + 'an edit without a device id goes to the shared pool, which never reaches disk, so '
          + 'the graph would reload with no cable');
  }
}

// ── THE KEYBOARD PATH, which is what existed and was undiscoverable ─────────────────────────
// A second pair, so this cannot pass on the edge the mouse just made.
await run('addnode euclidean');
await settle(900);
await run('addnode out');
await settle(900);
const ids2 = await nodeIds();
const fresh = ids2.filter((i) => !(ids || []).includes(i));
check(fresh.length >= 2, 'two more nodes for the keyboard path', JSON.stringify(fresh));

if (fresh.length >= 2) {
  const beforeSet = new Set(((await savedEdges('ptconn_c')) || []).map(edgeKey));
  const before = beforeSet.size;
  // The two just added, in creation order — the same lesson the mouse path taught: the first two
  // on screen are the preset's and have no port in common.
  console.log(`  (keyboard path will link ${fresh.slice(-2).join(' -> ')} of ${JSON.stringify(ids2)})`);
  const picked0 = await clickNode(fresh.slice(-2)[0]);
  check(picked0, 'the first node of the keyboard pair is selected by clicking it');
  await settle(200);
  await page.evaluate(() => window.__uni.patchLink());
  await settle(200);
  const armed = await page.evaluate(() => {
    const p = window.__uni.patcherProbe ? window.__uni.patcherProbe() : null;
    return p ? p.linkFrom : -99;
  });
  check(armed !== -99 && armed !== -1 && armed !== null,
        'the first `c` ARMS the link rather than sending one', `linkFrom=${armed}`);
  const rects = await page.evaluate((wanted) => Object.fromEntries(wanted.map((n) => {
    const el = document.querySelector(`.pt-node[data-id="${n}"]`);
    if (!el) return [n, null];
    const r = el.getBoundingClientRect();
    return [n, { x: Math.round(r.x), y: Math.round(r.y),
                 w: Math.round(r.width), h: Math.round(r.height),
                 vis: el.offsetParent !== null }];
  })), fresh.slice(-2));
  console.log(`  (node boxes: ${JSON.stringify(rects)})`);
  const picked1 = await clickNode(fresh.slice(-2)[1]);
  console.log(`  (selectedNode after the second click: ${
    await page.evaluate(() => window.__uni.state().selectedNode)})`);
  check(picked1, 'and the second one is too — otherwise `c` links a node to itself');
  await settle(200);
  await page.evaluate(() => window.__uni.patchLink());
  await settle(1500);
  const afterEdges = (await savedEdges('ptconn_d')) || [];
  const newOnes = afterEdges.map(edgeKey).filter((k) => !beforeSet.has(k));
  const rej = await page.evaluate(() => window.__uni.state().reject);
  if (rej) console.log(`  (the app refused the keyboard link: ${JSON.stringify(rej)})`);
  const after = afterEdges.length;
  check(newOnes.length >= 1,
        'and the second `c` on another node connects them — the keyboard path still works',
        `${before} -> ${after} edges`);
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
