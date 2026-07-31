#!/usr/bin/env node
// Frame work against a REAL-SIZED song, on a live engine.
//
//   node test/scale.mjs [project]        (default stress-128)
//
// test/frametime.mjs measures the draw path against fixtures — a piano roll with
// THIRTEEN notes and a tracker of 46 rows. It is a good regression test and it
// says almost nothing about requirement 2 ("the UI must not lag"), because
// nothing it measures resembles a song. The bundled projects are no better: both
// are 158 notes over four bars.
//
// This loads a project with tens of thousands of notes into the engine and
// measures the same thing the fixture test does — the work of one draw — while
// scrolling and zooming each surface. The interesting number is not the absolute
// cost but where it stops scaling: a draw whose cost tracks the SONG rather than
// the VIEWPORT is a design error that a small fixture cannot show.
//
// It needs a live stack (tools/webstack.sh) and it is not part of verify.sh:
// it depends on an engine, and a test that fails because something else is not
// running is a test people learn to ignore.

import { chromium } from 'playwright';

const PROJECT = process.argv[2] || 'stress-128';
/** Bars of material the project actually has, so scrolling stays inside it. */
const BARS = Number(process.argv[3] || (PROJECT === 'stress-512' ? 512 : 128));
const BAR = 3840000;
const BUDGET = 16.6;
/**
 * A STACK YOU STARTED YOURSELF, at the default port. This suite does not spawn one — it is a
 * hand-run measurement against a session you are already looking at, which is the point: the
 * numbers mean something because the engine has real plugins in it.
 *
 * Said here because running it with nothing on 8173 used to die with "Cannot read properties
 * of null" from inside a wait loop, which reads as a broken app rather than a missing stack.
 * It now says what is absent.
 */
const PAGE = 'http://127.0.0.1:8173/index.html';

const br = await chromium.launch({ channel: 'chrome', headless: false });
const page = await br.newPage({ viewport: { width: 1500, height: 760 }, deviceScaleFactor: 2 });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(PAGE);
await page.waitForFunction(() => !!window.__uni, null, { timeout: 20000 });
await page.evaluate(() => document.fonts.ready);

console.log(`\n  frame work on ${PROJECT}, budget ${BUDGET} ms\n`);

const t0 = Date.now();
await page.evaluate((p) => window.__uni.loadProject(p), PROJECT);
// Wait for the engine to actually publish the new material rather than sleeping.
let notes = 0;
for (let i = 0; i < 160; i++) {
  await page.waitForTimeout(250);
  const n = await page.evaluate(() => window.__uni.engineStats().lastSeq);
  /*
   * `loadStatus()` IS NULL until the first frame arrives, and this dereferenced it — so on any
   * run where the socket takes longer than 250ms to deliver a frame, the suite died with
   * "Cannot read properties of null" before making a single assertion. Not a failure about the
   * app: a crash in the waiting.
   */
  const st = await page.evaluate(() => window.__uni.loadStatus());
  if (st && st.seq > 0 && st.ok && i > 8) { notes = n; break; }
}
const loadMs = Date.now() - t0;
const stat = await page.evaluate(() => ({
  reject: window.__uni.state().reject,
  load: window.__uni.loadStatus(),
}));
// Likewise here: an absent status is "the engine never spoke", which is a different report
// from "the load was refused" and must not read as a crash.
if (!stat.load) {
  console.log('  BLOCKED  the engine published no frame at all — nothing to measure.');
  console.log('           This suite needs a stack you started: tools/webstack.sh, then rerun.');
  process.exit(1);
}
if (stat.load.ok !== 1) {
  console.log(`  the engine refused ${PROJECT}: ${JSON.stringify(stat)}`);
  await br.close();
  process.exit(2);
}
console.log(`  loaded in ${(loadMs / 1000).toFixed(1)}s  ${JSON.stringify(stat.load)}\n`);

/**
 * Time `src` over n synchronous draws. Synchronous so this measures OUR draw.
 *
 * `src` is the BODY of a function of `i`, as a string. The fixture test passes
 * arrow functions and rewrites their source, which cannot express a loop bound
 * that depends on the project — and a rewritten `new Function` produced source
 * that did not parse. A string is what this actually is; pretending otherwise
 * bought nothing.
 */
async function time(src, n = 300) {
  return page.evaluate(([fn, count]) => {
    const f = new Function('i', fn);
    for (let i = 0; i < 60; i++) f(i);            // warm
    const t = new Float64Array(count);
    for (let i = 0; i < count; i++) {
      const a = performance.now();
      f(i);
      t[i] = performance.now() - a;
    }
    const s = Array.from(t).sort((x, y) => x - y);
    return { p50: s[Math.floor(count * 0.5)], p95: s[Math.floor(count * 0.95)], max: s[count - 1] };
  }, [src, n]);
}

let fail = 0;
const check = async (label, setup, src, limit = BUDGET) => {
  if (setup) { await page.evaluate(setup); await page.waitForTimeout(600); }
  const r = await time(src);
  const ok = r.p95 <= limit;
  if (!ok) fail++;
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${label.padEnd(34)}`
            + ` p50 ${r.p50.toFixed(2)}  p95 ${r.p95.toFixed(2)}  max ${r.max.toFixed(2)} ms`
            + `   (limit ${limit})`);
  return r;
};

await check('tracker, scrolling a long song',
  () => window.__uni.view('tracker'),
  `window.__uni.scrollTo((i * 17) % ${BARS * 16})`);
await check('tracker, zooming',
  null,
  'window.__uni.setZoom(i % 6)');
await check('arrange, scrolling time',
  () => window.__uni.view('arrange'),
  `window.__uni.arrangeTo(((i * 7) % ${BARS}) * ${BAR})`);
await check('arrange, zooming',
  null,
  'window.__uni.arrangeZoom(i % 6)');
await check('piano roll, scrolling time',
  () => { window.__uni.view('piano'); window.__uni.pianoAll(true); },
  `window.__uni.pianoTo(((i * 7) % ${BARS}) * ${BAR})`);
await check('piano roll, zooming',
  null,
  'window.__uni.pianoZoom(i % 6)');
await check('mixer, full redraw',
  () => window.__uni.view('mixer'),
  'window.__uni.setPan(i % 8, ((i * 37) % 2000) - 1000); window.__uni.redraw()');

// What the surfaces actually held, so a fast number that drew nothing is caught.
// Measured at the START, where the material certainly is: the loops above leave
// the view wherever the last iteration put it, and the first version of this
// check read an empty screen and called seven surfaces fast.
const drew = await (async () => {
  await page.evaluate(() => { window.__uni.view('piano'); window.__uni.pianoAll(true);
                              window.__uni.pianoTo(0); window.__uni.pianoFit(); });
  await page.waitForTimeout(800);
  const piano = await page.evaluate(() => window.__uni.pianoProbe().notes);
  await page.evaluate(() => { window.__uni.view('arrange'); window.__uni.arrangeTo(0); });
  await page.waitForTimeout(800);
  const arrange = await page.evaluate(() => window.__uni.arrangeProbe().clips);
  return { piano, arrange };
})();
console.log(`\n  drew: piano ${drew.piano} notes in view, arrange ${drew.arrange} clips`);
if (!drew.piano) { console.log('  the piano roll drew NOTHING — a fast draw of an empty surface is not a result'); fail++; }

if (errors.length) { console.log(`\n  page errors: ${errors.slice(0, 3).join(' | ')}`); fail++; }
await br.close();
console.log(`\n${fail === 0 ? 'ALL PASS' : `${fail} FAILURES`}`);
process.exit(fail ? 1 : 0);
