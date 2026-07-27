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
// It uses webtest.uniproj (maximal with device_chain emptied). NOTE: the engine
// currently loses its plugin host after roughly 38 UI commands and exits — see
// tools/repro-hang.mjs and the report to backend. One run of this test sends
// about 30, so the SECOND run usually starts against a dead engine. Restart the
// stack between runs until that is fixed; the test says so rather than
// cascading.

import { chromium } from 'playwright';
import { rmSync } from 'node:fs';
import { join } from 'node:path';

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

section('undo / redo');
const beforeUndo = (await E()).noteCount;
await run('note 71', 900);
const afterWrite = (await E()).noteCount;
ok(afterWrite !== beforeUndo, 'a write changes the note count', `${beforeUndo} -> ${afterWrite}`);
await run('undo', 1000);
ok((await E()).noteCount !== afterWrite, 'undo takes it back');
await run('redo', 1000);
ok((await E()).noteCount === afterWrite, 'redo puts it back', String(afterWrite));
await run('undo', 1000);

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
// Asserts the VALUE, not the pending flag. Optimism is observable as "the fader
// shows the new gain before the engine has answered"; the flag itself is a
// transient the local engine can clear inside two frames, so asserting on it
// made this fail about one run in four — a flaky test is worse than no test.
const mid = await page.evaluate(() => window.__uni.mixerProbe());
ok(mid.detail[1].db !== wasDb, 'a fader move shows immediately, before the engine answers',
   `${wasDb} -> ${mid.detail[1].db}`);
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

section('loop region');
await page.evaluate(() => { window.__uni.view('arrange'); window.__uni.arrangeTo(0); });
await frames();
// A REAL pointer, not the __uni shim. The first cut of this worked through the
// shim and did nothing under the mouse, because .ar-ruler was still
// pointer-events:none from when it was decorative — a listener nobody can reach
// throws no error and passes any test that calls past it.
const rulerBox = await page.evaluate(() => {
  const b = document.querySelector('.ar-ruler').getBoundingClientRect();
  return { x: b.x, y: b.y + b.height / 2 };
});
const loopBefore = await page.evaluate(() => window.__uni.loop());
await page.mouse.move(rulerBox.x + 60, rulerBox.y);
await page.mouse.down();
await page.mouse.move(rulerBox.x + 260, rulerBox.y, { steps: 8 });
const dragging = await page.evaluate(() => window.__uni.loopShown());
const engineMid = await page.evaluate(() => window.__uni.loop());
ok(dragging.start !== engineMid.start || dragging.end !== engineMid.end,
   'the bracket follows the pointer before the engine is told',
   `${JSON.stringify(dragging)} vs engine ${JSON.stringify(engineMid)}`);
ok(dragging.start % 3840000 === 0 && dragging.end % 3840000 === 0,
   'and snaps to whole bars', JSON.stringify(dragging));
await page.mouse.up();
await page.waitForTimeout(1200);
const loopAfter = await page.evaluate(() => window.__uni.loop());
ok(loopAfter.start === dragging.start && loopAfter.end === dragging.end,
   'releasing sets the engine\'s loop',
   `${JSON.stringify(loopBefore)} -> ${JSON.stringify(loopAfter)}`);
// Shift snaps finer, and a right-to-left drag is a span rather than nothing.
await page.keyboard.down('Shift');
await page.mouse.move(rulerBox.x + 430, rulerBox.y);
await page.mouse.down();
await page.mouse.move(rulerBox.x + 400, rulerBox.y, { steps: 4 });
await page.mouse.up();
await page.keyboard.up('Shift');
await page.waitForTimeout(1200);
const fineLoop = await page.evaluate(() => window.__uni.loop());
ok(fineLoop.end > fineLoop.start && (fineLoop.end - fineLoop.start) < 3840000,
   'shift-dragging backwards gives a sub-bar span, not an empty one',
   JSON.stringify(fineLoop));
const byBar = await page.evaluate(() => window.__uni.run('loop 3 7'));
await page.waitForTimeout(1200);
const barLoop = await page.evaluate(() => window.__uni.loop());
ok(barLoop.start === 2 * 3840000 && barLoop.end === 6 * 3840000,
   'the console sets it in the bar numbers the ruler prints',
   JSON.stringify(barLoop));

section('patcher editing');
await page.evaluate(() => window.__uni.view('patcher'));
await frames();
const graph = await page.evaluate(() => window.__uni.patchNodes());
const euclid = graph.find((n) => n.type === 'euclidean');
ok(!!euclid && euclid.fields.length > 0, 'a node advertises its editable fields',
   euclid ? euclid.fields.join(' ') : 'none');
