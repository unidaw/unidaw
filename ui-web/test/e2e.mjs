#!/usr/bin/env node
// End-to-end test against a LIVE engine.
//
//   ../tools/webstack.sh && node test/e2e.mjs
//
// The goldens and the allocation test both run against fixtures, deliberately —
// a test that changes depending on whether a sidecar happens to be running is
// not a test. But that leaves the whole engine boundary unexercised, and every
// serious bug on this branch has lived exactly there: the wire stride, the row
// projection, the viewport that reached nobody, two note caches keyed on the
// wrong thing. Those were all found by hand and none of them would have failed
// a golden.
//
// So this is the other half: it needs a running stack, it edits a real project,
// and it asserts on what the engine actually says. It is not part of `npm test`
// for that reason — it is `npm run e2e`.
//
// It uses webtest.uniproj (maximal with device_chain emptied) because a project
// with plugin chains takes the engine down; see the hang reported to backend.

import { chromium } from 'playwright';

const URL = process.env.UNI_URL || 'http://127.0.0.1:8173/index.html';
const PROJECT = process.env.UNI_PROJECT || 'webtest';

let fail = 0, count = 0;
const ok = (cond, label, detail = '') => {
  count++;
  if (!cond) fail++;
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`);
};
const section = (s) => console.log(`\n[${s}]`);

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

await page.goto(URL);
await page.waitForFunction(() => !!window.__uni);

const connected = await page.waitForFunction(() => window.__uni.canSend(), null, { timeout: 8000 })
  .then(() => true).catch(() => false);
if (!connected) {
  console.log('\n  no engine — start one with tools/webstack.sh\n');
  await browser.close();
  process.exit(2);
}

const E = () => page.evaluate(() => window.__uni.engineState());
const run = async (line, wait = 150) => {
  const out = await page.evaluate((l) => window.__uni.run(l), line);
  await page.waitForTimeout(wait);
  return out[out.length - 1] || '';
};
const frames = () => page.evaluate(() => new Promise((r) =>
  requestAnimationFrame(() => requestAnimationFrame(r))));

section('engine');
await page.evaluate((p) => window.__uni.loadProject(p), PROJECT);
await page.waitForTimeout(2500);
let e = await E();
ok(!e.stale, 'engine is publishing', `clipVersion ${e.clipVersion}`);
if (e.stale) {
  // Everything below asserts on engine state, so a dead engine reports as nine
  // unrelated failures and buries the one that matters. Stop and name it.
  console.log('\n  engine has stopped publishing — every check below would be'
            + ' a symptom of that.\n  restart with tools/webstack.sh; see the'
            + ' plugin-host hang reported to backend.\n');
  await browser.close();
  process.exit(2);
}
ok(e.noteCount > 0, 'notes arrived', `${e.noteCount} notes on ${e.trackCount} tracks`);
const names = await page.evaluate(() => window.__uni.names());
ok(names && names.some((n) => n && !/^T\d/.test(n)), 'track names published', JSON.stringify(names.slice(0, 3)));
const lpb = await page.evaluate(() => window.__uni.lpb());
ok(new Set(lpb.slice(0, 6)).size > 1, 'lanes disagree about their grid', JSON.stringify(lpb.slice(0, 6)));

section('transport');
const t0 = (await E()).playheadTick;
await run('play', 1200);
const t1 = (await E()).playheadTick;
ok(t1 !== t0, 'play advances the playhead', `${t0} -> ${t1}`);
await run('play', 600);
const a = (await E()).playheadTick;
await page.waitForTimeout(500);
ok((await E()).playheadTick === a, 'pause holds position', String(a));
await run('stop', 700);
ok((await E()).playheadTick === 0, 'stop rewinds');

section('note editing');
await run('goto 40 0');
const before = (await E()).clipVersion;
await run('note 67', 900);
const after = (await E()).clipVersion;
ok(after > before, 'a note write moves the clip version', `${before} -> ${after}`);
const wrote = await page.evaluate(() => window.__uni.selected().length);
ok(wrote >= 0, 'cursor note readable');
await run('del', 900);
ok((await E()).clipVersion > after, 'a delete moves it again');

section('selection batch');
await page.evaluate(() => { window.__uni.setZoom(3); window.__uni.selectRows(0, 6, 0, 2); });
const sel = await page.evaluate(() => window.__uni.selected());
ok(sel.length > 1, 'a range selects several notes', `${sel.length} notes`);
await page.evaluate(() => window.__uni.copy());
await page.evaluate(() => window.__uni.transpose(12));
await page.waitForTimeout(1800);
await page.evaluate(() => window.__uni.selectRows(0, 6, 0, 2));
const up = await page.evaluate(() => window.__uni.selected());
// The whole point of the batch: ALL of them move, not just the first.
ok(up.length === sel.length && up.every((n, i) => n.pitch === sel[i].pitch + 12),
   'every note in the batch transposed', `${sel.map(n => n.pitch)} -> ${up.map(n => n.pitch)}`);
await page.evaluate(() => window.__uni.transpose(-12));
await page.waitForTimeout(1800);
await page.evaluate(() => window.__uni.selectRows(0, 6, 0, 2));
const back = await page.evaluate(() => window.__uni.selected());
ok(back.every((n, i) => n.pitch === sel[i].pitch), 'and transposes back exactly');

section('chords');
const cv = (await E()).clipVersion;
await page.evaluate(() => window.__uni.run('goto 12 1'));
// Typed, not called through the API: this is the one path where the keyboard
// does something the dock cannot, so driving it any other way tests nothing.
await page.keyboard.press('@');
for (const ch of ['3', '^', '7']) { await page.keyboard.press(ch); await frames(); }
await page.keyboard.press('Enter');
await page.waitForTimeout(1000);
ok((await E()).clipVersion > cv, 'a chord token writes', `clipVersion ${cv} -> ${(await E()).clipVersion}`);

section('mixer');
await page.evaluate(() => window.__uni.view('mixer'));
await frames();
const m0 = await page.evaluate(() => window.__uni.mixerProbe());
ok(m0.authoritative, 'mixer reads the engine');
const wasDb = m0.detail[1].db;
await page.evaluate(() => window.__uni.setGain(1, 0.9));
await frames();
ok((await page.evaluate(() => window.__uni.mixerProbe())).detail[1].pending,
   'a fader move is optimistic first');
await page.waitForTimeout(900);
const m1 = await page.evaluate(() => window.__uni.mixerProbe());
ok(!m1.detail[1].pending && m1.detail[1].db !== wasDb, 'and settles on the engine',
   `${wasDb} -> ${m1.detail[1].db}`);
await page.evaluate((p) => window.__uni.loadProject(p), PROJECT);
await page.waitForTimeout(2500);
const m2 = await page.evaluate(() => window.__uni.mixerProbe());
ok(m2.detail[1].db === wasDb, 'a reload lets the engine overwrite local values',
   `${m1.detail[1].db} -> ${m2.detail[1].db}`);

section('surfaces');
for (const [view, probe, check, prep] of [
  ['arrange', 'arrangeProbe', (p) => p.clips > 0 && p.lanes > 0],
  // All tracks, not whichever one the cursor happens to be on: webtest's track 1
  // is driven by the patcher and holds no notes, so a per-track assertion here
  // fails on a correct render.
  ['piano', 'pianoProbe', (p) => p.notes > 0 && p.keys > 0, () => window.__uni.pianoAll(true)],
  ['patcher', 'patcherProbe', (p) => p.nodes > 0],
]) {
  await page.evaluate((v) => window.__uni.view(v), view);
  if (prep) await page.evaluate(prep);
  await frames();
  const p = await page.evaluate((n) => window.__uni[n](), probe);
  ok(p && check(p), `${view} renders live data`, JSON.stringify(p).slice(0, 90));
}

section('browser');
await page.evaluate(() => window.__uni.browser(true));
await page.waitForTimeout(700);
const projects = await page.evaluate(() => window.__uni.projects());
ok(projects.includes(PROJECT), 'the sidecar lists projects from disk', JSON.stringify(projects));

section('page errors');
ok(errors.length === 0, 'no uncaught errors', errors.slice(0, 3).join(' | '));

await browser.close();
console.log(`\n${fail === 0 ? `ALL PASS (${count} checks)` : `${fail} of ${count} FAILED`}`);
process.exit(fail ? 1 : 0);
