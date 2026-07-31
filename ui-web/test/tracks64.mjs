#!/usr/bin/env node
/**
 * WHAT HAPPENS AT SIXTY-FOUR TRACKS.
 *
 * Every fixture in this repo has six tracks or fewer — including the three called "stress",
 * which stress the NOTE dimension and say nothing about the track one. The engine's limit is 64.
 * So the surfaces that draw a node per track have never been looked at anywhere near the size
 * they are allowed to reach, and "the UI must not lag" is a requirement, not a preference.
 *
 * This is the same blindness backend hit from the other side and described in as many words: a
 * kit read-back that returned the previous answer survived because every fixture has ONE sampler
 * track, and with one track the previous answer and the current one are the same kit. A fixture
 * that never varies a dimension cannot find a bug that lives in it.
 *
 * WHAT IT MEASURES, and why node count rather than milliseconds: a frame time is a statement
 * about this laptop on this morning, and it moves with whatever else is running. The number of
 * DOM nodes a surface builds for a song is a statement about the DESIGN — it is the thing that
 * says whether a draw's cost tracks the viewport or the song, which is the distinction
 * scale.mjs was written to protect and the one that decides whether a surface needs
 * virtualizing. The tracker already virtualizes; this asks whether the others must.
 */

import { chromium } from 'playwright';
import { writeFileSync } from 'node:fs';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const TRACKS = 64;                 // kUiMaxTracks
/*
 * THE SECOND SIZE, WHICH IS THE POINT.
 *
 * One measurement is an absolute number and a budget is a guess about a laptop. TWO sizes measure
 * the SLOPE, which is a fact about the design: a surface whose node count quadruples when the
 * track count quadruples is drawing the SONG, and one that barely moves is drawing the VIEWPORT.
 * That distinction is what decides whether a surface needs virtualizing, and it does not depend
 * on how fast this machine is or what else is running on it.
 */
const FEW = 16;
/*
 * THE BUDGET. Chosen as a number a browser lays out comfortably rather than derived from
 * anything — a few thousand nodes is a frame's work, tens of thousands is a stutter. It is here
 * to fail LOUDLY when a surface's node count starts tracking the song, not to be precise.
 */
const BUDGET = 12000;

const stack = await startStack({ numBlocks: 8 });

const Q = 960000;
const clip = (id) => ({
  id, name: `c${id}`, length: Q * 64, lines_per_beat: 4, kind: 'symbolic',
  time_sig_numerator: 4, time_sig_denominator: 4,
  // A few notes each, spread over the clip: enough that every track has something to draw in
  // every surface, few enough that this measures the TRACK dimension and not the note one.
  notes: [0, 1, 2, 3, 4, 5, 6, 7].map((i) => ({
    nanotick: i * Q * 2, duration: Q / 2, pitch: 48 + ((id * 3 + i * 5) % 24), velocity: 100,
    column: 0, note_id: id * 100 + i + 1,
  })),
  chords: [],
});
const writeSong = (name, tracks) => {
  const doc = {
    schema_version: 4, meta: { name, created_utc: 0, modified_utc: 0 },
    timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
    nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
    harmony_timeline: [], clips: [], tracks: [],
  };
  for (let t = 0; t < tracks; t++) {
    doc.clips.push(clip(t + 1));
    doc.tracks.push({
      track_id: t, name: `T${t}`, harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
      // NO PLUGINS. Sixty-four plugin hosts is a test of the host launcher, not of the drawing,
      // and it would take minutes to start. The surfaces draw a strip per track either way.
      device_chain: [], mod_links: [],
      placements: [{ clip_id: t + 1, at: 0, length: Q * 64, notes: [], chords: [], mutes: [] }],
    });
  }
  writeFileSync(`${stack.dir}/${name}.uniproj.json`, JSON.stringify(doc));
};
writeSong('tracksfew', FEW);
writeSong('tracks64', TRACKS);

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 })
  .catch(() => {});
await page.waitForTimeout(1200);

// Wait for the TRACK COUNT rather than a delay: the whole test is about how many there are.
await page.evaluate(() => window.__uni.loadProject('tracks64'));
await page.waitForFunction((n) => (window.__uni.state() || {}).tracks >= n, TRACKS,
                           { timeout: 30000 }).catch(() => {});
const seen = await page.evaluate(() => (window.__uni.state() || {}).tracks);
check(seen === TRACKS, `the engine loaded all ${TRACKS} tracks`, `tracks=${seen}`);

/*
 * COUNT EACH SURFACE'S OWN SUBTREE, NOT THE DOCUMENT.
 *
 * The first version counted `document.querySelectorAll('*')`, which is one number with one
 * meaning — and the wrong one. Surfaces build their DOM lazily and KEEP it when hidden, so the
 * document total is cumulative over every view visited: measuring tracker, then mixer, then
 * arrange gave 5991, 10455, 10455 — the mixer and arrange figures identical to the node,
 * which is the tell. It was reporting the running total of everything drawn so far and
 * attributing it to whichever surface happened to be last.
 *
 * Each surface owns a host element, so ask that. The document total is still worth printing —
 * it is what the browser actually pays for — but as its own number rather than as a surface's.
 */
const HOSTS = { tracker: '#tracker', mixer: '#mixer', arrange: '#arrange' };
const measure = async (view) => {
  await page.evaluate((v) => window.__uni.setView(v), view);
  await page.waitForTimeout(900);
  return page.evaluate((sel) => {
    const host = document.querySelector(sel);
    return { own: host ? host.querySelectorAll('*').length : -1,
             doc: document.querySelectorAll('*').length };
  }, HOSTS[view]);
};