const hitsWas = euclid.config[1];
// Three steps in ONE command, all sent before the engine can answer any of them.
// A nudge that re-read the engine each time would send the same value three
// times and land on +1.
await page.evaluate((id) => window.__uni.run(`patch ${id} hits 3`), euclid.id);
// One frame, not one engine round trip: the box has to show the value on the
// next paint. Asserting the VALUE rather than the pending flag, for the reason
// the mixer's equivalent does — the flag is a transient a local engine can clear
// inside two frames, and a flaky test is worse than no test.
await frames();
const optimistic = await page.evaluate(() => window.__uni.patcherProbe().configs[0]);
ok(optimistic.includes('hits ' + (hitsWas + 3)), 'the box shows the edit on the next paint',
   optimistic);
await page.waitForTimeout(1200);
const euclidAfter = await page.evaluate(() => window.__uni.patchNodes());
ok(euclidAfter.find((n) => n.id === euclid.id).config[1] === hitsWas + 3,
   'three nudges chain rather than repeating the first',
   `${hitsWas} -> ${euclidAfter.find((n) => n.id === euclid.id).config[1]}`);
// Every other value survives: the engine rebuilds the config from what it gets,
// so a field the UI dropped would be a field the engine zeroed.
ok(euclidAfter.find((n) => n.id === euclid.id).config.every((v, i) => i === 1 || v === euclid.config[i]),
   'and no other field is disturbed',
   JSON.stringify(euclidAfter.find((n) => n.id === euclid.id).config));
await page.evaluate((id) => window.__uni.run(`patch ${id} hits -99`), euclid.id);
await page.waitForTimeout(1200);
const low = await page.evaluate(() => window.__uni.patchNodes());
ok(low.find((n) => n.id === euclid.id).config[1] === 0, 'a big negative clamps at the floor',
   JSON.stringify(low.find((n) => n.id === euclid.id).config));
const refused = await page.evaluate(() => {
  try { window.__uni.patch(window.__uni.patchNodes()[0].id, 'nope', 1); return 'no throw'; }
  catch (e) { return e.message; }
});
ok(/no field/.test(refused), 'an unknown field is named, not silently ignored', refused);
const noCfg = await page.evaluate(() => {
  const plain = window.__uni.patchNodes().find((n) => !n.fields.length);
  if (!plain) return 'no such node';
  try { window.__uni.patch(plain.id, 'steps', 1); return 'no throw'; } catch (e) { return e.message; }
});
ok(/no editable configuration/.test(noCfg), 'a node without a layout refuses out loud', noCfg);
await page.evaluate(([id, n]) => window.__uni.run(`patch ${id} hits ${n}`), [euclid.id, hitsWas]);
await page.waitForTimeout(1200);

section('piano roll selection');
await page.evaluate(() => { window.__uni.view('piano'); window.__uni.pianoAll(true); });
await frames();
const picked = await page.evaluate(() => window.__uni.pianoSelect(0, 0, 120, 300));
ok(picked > 1, 'marquee selects several notes', `${picked} notes`);
const pb = await page.evaluate(() => window.__uni.pianoSelected());
await page.evaluate(() => window.__uni.pianoEdit('transpose', 12));
await page.waitForTimeout(3500);
const pa = await page.evaluate(() => window.__uni.pianoSelected());
// Both halves matter: every note moves (the batch), and the selection still
// matches them afterwards (it is keyed on position, not on the note id the
// engine reassigns when it rewrites).
ok(pa.length === pb.length && pa.every((n, i) => n.pitch === pb[i].pitch + 12),
   'the whole selection transposes and survives the edit',
   `${pb.length} notes`);
await page.evaluate(() => window.__uni.pianoEdit('transpose', -12));
await page.waitForTimeout(3500);

section('arrange interaction');
await page.evaluate(() => window.__uni.view('arrange'));
await frames();
const clipBox = await page.evaluate(() => {
  const c = document.querySelector('.ar-clip');
  if (!c) return null;
  const r = c.getBoundingClientRect();
  return { x: Math.round(r.x + r.width / 2), y: Math.round(r.y + r.height / 2) };
});
if (clipBox) {
  const tickWas = (await E()).playheadTick;
  await page.mouse.click(clipBox.x, clipBox.y);
  await page.waitForTimeout(700);
  const s1 = await page.evaluate(() => ({
    sel: window.__uni.state().selectedPlacement,
    marked: document.querySelectorAll('.ar-clip.sel').length }));
  // Clicking a clip must NOT move the playhead: dragging clips is the commoner
  // intent there and a stray seek would be a surprise.
  ok(s1.sel !== null && s1.marked === 1 && (await E()).playheadTick === tickWas,
     'clicking a clip selects it and does not seek', JSON.stringify(s1));
  await page.mouse.click(clipBox.x + 700, clipBox.y);
  await page.waitForTimeout(700);
  const s2 = await page.evaluate(() => window.__uni.state().selectedPlacement);
  ok(s2 === null && (await E()).playheadTick !== tickWas,
     'clicking empty lane seeks and clears the selection');
} else {
  ok(false, 'a clip to click');
}

