#!/usr/bin/env node
/**
 * DOES A PATCHER KNOB SURVIVE A SAVE?
 *
 * Two findings that look like they contradict each other, and the point of this file is that
 * they do not:
 *
 *   - patchcfg.mjs proves a config nudge changes the AUDIO, end to end, with a capture.
 *   - backend proves the same command leaves the SAVED PROJECT unchanged: `patcher-config
 *     --device 1 --node 0 --hits 3` against a device graph whose euclidean has hits 11, and the
 *     file still says 11 afterwards.
 *
 * Both can be true at once, and if they are, the shape is worse than either alone: opcode 28
 * edits the shared pool, the pool is what the producer reads, and the DEVICE is what gets
 * written to disk. So the knob is heard and not kept — you nudge, you hear it change, you save,
 * you reload, and the sound is back to what it was with nothing anywhere reporting a loss.
 *
 * That is the question this asks, and it asks it the only way that can answer it: through the
 * app's own knob, then the app's own save, then a reload, then the app's own read-back. No
 * daw-cli — a second client is a second set of assumptions about which graph is addressed.
 */

import { chromium } from 'playwright';
import { readFileSync, existsSync } from 'node:fs';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8, keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 })
  .catch(() => {});
await page.waitForTimeout(1200);

// `generator` carries a patcher_event device whose graph is stored ON THE DEVICE — the shape
// that matters here. A project with only a shared-pool graph could not show the bug.
await page.evaluate(() => window.__uni.loadProject('generator'));
await page.waitForTimeout(3000);
await page.evaluate(() => window.__uni.setView('patcher'));
await page.waitForTimeout(1200);

/*
 * THE FILE IS THE GROUND TRUTH, so this reads the file.
 *
 * The patcher probe reports node COUNTS and type names, not per-node config — and the region it
 * reads from does not publish a device id at all (`UiPatcherRegion.deviceId` is declared and
 * never written by the engine, so it is 0 for every graph). Neither is a defect for the view's
 * own purposes and both make the probe the wrong instrument for this question. The saved project
 * answers it directly: `generator`'s euclidean has hits 5 on disk, and after a nudge and a save
 * it either does not, or it does and the knob was never kept.
 */
const projectPath = `${stack.dir}/generator.uniproj.json`;
const hitsOnDisk = () => {
  if (!existsSync(projectPath)) return null;
  const doc = JSON.parse(readFileSync(projectPath, 'utf8'));
  for (const t of doc.tracks || []) {
    for (const d of t.device_chain || []) {
      for (const n of ((d.patcher || {}).nodes) || []) {
        if (n.euclidean) return n.euclidean.hits;
      }
    }
  }
  return null;
};

const was = hitsOnDisk();
check(was !== null, "the project's euclidean node is on a DEVICE's graph, which is the case "
                   + 'that can fail', `hits ${was}`);

/*
 * NUDGE THROUGH THE APP'S OWN VERB — `patch <node> <field> <steps>`, the same function the
 * keyboard's nudge calls.
 *
 * The first version pressed arrow keys to move a field cursor and then nudged it, and the probe
 * reported `field: ""` afterwards: the selection had not taken, so nothing was nudged and the
 * save check below would have "confirmed" a bug from a run in which no edit was ever attempted.
 * That is the worst kind of pass — a report about the engine produced by a test that did
 * nothing — so the nudge is now a command whose refusal is a returned string.
 */
/*
 * READ THE DRAWN VALUE, before and after. "The command was accepted" is not "the value moved",
 * and a save check built on a nudge that did nothing would report a bug from a run in which no
 * edit was ever attempted — a finding about the engine manufactured by a test doing nothing.
 */
const drawnHits = () => page.evaluate(() => {
  const rows = [...document.querySelectorAll('.pt-node .pt-row')];
  const r = rows.find((x) => {
    const n = x.querySelector('.pt-row-n');
    return n && n.textContent.trim() === 'hits';
  });
  const v = r && r.querySelector('.pt-row-v');
  return v ? v.textContent.trim() : null;
});
const drawnBefore = await drawnHits();
const said = await page.evaluate(() => window.__uni.run('patch 0 hits 1'));
/*
 * WHO OWNS THE NODE? The engine publishes it per node (`UiPatcherNode.ownerDeviceId`) because
 * the region is the ASSEMBLED POOL and "which device is this region" has no answer — a union of
 * every device's graph with re-id'd nodes belongs to no one device.
 *
 * That per-node owner is the fact a client needs before it can set kUiPatcherFlagHasDeviceId,
 * and without it every patcher edit is pool-scoped: heard, drawn, and not written to disk.
 *
 * THIS PROJECT PUBLISHES ZERO. It carries ONE patcher device, so the engine takes its legacy
 * single-graph path and assembles nothing, and an owner of 0 means "no owning device". So the
 * one-device case — which is every project anyone has made so far — still cannot name its
 * device, and the check that proves the fix works uses TWO. Same blind spot as everything else
 * tonight: a fixture with one of something cannot tell "always 0" from "correctly 0".
 */
const owners = await page.evaluate(() => {
  const p = window.__uni.patcher();
  return p ? { device: p.device, version: p.version } : null;
});
{
  const { execFileSync } = await import('node:child_process');
  let pool = null;
  try {
    pool = JSON.parse(execFileSync('./ui/target/release/daw-cli', ['get', 'patcher'],
      { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm }, encoding: 'utf8' }));
  } catch { /* reported by the check below */ }
  const owned = pool && pool.nodes ? pool.nodes.filter((n) => n.owner_device) .length : -1;
  check(owned > 0, 'the published nodes name the DEVICE that owns them',
        `${owned} of ${pool && pool.nodes ? pool.nodes.length : '?'} nodes carry an owner ` +
        `(${JSON.stringify(owners)}) — a one-device project takes the legacy single-graph path, ` +
        `publishes owner 0, and so cannot be addressed by device at all`);
}
await page.waitForTimeout(1500);
const drawnAfter = await drawnHits();
check(drawnBefore !== null && drawnAfter !== null && drawnBefore !== drawnAfter,
      'the nudge MOVES the value the app is drawing',
      `${drawnBefore} -> ${drawnAfter} (${String(said).slice(-60)})`);

await page.evaluate(() => window.__uni.run('save'));
await page.waitForTimeout(3000);
const now = hitsOnDisk();

/*
 * THE CLAIM. A knob that moves the sound and not the file is the worst of the three states: the
 * app agrees with you, the engine agrees with you, and the work is gone the next time you open
 * it, with nothing anywhere reporting a loss.
 *
 * Backend found this from the other side — opcode 28 edits the shared POOL, and since
 * patcher-is-a-device the graph that is written to disk lives on the DEVICE. The command reports
 * success either way: it edits the pool, the pool has no node with that id, `updated` stays
 * false, and the error goes into an SHM diff nothing surfaces.
 */
check(now !== null && was !== null && now !== was,
      'a patcher knob nudged in the UI SURVIVES a save',
      `hits was ${was}, file says ${now} after the nudge and the save — opcode 28 edits the ` +
      `shared pool and the DEVICE is what is written, so the knob is heard and not kept`);

check(errors.length === 0, 'nothing threw', errors.slice(0, 2).join(' | '));
await browser.close();
stack.stop();
console.log(fail ? `\n${fail} of ${pass + fail} FAILED` : `\nALL PASS (${pass})`);
process.exit(fail ? 1 : 0);