const VIEWS = ['tracker', 'mixer', 'arrange'];
const at = {};
for (const [name, n] of [['tracksfew', FEW], ['tracks64', TRACKS]]) {
  await page.evaluate((p) => window.__uni.loadProject(p), name);
  await page.waitForFunction((k) => (window.__uni.state() || {}).tracks === k, n,
                             { timeout: 30000 }).catch(() => {});
  at[n] = {};
  for (const view of VIEWS) {
    const m = await measure(view);
    at[n][view] = m.own;
    at[n]._doc = m.doc;              // the page total after the last surface, for context
  }
}

console.log(`\n  DOM nodes, ${FEW} tracks -> ${TRACKS} tracks (budget ${BUDGET})\n`);
for (const view of VIEWS) {
  const a = at[FEW][view], b = at[TRACKS][view];
  console.log(`    ${view.padEnd(9)} ${String(a).padStart(6)} -> ${String(b).padStart(6)}` +
              `   x${(b / Math.max(1, a)).toFixed(2)} for x${TRACKS / FEW} the tracks`);
}
console.log('');

console.log(`    ${'(whole page)'.padEnd(9)} ${String(at[FEW]._doc).padStart(6)} -> ` +
            `${String(at[TRACKS]._doc).padStart(6)}   every surface that has been drawn\n`);

for (const view of VIEWS) {
  check(at[TRACKS][view] < BUDGET, `${view} stays under the node budget at ${TRACKS} tracks`,
        `${at[TRACKS][view]} nodes against a budget of ${BUDGET}`);
}

/*
 * THE TRACKER IS THE WORKED EXAMPLE. It virtualizes — sequential slots with lead and tail
 * spacers — so quadrupling the tracks must barely move its node count. Asserted rather than
 * merely printed, because it is the property the virtualization exists to provide and the one a
 * later change would silently take away.
 */
const trackerRatio = at[TRACKS].tracker / Math.max(1, at[FEW].tracker);
check(trackerRatio < 1.5,
      'the tracker draws the VIEWPORT: four times the tracks is not four times the nodes',
      `x${trackerRatio.toFixed(2)} for x${TRACKS / FEW} the tracks ` +
      `(${at[FEW].tracker} -> ${at[TRACKS].tracker})`);

/*
 * AND THE OTHER TWO ARE NOT VIRTUALIZED, which this states out loud rather than leaving as a
 * surprise. They pass the budget today at ~85% of it, and they pass it because 64 tracks is the
 * engine's limit — not because the design is bounded. A surface at 85% of budget has room for
 * one more feature per strip, and the budget is a guess about a laptop.
 */
for (const view of ['mixer', 'arrange']) {
  const r = at[TRACKS][view] / Math.max(1, at[FEW][view]);
  console.log(`  NOTE  ${view} scales x${r.toFixed(2)} with the track count ` +
              `(${at[TRACKS][view]} nodes, ${Math.round(100 * at[TRACKS][view] / BUDGET)}% of budget)` +
              (r > 2 ? ' — drawing the song, not the viewport' : ''));
}

/*
 * DOES A SURFACE KNOW HOW BIG IT IS?
 *
 * Each one reports a `domNodes` of its own, and those numbers are what a probe or a future perf
 * check would reach for. The mixer computes its as `pool.length * 12` — a per-strip constant
 * written down once and never re-measured — so if a strip has grown since, the surface reports a
 * size several times under the truth. That is worse than not reporting one: a scaling problem
 * looks solved when the number that would have shown it is the one that is wrong.
 *
 * Compared against the host subtree, which is the count the browser actually pays for.
 */
{
  await page.evaluate(() => window.__uni.setView('mixer'));
  await page.waitForTimeout(900);
  const said = await page.evaluate(() => {
    const p = window.__uni.mixerProbe ? window.__uni.mixerProbe() : null;
    return p && p.domNodes !== undefined ? p.domNodes : null;
  });
  const real = at[TRACKS].mixer;
  const ratio = said ? real / said : 0;
  console.log(`  NOTE  mixer reports ${said} nodes, its host holds ${real} — x${ratio.toFixed(1)}`);
  /*
   * ARRANGE TOO, since it estimates the same way — three pool lengths added up. Reported rather
   * than asserted: shot.mjs already bounds arrange's own number, and two tests asserting on the
   * same figure from different definitions is how a bound comes to mean nothing.
   */
  await page.evaluate(() => window.__uni.setView('arrange'));
  await page.waitForTimeout(700);
  const aSaid = await page.evaluate(() => {
    const p = window.__uni.arrangeProbe ? window.__uni.arrangeProbe() : null;
    return p && p.domNodes !== undefined ? p.domNodes : null;
  });
  console.log(`  NOTE  arrange reports ${aSaid} nodes, its host holds ${at[TRACKS].arrange}`);

  check(said !== null && ratio < 2,
        "the mixer's own node count is close to what it actually built",
        `it reports ${said} and holds ${real} (x${ratio.toFixed(1)}) — the report is ` +
        `pool.length * 12, a per-strip constant that stops being true when a strip gains a row`);
}

check(errors.length === 0, 'nothing threw while drawing any of them', errors.slice(0, 3).join(' | '));

await browser.close();
stack.stop();
console.log(fail ? `\n${fail} of ${pass + fail} FAILED` : `\nALL PASS (${pass})`);
process.exit(fail ? 1 : 0);