section('aggregate zooms');
await page.evaluate(() => window.__uni.view('tracker'));
const aggAt = async (z) => {
  await page.evaluate((x) => window.__uni.setZoom(x), z);
  await page.waitForTimeout(900);
  await frames();
  return page.evaluate(() => ({
    engineRows: window.__uni.aggregates().rows,
    contour: document.querySelectorAll('.tk-cell[data-kind="contour"]').length,
    notes: document.querySelectorAll('.tk-cell[data-kind="note"]').length,
  }));
};
const fine = await aggAt(3), bar = await aggAt(4), fourBar = await aggAt(5);
ok(fine.contour === 0 && fine.notes > 0, 'a fine zoom draws notes, not contours');
ok(bar.contour > 0 && bar.notes === 0, 'a bar zoom draws contours, not notes');
// The bug this replaced: both aggregate zooms asked for beat resolution and got
// identical data, so a 4-bar row showed one beat of it.
ok(fourBar.engineRows === bar.engineRows * 4,
   'each coarser zoom asks for four times the beats',
   `${bar.engineRows} -> ${fourBar.engineRows}`);
ok(fourBar.contour < bar.contour,
   'and folds them into fewer rows', `${bar.contour} -> ${fourBar.contour}`);
await page.evaluate(() => window.__uni.setZoom(3));

section('piano roll drag');
await page.evaluate(() => { window.__uni.view('piano'); window.__uni.pianoAll(false);
                            window.__uni.run('goto 0 0'); });
await frames();
const firstNote = () => page.evaluate(() => {
  const ns = window.__uni.notes().filter((x) => x.tr === 0).sort((a, b) => a.t - b.t);
  return ns[0] ? { t: ns[0].t, dur: ns[0].dur, p: ns[0].p } : null;
});
const n0 = await firstNote();
const dragId = await page.evaluate(() => {
  const el = document.querySelector('.pr-note'); return el ? Number(el.dataset.id) : null; });
if (n0 && dragId !== null) {
  await page.evaluate((id) => window.__uni.pianoDrag(id, 64, -22, 'move'), dragId);
  await page.waitForTimeout(2200);
  const n1 = await firstNote();
  ok(n1.t > n0.t && n1.p === n0.p + 2 && n1.dur === n0.dur,
     'dragging a note moves it in time and pitch, keeping its length',
     `t ${n0.t}->${n1.t} pitch ${n0.p}->${n1.p} dur ${n1.dur}`);
  const id2 = await page.evaluate(() => {
    const el = document.querySelector('.pr-note'); return el ? Number(el.dataset.id) : null; });
  await page.evaluate((id) => window.__uni.pianoDrag(id, 60, 0, 'resize'), id2);
  await page.waitForTimeout(2200);
  const n2 = await firstNote();
  ok(n2.dur > n1.dur && n2.t === n1.t, 'dragging its edge changes only the length',
     `dur ${n1.dur} -> ${n2.dur}`);
} else {
  ok(false, 'a note to drag');
}

section('browser');
await page.evaluate(() => window.__uni.browser(true));
await page.waitForTimeout(700);
const projects = await page.evaluate(() => window.__uni.projects());
ok(projects.includes(PROJECT), 'the sidecar lists projects from disk', JSON.stringify(projects));
// Close it. An open rail owns the keyboard, so leaving it open makes every later
// section's keystrokes land somewhere else — which is how "scrolling stops the
// view following" failed while working perfectly by hand.
await page.evaluate(() => window.__uni.browser(false));

section('loop and load status');
const loop = await page.evaluate(() => window.__uni.loop());
ok(loop && loop.end > loop.start, 'the engine publishes a loop region',
   `${loop && loop.start} .. ${loop && loop.end}`);
await page.evaluate(() => window.__uni.view('arrange'));
await frames();
const drawn = (await page.evaluate(() => window.__uni.arrangeProbe())).loop;
ok(drawn && drawn.w > 0, 'and arrange draws it', JSON.stringify(drawn));
const ls0 = await page.evaluate(() => window.__uni.loadStatus());
ok(ls0 && ls0.ok === 1, 'a good load reports ok', JSON.stringify(ls0));
// A load the engine refuses must be distinguishable from one that worked and
// happened to produce identical content — it used to ack {"ok":true} either way.
await page.evaluate(() => window.__uni.loadProject('definitely-not-a-project'));
await page.waitForTimeout(2000);
const ls1 = await page.evaluate(() => window.__uni.loadStatus());
ok(ls1.seq > ls0.seq && ls1.ok === 0, 'a refused load reports a failure',
   JSON.stringify(ls1));
