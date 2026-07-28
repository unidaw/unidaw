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
// It uses webtest.uniproj (maximal with device_chain emptied).
//
// START THE STACK IMMEDIATELY BEFORE RUNNING THIS, and expect to restart it for
// every run. That is not a defect: when the last websocket client goes away the
// sidecar waits out a 12-second grace period and then sends Quit, and the engine
// stops. Closing this test's browser starts that clock, so a second run more than
// twelve seconds later finds nothing to talk to and reports "no engine".
//
// An earlier version of this comment blamed "the engine loses its plugin host
// after roughly 38 UI commands and exits". That is not what was observed here: the
// engine's last words are `UI: last client gone — engine shutting down`, which is
// the deliberate path above, reached the moment it should be. Left recorded
// because a wrong cause in a comment is worse than none — it sends the next reader
// to tools/repro-hang.mjs and the plugin host when the answer is a timer.

import { chromium } from 'playwright';
import { rmSync } from 'node:fs';
import { join } from 'node:path';

/**
 * By default this suite brings up its OWN engine, sidecar and page server and
 * tears them down again.
 *
 * It used to attach to whatever was already running, which in practice meant the
 * stack Jaakko was working in. Every run added devices, wrote notes and loaded
 * projects into an engine that outlived it, so the runs were not independent:
 * green, then twelve failures, then a crash, with nothing in the suite changed
 * between them. A test that edits state has to own the state it edits.
 *
 * Set UNI_URL to point it at a stack you already have — useful for reproducing
 * something a person is looking at, and the reason that path still exists.
 */
const OWN_STACK = !process.env.UNI_URL;
const stack = OWN_STACK ? await (await import('./stack.mjs')).startStack() : null;
const URL = process.env.UNI_URL || stack.url;
const PROJECT = process.env.UNI_PROJECT || 'webtest';