const said = await page.evaluate(() => document.querySelector('.ch-reject')?.textContent);
ok(/refused/.test(said || ''), 'and the UI says so', JSON.stringify(said));
await page.evaluate((p) => window.__uni.loadProject(p), PROJECT);
await page.waitForTimeout(2200);

section('follow the playhead');
// At the finest zooms the window is a few beats wide, so a view that does not
// follow loses the playhead within a second — which is exactly what it did.
for (const [view, probe, prep] of [
  ['tracker', null, () => { window.__uni.view('tracker'); window.__uni.setZoom(0); }],
  ['arrange', 'arrangeProbe', () => { window.__uni.view('arrange'); window.__uni.arrangeZoom(0); }],
  ['piano', 'pianoProbe', () => { window.__uni.view('piano'); window.__uni.pianoZoom(0); }],
]) {
  await page.evaluate(prep);
  await page.evaluate(() => window.__uni.follow(true));
  await run('play', 300);
  let offscreen = 0;
  for (let i = 0; i < 3; i++) {
    await page.waitForTimeout(900);
    const vis = await page.evaluate(([v, pn]) => {
      const e = window.__uni.engineState();
      if (v === 'tracker') {
        const s = window.__uni.state(), p = window.__uni.probe();
        return s.playhead >= s.start && s.playhead < s.start + p.poolSize;
      }
      const p = window.__uni[pn]();
      return e.playheadTick >= p.startTick
          && e.playheadTick <= p.startTick + p.ticksPerPixel * p.width;
    }, [view, probe]);
    if (!vis) offscreen++;
  }
  await run('stop', 300);
  ok(offscreen === 0, `${view} keeps the playhead in view`, `${offscreen}/3 off screen`);
}
await page.evaluate(() => { window.__uni.view('tracker'); window.__uni.setZoom(3); });
// Looking elsewhere during playback is deliberate and must not be overridden.
await page.keyboard.press('ArrowDown');
ok((await page.evaluate(() => window.__uni.state().followPlayhead)) === false,
   'scrolling by hand stops the view following');
await run('play', 300);
ok((await page.evaluate(() => window.__uni.state().followPlayhead)) === true,
   'and pressing play re-arms it');
await run('stop', 300);

section('save');
// Writes a real file, so it cleans up after itself — a test that leaves state on
// disk changes the behaviour of the next run of the test that lists it.
const SCRATCH = 'e2e-scratch';
await page.evaluate((n) => window.__uni.saveAs(n), SCRATCH);
await page.waitForTimeout(2000);
await page.evaluate(() => window.__uni.browser(true));
await page.waitForTimeout(1200);
const listed = await page.evaluate(() => window.__uni.projects());
ok(listed.includes(SCRATCH), 'a saved project appears in the browser', JSON.stringify(listed));
// Exactly the name it wrote, nothing globbed: a test that deletes by pattern in
// a directory holding the user's projects is a bad trade for tidiness.
const PROJECTS = process.env.UNI_PROJECTS
  || '/Users/jak/src/daw-web/presets/projects';
for (const suffix of ['.uniproj.json', '.uniproj.state']) {
  rmSync(join(PROJECTS, SCRATCH + suffix), { recursive: true, force: true });
}
await page.evaluate(() => window.__uni.browser(false));

section('dock');
const bad = await page.evaluate(() => window.__uni.run('note 999'));
ok(String(bad).includes('out of range'), 'the dock reports a bad argument', String(bad));
const unknown = await page.evaluate(() => window.__uni.run('flurb'));
ok(String(unknown).includes('unknown'), 'and an unknown command', String(unknown));
const help = await page.evaluate(() => window.__uni.dockProbe().commands);
ok(help.includes('note') && help.includes('gain') && help.includes('view'),
   `the grammar spans editing, mixing and navigation: ${help.length} commands`);
// Requirement (d) is that an agent can do what a user can. Every keyboard action
// that changes something needs a command, or the console is a subset.
for (const c of ['follow', 'rename', 'select', 'transpose', 'copy', 'paste', 'cut']) {
  ok(help.includes(c), `the console can ${c}`);
}

section('page errors');
ok(errors.length === 0, 'no uncaught errors', errors.slice(0, 3).join(' | '));

await browser.close();
console.log(`\n${fail === 0 ? `ALL PASS (${count} checks)` : `${fail} of ${count} FAILED`}`);
process.exit(fail ? 1 : 0);