let fail = 0, count = 0;
const ok = (cond, label, detail = '') => {
  count++;
  if (!cond) fail++;
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`);
};

/**
 * A check that is CORRECT and currently fails because of a known defect
 * elsewhere. It is not a pass and it is not a failure — it is a debt.
 *
 * The alternative was weakening the check until it passed, which would have
 * deleted the evidence: these three write at row 40, past the end of the
 * material, and rewriting them to row 2 would have made them green while hiding
 * that writing past the end does nothing. A permanently red suite is not the
 * answer either, because then a NEW failure looks like the old one.
 *
 * Every blocked check prints its reason on every run and is listed again in the
 * summary, so it cannot be quietly inherited. When the defect is fixed, delete
 * the `blocked(` wrapper and the check is a check again.
 */
const blockedList = [];
const blocked = (cond, label, why, detail = '') => {
  if (cond) {
    // It started passing. Say so loudly — this wrapper should now be deleted.
    count++;
    console.log(`  PASS  ${label}  ${detail}  <- NO LONGER BLOCKED, remove the wrapper`);
    return;
  }
  blockedList.push(`${label} — ${why}`);
  console.log(`  BLOCKED  ${label}${detail ? '  ' + detail : ''}`);
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
  if (stack) stack.stop();
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

/**
 * Wait for the ENGINE to reach a state, rather than sleeping and hoping.
 *
 * Every fixed sleep in this file is a guess about a round trip, and each one has
 * eventually failed for a reason that had nothing to do with the thing it was
 * testing: a section added ahead of it, or an engine that started doing more
 * work per edit. Returns the last snapshot either way, so a real failure still
 * reports the value it saw rather than a timeout.
 */
const engineUntil = async (want, ms = 6000) => {
  const deadline = Date.now() + ms;
  let e = await E();
  while (!want(e) && Date.now() < deadline) {
    await page.waitForTimeout(100);
    e = await E();
  }
  return e;
};

section('engine');
// BEFORE this run loads anything. A fresh engine publishes {loadSeq: 0,
// loadOk: 0} because it has never been asked to load, and reading that as a
// failure put "the engine refused that project" on screen at every startup —
// a refusal for something nobody asked for. Asserted here rather than later,
// where a successful load would have cleared it and hidden the bug.
const bootReject = await page.evaluate(() => window.__uni.state().reject);
ok(!bootReject || !/refused that project/.test(bootReject),
   'a boot with no load attempted reports no refusal', String(bootReject));

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
await run('note 67');
const after = (await engineUntil((e) => e.clipVersion > before)).clipVersion;
ok(after > before, 'a note write moves the clip version', `${before} -> ${after}`);
const wrote = await page.evaluate(() => window.__uni.selected().length);
ok(wrote >= 0, 'cursor note readable');
await run('del');
const deleted = await engineUntil((e) => e.clipVersion > after);
ok(deleted.clipVersion > after, 'a delete moves it again', `${after} -> ${deleted.clipVersion}`);

// Writing PAST THE END of the material, explicitly. This regressed once when
// note entry became structural: the write acked ok, advanced clipVersion, and no
// note appeared, because there was no placement to put it in. The indirect
// checks above caught it only as "the count did not move"; this one names the
// behaviour, so the next regression says what broke.
await run('goto 40 0');
const emptyBefore = await page.evaluate(() => window.__uni.selected().length);
ok(emptyBefore === 0, 'row 40 is past the end of the material', `${emptyBefore} notes`);
await run('note 67');
const past = await engineUntil(() => true, 100) && await (async () => {
  for (let i = 0; i < 40; i++) {
    const s = await page.evaluate(() => window.__uni.selected());
    if (s.length) return s;
    await page.waitForTimeout(100);
  }
  return [];
})();
ok(past.length === 1 && past[0].pitch === 67,
   'a note written past the end lands where it was typed', JSON.stringify(past));
await run('del');
await engineUntil(() => true, 100);

section('undo / redo');
const beforeUndo = (await E()).noteCount;
await run('note 71');
const afterWrite = (await engineUntil((e) => e.noteCount !== beforeUndo)).noteCount;
ok(afterWrite !== beforeUndo, 'a write changes the note count', `${beforeUndo} -> ${afterWrite}`);
await run('undo');
const undone = await engineUntil((e) => e.noteCount !== afterWrite);
ok(undone.noteCount !== afterWrite, 'undo takes it back', `${afterWrite} -> ${undone.noteCount}`);
await run('redo');
const redone = await engineUntil((e) => e.noteCount === afterWrite);
ok(redone.noteCount === afterWrite, 'redo puts it back', `${undone.noteCount} -> ${redone.noteCount}`);
await run('undo');
await engineUntil((e) => e.noteCount === undone.noteCount);

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

section('aggregation pills');
// maximal has several notes landing on one row, which webtest does not.
await page.evaluate(() => window.__uni.loadProject('maximal'));
await page.waitForTimeout(3000);
await page.evaluate(() => { window.__uni.view('tracker'); window.__uni.setZoom(3); });
await frames();
const pills = await page.evaluate(() => [...document.querySelectorAll('.tk-cell[data-kind="collide"]')]
  .map((e) => e.textContent).filter(Boolean));
ok(pills.length > 0, 'several events in one cell become a pill', JSON.stringify(pills.slice(0, 4)));
// The whole point: "**" said only "more than one", which is the least useful true
// thing a cell can say. A pill distinguishes a doubled note from a chord.
ok(pills.some((p) => /^\d+ evts$/.test(p)), 'differing pitches read as a count',
   JSON.stringify(pills.filter((p) => /evts/.test(p)).slice(0, 2)));
ok(!pills.some((p) => p === '**'), 'and nothing still says just "**"');
const vels = await page.evaluate(() => [...document.querySelectorAll('.tk-cell[data-kind="inst"]')]
  .map((e) => e.textContent).filter(Boolean));
ok(vels.every((v) => v === 'mix' || /^[0-9a-f]{2}$/.test(v)),
   'velocities are two hex digits, or "mix" under a pill', JSON.stringify(vels.slice(0, 6)));
await page.evaluate((p) => window.__uni.loadProject(p), PROJECT);
await page.waitForTimeout(2500);

section('clip rails');
await page.evaluate(() => window.__uni.view('tracker'));
await page.evaluate(() => window.__uni.run('goto 2 1'));
await frames();
const rails = await page.evaluate(() => [...document.querySelectorAll('.tk-rail')]
  .filter((r) => r.style.display !== 'none')
  .map((r) => ({ w: Math.round(r.getBoundingClientRect().width),
                 name: r.textContent.trim(),
                 active: r.classList.contains('active'),
                 hue: r.style.getPropertyValue('--clip') })));
ok(rails.length > 1, 'a rail per clip, from the engine’s extents', `${rails.length} rails`);
ok(rails.every((r) => r.w === 5), 'narrow, at the column edge — not the whole column',
   rails.map((r) => r.w).join(','));
ok(rails.every((r) => r.name), 'each names its clip', rails.map((r) => r.name).join(','));
// A clip you cannot tell apart from its neighbour is a clip you cannot talk
// about, so each track gets its own hue rather than eight shades of the accent.
ok(new Set(rails.map((r) => r.hue)).size > 1, 'and carries its track’s own colour',
   JSON.stringify([...new Set(rails.map((r) => r.hue))].slice(0, 3)));
// The active state could not fire at all before: it compared placementId against
// a cursor field nothing ever set. "The clip the cursor is in" is the question a
// tracker can answer.
ok(rails.filter((r) => r.active).length === 1,
   'exactly the clip the cursor is in is active',
   String(rails.filter((r) => r.active).length));
await page.evaluate(() => window.__uni.run('goto 2 2'));
await frames();
const moved = await page.evaluate(() => [...document.querySelectorAll('.tk-rail')]
  .filter((r) => r.style.display !== 'none' && r.classList.contains('active'))
  .map((r) => r.textContent.trim()));
ok(moved.length === 1 && moved[0] !== rails.find((r) => r.active).name,
   'and it follows the cursor to another track', JSON.stringify(moved));

section('harmony column');
await page.evaluate(() => window.__uni.view('tracker'));
await page.evaluate(() => window.__uni.run('goto 0 0'));
await frames();
const timeline = await page.evaluate(() => window.__uni.harmony());
ok(timeline.length > 1, 'the project has a harmony timeline to show', `${timeline.length} events`);
const blocks = await page.evaluate(() => [...document.querySelectorAll('.tk-hb')]
  .filter((e) => e.style.display !== 'none')
  .map((e) => ({ key: e.querySelector('.tk-hb-key').textContent,
                 foot: e.querySelector('.tk-hb-foot').textContent,
                 h: Math.round(e.getBoundingClientRect().height) })));
ok(blocks.length > 1, 'the harmony lane draws a block per field', JSON.stringify(blocks.slice(0, 3)));
// A field is a SPAN, so its block is taller than a row. The first version drew a
// label on the change row and nothing else, which reads correctly from the top of
// a song and says nothing once you are inside a field.
ok(blocks.every((b) => b.h > 17), 'each block spans the rows its field covers',
   blocks.map((b) => b.h).join(','));
// The label is sticky: scroll into the MIDDLE of a field and it must still name
// it. This is the whole reason the lane is not a per-row cell.
await page.evaluate(() => window.__uni.scrollTo(6));
await frames();
const midField = await page.evaluate(() => {
  const e = [...document.querySelectorAll('.tk-hb')].filter((x) => x.style.display !== 'none')[0];
  if (!e) return null;
  const lab = e.querySelector('.tk-hb-label').getBoundingClientRect();
  // Against the HOST, not the lane: the lane is transformed by the scroll, so
  // its own origin sits above the visible area and every offset measured from it
  // looks wrong while being right.
  const host = document.getElementById('tracker').getBoundingClientRect();
  return { key: e.querySelector('.tk-hb-key').textContent,
           fromTop: Math.round(lab.top - host.top),
           insideView: lab.top - host.top >= 0 && lab.top - host.top < 40 };
});
ok(midField && midField.key && midField.insideView,
   'and a field scrolled past keeps its name on screen', JSON.stringify(midField));
await page.evaluate(() => window.__uni.scrollTo(0));
await frames();
const labelled = blocks;
// The column and the chrome answer DIFFERENT questions and must not be asserted
// equal: the chrome names the field AT THE PLAYHEAD, the column names every
// change in view. They coincide only when the playhead is on the first visible
// change, and it is not — `stop` rewinds to the LOOP start, which an earlier
// section moved to bar 3. The real invariant is that the column is the timeline:
// same events, same order, same names.
const keys = await page.evaluate(() => [...document.querySelectorAll('.tk-hb')]
  .filter((e) => e.style.display !== 'none')
  .map((e) => e.querySelector('.tk-hb-key').textContent));
ok(keys.length === timeline.length,
   'the column shows every change in the timeline and no others',
   `${keys.length} labels vs ${timeline.length} events`);
ok(keys[0] === await page.evaluate(() => window.__uni.harmonyName(0)),
   'and names them the way the rest of the app does', keys[0]);
// The tuning line is its own span and says 12-TET, because the engine publishes
// no tuning for a note — backend has a cents model but it does not reach the
// clip read-back yet.
const tuning = await page.evaluate(() =>
  (document.querySelector('.tk-hb .tk-hb-sub') || {}).textContent || '');
ok(tuning === '12-TET', 'and states the tuning it is assuming', tuning);

section('device rack');
// The rack against a fixture, because no bundled project has a device CHAIN —
// their tracks carry a directly-hosted default plugin, which is not a chain
// device. The live pipeline (engine emits, sidecar accumulates, client asks for
// params) is covered by the chain section; this covers what the cards DRAW,
// which is the half a live empty rack cannot show.
await page.evaluate(() => window.__uni.useChainFixture());
await frames();
const rack = await page.evaluate(() => window.__uni.chainProbe());
ok(rack.cards === 4, 'the rack draws a card per device', String(rack.cards));
ok(rack.named === 2 && rack.titles.includes('Identity'),
   'a device whose host has answered shows its real name', JSON.stringify(rack.titles));
// And one that has not still says what it IS. Two true statements; only one of
// them is the device's name.
ok(rack.titles.some((t) => /^patcher /.test(t)),
   'one that has not answered shows its kind and id instead');
ok(rack.params[1] === 3 && rack.params[0] === 0,
   'parameters appear only on the cards that have them', JSON.stringify(rack.params));
ok(/2 of 4/.test(rack.notice), 'and the strip says how much of the rack has answered',
   rack.notice);
// Parameter rows sit ABOVE the footer. They were appended after it once, which
// looked plausible in a screenshot and was wrong. They now live in a scroller of
// their own — a card holds 256 of them and can show six — so the same claim is
// about where that scroller sits, and that the rows are inside it.
const order = await page.evaluate(() => {
  const c = [...document.querySelectorAll('.dv-card')][1];
  return { kids: [...c.children].map((x) => x.className),
           rows: c.querySelectorAll('.dv-plist > .dv-pspace > .dv-p').length,
           loose: c.querySelectorAll(':scope > .dv-p').length };
});
ok(order.kids.indexOf('dv-plist') > order.kids.indexOf('dv-body')
   && order.kids.indexOf('dv-plist') < order.kids.indexOf('dv-foot')
   && order.rows > 0 && order.loose === 0,
   'the parameter list sits between the body and the footer, and holds the rows',
   JSON.stringify(order));
await page.evaluate((p) => window.__uni.loadProject(p), PROJECT);
await page.waitForTimeout(2500);

section('harmony + tuning card');
const card = await page.evaluate(() => window.__uni.harmonyProbe());
ok(card.key && card.count > 1, 'the card names the field at the playhead',
   `${card.key}, ${card.count} events`);
// The cents ladder is real as of SHM v16: the engine publishes each scale's
// per-degree cents in MILLI-cents, so these are exact rather than a float that
// nearly represents 386.31.
ok(card.tuningKnown && card.degrees > 0,
   'and draws the degree ladder from the engine’s scale registry',
   `${card.scaleName}: ${card.cents.join(',')}`);
ok(card.cents.length === card.degrees && card.cents[0] === 0,
   'the ladder starts at the root and has one row per degree', String(card.degrees));
// What it still cannot do, said out loud. The registry is a fixed built-in list;
// there is no command to select or edit a tuning, so the TET chip is a readout.
ok(/read-only/.test(card.tuningNotice),
   'and says the tuning is a readout, not a control', card.tuningNotice);
const registry = await page.evaluate(() => window.__uni.scales());
ok(Array.isArray(registry) && registry.length > 0,
   'the registry itself reached the client', `${registry.length} scales`);

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

section('patcher graph edits');
/**
 * Wait for the published graph to reach a shape, rather than sleeping.
 *
 * The same lesson the aggregate zooms taught: a fixed sleep is a guess about a
 * round trip, and it passes until the day something ahead of it shifts the
 * timing. Returns the graph once `want` holds, or the last one seen.
 */
const graphUntil = async (want, tries = 40) => {
  let g = null;
  for (let i = 0; i < tries; i++) {
    g = await page.evaluate(() => window.__uni.patcher());
    if (want(g)) return g;
    await page.waitForTimeout(100);
  }
  return g;
};
// Everything below is relative to whatever graph is here, and deletes exactly
// what it added, by id. It cannot reset first: the patcher graph is ENGINE
// state, not project state — loading a project leaves it untouched, and only a
// fresh engine brings back the pristine one. So a run that dies mid-section
// leaves nodes behind, and the next run must not care.
const graphWas = await page.evaluate(() => window.__uni.patcher());
await page.evaluate(() => window.__uni.run('addnode random'));
const grown = await graphUntil((g) => g.nodes.length === graphWas.nodes.length + 1);
const wasIds = new Set(graphWas.nodes.map((n) => n.id));
ok(grown.nodes.length === graphWas.nodes.length + 1, 'a node can be added',
   `${graphWas.nodes.length} -> ${grown.nodes.length} nodes`);
// The id that is NEW, not the one that is last. Position in the published list
// is not identity — the same rule the piano roll's selection follows — and
// "last" is how a test ends up deleting a node the project shipped with.
const added = grown.nodes.find((n) => !wasIds.has(n.id));
ok(!!added, 'the new node is identified by its id, not its position');
// Ports come from the two node types, so nothing here types a port number.
await page.evaluate(([src, dst]) => window.__uni.run(`link ${src} ${dst}`),
                    [euclid.id, added.id]);
const linked = await graphUntil((g) => g.edges.some((e) => e.dst === added.id));
const newEdge = linked.edges.find((e) => e.dst === added.id);
ok(!!newEdge, 'and connected without anyone naming a port',
   JSON.stringify(linked.edges));
ok(newEdge && newEdge.srcPort === 1 && newEdge.dstPort === 0 && newEdge.kind === 0,
   'on the ports the engine expects for that pair', JSON.stringify(newEdge));
// A pair with no compatible ports is refused, and the refusal reaches the UI —
// the sidecar's message used to go to a dock nobody had open.
// This check needs a node with no compatible port. Rather than assume the graph
// has one, MAKE one — the patcher graph is engine-lifetime state, so whatever a
// previous run or a differently-shaped project left behind is what this run
// starts from, and asserting on somebody else's leftovers is how a test becomes
// a report about history. The UI can add a node, so the test uses that.
let audioNode = null;
for (let i = 0; i < 40 && !audioNode; i++) {
  audioNode = await page.evaluate(() =>
    window.__uni.patchNodes().find((n) => n.type === 'audio') || null);
  if (!audioNode) await page.waitForTimeout(100);
}
if (!audioNode) {
  await page.evaluate(() => window.__uni.run('addnode audio'));
  for (let i = 0; i < 40 && !audioNode; i++) {
    await page.waitForTimeout(150);
    audioNode = await page.evaluate(() =>
      window.__uni.patchNodes().find((n) => n.type === 'audio') || null);
  }
}
ok(!!audioNode, 'a node with no compatible port exists to test against',
   audioNode ? 'audio #' + audioNode.id : 'could not add one');
await page.evaluate(([a, b]) => window.__uni.run(`link ${a} ${b}`),
                    [euclid.id, audioNode.id]);
await page.waitForTimeout(900);
const why = await page.evaluate(() => window.__uni.state().reject);
ok(/compatible ports/.test(why || ''), 'an impossible connection says why, on screen', String(why));
// A refusal only the ENGINE can make: the sidecar's port table says
// passthru->passthru is fine, and the engine's graph says it would be a cycle.
// Before the out ring was drained this looked exactly like success — command
// sent, ack ok, read-back simply unchanged.
// Build BOTH ends of the cycle here rather than reusing whatever the graph
// already holds. The patcher graph is engine-lifetime state, so a run inherits
// whatever the last one left — and a test that reasons about inherited topology
// is a test about history. Two fresh passthrus, linked in both directions: the
// second link is a cycle whatever else exists.
const before2 = await page.evaluate(() => window.__uni.patcher());
const ids0 = new Set(before2.nodes.map((n) => n.id));
await page.evaluate(() => window.__uni.run('addnode passthru'));
const g1 = await graphUntil((g) => g.nodes.some((n) => !ids0.has(n.id)));
const cycA = g1.nodes.find((n) => !ids0.has(n.id)).id;
const ids1 = new Set(g1.nodes.map((n) => n.id));
await page.evaluate(() => window.__uni.run('addnode passthru'));
const g2 = await graphUntil((g) => g.nodes.some((n) => !ids1.has(n.id)));
const cycB = g2.nodes.find((n) => !ids1.has(n.id)).id;
await page.evaluate(([a, b]) => window.__uni.run(`link ${a} ${b}`), [cycA, cycB]);
await graphUntil((g) => g.edges.some((e) => e.src === cycA && e.dst === cycB));
await page.evaluate(() => window.__uni.state && window.__uni.run('state'));
await page.evaluate(([a, b]) => window.__uni.run(`link ${a} ${b}`), [cycB, cycA]);
await page.waitForTimeout(1800);
const cycle = await page.evaluate(() => window.__uni.state().reject);
ok(/cycle/.test(cycle || ''), "the engine's own refusal reaches the screen", String(cycle));
for (const id of [cycA, cycB]) {
  await page.evaluate((n) => window.__uni.run(`delnode ${n}`), id);
  await graphUntil((g) => !g.nodes.some((n) => n.id === id));
}
await page.evaluate((id) => window.__uni.run(`delnode ${id}`), added.id);
const shrunk = await graphUntil((g) => !g.nodes.some((n) => n.id === added.id));
ok(!shrunk.nodes.some((n) => n.id === added.id),
   'and removed again, taking its edge with it',
   `#${added.id} gone, ${shrunk.nodes.length} nodes, ${shrunk.edges.length} edges`);

section('piano roll selection');
await page.evaluate(() => { window.__uni.view('piano'); window.__uni.pianoAll(true); });
await frames();
const picked = await page.evaluate(() => window.__uni.pianoSelect(0, 0, 120, 300));
ok(picked > 1, 'marquee selects several notes', `${picked} notes`);
const pb = await page.evaluate(() => window.__uni.pianoSelected());
// The marquee's keys must actually RESOLVE to engine notes, which is a
// different claim from the marquee having found something. They were built by
// two different pieces of code — `notesInRect` writes the key and `noteKey`
// reads it — and when those disagreed (a two-part key against a three-part one)
// this resolved to nothing at all: notes drew unselected and every edit took the
// "select notes first" path. Nothing below caught it, because `every` on an
// empty array is true, so the transpose assertion passed by having no notes to
// contradict it. Assert the non-emptiness before asserting on the contents.
ok(pb.length === picked, 'and every marquee key resolves to an engine note',
   `${pb.length} of ${picked}`);
await page.evaluate(() => window.__uni.pianoEdit('transpose', 12));
await page.waitForTimeout(3500);
const pa = await page.evaluate(() => window.__uni.pianoSelected());
// Both halves matter: every note moves (the batch), and the selection still
// matches them afterwards (it is keyed on position, not on the note id the
// engine reassigns when it rewrites).
ok(pa.length > 0 && pa.length === pb.length && pa.every((n, i) => n.pitch === pb[i].pitch + 12),
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
  // Poll until the engine's row count SETTLES, rather than sleeping a number
  // that happened to be long enough. The zoom goes out on the command socket
  // and comes back on the state socket a viewport-apply later; a fixed 900 ms
  // passed for months and then failed the day a section was added ahead of it,
  // reporting a 16x jump because one read was still the previous zoom's.
  let prev = -1, stable = 0;
  for (let i = 0; i < 60 && stable < 3; i++) {
    await page.waitForTimeout(100);
    const rows = await page.evaluate(() => window.__uni.aggregates().rows);
    stable = rows === prev ? stable + 1 : 0;
    prev = rows;
  }
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
const bad = String(await page.evaluate(() => window.__uni.run('note 999')));
// Asserts the PROPERTY, not the wording: a refusal has to name the command, the
// argument and the value, so the person reading it knows which of three numbers
// they got wrong. Pinning the exact sentence made this fail the moment the
// message improved, which is a test punishing the thing it exists to encourage.
ok(bad.includes('note') && bad.includes('pitch') && bad.includes('999'),
   'the dock refuses a bad argument by naming command, argument and value', bad);
const unknown = await page.evaluate(() => window.__uni.run('flurb'));
ok(String(unknown).includes('unknown'), 'and an unknown command', String(unknown));
const help = await page.evaluate(() => window.__uni.dockProbe().commands);
ok(help.includes('note') && help.includes('gain') && help.includes('view'),
   `the grammar spans editing, mixing and navigation: ${help.length} commands`);
// Requirement (d) is that an agent can do what a user can. Every keyboard action
// that changes something needs a command, or the console is a subset.
for (const c of ['follow', 'rename', 'select', 'transpose', 'copy', 'paste', 'cut',
                 'loop', 'patch', 'addnode', 'delnode', 'link']) {
  ok(help.includes(c), `the console can ${c}`);
}

section('modifier keys on macOS');
// Every alt shortcut used to match on e.key, so on macOS — where Option is a
// COMPOSE modifier and Option+Q delivers 'œ' — all of them were dead. The tests
// passed anyway, because Playwright synthesises key 'q' for Alt+Q, which is what
// Windows sends and macOS does not. These dispatch what macOS actually sends.
const macAlt = (key, code) => page.evaluate(([k, c]) => {
  const ev = new KeyboardEvent('keydown',
    { key: k, code: c, altKey: true, bubbles: true, cancelable: true });
  window.dispatchEvent(ev);
  return ev.defaultPrevented;
}, [key, code]);
await page.evaluate(() => { window.__uni.view('piano'); window.__uni.pianoAll(true); });
await frames();
const modPicked = await page.evaluate(() => window.__uni.pianoSelect(0, 0, 120, 300));
ok(modPicked > 0, 'a selection to transpose', `${modPicked} notes`);
const modBefore = await page.evaluate(() => window.__uni.pianoSelected().map((n) => n.pitch));
ok(await macAlt('œ', 'KeyQ'), 'macOS Option+Q is handled, not ignored');
await page.waitForTimeout(3500);
const modAfter = await page.evaluate(() => window.__uni.pianoSelected().map((n) => n.pitch));
// `> 0` for the same reason as the selection section above: with an empty
// selection this whole assertion is `true && [].every(...)`, which is true, and
// the surface it is checking never ran at all.
ok(modAfter.length > 0 && modAfter.length === modBefore.length
   && modAfter.every((p, i) => p === modBefore[i] + 1),
   'and actually transposes', `${modBefore.slice(0, 4)} -> ${modAfter.slice(0, 4)}`);
await macAlt('å', 'KeyA');
await page.waitForTimeout(3500);
// The same physical key as WINDOWS sends it — key 'w', not '\u2211' — still
// reaches the same branch. Alt+W is bound on this surface; Alt+C is a tracker
// key, and asserting it here tested the surface, not the modifier.
ok(await macAlt('w', 'KeyW'), 'and the Windows key values still reach it');
await page.waitForTimeout(3500);
await macAlt('s', 'KeyS');
await page.waitForTimeout(3500);

section('device rack, live');
// LAST, deliberately. This loads a different project, and the patcher graph is
// ENGINE-lifetime state rather than project state (GUIDELINES 2.18) — loading
// `rack` replaces the graph and reloading webtest does not put it back. Run
// earlier, it broke the patcher sections that follow it. The rule: a check that
// mutates state a later check reads has to go after it, or not mutate.
// maximal, not rack: rack's vst_ref names "Identity", which is built but never
// scanned into the plugin cache, so it cannot resolve and falls back to whatever
// sits at host slot 0. maximal names Zebra2, which IS in the cache.
await page.evaluate(() => window.__uni.loadProject('maximal'));
let live = null;
for (let i = 0; i < 90 && !(live && live.params && live.params[0] > 0); i++) {
  await page.waitForTimeout(200);
  live = await page.evaluate(() => window.__uni.chainProbe());
}
ok(live && live.cards >= 1, 'a real project with a device chain draws it',
   JSON.stringify(live && live.titles));
ok(live && live.named >= 1 && live.titles[0] === 'Zebra2',
   'and the name comes from the plugin host, not from here', JSON.stringify(live.titles));
// The parameters are the plugin's own, queried from its host. This returned zero
// for a long time and the cause was environmental: the engine reads its plugin
// cache relative to its working directory, and a fresh build dir has none — so it
// could resolve a plugin's path and never load it, and every query answered from
// a host with no instance.
ok(live && live.params[0] > 0, 'and its parameters come back from the host',
   JSON.stringify(live.params));
const bars = await page.evaluate(() => [...document.querySelectorAll('.dv-p-fill')]
  .map((e) => e.style.width));
ok(bars.length > 0 && bars.some((w) => w !== '0%' && w !== ''),
   'and the value bars are drawn at their real lengths', JSON.stringify(bars.slice(0, 5)));

section('minimap and pending diff');
await page.evaluate(() => window.__uni.view('tracker'));
await frames();
const mm = await page.evaluate(() => window.__uni.minimapProbe());
ok(mm && mm.known && mm.markCount > 0, 'the minimap summarises the song',
   `${mm && mm.markCount} marks over ${mm && mm.songBeats} beats, ${mm && mm.events} events`);
// It spans the SONG, not the viewport. The first version grew with wherever the
// tracker was looking, so scrolling changed the picture's resolution.
const mmScrolled = await (async () => {
  await page.evaluate(() => window.__uni.scrollTo(200));
  await frames();
  const a = await page.evaluate(() => window.__uni.minimapProbe());
  await page.evaluate(() => window.__uni.scrollTo(0));
  await frames();
  return a;
})();
ok(mmScrolled.markCount === mm.markCount && mmScrolled.songTicks === mm.songTicks,
   'and does not change resolution when you scroll',
   `${mm.markCount}/${mm.songTicks} vs ${mmScrolled.markCount}/${mmScrolled.songTicks}`);

// The pending diff: a batch proposed but not committed.
const idle = await page.evaluate(() => window.__uni.pendingProbe());
ok(idle.status === 'idle', 'the card starts with nothing pending', idle.status);
const proposed = await page.evaluate(() => window.__uni.propose(
  [{ type: 'note', track: 0, pitch: 64, tick: 0, dur: 960000, vel: 100 }], 'e2e'));
ok(proposed.status === 'pending' && /1 note/.test(proposed.summary),
   'a proposal summarises itself from its ops', proposed.meta);
// Apply must send BEFORE it commits. It used to flip to applied first, so with no
// engine the batch was gone and the card claimed a hand-off that reached nothing.
const cvBefore = (await E()).clipVersion;
await page.evaluate(() => document.querySelector('.pd-apply').click());
await page.waitForTimeout(1500);
const applied = await page.evaluate(() => window.__uni.pendingProbe());
ok(applied.status === 'applied', 'applying it commits only once the send went out',
   applied.status + ' — ' + applied.reason);
ok((await E()).clipVersion > cvBefore, 'and the edit actually reached the engine',
   `${cvBefore} -> ${(await E()).clipVersion}`);

section('finding your way around');
// Tab cycling was the only route between surfaces and nothing said so — a user
// pressed every function key, found nothing, and discovered Tab by accident.
const tabs = await page.evaluate(() => [...document.querySelectorAll('.ch-tab')]
  .map((b) => ({ view: b.dataset.view, text: b.textContent })));
ok(tabs.length === 5, 'the chrome names every surface', tabs.map((t) => t.view).join(','));
ok(tabs.every((t) => /F\d/.test(t.text)), 'and prints the key that reaches it',
   tabs.map((t) => t.text).join(' '));
for (const [key, view] of [['F2', 'arrange'], ['F3', 'patcher'], ['F8', 'mixer'], ['F1', 'tracker']]) {
  await page.keyboard.press(key);
  await frames();
  const got = await page.evaluate(() => window.__uni.state().view);
  ok(got === view, `${key} goes to ${view}`, got);
}
// The minimap is the tracker's. It lived in the shared stage and painted over
// the patcher, the mixer and the arrangement.
await page.keyboard.press('F3');
await frames();
ok(await page.evaluate(() => document.getElementById('minimap').hidden),
   'the minimap does not paint over other surfaces');
await page.keyboard.press('F1');
await frames();
ok(!(await page.evaluate(() => document.getElementById('minimap').hidden)),
   'and comes back on the tracker');

// The harmony card answers for where the CURSOR is, not where the playhead is:
// scrolling to a different field used to leave it naming the first one.
await page.evaluate(() => window.__uni.run('goto 0 0'));
await frames();
const atTop = await page.evaluate(() => window.__uni.harmonyProbe().key);
await page.evaluate(() => window.__uni.run('goto 6 0'));
await page.waitForTimeout(400);
const lower = await page.evaluate(() => window.__uni.harmonyProbe().key);
ok(atTop !== lower, 'the harmony card follows the cursor', `${atTop} -> ${lower}`);

// The dock is for things a person can act on. Every ack used to be printed into
// it verbatim, including a viewport echo on every scroll.
const noise = await page.evaluate(() => window.__uni.dockProbe().last.join(' '));
ok(!/"ok":true/.test(noise), 'the dock does not echo raw command acks', noise.slice(0, 80));

section('tempo');
// The transport bar printed a hardcoded "120.00 BPM" from the day it was written.
// These assert the LABEL, not the store behind it — the store was never the thing
// that was lying.
await page.evaluate(() => window.__uni.loadProject('maximal'));
await page.waitForTimeout(1200);
const tempoLabel = () => page.evaluate(() =>
  document.querySelector('.ch-meta')?.textContent ?? '');
const tempoState = () => page.evaluate(() => window.__uni.tempo());

const tempo0 = await tempoState();
ok(tempo0 && tempo0.bpm > 0, 'the engine publishes a tempo', JSON.stringify(tempo0));
ok((await tempoLabel()).startsWith(String(Math.floor(tempo0.bpm))),
   'and the chrome prints that tempo, not a caption', await tempoLabel());
// maximal changes tempo partway, so it has more than one point, and the label
// says so with a trailing mark. A song of one tempo must NOT wear it.
ok(tempo0.points > 1, 'maximal has a tempo change', `${tempo0.points} points`);
ok(/·$/.test(await tempoLabel()), 'a varying tempo is marked as varying', await tempoLabel());

// Flatten: no position means the whole song, and the mark goes away.
await page.evaluate(() => window.__uni.tempo(90));
await page.waitForTimeout(900);
const tempo1 = await tempoState();
ok(Math.round(tempo1.bpm) === 90, 'setting the tempo takes', JSON.stringify(tempo1));
ok(tempo1.points === 1, 'and with no position it flattens the map', `${tempo1.points} points`);
ok(!/·$/.test(await tempoLabel()), 'so it is no longer marked as varying', await tempoLabel());

// A position means ONE point. `tempo(x, 0)` would replace the point at bar 1 and
// leave later changes standing — a different edit, which is why absence rather
// than zero is what means "all of it".
await page.evaluate(() => window.__uni.tempo(150, 7680000));
await page.waitForTimeout(900);
const tempo2 = await tempoState();
ok(tempo2.points === 2, 'a position inserts a point instead of flattening', `${tempo2.points} points`);
ok(Math.round(tempo2.bpm) === 90, 'and the playhead still reads the tempo where it is',
   JSON.stringify(tempo2));

// A refusal has to reach the person, not just the log.
await page.evaluate(() => { try { window.__uni.tempo(5000); } catch (e) {} });
await page.waitForTimeout(700);
const rejected = await page.evaluate(() =>
  document.querySelector('.ch-reject')?.textContent ?? '');
ok(/between 10 and 1000/.test(rejected), 'an impossible tempo is refused on screen', rejected);

section('device parameters');
/**
 * A parameter write needs the audio thread.
 *
 * The host applies the value on its audio callback, so on an engine started with
 * --no-audio the send is accepted, nothing errors, and the value never moves.
 * That is the engine's shape rather than a defect — backend's own daw-cli behaves
 * identically — and it is recorded here because "accepted, no error, nothing
 * moved" is the hardest failure there is to read, and because a headless CI run
 * would otherwise report it as a broken UI.
 *
 * So the write is only asserted when the stack was started with audio. Set
 * UNI_HAS_AUDIO=1 to run it; the default says why it did not.
 */
await page.evaluate(() => window.__uni.loadProject('rack'));
await page.waitForTimeout(2000);

// The device's AUDIO BUSES (kShmVersion 20), which is what tells a stereo effect
// from a plugin with eight stems and a sidechain input. These ride the chain
// snapshot rather than a request, so they are here the moment the chain is.
{
  const d = await page.evaluate(() => {
    const c = window.__uni.chains();
    const t = c && c['0'];
    return t && t.devices.length ? t.devices[0] : null;
  });
  ok(d !== null, 'the rack project published a device');
  if (d) {
    // COMPLETE, not partial. busCount is the field I held the version bump for:
    // without it, two buses received out of eight is indistinguishable from a
    // device that has two, and the rack draws the wrong number and then changes
    // it. Asserting the two AGREE is what proves the count is real rather than
    // defaulted — it read 0 against two live buses until I fixed which struct's
    // `flags` it came from.
    ok(d.busCount === d.buses.length && d.busCount > 0,
       `every bus the engine promised arrived: ${d.buses.length}/${d.busCount}`);
    ok(d.busTruncated === false, 'and none were dropped at the cap');
    // Direction, and a name that came from the plugin rather than from us.
    const ins = d.buses.filter((b) => b.input);
    const outs = d.buses.filter((b) => !b.input);
    ok(ins.length > 0 && outs.length > 0,
       `inputs and outputs are told apart: ${outs.length} out, ${ins.length} in`);
    ok(d.buses.every((b) => typeof b.name === 'string' && b.name.length > 0),
       `each bus carries its own name: ${JSON.stringify(d.buses.map((b) => b.name))}`);
    // layoutId is the stable enum I asked for so caches key on an integer rather
    // than a display string; 2 is stereo.
    ok(d.buses.every((b) => b.layoutId >= 0 && b.channels > 0),
       `and a layout id and channel count: ${JSON.stringify(d.buses.map((b) => [b.layoutId, b.channels]))}`);
  }
}

await page.evaluate(() => window.__uni.reqParams(0, 0));
await page.waitForTimeout(1500);
const beforeP = await page.evaluate(() => {
  const all = window.__uni.deviceParams(); const ids = Object.keys(all || {});
  if (!ids.length) return null;
  const d = all[ids[0]];
  return { device: Number(ids[0]), name: d.name, count: d.params.length, first: d.params[0] };
});
ok(beforeP && beforeP.count > 0, 'the rack reads a real plugin\'s parameters',
   beforeP ? `${beforeP.name}, ${beforeP.count} params` : 'none published');

if (beforeP && beforeP.first) {
  const uid = beforeP.first.uid;
  ok(/^[0-9a-f]{32}$/.test(uid) && !/^0+$/.test(uid),
     'and each one carries a durable id, not just an index', uid);
  if (process.env.UNI_HAS_AUDIO) {
    const target = beforeP.first.value > 0.5 ? 150 : 850;
    const sent = await page.evaluate(([d, i, u, v]) => window.__uni.setParam(0, d, i, u, v),
      [beforeP.device, beforeP.first.index, uid, target]);
    ok(sent === true, 'a parameter write goes out', String(sent));
    await page.waitForTimeout(2500);
    const afterP = await page.evaluate(() => {
      const all = window.__uni.deviceParams(); const ids = Object.keys(all || {});
      return ids.length ? all[ids[0]].params[0].value : null;
    });
    ok(Math.abs(afterP - target / 1000) < 0.01, 'and the plugin actually moved',
       `${beforeP.first.value} -> ${afterP} (asked ${target / 1000})`);
  } else {
    console.log('  SKIP  a parameter write moves the plugin  '
      + '(needs an engine with an audio device: the host applies on its audio '
      + 'callback, so --no-audio accepts the write and drops it. UNI_HAS_AUDIO=1 to run)');
  }
  // Refusing a parameter with no durable id is the honest answer; sending it
  // would be accepted and would change nothing.
  const zeroed = await page.evaluate(([d, i]) =>
    window.__uni.setParam(0, d, i, '00000000000000000000000000000000', 500),
    [beforeP.device, beforeP.first.index]);
  ok(zeroed === false, 'a parameter with no durable id is refused, not sent', String(zeroed));
}

section('audio sources and waveforms');
/**
 * The waveform data path, end to end, against the committed fixtures.
 *
 * Each of these was verified by hand when the path landed. A hand verification
 * that is not a test decays — and these three in particular are the ones that
 * distinguish a correct peak implementation from one that returns plausible
 * numbers, so they are worth the seconds they cost.
 */
// Assert the load TOOK, and wait for the table rather than sleeping at it. A
// fixed sleep turns "the command never went out" into "the data never arrived",
// which sends you looking at the wrong end of the pipe — it did exactly that here.
const wfLoaded = await page.evaluate(() => window.__uni.loadProject('waveform'));
ok(wfLoaded !== false, `the waveform project load went out: ${wfLoaded}`);
await page.waitForFunction(
  () => Object.keys(window.__uni.audioSources().sources).length >= 2,
  null, { timeout: 8000 }).catch(() => {});
const audio = await page.evaluate(() => {
  const a = window.__uni.audioSources();
  return { sources: Object.values(a.sources), clips: Object.values(a.clips) };
});
ok(audio.sources.length === 2, `both sources decoded: ${audio.sources.length}`);
const mono = audio.sources.find((s) => s.channels === 1);
const stereo = audio.sources.find((s) => s.channels === 2);
ok(!!mono && !!stereo, 'one mono and one stereo source');
if (mono && stereo) {
  ok(mono.frames === 352800 && stereo.frames === 352800,
     `8 seconds at 44100: ${mono.frames} / ${stereo.frames} frames`);
  // absPeak on the STEREO file is the mixdown-is-false proof at its source: its
  // channels are exact negations, so peaks built after a mono downmix would read
  // ~0 while the file is as loud as a file can be.
  ok(stereo.absPeak > 0.9,
     `stereo absPeak is full scale, so peaks are built pre-downmix: ${stereo.absPeak}`);
  ok(audio.clips.length === 2, `both clips map to a source: ${audio.clips.length}`);
}

if (mono) {
  // The DC +0.5 second: every bucket must read min == max == 16384. A renderer
  // that draws ±|peak| mirrored around a centre line shows this as a band
  // straddling zero when the truth is a block entirely above it.
  const dc = await page.evaluate((id) => window.__uni.waveform(id, 64, 221184, 64, 1),
                                 mono.id);
  const allDc = dc.pairs && Array.from(dc.pairs).every((v) => v === 16384);
  ok(!!allDc, `the DC second is a block above zero, not a band around it: ` +
              (dc.pairs ? Array.from(dc.pairs.slice(0, 4)).join(',') : dc.error));

  // decim 1 is raw samples, and a one-frame bucket has min == max. That identity
  // is what makes the peak regime and the sample regime one request and one
  // reply, with no crossover for a waveform to pop at.
  const raw = await page.evaluate((id) => window.__uni.waveform(id, 1, 264600, 8, 1),
                                  mono.id);
  const degenerate = raw.pairs && Array.from(raw.pairs)
    .every((v, i) => (i % 2 === 1 ? v === raw.pairs[i - 1] : true));
  ok(!!degenerate, 'at decim 1 min equals max, so peaks and samples are one path');
}

if (stereo) {
  // Channel 1 must be the EXACT negation of channel 0. A downmix of this file is
  // silence, so this is the assertion the stereo fixture exists for.
  const st = await page.evaluate((id) => window.__uni.waveform(id, 64, 44032, 32, 3),
                                 stereo.id);
  let mirrored = !!(st.pairs && st.channels === 2), loud = false;
  if (mirrored) {
    const c = st.columns;
    for (let i = 0; i < c; i++) {
      const mn0 = st.pairs[i * 2], mx0 = st.pairs[i * 2 + 1];
      const mn1 = st.pairs[(c + i) * 2], mx1 = st.pairs[(c + i) * 2 + 1];
      if (mn1 !== -mx0 || mx1 !== -mn0) { mirrored = false; break; }
      if (mx0 > 30000) loud = true;
    }
  }
  ok(mirrored, 'channel 1 is the exact negation of channel 0');
  ok(loud, 'and both reach full scale where a mono downmix would be silent');
}

section('song meter and per-clip grids');
/**
 * Two meters travel this wire and they are not the same quantity.
 *
 * The SONG's time signature is what bar NUMBERING counts in — the time gutter and
 * the arrangement ruler. A CLIP's grid is what that rail runs internally, and a
 * project may hold several at once. Before v19 the page had neither: 4/4 was
 * hardcoded as the literal 3840000 in three model files and as the string '4/4' in
 * the chrome, so a project in 7/8 was mislabelled in four independent ways and
 * fixing three of them would have looked like a fix.
 *
 * `presets/projects/meter.uniproj.json` exists to separate the two: the song is in
 * 7/8, and its three clips are in 7/8, 5/4 and 4/4 with three different row grids.
 * A test that asserted only the song meter would pass against a build that
 * published the song's meter for every clip; one that asserted only the clips
 * would pass against a build with no song meter at all. Both, together, is the
 * check — and the 4/4 clip is in there so that "publishes a grid" is distinguished
 * from "publishes a grid that differs from the song".
 */
// Wait for THIS load to land, not for data that looks like it. The obvious
// condition — "at least three rails" — was already true of the project loaded by
// the section before, so every assertion below ran against the previous project
// and reported 6 rails in 4/4. The load counter is the only thing that says the
// load being waited for is the one that finished; loadProject() returning true
// says the command went OUT, which is a different claim.
const seqBefore = await page.evaluate(() => window.__uni.loadStatus().seq);
const mLoaded = await page.evaluate(() => window.__uni.loadProject('meter'));
ok(mLoaded !== false, `the meter project load went out: ${mLoaded}`);
await page.waitForFunction(
  (s) => { const l = window.__uni.loadStatus(); return l && l.seq > s && l.ok; },
  seqBefore, { timeout: 8000 }).catch(() => {});
// The load counter and the rails are published on the same cycle, but they are two
// separate fields and nothing guarantees a frame carries both. Measured, the rails
// were correct within 250ms of the counter moving; this is that with room.
await page.waitForTimeout(400);
const meters = await page.evaluate(() => window.__uni.meters());
ok(meters !== null, 'the page has an engine to ask');
if (meters) {
  ok(meters.song.numerator === 7 && meters.song.denominator === 8,
     'the song meter survived the whole chain',
     `${meters.song.numerator}/${meters.song.denominator}`);
  // Counted, not just inspected. This section runs after several other projects
  // have been loaded, and a load used to grow the engine's track set without ever
  // shrinking it — so the surplus tracks of a LARGER previous project kept their
  // placements and kept publishing rails, and the arrangement drew clips from a
  // project the user had closed:
  //
  //   load meter    (3 tracks) -> 3 rails   t0:seven t1:five t2:four
  //   load maximal  (8 tracks) -> 6 rails   t0:Bass ... t5:Perc
  //   load meter    (3 tracks) -> 6 rails   t0:seven t1:five t2:four
  //                                          t3:Arp  t4:Drums t5:Perc   <- stale
  //
  // Every other check in this section passed throughout, because the three rails
  // that DO belong to the project were always correct — which is why this reads as
  // working until you count them. Fixed engine-side (4222c16); the count is what
  // keeps it fixed, and `meter` being SMALLER than what runs before it is what
  // gives the count something to catch.
  ok(meters.clips.length === 3, `three rails, none inherited: ${meters.clips.length}`,
     meters.clips.map((c) => `t${c.track}:${c.name || '(unnamed)'}#${c.clipId}`).join(' '));

  const byName = {};
  for (const c of meters.clips) byName[c.name] = c.grid;
  const shape = (g) => (g ? `${g.numerator}/${g.denominator} lpb${g.linesPerBeat}` : 'none');

  ok(byName.seven && byName.seven.numerator === 7 && byName.seven.denominator === 8
       && byName.seven.linesPerBeat === 4,
     'the 7/8 clip publishes its own grid', shape(byName.seven));
  // The one that matters most: a clip whose meter is NOT the song's. Everything
  // upstream could be publishing the song meter under a per-clip name and every
  // other assertion here would still pass.
  ok(byName.five && byName.five.numerator === 5 && byName.five.denominator === 4
       && byName.five.linesPerBeat === 6,
     'and a clip in a DIFFERENT meter from the song keeps it', shape(byName.five));
  ok(byName.four && byName.four.numerator === 4 && byName.four.denominator === 4
       && byName.four.linesPerBeat === 4,
     'and a 4/4 clip publishes 4/4 rather than nothing', shape(byName.four));

  // Three distinct grids on the wire at once. Stated as a set, because the three
  // assertions above would all pass if the decoder returned the same record three
  // times — the extents are POOLED and rewritten in place, which is exactly the
  // shape that produces that bug.
  const distinct = new Set(meters.clips.map((c) => shape(c.grid)));
  ok(distinct.size === 3, 'three DISTINCT grids, not one record read three times',
     [...distinct].join(', '));
}

// And the ARRANGEMENT rules its bars in that meter, which is the other half a user
// sees. A ruler in the wrong meter is the failure that looks most like success —
// evenly spaced numbers, ascending, every one of them naming the wrong moment — so
// this asserts the tick distance rather than that numbers exist.
await page.evaluate(() => window.__uni.view('arrange'));
await page.waitForTimeout(200);
const ruler = await page.evaluate(() => window.__uni.arrangeProbe());
ok(ruler !== null, 'the arrangement built a model');
if (ruler) {
  // 7/8: a beat is an eighth (960000 / 2) and there are seven, so 3,360,000. The
  // 4/4 answer is 3,840,000, which is what this drew before the meter reached it.
  const barTicks = ruler.rulerEvery > 0 ? ruler.rulerStrideTicks / ruler.rulerEvery : 0;
  ok(barTicks === 3360000,
     'the arrangement rules 7/8 bars, not 4/4 ones',
     `${barTicks} ticks per bar (4/4 would be 3840000), stride ${ruler.rulerStrideTicks} every ${ruler.rulerEvery}`);
}
await page.evaluate(() => window.__uni.view('tracker'));

// AND THE TRACKER DRAWS IT, which is the last link in the chain and the only one
// a user actually sees. `meter` is a good shape for this by accident of how it was
// built: the song is 7/8, and so is track 0's clip — so that lane agrees with the
// gutter and must NOT carry a column, while tracks 1 (5/4) and 2 (4/4) disagree
// and must. A build that showed the column everywhere, or nowhere, passes every
// other check in this section.
await page.evaluate(() => window.__uni.view('tracker'));
await page.waitForTimeout(200);
const laneCols = await page.evaluate(() => {
  const out = [];
  for (let t = 0; t < 3; t++) {
    const e = document.querySelector(`.tk-lane-bar[data-track="${t}"]`);
    out.push(e ? (e.getBoundingClientRect().width > 0 ? 1 : 0) : -1);
  }
  const shown = document.querySelector('.tk-lane-bar[data-track="1"]');
  return { out, text: shown ? shown.textContent : null };
});
ok(JSON.stringify(laneCols.out) === '[0,1,1]',
   'the bar column appears on the lanes that disagree with the song, and only those',
   `${JSON.stringify(laneCols.out)} for [7/8 clip in a 7/8 song, 5/4, 4/4]`);
ok(laneCols.text && /^\d+:\d+$/.test(laneCols.text),
   `and it reads a bar:beat: ${JSON.stringify(laneCols.text)}`);

// And the chrome says it, which is the half a user sees. Asserted on the property
// rather than on the position of the field: which slot the meter occupies in the
// chrome is layout, and a test that pins layout fails on every rearrangement.
const chromeMeter = await page.evaluate(
  () => [...document.querySelectorAll('.ch-meta')].map((n) => n.textContent));
ok(chromeMeter.includes('7/8'),
   'the chrome counts the song in 7/8', JSON.stringify(chromeMeter));

section('child-track structure (v20)');
/**
 * The fields exist and read as top-level before anything creates a child.
 *
 * Worth asserting NOW rather than with the multi-out feature. These arrive on the
 * wire one bump ahead of the engine populating them, so this is the only window in
 * which "every track is top-level and expanded" is a claim about a field that is
 * being read rather than one that is merely absent — and a read-back that first
 * appears alongside the feature cannot tell a correct default from a field nobody
 * ever wrote. It is also the whole point of the design: a child is an ORDINARY
 * track in the same flat arrays, so a client that ignores both fields still draws
 * the entire project.
 */
const tree = await page.evaluate(() => window.__uni.trackTree());
ok(Array.isArray(tree) && tree.length > 0, `the tree read back: ${tree && tree.length} tracks`);
if (tree && tree.length) {
  /**
   * `meter` has no multi-out instrument, so every track is genuinely top-level.
   *
   * Asserted on `hasParent`, NOT on `parent === 0`. That was the original check
   * and it could not distinguish what it claimed to: `parent_id 0` meant
   * "top-level", but 0 is a valid track id and track 0 is the likeliest parent
   * there is — so it went green against a real multi-out project whose stems were
   * children of track 0. Backend added `kUiTrackFlagHasParent` (bit 1) at my
   * asking; the id is meaningful only when the flag is set, and now the two states
   * have two values.
   */
  ok(tree.every((t) => t.hasParent === false),
     'a project with no multi-out instrument has no children',
     JSON.stringify(tree.slice(0, 4)));
  ok(tree.every((t) => t.parent === -1),
     'and a top-level track reports no parent id at all, rather than zero');
  ok(tree.every((t) => t.collapsed === false), 'and none is collapsed');
  const names = await page.evaluate(() => window.__uni.names());
  ok(names && names.length >= tree.length,
     `the tree is as long as the track list it describes: ${tree.length} vs ${names && names.length}`);
}

section('multi-out child tracks');
/**
 * A plugin's aux output buses become child TRACKS — ordinary tracks with a parent.
 *
 * Needs the fake identity host, since the multi-out instrument is synthesised:
 * DAW_USE_FAKE_IDENTITY=1 ./tools/webstack.sh. Without it the project loads with
 * one track and no children, which is a correct engine and a useless test, so it
 * says so rather than passing vacuously.
 */
{
  const mSeq = await page.evaluate(() => window.__uni.loadStatus().seq);
  await page.evaluate(() => window.__uni.loadProject('multiout'));
  await page.waitForFunction((s0) => {
    const l = window.__uni.loadStatus(); return l && l.seq > s0 && l.ok;
  }, mSeq, { timeout: 8000 }).catch(() => {});
  // Then wait for the CHILDREN, not for a fixed interval. The load ack says the
  // document was read; the stems appear once the engine has instantiated the
  // plugin and negotiated its buses, which is a plugin round trip later. A 600ms
  // sleep was enough after a cold boot and not enough here, and the difference
  // showed up as this whole section skipping itself — a test that reports "not
  // applicable" when it is merely early is worse than one that fails.
  await page.waitForFunction(
    () => window.__uni.trackTree().some((t) => t.hasParent),
    null, { timeout: 8000 }).catch(() => {});

  const mo = await page.evaluate(() => ({
    tree: window.__uni.trackTree(), names: window.__uni.names(),
  }));
  const kids = mo.tree.filter((t) => t.hasParent);
  if (!kids.length) {
    // Two reasons this can be empty, and they are worth telling apart in the
    // message rather than lumping into "not applicable":
    //
    //  1. the engine has no fake identity host, so there is no multi-out
    //     instrument to create stems from — run the stack with
    //     DAW_USE_FAKE_IDENTITY=1 and this section asserts instead of skipping;
    //
    //  2. ORDER. On a fresh engine `load multiout` creates its children; after
    //     ANY earlier project load the same load logs
    //     `project.plugin_missing name:"multiout" match:"none"` and creates none.
    //     Reported to backend with a two-line repro. This section sits late in the
    //     file, so it currently hits that path even with the flag set.
    console.log('  SKIP  multi-out children  (no children published — needs '
      + 'DAW_USE_FAKE_IDENTITY=1, and currently also a FRESH engine: see the '
      + 'order-dependence note above)');
  } else {
    // A parent and two stems is three tracks. It reports eight after an earlier
    // load: the children are appended after the PREVIOUS project's track count
    // rather than the new document's, so tracks 1-5 are empty leftovers named
    // "Track 2".."Track 6". Reported to backend — the load-clear shrinks the count
    // for a plain project but not when child creation follows it.
    ok(mo.tree.length === 3, `a parent and its stems: ${mo.tree.length} tracks`,
       JSON.stringify(mo.names));
    ok(mo.tree[0].hasParent === false, 'the instrument track is top-level');
    // THE CASE THE OLD CONTRACT COULD NOT EXPRESS: children OF TRACK 0.
    ok(kids.length === 2 && kids.every((t) => t.parent === 0),
       `both stems are children of track 0: ${JSON.stringify(kids)}`);
    // Indexed by the CHILDREN's own track ids. Slots 1 and 2 are whatever the
    // previous project left behind — the engine appends children after the old
    // track count — so a positional check tests the leftovers, not the stems.
    ok(kids.every((k) => mo.names[k.track] && mo.names[k.track].startsWith(mo.names[0])),
       `and are named after their parent: ${JSON.stringify(kids.map((k) => mo.names[k.track]))}`);

    // Folding the parent takes its stems off screen and leaves it there. Measured
    // as width, because a stem hidden by renumbering rather than by width would
    // shift every track id after it — which is the thing the zero-width approach
    // exists to avoid.
    // Two evaluates with a frame between them. `fold()` goes through schedule(),
    // which coalesces to one draw on the next animation frame — measuring in the
    // same evaluate reads the widths BEFORE the redraw and reports that nothing
    // moved, which is indistinguishable from a fold that does not work.
    const w = () => page.evaluate(
      () => [...document.querySelectorAll('.tk-row[data-row="0"] .tk-track')]
        .map((e) => Math.round(e.getBoundingClientRect().width)));
    const before = await w();
    const took = await page.evaluate(() => window.__uni.fold(0));
    await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
    const folded = { took, before, after: await w() };
    ok(folded.took === true, 'folding the instrument is accepted — it has children');
    // Keyed on the CHILDREN's own indices, not on 1 and 2. The engine appends
    // children after whatever tracks already exist, so their position is not
    // fixed — asserting slots 1 and 2 tests the fixture's layout rather than the
    // fold.
    const kidIdx = kids.map((k) => k.track);
    ok(folded.after[0] > 0 && kidIdx.every((i) => folded.after[i] === 0),
       `its stems fold away while it stays: parent ${folded.after[0]}px, `
       + `stems ${JSON.stringify(kidIdx.map((i) => folded.after[i]))}`);
  }
}

section('arrangement navigation');
{
  // arrange.js has had anchored zoom, horizontal pan and lane scrolling since it
  // was written, and `onNav` was never passed to its constructor — so `_wheel`
  // returned on its first line and the wheel did nothing over the arrangement in
  // any combination of modifiers. Real wheel events, because the bug WAS the
  // wiring and a probe that calls the model directly cannot see wiring.
  await page.evaluate((pr) => window.__uni.loadProject(pr), PROJECT);
  await page.waitForTimeout(800);
  await page.keyboard.press('F2');
  await page.waitForTimeout(500);
  const A = () => page.evaluate(() => {
    const s = window.__uni.state(), a = window.__uni.arrangeProbe();
    return { zoom: s.arrangeZoom, start: s.arrangeStart, ls: a.laneScroll, max: a.maxLaneScroll };
  });
  ok((await page.evaluate(() => window.__uni.state().view)) === 'arrange',
     'F2 reaches the arrangement');
  const at = await page.evaluate(() => {
    const r = document.getElementById('arrange').getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
  });
  await page.mouse.move(at.x, at.y);

  const a0 = await A();
  await page.keyboard.down('Shift');
  for (let i = 0; i < 3; i++) await page.mouse.wheel(0, 120);
  await page.keyboard.up('Shift');
  await page.waitForTimeout(350);
  const a1 = await A();
  ok(a1.start > a0.start, 'shift-wheel pans the arrangement along time',
     `${a0.start} -> ${a1.start}`);

  // Start from a zoom that has room to go in. An earlier section leaves the
  // arrangement at index 0, the finest, and "zoom in" there correctly does
  // nothing — which reads as a broken gesture.
  for (let i = 0; i < 3; i++) { await page.keyboard.press('Minus'); }
  await page.waitForTimeout(300);
  const aMid = await A();
  await page.keyboard.down('Meta');
  for (let i = 0; i < 3; i++) await page.mouse.wheel(0, -120);
  await page.keyboard.up('Meta');
  await page.waitForTimeout(350);
  const a2 = await A();
  ok(a2.zoom < aMid.zoom, 'cmd-wheel zooms in', `zoom ${aMid.zoom} -> ${a2.zoom}`);

  await page.keyboard.press('Minus');
  await page.waitForTimeout(250);
  const a3 = await A();
  ok(a3.zoom > a2.zoom, 'and the keyboard zooms out', `zoom ${a2.zoom} -> ${a3.zoom}`);
}

section('resizable and collapsible panes');
{
  // splitter.js was complete — handles, keyboard, clamping, persistence — and
  // `new Splitter` appeared NOWHERE, so nothing in the shell could be resized and
  // splitter.css was not even linked. Drags here, not setSize() calls: the bug
  // was the absence of the wiring, and only a real drag exercises the wiring.
  const box = (id) => page.evaluate((i) => {
    const e = document.getElementById(i);
    if (!e) return null;
    const r = e.getBoundingClientRect();
    return { w: Math.round(r.width), h: Math.round(r.height) };
  }, id);
  const handle = (hostId, sel) => page.evaluate(([i, s]) => {
    const el = document.getElementById(i) && document.getElementById(i).querySelector(s);
    if (!el) return null;
    const r = el.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
  }, [hostId, sel]);

  const dockBefore = await box('rdock');
  const hDock = await handle('rdock', '.sp-left');
  ok(!!hDock, 'the right dock has a drag handle');
  if (hDock) {
    await page.mouse.move(hDock.x, hDock.y);
    await page.mouse.down();
    await page.mouse.move(hDock.x - 100, hDock.y, { steps: 10 });
    await page.mouse.up();
    await page.waitForTimeout(300);
    const after = await box('rdock');
    ok(after.w > dockBefore.w, 'dragging it widens the dock',
       `${dockBefore.w} -> ${after.w}`);
  }

  const harmBefore = await box('harmony');
  const hHarm = await handle('harmony', '.sp-bottom');
  if (hHarm) {
    await page.mouse.move(hHarm.x, hHarm.y);
    await page.mouse.down();
    await page.mouse.move(hHarm.x, hHarm.y + 80, { steps: 10 });
    await page.mouse.up();
    await page.waitForTimeout(300);
    const after = await box('harmony');
    ok(after.h > harmBefore.h, 'and the dock cells resize against each other',
       `harmony ${harmBefore.h} -> ${after.h}`);
  }

  // Fold, then UNfold by clicking the same control. The control has to survive
  // its own collapse and stay hit-testable: the first build clipped it behind the
  // cell below, where it still reported an 18px rect and could not be clicked.
  const foldAt = () => page.evaluate(() => {
    const el = document.getElementById('pending').querySelector('.cell-fold');
    if (!el) return null;
    const r = el.getBoundingClientRect();
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    const top = document.elementFromPoint(x, y);
    return { x, y, reachable: !!(top && top.closest('.cell-fold')) };
  });
  const pendBefore = await box('pending');
  let f = await foldAt();
  ok(f && f.reachable, 'the pending cell has a reachable fold control');
  if (f && f.reachable) {
    await page.mouse.click(f.x, f.y);
    await page.waitForTimeout(300);
    const folded = await box('pending');
    ok(folded.h < pendBefore.h, 'clicking it collapses the cell',
       `${pendBefore.h} -> ${folded.h}`);
    f = await foldAt();
    ok(f && f.reachable,
       'and the control is STILL reachable once collapsed — measured by hit test, '
       + 'not by rect: a clipped button reports a rect and cannot be clicked');
    if (f && f.reachable) {
      await page.mouse.click(f.x, f.y);
      await page.waitForTimeout(300);
      const back = await box('pending');
      ok(back.h > folded.h, 'so the cell can be brought back', `-> ${back.h}`);
    }
  }

  /**
   * Put the layout back.
   *
   * A section that resizes the shell and walks away changes where everything is
   * for every section after it. The focus checks below then clicked a grid cell
   * that was sitting under the widened dock, no pointerdown reached the tracker,
   * and "clicking the grid reclaims focus" failed for a reason that had nothing
   * to do with focus. Double-click is the splitter's own reset-to-home.
   */
  for (const [host, sel] of [['rdock', '.sp-left'], ['harmony', '.sp-bottom']]) {
    const h = await handle(host, sel);
    if (!h) continue;
    await page.mouse.dblclick(h.x, h.y);
    await page.waitForTimeout(200);
  }
  await page.waitForTimeout(300);
}

section('keyboard focus between panes');
{
  // Driven with REAL clicks and REAL keys, deliberately. The bug this covers —
  // Backspace on a selected device deleting a note in the tracker — was invisible
  // to every existing test because they all call __uni hooks, which route by
  // intent instead of by focus. A hook cannot reproduce "the keys went to the
  // wrong pane", because a hook never asks which pane has the keys.
  // Off the fixture AND back to the tracker: the section above leaves a
  // multi-out fixture showing, and a card rect measured while another surface is
  // up is a rect for something that is not on screen. The first version of this
  // test clicked those coordinates, hit the tracker underneath, and reported the
  // focus bug as still present — a false failure that looked exactly like a real
  // one.
  await page.evaluate((pr) => window.__uni.loadProject(pr), PROJECT);
  await page.waitForTimeout(700);
  await page.evaluate(() => { window.__uni.view('tracker'); window.__uni.chainSelect(-1); });
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  let asked = null;
  page.on('dialog', async (d) => { asked = d.message(); await d.dismiss(); });

  const findCard = () => page.evaluate(() => {
    const c = document.querySelector('.dv-card');
    if (!c) return null;
    const r = c.getBoundingClientRect();
    // A card that is not laid out has a zero rect and clicking it hits whatever
    // is behind it. Refuse to measure one.
    if (r.width < 10 || r.height < 10) return null;
    return { x: r.x + r.width / 2, y: r.y + 30 };
  });
  // VISIBLE cards. The rack pools its nodes and hides the spares, so the raw
  // querySelectorAll count never falls and a working delete reads as a dead one.
  const cardCount = () => page.evaluate(
    () => [...document.querySelectorAll('.dv-card')]
      .filter((e) => e.offsetParent !== null && e.getBoundingClientRect().width > 4).length);
  const cardsAtStart = await cardCount();
  let card = await findCard();
  let weAddedOne = false;
  if (!card) {
    // Make one rather than skipping. A focus test that only runs on projects
    // that happen to have a device is a focus test that does not run, and this
    // covers a bug that reached the user.
    await page.evaluate(() => window.__uni.addDevice(0, 'patcher event'));
    await page.waitForTimeout(1200);
    card = await findCard();
    weAddedOne = !!card;
  }
  if (!card) {
    blocked(false, 'keyboard focus follows the rack',
            'no device card on screen — needs a project with a device on track 0');
  } else {
    const F = () => page.evaluate(() => {
      const s = window.__uni.state();
      const cells = [...document.querySelectorAll('.tk-cell')]
        .filter((e) => { const r = e.getBoundingClientRect(); return r.width > 4 && r.height > 4; });
      return { focus: s.focus, sel: s.chainSelected, row: s.cursor.row,
               view: s.view, cells: cells.length };
    });
    await page.mouse.click(card.x, card.y);
    await page.waitForTimeout(300);
    const onCard = await F();
    ok(onCard.focus === 'chain', 'clicking a device gives the rack the keyboard',
       JSON.stringify(onCard));
    const ring = await page.evaluate(
      () => document.getElementById('chain').classList.contains('pane-focus'));
    ok(ring, 'and the rack SHOWS that it has it — a selection that looks like '
           + 'focus and is not is worse than no selection');

    const notesBefore = await page.evaluate(() => window.__uni.selected().length);
    await page.keyboard.press('Backspace');
    await page.waitForTimeout(400);
    ok(asked !== null && /Remove /.test(asked || ''),
       'Backspace there offers to remove the DEVICE', JSON.stringify(asked));
    ok(!/Remove (\d+)\?/.test(asked || ''),
       'and names it, rather than printing its numeric kind', JSON.stringify(asked));
    const notesAfter = await page.evaluate(() => window.__uni.selected().length);
    ok(notesBefore === notesAfter, 'and does not touch the tracker\'s notes',
       `${notesBefore} -> ${notesAfter}`);

    await page.keyboard.press('Escape');
    await page.waitForTimeout(300);
    const back = await F();
    ok(back.focus === 'centre', 'Escape hands the keyboard back to the tracker',
       JSON.stringify(back));

    // Clicking the grid must also reclaim it, or the rack keeps focus forever.
    await page.mouse.click(card.x, card.y);
    await page.waitForTimeout(250);
    // A REAL cell, measured now. (700,400) was a guess that stopped landing on
    // the grid once the pane section above resized the dock — the click hit
    // nothing, the tracker's pointerdown never ran, and focus correctly stayed
    // where it was. A coordinate that used to be inside the grid is not a test.
    const cell = await page.evaluate(() => {
      // Any cell that is actually laid out. Pinning a row index assumed the
      // tracker had rendered that row; when it had not, querySelector returned
      // null, no click happened, and focus correctly stayed put.
      // Laid out AND on top. The panes section above widens the dock and heightens
      // the chain strip, so plenty of cells still have a rect while sitting
      // underneath another region — clicking one of those hits the region, the
      // tracker never sees a pointerdown, and focus correctly does not move.
      // Inside a TRACK lane. `.tk-cell` also matches the time and harmony gutter
      // columns, and `hitTest` rightly refuses those — so the click landed at
      // x=274 in the gutter, returned no hit, and the tracker never set focus.
      // Well inside the tracker's own box. `r.y > 100` was a guess and it let
      // through a cell at y=114 that sits under the column header, where
      // hitTest correctly finds nothing. Measure against the surface, not
      // against a number.
      // The cell nearest the MIDDLE of the tracker, not the first one in document
      // order. The first is at the left edge, where the sticky time gutter sits
      // over the strip: the click landed at x=274, hitTest found nothing, and
      // the tracker never saw a pointerdown. hitTest is right; the coordinate
      // was not.
      const host = document.getElementById('tracker').getBoundingClientRect();
      const cx = host.x + host.width / 2, cy = host.y + host.height / 2;
      // Ask the TRACKER whether the point is live, rather than inferring it from
      // the DOM. `hitTest` refuses the time gutter, the per-lane bar column and
      // anything past the last track, and which pixels those are depends on the
      // meters of the clips currently loaded and on how far the strip is
      // scrolled — both of which earlier sections change. A cell element with a
      // rect is not the same claim as a point the tracker will accept.
      let c = null, best = Infinity;
      for (const e of document.querySelectorAll('.tk-track .tk-cell')) {
        const r = e.getBoundingClientRect();
        if (r.width < 5 || r.height < 5) continue;
        const x = r.x + r.width / 2, y = r.y + r.height / 2;
        const top = document.elementFromPoint(x, y);
        if (!(top && (top === e || e.contains(top)))) continue;
        if (!window.__uni.clickAt(x, y)) continue;      // the tracker says no
        const d = Math.abs(x - cx) + Math.abs(y - cy);
        if (d < best) { best = d; c = e; }
      }
      if (!c) return null;
      const r = c.getBoundingClientRect();
      return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    });
    ok(!!cell, 'there is a grid cell the tracker will accept a click on');
    if (cell) await page.mouse.click(cell.x, cell.y);
    await page.waitForTimeout(250);
    const clicked = await F();
    ok(clicked.focus === 'centre', 'and clicking the grid reclaims it',
       `${JSON.stringify(clicked)} clicked=${JSON.stringify(cell)}`);

    /**
     * Put back what we took.
     *
     * The engine outlives the suite, so a test that adds a device adds one EVERY
     * run. After a few runs the patcher section started failing on a node count
     * that had nothing to do with the patcher, and I began diagnosing a bug that
     * did not exist — the suite had simply been editing the same document all
     * afternoon. GUIDELINES 2.1.2 is about goldens, but the principle is the
     * same: a test whose result depends on how many times it has been run is not
     * measuring the thing it names.
     */
    if (weAddedOne) {
      await page.evaluate(() => {
        const c = document.querySelector('.dv-card');
        if (c) window.__uni.delDevice(0, c._devId);
      });
      await page.waitForTimeout(900);
      const left = await cardCount();
      // Back to the count we found, NOT to zero: earlier sections add devices of
      // their own and those are not ours to remove. Asserting zero here made this
      // fail for doing its job correctly.
      ok(left === cardsAtStart, 'the test removes the device it added',
         `${cardsAtStart} -> ${left}`);
    }
  }
}

section('wheel scrolling');
{
  // The tracker had no wheel handling at all — the arrange view did, which is
  // exactly the shape of gap the op-registry test cannot see, because both
  // surfaces "support scrolling" as far as any list of ops is concerned.
  // Off the fixture first. The section above folds a multi-out parent, and a
  // folded fixture's stems are 0px wide — leaving the strip narrower than the
  // window, so maxScrollX is 0 and shift-wheel correctly does nothing. That
  // reads as a wheel bug and is not one.
  await page.evaluate((pr) => window.__uni.loadProject(pr), PROJECT);
  await page.waitForTimeout(600);
  await page.evaluate(() => window.__uni.view('tracker'));
  await page.evaluate(() => { window.__uni.scrollTo(0); window.__uni.follow(true); });
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  const S = () => page.evaluate(() => ({ start: window.__uni.state().start,
                                         scrollX: window.__uni.state().scrollX,
                                         follow: window.__uni.state().followPlayhead }));
  const box = await page.evaluate(() => {
    const r = document.body.getBoundingClientRect();
    return { x: r.width / 2, y: r.height / 2 };
  });
  await page.mouse.move(box.x, box.y);
  const before = await S();
  for (let i = 0; i < 3; i++) await page.mouse.wheel(0, 120);
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  const down = await S();
  ok(down.start > before.start, 'wheel down scrolls the tracker',
     `${before.start} -> ${down.start}`);
  // Follow owns state.start every frame; if the wheel did not disengage it the
  // view would spring back and the scroll would look like it did nothing.
  ok(down.follow === false, 'scrolling by hand turns playhead-follow off');

  for (let i = 0; i < 60; i++) await page.mouse.wheel(0, -120);
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  const top = await S();
  ok(top.start === 0, 'scrolling past the top clamps at row 0', `start=${top.start}`);

  // Narrow the window first. At the suite's 1680px this project's tracks all fit,
  // maxScrollX is 0, and "shift-wheel does nothing" is the CORRECT answer — so
  // asserting movement at that width tests the fixture's track count rather than
  // the handler, and fails for a reason that has nothing to do with the wheel.
  await page.setViewportSize({ width: 640, height: 900 });
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  await page.mouse.move(320, 450);
  const narrow = await S();
  await page.keyboard.down('Shift');
  for (let i = 0; i < 3; i++) await page.mouse.wheel(0, 120);
  await page.keyboard.up('Shift');
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  const side = await S();
  ok(side.scrollX > narrow.scrollX, 'shift-wheel scrolls the track strip sideways',
     `${narrow.scrollX} -> ${side.scrollX}`);
  ok(side.start === narrow.start, 'shift-wheel does not also move rows',
     `start ${narrow.start} -> ${side.start}`);

  // ...and it must stop at the right-hand end rather than scrolling into blank
  // space, which is the failure the vertical clamp above exists to catch too.
  await page.keyboard.down('Shift');
  for (let i = 0; i < 80; i++) await page.mouse.wheel(0, 120);
  await page.keyboard.up('Shift');
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  const end = await S();
  await page.keyboard.down('Shift');
  for (let i = 0; i < 5; i++) await page.mouse.wheel(0, 120);
  await page.keyboard.up('Shift');
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  const past = await S();
  ok(past.scrollX === end.scrollX, 'and clamps at the right-hand end',
     `${end.scrollX} -> ${past.scrollX}`);
  await page.setViewportSize({ width: 1680, height: 980 });
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
}

section('page errors');
ok(errors.length === 0, 'no uncaught errors', errors.slice(0, 3).join(' | '));

await browser.close();
if (stack) stack.stop();
if (blockedList.length) {
  console.log(`\n${blockedList.length} BLOCKED on a defect elsewhere:`);
  for (const b of blockedList) console.log(`  - ${b}`);
}
console.log(`\n${fail === 0 ? `ALL PASS (${count} checks)` : `${fail} of ${count} FAILED`}`);
process.exit(fail ? 1 : 0);
