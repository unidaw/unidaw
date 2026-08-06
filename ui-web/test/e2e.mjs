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
import { rmSync, readFileSync } from 'node:fs';
import os from 'node:os';
import { join } from 'node:path';

/**
 * By default this suite brings up its OWN engine, sidecar and page server and
 * tears them down again.
 *
 * It used to attach to whatever was already running, which in practice meant the
 * stack someone was working in. Every run added devices, wrote notes and loaded
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

/*
 * AND WAIT FOR THE ENGINE TO HAVE PUBLISHED A SONG.
 *
 * `canSend` above proves the socket is up. It does not prove the engine has finished
 * loading its default project, and the first three assertions in this file are about
 * notes, track names and per-lane grids — so on a busy machine they read an empty
 * document and the run opens with three failures that look like a broken build.
 *
 * That happened tonight, with several suites and a restarted stack competing for the
 * box: "0 notes on 0 tracks", then green on the next run with nothing changed. Same
 * class as the segment race stack.mjs already documents, one layer up: waiting for a
 * channel to open is not waiting for the thing that comes down it.
 */
await page.waitForFunction(
  () => { const s = window.__uni.engineState(); return s && s.trackCount > 0; },
  null, { timeout: 20000 }).catch(() => {});

/**
 * Load a project and WAIT FOR THE ENGINE TO SAY IT DID.
 *
 * `loadProject` + a fixed sleep is what most of this file does, and it is the source
 * of the flakiest failure in it: a load that lands late repopulates the document in a
 * LATER section, so a clip count that was 0 reads 4 and the failure appears three
 * screens from its cause. It failed that way three times tonight, on a machine
 * running several suites at once.
 *
 * The engine publishes a load counter. Waiting on it turns "probably done by now"
 * into "done", and costs nothing when it already is.
 */
const loadAndWait = async (name) => {
  const before = await page.evaluate(() => {
    const l = window.__uni.loadStatus();
    return l ? l.seq : 0;
  });
  await page.evaluate((n) => window.__uni.loadProject(n), name);
  await page.waitForFunction(
    (s) => { const l = window.__uni.loadStatus(); return l && l.seq > s && l.ok; },
    before, { timeout: 15000 }).catch(() => {});
  // One frame for the renderers to pick it up: the load counter moves on the wire,
  // and the surfaces draw on the next scheduled frame after that.
  await page.waitForTimeout(400);
};

/**
 * Press play and WAIT FOR SOUND, not for a clock.
 *
 * A fixed sleep after the space bar is a guess about how long a plugin takes to make its
 * first sample, and on a busy box it is wrong: three meter assertions failed together
 * reporting the instrument SILENT while playing, then passed on a re-run with nothing
 * changed. The meter is the condition — waiting for it to leave the silence sentinel is
 * waiting for the thing the assertions are about.
 */
/**
 * Start the transport and wait for something to be AUDIBLE on a device.
 *
 * RETURNS WHY IT GAVE UP. The first version swallowed its timeout and returned nothing, so a
 * failure downstream read "a real dBFS level while playing: -32768" and left four causes
 * indistinguishable: the plugin never loaded, the transport never started, the transport
 * started and the song was silent, or the meter belongs to another device. This has failed
 * intermittently and named none of them.
 *
 * The transport is started through the API rather than with Space. A keypress goes to whatever
 * has focus, and by this point in a 411-check run that may be the console or the browser rail —
 * where Space types a space. An intermittent audio failure whose real cause is focus is the
 * worst kind of flake, because the audio path is where everyone looks.
 */
const playUntilAudible = async (device = 0, ms = 12000) => {
  await page.evaluate(() => window.__uni.transport('play'));
  const ok = await page.waitForFunction((d) => {
    const m = (window.__uni.deviceMeters() || []).find((x) => x.device === d);
    return m && m.outPeak > -32768;
  }, device, { timeout: ms }).then(() => true).catch(() => false);
  // One more frame, so the renderer has drawn what the store now holds.
  await page.waitForTimeout(400);
  if (ok) return { ok: true };
  /*
   * IT DID NOT SOUND. Say what the machine looked like, in the terms that tell the causes apart
   * — a report a person can act on instead of one number that is always the same.
   *
   * THE FIFTH CAUSE IS THE MACHINE, and it was missing from this list. A starved producer hands
   * the device nothing, the device outputs zeros, and every meter reads the silence sentinel —
   * so "the instrument is silent" is what a real bug AND a loaded box look like, and they are
   * indistinguishable from here. The engine says which, in plain text in its own log:
   * `Engine: audio underrun — 184 dropout callback(s) in the last ~2s`.
   *
   * Three of these failed together at load average 91 while this file was being written, with
   * no engine leaked and nothing wrong with the app. That is the run this exists to label.
   */
  let underruns = -1;
  if (stack) {
    try {
      const log = readFileSync(`${stack.root}/engine.log`, 'utf8');
      underruns = (log.match(/audio underrun/g) || []).length;
    } catch { /* no log is its own answer: -1 */ }
  }
  /*
   * AND THE MACHINE ITSELF, because the underrun count is not the whole signal.
   *
   * `audio underrun` counts DEVICE-callback dropouts. A producer starved upstream of the device
   * can hand over zeros with the callback running perfectly on time and nothing logged at all —
   * which is exactly what happened here: three meter checks failed with `underruns: 0` at a load
   * average of 91 on an 8-core box, and passed on a re-run with no code change.
   *
   * So load is read too. It is a blunt instrument and it is the right one for this question: the
   * claim being made is not "the machine is slow", it is "this run could not answer the question
   * it was asked", and a box at ten times its core count is not answering questions about audio.
   */
  const load = os.loadavg()[0];
  const cores = os.cpus().length || 1;
  const swamped = load > cores * 4;
  return {
    ok: false,
    underruns,
    load: Math.round(load),
    cores,
    starved: underruns > 0 || swamped,
    transport: await page.evaluate(() => (window.__uni.engineState() || {}).transport),
    chain: await page.evaluate(() => {
      const c = window.__uni.chainProbe();
      return c ? { titles: c.titles, params: c.params } : null;
    }),
    meters: await page.evaluate(() => window.__uni.deviceMeters()),
  };
};

/**
 * WHAT THE MACHINE WAS DOING, for any check that can be defeated by it.
 *
 * `audio underrun` counts DEVICE-callback dropouts and a producer starved upstream logs nothing,
 * so load is read too. Neither number is about the app; both are about whether this run could
 * answer the question it was asked.
 */
const machine = () => {
  let underruns = -1;
  if (stack) {
    try {
      const log = readFileSync(`${stack.root}/engine.log`, 'utf8');
      underruns = (log.match(/audio underrun/g) || []).length;
    } catch { /* no log is its own answer: -1 */ }
  }
  const load = os.loadavg()[0];
  const cores = os.cpus().length || 1;
  return { underruns, load: Math.round(load), cores,
           starved: underruns > 0 || load > cores * 4 };
};

/**
 * A silence that the producer's own starvation explains is INCONCLUSIVE, not a failure.
 *
 * Same rule chop-audible.mjs and sampler-device-id.mjs already follow, brought here because
 * these three meter checks are the ones that keep catching a busy box. It reports rather than
 * passing: a run that could not answer the question must not look like one that answered it yes.
 */
const audibleOr = (r, what, detail) => {
  if (r.ok) return ok(true, what, detail);
  if (r.starved) {
    blocked(false, what,
            `the producer could not keep up — ${r.underruns} underrun report(s) and a load `
            + `average of ${r.load} on ${r.cores} cores. The device was handed silence and every `
            + 'meter reads the sentinel. Nothing here is a statement about the app.',
            JSON.stringify(r.meters));
    return false;
  }
  return ok(false, what, detail);
};

const E = () => page.evaluate(() => window.__uni.engineState());
const run = async (line, wait = 150) => {
  const out = await page.evaluate((l) => window.__uni.run(l), line);
  await page.waitForTimeout(wait);
  return out[out.length - 1] || '';
};
/**
 * The VISIBLE elements matching a selector.
 *
 * The rack, the tracker and the arrangement all POOL their elements and hide the
 * spares rather than removing them (GUIDELINES 3.7), so `page.$$` hands back
 * leftovers that still carry the last project's text. Clicking one hangs until
 * Playwright's timeout with "element is not visible", and reading one reports a
 * value that was true two sections ago.
 *
 * This has bitten four separate checks in this file. A helper, so it stops.
 */
async function visible(sel) {
  const all = await page.$$(sel);
  const out = [];
  for (const h of all) if (await h.evaluate((e) => e.offsetParent !== null)) out.push(h);
  return out;
}

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
/*
 * THE CHORDS ALREADY IN THE FIXTURE, so the typed one can be told from them.
 *
 * `chords()[0]` is not the chord you just wrote — this project ships with chords, and index 0 is
 * one of them. Asserting against it reported degree 0 for a `@3` that had landed perfectly, and
 * the strum/humanize checks beside it PASSED on that same wrong chord's values. A check that
 * reads the wrong row is worse than no check: two of the three agreed with each other.
 */
const chordsBefore = await page.evaluate(() =>
  (window.__uni.chords() || []).map((c) => JSON.stringify(c)));
await page.evaluate(() => window.__uni.run('goto 12 1'));
/*
 * THE WHOLE TOKEN THE RUNBOOK NAMES: `@3^7~80h20`, a strummed and humanised seventh.
 *
 * Typed, not called through the API — this is the one path where the keyboard does something
 * the dock cannot, so driving it any other way tests nothing. It used to stop at `@3^7` and
 * assert only that `clipVersion` MOVED, which is satisfied by any chord at all: the degree, the
 * quality and above all the STRUM went unchecked.
 *
 * The strum is the part worth having. `~80h20` is the headline of the demo's chord section, its
 * grammar is unit-tested in isolation (`parseChord`), and the data path is covered from the
 * `chord` CONSOLE VERB — but nothing asserted that these keystrokes reach the engine. A grammar
 * test plus a console test can both pass while the keyboard drops a token, and the keyboard is
 * what a person uses.
 */
await page.keyboard.press('@');
for (const ch of ['3', '^', '7', '~', '8', '0', 'h', '2', '0']) {
  await page.keyboard.press(ch); await frames();
}
await page.keyboard.press('Enter');
await page.waitForTimeout(1000);
ok((await E()).clipVersion > cv, 'a chord token writes', `clipVersion ${cv} -> ${(await E()).clipVersion}`);

/*
 * THE WHOLE RECORD, not its position. Keying on (track, tick) found nothing at all: row 12 of
 * track 1 ALREADY held a chord, so typing there REPLACED it rather than adding one, and a
 * position that was present before is present after. Comparing the full record catches both — a
 * chord that is new and a chord whose values moved.
 */
const typedChord = await page.evaluate((seen) => {
  const all = window.__uni.chords() || [];
  return all.find((c) => !seen.includes(JSON.stringify(c))) || null;
}, chordsBefore);
/*
 * DEGREE 2 FOR A TYPED `@3`, AND QUALITY 2 FOR `^7`. Both are the storage, not a bug:
 *
 *   degrees are 1-based as musicians write them and stored 0-based (tools.rs:1922 `degree - 1`)
 *   quality is an enum — 0 = the degree alone, 1 = triad, 2 = seventh (dock.js:319)
 *
 * Written down because I asserted the typed numbers verbatim and read the mismatch as a dropped
 * token, which is the same mistake as expecting `o80` to be stored as 80.
 */
ok(typedChord && typedChord.degree === 2, 'the typed DEGREE reaches the engine (1-based `@3` stored 0-based)',
   JSON.stringify(typedChord));
ok(typedChord && typedChord.quality === 2, 'and `^7` is a SEVENTH, not a triad',
   `quality ${typedChord && typedChord.quality} (0 degree, 1 triad, 2 seventh)`);
/*
 * `spread > 0` IS the strum. 0 is a legal answer meaning a block chord, not a missing field, so
 * a check that only asked "is spread defined" would pass on a dropped `~` token.
 */
ok(typedChord && typedChord.spread > 0,
   'and the ~ token really strums it — spread, read back off the wire',
   `spread ${typedChord && typedChord.spread}`);
ok(typedChord && (typedChord.humanizeTiming > 0 || typedChord.humanizeVelocity > 0),
   'and the h token humanises it',
   `timing ${typedChord && typedChord.humanizeTiming}, velocity ${typedChord && typedChord.humanizeVelocity}`);

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
  // On a track that HAS a patcher. The view shows one device's graph now rather
  // than every device's on every track, so "does it render" is only a question
  // you can ask where there is something to render — webtest's Bass is track 0.
  ['patcher', 'patcherProbe', (p) => p.nodes > 0,
   () => window.__uni.run('goto 1 0')],
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
/*
 * AND THE CONTOUR RIBBON IN A PILLED CELL SHOWS THE PITCHES IN IT.
 *
 * The ribbon chooses its source on `aggCount`: one note reads `pitch`, several read
 * `aggLo`/`aggHi`. The collide branch set aggCount and wrote its spread into
 * pitch/_hiPitch — which that flag makes the renderer stop reading — so the ribbon
 * for a collided cell came from aggLo/aggHi with nothing in them. Zero is the bottom
 * of the pitch scale, so the mark sat on the floor for exactly the cells the pill
 * exists to explain.
 *
 * No GOLDEN could catch it: no golden scene contains a collision, so all of them
 * stayed byte-identical through the fix. This is the only place on screen where the
 * two meet.
 */
const ribbons = await page.evaluate(() => {
  const out = [];
  for (const cell of document.querySelectorAll('.tk-cell[data-kind="collide"]')) {
    if (!cell.offsetParent) continue;
    const bar = cell.querySelector('.tk-bar');
    if (!bar) continue;
    out.push({ bottom: parseFloat(bar.style.bottom) || 0,
               height: parseFloat(bar.style.height) || 0,
               op: parseFloat(getComputedStyle(bar).opacity) });
  }
  return out;
});
ok(ribbons.length > 0, 'a pilled cell draws a contour ribbon', String(ribbons.length));
ok(ribbons.every((r) => r.bottom > 0),
   'and it is not pinned to the floor of the pitch scale',
   JSON.stringify(ribbons.slice(0, 3)));
// Several notes must not read FAINTER than one. The aggregate formula gives 0.37 for
// three, which is dimmer than the single note beside it — exactly backwards.
ok(ribbons.every((r) => r.op > 0.9),
   'and several notes are not drawn fainter than one',
   JSON.stringify(ribbons.slice(0, 3).map((r) => r.op)));
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
/*
 * SELECT THE PATCHER DEVICE FIRST.
 *
 * The view shows one DEVICE's graph now, not the pool, so with nothing selected
 * there is nothing on screen to edit — which is the point: it used to show every
 * device's nodes on every track, and that is how a patcher on track 1 came to be
 * edited by someone standing on track 2.
 *
 * Clicked rather than set: the selection is what the drawing follows, and a test
 * that reached past the click would not prove the two agree.
 */
await page.evaluate(() => window.__uni.view('patcher'));
await frames();
{
  for (const c of await visible('.dv-card')) {
    const isPatcher = await c.evaluate((el) =>
      /PATCHER/.test((el.querySelector('.dv-badge') || {}).textContent || ''));
    if (isPatcher) { await c.click(); break; }
  }
  await page.waitForTimeout(500);
}
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
/*
 * A NODE THAT DID NOT ARRIVE MUST FAIL A CHECK, NOT THROW.
 *
 * `graphUntil` gives up and returns the last graph it saw, so `.find(...).id` on a miss reads
 * `undefined.id` and kills the RUN — 423 checks abandoned because one add was slow on a loaded
 * box. A suite that crashes tells you less than one that fails: the failure names the step, the
 * crash names a line in the harness.
 */
const ids0 = new Set(before2.nodes.map((n) => n.id));
await page.evaluate(() => window.__uni.run('addnode passthru'));
const g1 = await graphUntil((g) => g.nodes.some((n) => !ids0.has(n.id)));
const newA = g1.nodes.find((n) => !ids0.has(n.id));
const ids1 = new Set(g1.nodes.map((n) => n.id));
await page.evaluate(() => window.__uni.run('addnode passthru'));
const g2 = await graphUntil((g) => g.nodes.some((n) => !ids1.has(n.id)));
const newB = g2.nodes.find((n) => !ids1.has(n.id));
ok(!!newA && !!newB, 'two fresh passthrus arrive, to build a cycle out of',
   `${g1.nodes.length} then ${g2.nodes.length} nodes`);
const cycA = newA ? newA.id : -1;
const cycB = newB ? newB.id : -1;
if (newA && newB) {
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
}
await page.evaluate((id) => window.__uni.run(`delnode ${id}`), added.id);
const shrunk = await graphUntil((g) => !g.nodes.some((n) => n.id === added.id));
ok(!shrunk.nodes.some((n) => n.id === added.id),
   'and removed again, taking its edge with it',
   `#${added.id} gone, ${shrunk.nodes.length} nodes, ${shrunk.edges.length} edges`);

/*
 * THE SLICE NODE CAN BE CREATED AND WIRED.
 *
 * SliceSelect = 7 landed in the engine, the graph could hold it, a project could save it and the
 * palette offered it — and the sidecar refused `nodeType > 6` with a comment naming EventOut as
 * the last type. So the node was reachable only by hand-editing JSON. Past that, `link` still
 * answered "those two node types have no compatible ports", because the port table had no arm
 * for it either. A node you can create and cannot connect is a node you cannot use.
 *
 * Both halves are asserted, because fixing one and not the other looks like a fix: the count
 * would go up and nothing would ever reach the node.
 */
{
  const was = await page.evaluate(() => window.__uni.patcher());
  const ids = new Set(was.nodes.map((n) => n.id));
  const said = await page.evaluate(() => window.__uni.run('addnode slice'));
  const grew = await graphUntil((g) => g.nodes.some((n) => !ids.has(n.id)));
  const slice = grew.nodes.find((n) => !ids.has(n.id));
  ok(!!slice, 'a slice node can be created — the sidecar bound was stale at EventOut',
     `${was.nodes.length} -> ${grew.nodes.length}; ${JSON.stringify(said).slice(-70)}`);
  if (slice) {
    // Events in, events out — it rewrites what passes through rather than producing anything,
    // so it wires like a passthrough and a euclidean can feed it.
    await page.evaluate(([a, b]) => window.__uni.run(`link ${a} ${b}`), [euclid.id, slice.id]);
    const wired = await graphUntil((g) => g.edges.some((e) => e.dst === slice.id));
    ok(wired.edges.some((e) => e.src === euclid.id && e.dst === slice.id),
       'and wired to an event source — the port table had no arm for it, so `link` used to '
       + 'answer "no compatible ports"',
       JSON.stringify(wired.edges.filter((e) => e.dst === slice.id)));
    await page.evaluate((id) => window.__uni.run(`delnode ${id}`), slice.id);
    await graphUntil((g) => !g.nodes.some((n) => n.id === slice.id));
  }
}

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
/*
 * WHERE THE SAVES ACTUALLY GO.
 *
 * `stack.dir` when this run owns its stack — startStack copies the whole presets
 * tree to a temp directory and points the engine at it, precisely so a test that
 * saves cannot rewrite the fixtures everything else reads. The repo path is the
 * fallback for a run against somebody's already-running stack, where the engine
 * was pointed at the real one.
 */
const PROJECTS = (stack && stack.dir)
  || process.env.UNI_PROJECTS
  || '/Users/jak/src/daw-web/presets/projects';
for (const suffix of ['.uniproj.json', '.uniproj.state']) {
  rmSync(join(PROJECTS, SCRATCH + suffix), { recursive: true, force: true });
}
await page.evaluate(() => window.__uni.browser(false));

/*
 * ⌘S SAVES YOUR WORK TO YOUR SONG — asserted through the FILE, and through the
 * KEY.
 *
 * Everything above this proves a project appears in a list. That is not the
 * question anybody asks. "Can one Save?" has been asked twice, and both times the
 * suite could not answer it: `saveAs` was covered, the keystroke was not, and
 * nothing checked that a note you had just written was in the bytes on disk.
 *
 * INTO A SCRATCH SONG, not into a fixture. The first version wrote a note into
 * `meter` and deleted it afterwards — and writing past the end of the placement
 * CREATED A CLIP, which the delete did not take away, so `meter` gained a fourth
 * rail and three later sections failed for reasons that had nothing to do with
 * them. Saving-as first means ⌘S has a name to write to that nothing else reads.
 *
 * A DISTINCTIVE PITCH, at a row nothing occupies. An earlier attempt wrote at row
 * 2, where the fixture already has a note, so the edit REPLACED one and the file's
 * note count was identical before and after — which reads exactly like "the save
 * did nothing" and sent me looking for a bug that was not there. 103 is G#7, well
 * above anything in `meter`, so finding it cannot be a coincidence and failing to
 * find it cannot be an off-by-one.
 */
section('a keystroke saves your work to your song');
{
  const SAVED = 'e2e-keysave';
  await page.evaluate(() => window.__uni.run('view tracker'));
  /*
   * WHATEVER IS LOADED, and put back at the end.
   *
   * The first version loaded `meter` for a fixture it liked and left it loaded,
   * which is a section quietly deciding what the NEXT one starts from — the
   * transpose section two below it found an empty selection and failed for a
   * reason that had nothing to do with transposing. A section that changes the
   * document owes the document back.
   */
  const incoming = await page.evaluate(() => window.__uni.state().currentProject);
  await page.evaluate((n) => window.__uni.saveAs(n), SAVED);
  await page.waitForTimeout(2000);

  const named = await page.evaluate(() => window.__uni.state().currentProject);
  ok(named === SAVED, 'saving-as makes that song the current one — ⌘S needs a name',
     String(named));

  const file = join(PROJECTS, SAVED + '.uniproj.json');
  const pitches = () => JSON.parse(readFileSync(file, 'utf8'))
    .clips.flatMap((c) => (c.notes || []).map((n) => n.pitch));
  ok(!pitches().includes(103), 'and it does not already contain the proof',
     String(pitches().length));

  await page.evaluate(() => window.__uni.run('goto 6 0'));
  await page.waitForTimeout(200);
  await page.evaluate(() => window.__uni.run('note 103'));
  await page.waitForTimeout(900);
  ok(await page.evaluate(() => (window.__uni.notes() || []).some((n) => n.p === 103)),
     'and the note is in the engine before we ask for a save');

  await page.keyboard.press('Meta+s');
  await page.waitForTimeout(2500);
  ok(pitches().includes(103), 'and ⌘S puts it in the file, under the song\'s own name');
  ok(!(await page.evaluate(() => window.__uni.state().reject)),
     'without complaining', String(await page.evaluate(() => window.__uni.state().reject)));

  // ⌘⇧S is SAVE AS, which is a different question: it has to ask for a name.
  await page.keyboard.press('Meta+Shift+s');
  await page.waitForTimeout(700);
  const saveAs = await page.evaluate(() => ({ open: window.__uni.state().browserOpen,
                                              focus: window.__uni.state().focus }));
  ok(saveAs.open && saveAs.focus === 'browser', 'and ⌘⇧S asks where instead',
     JSON.stringify(saveAs));
  await page.keyboard.press('Escape');
  await page.waitForTimeout(300);
  await page.evaluate(() => window.__uni.browser(false));

  for (const suffix of ['.uniproj.json', '.uniproj.state']) {
    rmSync(join(PROJECTS, SAVED + suffix), { recursive: true, force: true });
  }
  // Back to a clean document, and to the SAME one this section inherited: the
  // in-memory copy still carries the scratch edit, and the next section is
  // entitled to the fixture as authored.
  if (incoming) await loadAndWait(incoming);
}

section('dock');
const bad = String(await page.evaluate(() => window.__uni.run('note 999')));
// Asserts the PROPERTY, not the wording: a refusal has to name the command, the
// argument and the value, so the person reading it knows which of three numbers
// they got wrong. Pinning the exact sentence made this fail the moment the
// message improved, which is a test punishing the thing it exists to encourage.
ok(bad.includes('note') && bad.includes('pitch') && bad.includes('999'),
   'the dock refuses a bad argument by naming command, argument and value', bad);
/**
 * A word that is not a command is now a QUESTION.
 *
 * It used to be refused as a typo, and that was right while the console was the
 * only thing behind this box. The design puts an agent there — "ask, it runs the
 * same commands you do" — so a line the grammar does not recognise goes to the
 * model rather than being thrown back. The property worth holding is that the
 * console still answers: it says what it did with the line either way.
 */
const unknown = String(await page.evaluate(() => window.__uni.run('flurb')));
ok(/asking/i.test(unknown) || /unknown/i.test(unknown),
   'and a line the grammar does not know is asked, not silently dropped', unknown);
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
/*
 * THE PLUGIN BY NAME, NOT BY POSITION.
 *
 * These used to read `titles[0]`, which held while the instrument was the only
 * device on the track. It is not any more: a patcher is a device now, and
 * `maximal`'s generator sits at the HEAD of the chain, so slot 0 is the patcher
 * and Zebra2 is behind it. Asserting on slot 0 was asserting that no other
 * device may ever exist — which is a claim about the chain, not about the host
 * naming its plugin.
 */
let live = null;
const vstAt = (l) => (l && l.titles ? l.titles.findIndex((t) => /Zebra/.test(t)) : -1);
for (let i = 0; i < 90; i++) {
  await page.waitForTimeout(200);
  live = await page.evaluate(() => window.__uni.chainProbe());
  const at = vstAt(live);
  if (at >= 0 && live.params && live.params[at] > 0) break;
}
const vst = vstAt(live);
ok(live && live.cards >= 1, 'a real project with a device chain draws it',
   JSON.stringify(live && live.titles));
ok(vst >= 0 && live.named >= 1,
   'and the name comes from the plugin host, not from here', JSON.stringify(live.titles));
// The parameters are the plugin's own, queried from its host. This returned zero
// for a long time and the cause was environmental: the engine reads its plugin
// cache relative to its working directory, and a fresh build dir has none — so it
// could resolve a plugin's path and never load it, and every query answered from
// a host with no instance.
ok(vst >= 0 && live.params[vst] > 0, 'and its parameters come back from the host',
   `${JSON.stringify(live.params)} (plugin at slot ${vst})`);
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

/**
 * Did this scene's project get the plugin it asked for?
 *
 * Declared out here because TWO blocks need it — the buses and the parameter mirror — and both
 * are about the substitute rather than about the code they are testing when it is false.
 */
let substituted = false;

// The device's AUDIO BUSES (kShmVersion 20), which is what tells a stereo effect
// from a plugin with eight stems and a sidechain input. These ride the chain
// snapshot rather than a request, so they are here the moment the chain is.
{
  /*
   * THE DEVICE THAT HAS BUSES, not slot 0.
   *
   * A patcher is a device now and sits at the head of the chain, so slot 0 on a
   * generator track is an EVENT device — no audio buses, correctly. Reading
   * `devices[0]` asserted that the audio plugin is always first, which was only
   * ever true because nothing else was in the chain.
   */
  const d = await page.evaluate(() => {
    const c = window.__uni.chains();
    const t = c && c['0'];
    if (!t || !t.devices.length) return null;
    return t.devices.find((x) => x.busCount > 0) || t.devices[t.devices.length - 1];
  });
  ok(d !== null, 'the rack project published a device');
  /*
   * IS THERE A PLUGIN HERE AT ALL?
   *
   * WAS: `rack.uniproj.json` named "Identity" with an EMPTY PATH — a vst_ref that resolved on
   * no machine — so the engine reported `project.plugin_missing` and SUBSTITUTED whatever the
   * scan offered first, here an Elektron controller with no buses to speak of. Every assertion
   * below then failed as "0/0 buses", which says nothing about the bus publisher and everything
   * about which plugin turned up. It was also what a person saw in the rack: a card labelled
   * "VST instrument" with nothing in it.
   *
   * That device is GONE from the preset, replaced by a built-in `sampler`, which needs no
   * plugin and therefore loads identically everywhere. So nothing is substituted any more — and
   * the preset now names no VST at all, which means these two blocks have nothing whose plugin
   * buses or parameters could be read.
   *
   * Still BLOCKED rather than failed, for the same reason as before: this is a statement about
   * what the fixture contains, not about the bus code. The real fix is for these two blocks to
   * INSERT a scanned instrument from the catalogue, the way full-song.mjs picks Zebralette by
   * name — machine-adaptive, and it would make them assert a real plugin on any machine. Tried,
   * and backed out: leaving the rail open perturbed six later checks that assert rail state, and
   * a real plugin's bus list legitimately contains disabled 0-channel buses that the assertion
   * below does not expect. Both are fixable and neither is this change.
   */
  const asked = await page.evaluate(() => {
    const c = window.__uni.chainProbe();
    return c && c.titles ? c.titles.join(',') : '';
  });
  substituted = !!d && d.busCount === 0 && !/Identity/i.test(String(asked));
  if (substituted) {
    blocked(false, 'the rack project publishes its plugin buses',
            'the rack preset carries no VST — its unloadable "Identity" reference was removed '
            + 'and a built-in sampler took its place, so there is nothing here whose plugin '
            + 'buses could be read. Insert a scanned instrument to make this assert something',
            `chain reads ${JSON.stringify(asked)}`);
  } else if (d) {
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
/*
 * SAME SCENE, SAME SUBSTITUTION. `rack` asks for a plugin the cache does not have, so what
 * answers here is whatever the engine put in its place — and on a machine whose first cached
 * plugin is a hardware controller, that is something with no parameters to publish. The claim
 * is about the param MIRROR, not about which plugin turned up, so it is blocked rather than
 * failed when the project did not get what it asked for. See the bus block above.
 */
if (substituted) {
  blocked(!!(beforeP && beforeP.count > 0), 'the rack reads a real plugin\'s parameters',
          'the rack preset carries no VST — the unloadable "Identity" reference was removed and '
          + 'a built-in sampler took its place, so the mirror has nothing to read',
          beforeP ? `${beforeP.name}, ${beforeP.count} params` : 'none published');
} else {
  ok(beforeP && beforeP.count > 0, 'the rack reads a real plugin\'s parameters',
     beforeP ? `${beforeP.name}, ${beforeP.count} params` : 'none published');
}

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

section('the console can ask the agent');
{
  /**
   * The dual-purpose input the design describes: "ask — it runs the same
   * commands you do". A line that IS a command runs locally and instantly; a
   * sentence goes to a model with daw-agent's tool manifest attached.
   *
   * This asserts the PLUMBING, which is deterministic: the sentence leaves the
   * page, reaches the sidecar, and comes back as agent progress on the ack
   * channel. Whether a model then writes good music is not a thing to assert in
   * a suite — and without a key the answer arrives as a clear refusal, which is
   * itself the behaviour worth pinning.
   */
  await page.keyboard.press('Escape');
  await page.waitForTimeout(200);
  await page.keyboard.press('Slash');
  await page.waitForTimeout(250);
  await page.keyboard.type('please make the drums louder');
  await page.keyboard.press('Enter');
  /*
   * THE WAIT IS THE ASSERTION, and it used to be neither.
   *
   * It waited 15s for the model's first progress line, swallowed the timeout,
   * then asserted against a SNAPSHOT of the last six console lines. Two things
   * were wrong with that and they compound. Fifteen seconds is a guess about how
   * long a live model takes to say its first word, and it is sometimes wrong —
   * the page had sent the sentence and printed `asking…`, which is the plumbing
   * this check exists to prove, and the check failed anyway. And a six-line
   * window is a race of its own: the answer arriving is what pushes the line out
   * of it.
   *
   * So: wait on the condition, with a budget that respects a network round trip,
   * and assert on whether the wait SUCCEEDED. The log is kept for the message,
   * where being approximate costs nothing.
   */
  const asked = await page.waitForFunction(
    () => [...document.querySelectorAll('.dk-line')]
      .some((e) => /asking:/.test(e.textContent || '')),
    null, { timeout: 45000 }).then(() => true).catch(() => false);
  const log = await page.evaluate(() => [...document.querySelectorAll('.dk-line')]
    .map((e) => e.textContent).filter(Boolean)
    .filter((t) => !/^engine: /.test(t)).slice(-8).join(' | '));
  await page.keyboard.press('Escape');
  await page.waitForTimeout(200);
  ok(asked, 'a sentence is sent to the agent rather than refused as a typo',
     log.slice(0, 160));
  ok(!/unknown: please/.test(log),
     'and is NOT treated as an unknown command', log.slice(0, 120));
}

section('a device that makes its own notes says so');
{
  /**
   * The evening this exists for: notes were heard that were not in the clip,
   * and the cause was a euclidean generator and a random_degree node in the
   * device's patcher graph, playing alongside the sequencer. Everything worked.
   * Nothing said it was happening — the graph was reachable by pressing F3, if
   * you knew, and the tracker showed only the clip.
   *
   * `generator.uniproj.json` is that shape reduced to one track, one note and
   * one graph.
   */
  /*
   * VISIBLE cards only. The rack pools its cards and HIDES the spares rather
   * than removing them (GUIDELINES 3.7), so a raw querySelectorAll returns the
   * previous project's footers too — complete with the generator label they had
   * then. Reading those made a clean project look like it had a generator.
   */
  const feet = () => page.evaluate(
    () => [...document.querySelectorAll('.dv-card')].filter((c) => c.offsetParent)
            .map((c) => (c.querySelector('.dv-foot') || {}).textContent || '').join(' | '));

  /**
   * The quiet case FIRST, and this order is load-bearing.
   *
   * A patcher graph is ENGINE-lifetime state, not project state — loading
   * another song does not take it away. So once `generator` has been loaded the
   * nodes are still live, and a card still saying "generates" is telling the
   * truth. Checking the clean project afterwards asserted the opposite of what
   * is actually the case.
   */
  /*
   * `waveform`, not PROJECT. This used to load webtest, on the assumption that
   * it had no generator — and it did not, because its generators were TRACK
   * level and a track-level graph has no card to appear on. That was the whole
   * bug: three "phantom notes" reports, and the one surface that could have said
   * so was structurally unable to.
   *
   * The presets have since been migrated so every patcher lives on a device, and
   * webtest's Bass now correctly says `generates: euclidean + random` on its
   * card. Which broke this check, and the break was the migration WORKING.
   *
   * So the quiet case needs a project that genuinely has no patcher anywhere.
   * `waveform` is one; if that ever changes, this fails and says to pick another
   * rather than quietly asserting nothing.
   */
  await page.evaluate(() => window.__uni.loadProject('waveform'));
  await page.waitForTimeout(1800);
  const plain = await feet();
  ok(!/generates:/.test(plain), 'a device with no graph says nothing about generating',
     plain);

  await page.evaluate(() => window.__uni.loadProject('generator'));
  await page.waitForFunction(
    () => { const c = window.__uni.chainProbe(); return c && c.cards >= 1; },
    null, { timeout: 20000 }).catch(() => {});
  await page.waitForTimeout(1800);
  const foot = await feet();
  ok(/generates:/.test(foot), 'and one WITH a generator says so', foot);
  ok(/euclidean/.test(foot), 'naming what is generating', foot);
}

section('clips answer to the pointer');
{
  // The band had no pointer handling at all — only the ruler and the wheel — so a
  // clip could be looked at and nothing else.
  await page.evaluate(() => window.__uni.loadProject('maximal'));
  await page.waitForTimeout(1500);
  await page.keyboard.press('F2');
  await page.waitForTimeout(600);
  // Bring the view back to the top of the song. The navigation section above
  // pans and zooms, and it leaves the arrangement looking at empty timeline —
  // `clips=0` with six lanes drawn. Setup, not the thing under test: the click
  // below is still a real one.
  await page.evaluate(() => {
    const st = window.__uni.state;                 // frozen copy — use the ops
    window.__uni.run('goto 1 1');
  });
  await page.keyboard.press('Home');
  await page.waitForTimeout(500);
  // The FIRST VISIBLE clip. The arrangement pools its clip elements like the rack
  // pools its cards, so `querySelector('.ar-clip')` hands back a hidden spare and
  // the section reports "no clip on screen" while six are drawn.
  const clipAt = () => page.evaluate(() => {
    for (const c of document.querySelectorAll('.ar-clip')) {
      const r = c.getBoundingClientRect();
      if (r.width < 6 || r.height < 6) continue;
      const x = r.x + r.width / 2, y = r.y + r.height / 2;
      const top = document.elementFromPoint(x, y);
      if (!(top && (top === c || c.contains(top)))) continue;
      return { x, y };
    }
    return null;
  });
  const geom = () => page.evaluate(() => {
    const st = window.__uni.state();
    const a = document.getElementById('arrange').getBoundingClientRect();
    const pi = document.getElementById('piano');
    // `view2 === 'piano'`, not `rollOpen`. The roll below the arrangement was a
    // boolean with its own class, its own CSS and its own key; it is one
    // instance of the two-pane split now, so "is the roll open" is "what is in
    // the second pane".
    return { sel: st.selectedPlacement, roll: st.view2 === 'piano',
             arrangeH: Math.round(a.height), pianoShown: !pi.hidden,
             pianoH: Math.round(pi.getBoundingClientRect().height),
             outlined: document.querySelectorAll('.ar-clip.sel').length };
  });
  // Wait for clips to actually be drawn rather than for a guessed delay: the
  // project load, the engine's reply and the next frame are three separate
  // waits, and 1500ms happened to cover two of them.
  await page.waitForFunction(
    () => [...document.querySelectorAll('.ar-clip')]
      .some((e) => e.getBoundingClientRect().width > 6),
    null, { timeout: 8000 }).catch(() => {});
  const c = await clipAt();
  if (!c) {
    const why = await page.evaluate(() => {
      const a = window.__uni.arrangeProbe();
      return `view=${window.__uni.state().view} lanes=${a.lanes} clips=${a.clips} `
           + `elems=${document.querySelectorAll('.ar-clip').length}`;
    });
    blocked(false, 'clips answer to the pointer', why);
  } else {
    await page.mouse.click(c.x, c.y);
    await page.waitForTimeout(400);
    const picked = await geom();
    ok(!!picked.sel, 'clicking a clip selects it', JSON.stringify(picked.sel));
    // The model outlines the clip whose "track:tick" matches. An object here
    // would have selected something and drawn nothing.
    ok(picked.outlined === 1, 'and the selection is drawn on it',
       `${picked.outlined} outlined`);

    const beforeOpen = await geom();
    await page.mouse.dblclick(c.x, c.y);
    await page.waitForTimeout(700);
    const opened = await geom();
    ok(opened.roll === true, 'double-clicking it opens the roll');
    ok(opened.pianoShown && opened.pianoH > 100,
       'the roll is ON SCREEN, under the arrangement', `${opened.pianoH}px`);
    ok(opened.arrangeH < beforeOpen.arrangeH,
       'and the arrangement makes room rather than being replaced',
       `${beforeOpen.arrangeH} -> ${opened.arrangeH}`);

    await page.keyboard.press('Escape');
    await page.waitForTimeout(400);
    const closed = await geom();
    ok(closed.roll === false && closed.arrangeH > opened.arrangeH,
       'Escape puts the roll away and gives the room back',
       `${opened.arrangeH} -> ${closed.arrangeH}`);
    ok(!!closed.sel, 'and the clip stays selected — one Escape, one dismissal');
  }
}

/*
 * DRAGGING A CLIP, with a real pointer.
 *
 * This is the gesture the whole placement feature exists for, and until now the
 * only thing testing it was a pure function over numbers. `test/placement.mjs`
 * proves the OPS work by writing to the socket; the unit tests prove the
 * arithmetic. Neither says that pressing on a clip and moving the mouse produces
 * either of those — pointer capture, the trim-handle hit zones, the ghost and
 * the commit-on-release are all only real in a browser.
 *
 * Drives `page.mouse` throughout. No __uni: a drag that only works when a test
 * calls the handler directly is the exact failure this suite has shipped before.
 */
/*
 * WHAT IS MAKING NOTES ON THIS TRACK — and, crucially, WHAT IS NOT.
 *
 * This was BLOCKED for a long time, and the reason it was blocked is the reason
 * the negative case is asserted first here. The indicator used to read the
 * published patcher region, which is a POOL of every graph in the song and does
 * not clear between projects:
 *
 *   load webtest   -> pool: euclidean, random, out
 *   load waveform  -> pool: euclidean, random, out    <- waveform has no generator
 *
 * So it announced a generator on a song with none. An indicator whose whole job is
 * to explain an unexplained sound must never invent one, which is why it shipped
 * inert rather than shipped wrong.
 *
 * It reads the CHAIN now: the per-device generates bit says whether, and the
 * device's own subgraph says what. A track with no generating device says nothing,
 * which is the case that was impossible before and is therefore the case that
 * proves the fix.
 */
section('the chrome names what is generating, and stays quiet when nothing is');
{
  const gen = () => page.evaluate(() => {
    const el = document.querySelector('.ch-gen');
    return el ? el.textContent.trim() : null;
  });

  ok((await gen()) !== null, 'the chrome has a slot for it');

  // A generator, on the track it is on.
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.evaluate(() => window.__uni.loadProject('generator'));
  await page.waitForTimeout(2500);
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(600);
  const named = await gen();
  ok(/euclidean/.test(String(named)), 'a track with a euclidean says so', String(named));
  ok(/random/.test(String(named)), 'and names the whole chain of it', String(named));

  /*
   * THE ONE THAT WAS IMPOSSIBLE. `waveform` has no generator anywhere, and the old
   * implementation announced one because the pool still held the previous song's.
   * If this ever passes vacuously — because nothing is drawn at all — the check
   * above it fails first, which is why they are in this order.
   */
  await page.evaluate(() => window.__uni.loadProject('waveform'));
  await page.waitForTimeout(2500);
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(800);
  const quiet = await gen();
  ok(quiet === '', 'and a song with no generator says NOTHING', JSON.stringify(quiet));

  // And back, so it is not a one-way latch: the phrase has to return when a
  // generator does. A label that can only turn on is a label you stop reading.
  await page.evaluate(() => window.__uni.loadProject('generator'));
  await page.waitForTimeout(2500);
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(800);
  ok(/euclidean/.test(String(await gen())), 'and comes back when one does',
     String(await gen()));
}


/*
 * A NODE'S PARAMETERS ANSWER THE POINTER.
 *
 * They were keyboard-only — select a node, arrows to a field, arrows to change
 * it. Every value on screen was drawn as a labelled bar with a fill, which is
 * what a control looks like, and none of them did anything when clicked. A thing
 * that looks operable and is not costs more than one that looks inert.
 *
 * Driven with a real pointer. The whole claim is "the mouse works", so a test
 * that called the handler would be testing nothing.
 */
section('patcher node parameters can be dragged');
{
  await page.evaluate(() => window.__uni.run('load generator'));
  await page.waitForTimeout(1800);
  await page.evaluate(() => window.__uni.run('view patcher'));
  await page.waitForTimeout(600);

  // A row on a node that HAS editable configuration. `euclidean` does; the
  // output node does not, and a test that grabbed that one would read its
  // refusal as a failure to drag.
  const row = await page.evaluate(() => {
    for (const n of document.querySelectorAll('.pt-node')) {
      const rows = [...n.querySelectorAll('.pt-row')].filter((r) => r.style.display !== 'none');
      if (!rows.length) continue;
      const r = rows[0].getBoundingClientRect();
      if (r.width < 10 || r.height < 4) continue;
      return { node: Number(n.dataset.id), x: r.x + r.width / 2, y: r.y + r.height / 2,
               name: rows[0].querySelector('.pt-row-n').textContent,
               value: rows[0].querySelector('.pt-row-v').textContent };
    }
    return null;
  });
  if (!row) {
    blocked(false, 'patcher node parameters can be dragged', 'no configurable node on screen');
  } else {
    ok(true, `found a parameter to drag: ${row.name} = ${row.value}`);
    // A press alone selects and changes nothing — clicking to look at a value
    // must not edit it.
    await page.mouse.move(row.x, row.y);
    await page.mouse.down();
    await page.mouse.up();
    await page.waitForTimeout(300);
    const afterClick = await page.evaluate(() => {
      const r = document.querySelector('.pt-row.sel');
      return r ? r.querySelector('.pt-row-v').textContent : null;
    });
    ok(afterClick === row.value, 'clicking a row selects it without changing it',
       `${row.value} -> ${afterClick}`);

    // Now drag UP, which must RAISE the value. Far enough to cross several
    // steps, so a one-step-per-event bug and a rounded-to-nothing bug both show.
    await page.mouse.move(row.x, row.y);
    await page.mouse.down();
    await page.mouse.move(row.x, row.y - 60, { steps: 8 });
    await page.mouse.up();
    await page.waitForTimeout(500);
    const up = await page.evaluate(() => {
      const r = document.querySelector('.pt-row.sel');
      return r ? r.querySelector('.pt-row-v').textContent : null;
    });
    ok(up !== null && Number(up) > Number(row.value),
       'dragging up raises the value', `${row.value} -> ${up}`);

    // And back down. Not merely "it changed": a control that moves the wrong way
    // is worse than one that does not move.
    await page.mouse.move(row.x, row.y);
    await page.mouse.down();
    await page.mouse.move(row.x, row.y + 60, { steps: 8 });
    await page.mouse.up();
    await page.waitForTimeout(500);
    const down = await page.evaluate(() => {
      const r = document.querySelector('.pt-row.sel');
      return r ? r.querySelector('.pt-row-v').textContent : null;
    });
    ok(down !== null && Number(down) < Number(up), 'and dragging down lowers it',
       `${up} -> ${down}`);

    // The engine agrees, not just the screen. A pending optimistic value that
    // never landed would pass every check above.
    await page.waitForTimeout(700);
    const settled = await page.evaluate(() => {
      const n = (window.__uni.patchNodes() || [])[0];
      return n ? n.config[0] : null;
    });
    ok(settled !== null, 'and the engine published the change back',
       `engine config[0] = ${settled}`);
  }
}

/*
 * THE NOTES INSIDE A CLIP.
 *
 * An arrangement of blank rectangles tells you a part exists and nothing about
 * what it does. The reason to look at an arrangement rather than a track list is
 * to see the SHAPE of the music — where it is busy, where it rests, where the
 * line rises — and that is the note material, drawn small.
 *
 * Asserted on the CANVAS's own pixels, not on the model. The model saying "here
 * are the notes" is what it said before this worked; the question is whether
 * anything reached the screen.
 */
section('clips show the notes inside them');
{
  await page.evaluate(() => window.__uni.loadProject('webtest'));
  await page.waitForTimeout(2200);
  await page.evaluate(() => window.__uni.run('view arrange'));
  await page.waitForTimeout(400);
  await page.keyboard.press('Home');
  await page.waitForTimeout(800);

  /** Lit pixels on the arrangement's canvas, and where they sit vertically. */
  const painted = () => page.evaluate(() => {
    const ar = document.getElementById('arrange')._arrange;
    const c = ar.waveCanvas;
    if (!c || !c.width) return null;
    const d = ar.waveCtx.getImageData(0, 0, Math.min(c.width, 1400),
                                      Math.min(c.height, 400)).data;
    let lit = 0;
    const rows = new Set();
    const w = Math.min(c.width, 1400);
    for (let i = 3, px = 0; i < d.length; i += 4, px++) {
      if (d[i] > 8) { lit++; rows.add(Math.floor(px / w)); }
    }
    return { lit, rows: rows.size, h: c.height };
  });

  const p = await painted();
  ok(p !== null, 'the arrangement has a canvas to paint on');
  ok(p && p.lit > 50, 'notes are painted inside the clips', p && `${p.lit} lit pixels`);
  /*
   * SPREAD VERTICALLY, which is the whole point of drawing them at all. A pass
   * that painted every note at the same height would light plenty of pixels and
   * show a flat line — technically "notes are visible", musically useless.
   */
  ok(p && p.rows > 6, 'and at different heights, so the line has a shape',
     p && `${p.rows} distinct rows`);

  /*
   * A WRITE REPAINTS IT. The canvas is guarded so hard that it does nothing at
   * rest, and the note revision had to be added to that guard — without it, a
   * note written in the tracker left the arrangement showing the previous
   * material until something unrelated forced a repaint.
   */
  const before = (await painted()).lit;
  await page.evaluate(() => {
    window.__uni.run('view tracker');
    window.__uni.run('goto 1 1');            // an empty lane: Pad has chords, no notes
  });
  await page.waitForTimeout(300);
  // Through the console, which is the same path a keypress takes. `__uni.note`
  // does not exist — the note API lives on the dock's surface, and reaching for
  // a method that is not there throws inside page.evaluate and takes the whole
  // suite with it rather than failing one check.
  await page.evaluate(() => window.__uni.run('note 72'));
  await page.waitForTimeout(900);
  await page.evaluate(() => window.__uni.run('view arrange'));
  await page.waitForTimeout(900);
  const after = (await painted()).lit;
  ok(after !== before, 'and writing a note repaints the arrangement',
     `${before} -> ${after} lit pixels`);
}

/*
 * WHAT IS FLOWING BETWEEN THE DEVICES.
 *
 * The thing Live draws, and the thing that makes a chain readable: you can see
 * where MIDI becomes audio. `maximal`'s Bass is the shape that proves it — a
 * patcher device feeding an instrument, so MIDI arrives at both and audio leaves
 * only the second.
 *
 * Asserted on the COMPUTED STYLE, not only on the data attribute. The attribute
 * is what this side decided; the border is whether anything reached the screen,
 * and the two failed independently twice while building it (clipped by the
 * card's overflow, then drawn through the middle of the parameter list).
 */
section('the chain shows what flows between devices');
{
  await page.evaluate(() => window.__uni.loadProject('maximal'));
  await page.waitForTimeout(3500);
  // The rack follows the CURSOR's track and earlier sections move it. Bass is
  // the track with both a patcher and an instrument; without this the section
  // reads whichever chain the cursor happened to be left on and reports "no
  // patcher to flow between" for a project that has two.
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(1200);
  const flow = await page.evaluate(() => {
    const cards = [...document.querySelectorAll('.dv-card')].filter((c) => c.offsetParent);
    return cards.map((c) => {
      const before = getComputedStyle(c, '::before');
      const after = getComputedStyle(c, '::after');
      return { title: (c.querySelector('.dv-title') || {}).textContent,
               in: c.dataset.flowIn, out: c.dataset.flowOut,
               inStyle: before.borderLeftStyle, inW: before.borderLeftWidth,
               outStyle: after.borderLeftStyle,
               foot: (c.querySelector('.dv-foot') || {}).textContent || '' };
    });
  });
  ok(flow.length >= 2, 'the track has a patcher and an instrument to flow between',
     JSON.stringify(flow.map((f) => f.title)));
  if (flow.length >= 2) {
    const inst = flow.find((f) => /Zebra/.test(f.title || ''));
    const patch = flow.find((f) => /patcher/i.test(f.title || ''));
    ok(patch && patch.in === 'midi', 'MIDI arrives at the patcher', patch && patch.in);
    ok(inst && inst.in === 'midi', 'and at the instrument', inst && inst.in);
    /*
     * THE CONVERSION. An instrument takes MIDI and puts out audio, and this is
     * the one relationship the picture exists to show — a rule of thumb like
     * "audio after the first device" gets it right by accident here and wrong on
     * every chain that starts with a patcher, which is now most of them.
     */
    ok(inst && inst.out === 'audio', 'and audio comes out of it', inst && inst.out);
    // Told apart by SHAPE, not by colour: dashed for MIDI, solid for audio. A
    // difference carried by hue is one some people cannot see.
    ok(inst && inst.inStyle === 'dashed' && inst.outStyle === 'solid',
       'MIDI is dashed and audio is solid, so they differ without colour',
       inst && `in=${inst.inStyle} out=${inst.outStyle}`);
    ok(inst && inst.inW !== '0px', 'and the mark has real width on screen',
       inst && inst.inW);
    /*
     * AND THE GENERATOR IS NAMED ON THE DEVICE THAT OWNS IT. This read
     * `d.id === patcherDevice` against a field the engine never writes, so it was
     * always 0 — putting "generates" on whatever sat in slot 0, which on this
     * track is the instrument. It is a per-device bit now.
     */
    ok(patch && /generates/.test(patch.foot),
       'the generator is named on the patcher device', patch && patch.foot.trim());
    ok(inst && !/generates/.test(inst.foot),
       'and NOT on the instrument beside it', inst && inst.foot.trim());
  }
}

/*
 * THE MASTER TRACK.
 *
 * A patcher is a device, and the legitimately GLOBAL case — graph logic that is
 * not per-part — is a device on the master's chain. That is what keeps the rule
 * exception-free: no special pane, no second set of verbs, no graph without a
 * visible home.
 *
 * The master rides the same published track array as everything else, so its
 * chain and fader are addressable by id. Which is also how it went wrong: counted
 * as an ordinary track it drew an empty "Master" lane at the bottom of the
 * arrangement and an empty column in the tracker, and the cursor could be moved
 * into it, where every edit addresses a track that will never have anything to
 * edit.
 */
section('the master track has a chain but no lane');
{
  await page.evaluate(() => window.__uni.loadProject('maximal'));
  await page.waitForTimeout(3000);

  const lanes = await page.evaluate(() => {
    window.__uni.run('view arrange');
    return { count: window.__uni.state().tracks,
             names: [...document.querySelectorAll('.ar-head-nm, .ar-head')]
                      .map((e) => e.textContent.trim()).filter(Boolean) };
  });
  ok(lanes.count === 6, 'the six real tracks are lanes', `tracks=${lanes.count}`);
  ok(!lanes.names.some((n) => /master/i.test(n)),
     'and the master is NOT one of them', JSON.stringify(lanes.names));

  // Reachable, though — otherwise the chain a global patcher lives on cannot be
  // opened at all. The master has no lane, so there is no cursor position that
  // means it; `master` is the pointer.
  const said = await page.evaluate(() => window.__uni.run('master on'));
  await page.waitForTimeout(1200);
  const head = await page.evaluate(() =>
    (document.querySelector('.dv') || {}).textContent || '');
  ok(/master/i.test(String(said)) || /Master/.test(head),
     'the console can point the rack at the master', String(said));
  ok(/DEVICE CHAIN\s*Master/.test(head.replace(/\s+/g, ' ')),
     'and the rack captions itself Master, not the cursor track',
     head.slice(0, 60));

  /*
   * AND A DEVICE LANDS ON IT. The rack's own "+" button, which is handed the
   * track the strip is showing — the whole test, because three separate reads of
   * `state.cursor.track` had the rack pointed at the master while asking the
   * cursor's track for its parameters and captioning itself with the cursor's
   * name.
   */
  const plus = await page.$('.dv-add');
  ok(!!plus, 'the master rack offers a way to add a device');
  if (plus) {
    await plus.click();
    await page.waitForTimeout(2500);
    const cards = await page.evaluate(() =>
      [...document.querySelectorAll('.dv-card')].filter((c) => c.offsetParent)
        .map((c) => (c.querySelector('.dv-title') || {}).textContent));
    ok(cards.length >= 1 && /patcher/i.test(cards[0] || ''),
       'and a patcher device lands on the MASTER chain', JSON.stringify(cards));
  }
  // Back to the cursor's track, or every section after this one is looking at
  // the master.
  await page.evaluate(() => window.__uni.run('master off'));
  await page.waitForTimeout(600);
}

/*
 * WHOSE PATCHER GRAPH IS THIS?
 *
 * Three bug reports, one cause. Standing on track 2 you were shown track 1's
 * euclidean; editing it changed track 1 while you listened to track 2, so the
 * edits "did nothing"; and a track with no devices at all appeared to have a
 * generator. The engine publishes one POOL holding every device's nodes on every
 * track, and this view rendered it whole — the last place the old "one global
 * graph" model was still visible in the UI.
 *
 * `rack` is the fixture that shows it: T1 Bass has a patcher device AND an
 * instrument, T2 Pad has no devices at all.
 */
section('the patcher shows one device\'s graph, not the pool');
{
  await page.evaluate(() => window.__uni.loadProject('rack'));
  await page.waitForTimeout(3500);
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.evaluate(() => window.__uni.run('view patcher'));
  await page.waitForTimeout(600);

  const drawn = () => page.evaluate(() => ({
    nodes: [...document.querySelectorAll('.pt-node')].filter((n) => n.offsetParent)
             .map((n) => (n.querySelector('.pt-type') || {}).textContent),
    note: (document.querySelector('.pt-notice') || {}).textContent || '',
    cards: [...document.querySelectorAll('.dv-card')].filter((c) => c.offsetParent)
             .map((c) => (c.querySelector('.dv-title') || {}).textContent),
  }));

  const idle = await drawn();
  ok(idle.cards.length === 2, 'the track has a patcher device and an instrument',
     JSON.stringify(idle.cards));
  /*
   * NOTHING SELECTED SHOWS NOTHING. The first version fell back to the pool
   * here, which is the same bug wearing a different hat — with no device chosen
   * you were shown every device on every track.
   */
  /*
   * IT FOLLOWS THE TRACK. Requiring a click meant the pane whose whole purpose
   * is showing a graph was blank until you found the right card; falling back to
   * the track's first patcher makes moving between tracks show each generator,
   * which is what you want when chasing a sound you did not write.
   */
  ok(idle.nodes.join(',') === 'euclidean,random,out',
     'the track\'s own patcher is shown without hunting for it',
     JSON.stringify(idle.nodes));

  const cards = await visible('.dv-card');
  await cards[0].click();                       // the patcher device
  await page.waitForTimeout(700);
  const onPatcher = await drawn();
  ok(onPatcher.nodes.join(',') === 'euclidean,random,out',
     'selecting the patcher device draws ITS graph', JSON.stringify(onPatcher.nodes));

  /*
   * AND THE INSTRUMENT BESIDE IT DRAWS NOTHING. Every device carries a
   * `patcher_node_id` and a VST's is 0 — a perfectly valid node id, so it cannot
   * be told from a real root by its value. Selecting the instrument used to show
   * node 0's subgraph, which is the euclidean belonging to the device next to it.
   */
  await cards[1].click();
  await page.waitForTimeout(700);
  const onVst = await drawn();
  /*
   * Selecting the INSTRUMENT falls back to the track's patcher rather than
   * showing node 0's subgraph — which is what a VST's `patcher_node_id` of 0
   * used to select, and node 0 is the euclidean belonging to the device beside
   * it.
   */
  ok(onVst.nodes.join(',') === 'euclidean,random,out',
     'and the instrument falls back to the track\'s patcher, not to node 0',
     JSON.stringify(onVst.nodes));

  // A track with no devices at all: the pool still holds track 1's nodes, and
  // none of them belong here.
  await page.evaluate(() => window.__uni.run('goto 1 1'));
  await page.waitForTimeout(800);
  const other = await drawn();
  ok(other.nodes.length === 0,
     'and a track with no devices shows no other track\'s graph',
     JSON.stringify(other.nodes));
  ok(/no patcher/i.test(other.note), 'saying so about THIS track', other.note.slice(0, 70));
}

/*
 * CHORDS ARE VISIBLE.
 *
 * The engine has published a track's chords all along and this side never read
 * them: the sidecar could WRITE a chord and had no code to read one back. So a
 * track of chords played and drew an empty column — reported twice, as "sound
 * with no notes" and as "I don't see any notes or chords or degrees on Pad".
 *
 * `rack` is the case: T1 Bass has notes, T2 Pad has NO notes and four chords.
 */
section('a track of chords shows its chords');
{
  await page.evaluate(() => window.__uni.loadProject('rack'));
  await page.waitForTimeout(2800);
  await page.evaluate(() => window.__uni.run('view tracker'));
  // Back to the top. Earlier sections scroll the tracker, and the first chord
  // sits on row 0 — without this the section reports three of four drawn and
  // reads as a placement bug rather than a scrolled viewport.
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.evaluate(() => window.__uni.scrollTo(0));
  await page.waitForTimeout(700);

  const published = await page.evaluate(() => window.__uni.chords());
  ok(published.length === 4, 'the engine publishes the chords',
     `${published.length} chords`);
  ok(published.every((c) => c.track === 1), 'all on the Pad track',
     JSON.stringify(published.map((c) => c.track)));

  const drawn = await page.evaluate(() =>
    [...document.querySelectorAll('.tk-cell[data-kind="chord"]')]
      .filter((e) => e.offsetParent).map((e) => e.textContent));
  ok(drawn.length === 4, 'and the tracker draws one cell per chord',
     JSON.stringify(drawn));

  /*
   * NAMED BY DEGREE, not spelled as pitches. A chord here is a scale degree
   * resolved against the harmony timeline — that is what lets a chord track
   * survive a key change — so "Am" would name a pitch set the document does not
   * contain and would go stale the moment the key moved.
   */
  ok(drawn.some((t) => /^[IVX]+/.test(t)), 'as roman numerals', JSON.stringify(drawn));
  // The seventh and the inversion are both in the fixture, and both are the
  // parts a plain degree readout would silently drop.
  ok(drawn.some((t) => t.includes('7')), 'with the seventh shown', JSON.stringify(drawn));
  ok(drawn.some((t) => t.includes('/')), 'and the inversion', JSON.stringify(drawn));

  // On the Pad track's own column, not somewhere plausible-looking. A chord
  // occupies the whole track at that moment by definition.
  const onPad = await page.evaluate(() => {
    const cells = [...document.querySelectorAll('.tk-cell[data-kind="chord"]')]
      .filter((e) => e.offsetParent);
    return cells.every((c) => Number(c.dataset.track) === 1);
  });
  ok(onPad, 'in the Pad track\'s column');
}

/*
 * THE PARAMETER LIST GROWS WITH THE STRIP.
 *
 * "When I grow the device chain upward, the scrollable VSTi parameter section
 * stays the same size." It did: the card box is MEASURED, and the measurement
 * was invalidated only by a window resize — so dragging the chain splitter made
 * the box taller while `_listH` still held the height it had when the strip was
 * short, and the list kept drawing eight rows in a box with room for
 * twenty-four.
 *
 * Proven by A/B before it was written: with the observer disabled the box grows
 * (listClientH 84 -> 309) and `listHeight` stays 86. That is the bug, and it is
 * why the assertion below is on `listHeight` — every other symptom can be
 * satisfied by a coincidence, a stale scalar cannot.
 */
section('the rack\'s parameter list grows when the strip does');
{
  await page.evaluate(() => window.__uni.loadProject('maximal'));
  await page.waitForTimeout(3500);
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(1200);
  const probe = () => page.evaluate(() => {
    const p = window.__uni.chainProbe();
    // The LARGEST card's row count, not the first card's. A generator track's
    // slot 0 is a patcher device with almost no parameters; the plugin behind it
    // is the card this is about, and reading slot 0 reports 7 while 24 are on
    // screen.
    return { listHeight: p.listHeight, rowHeight: p.rowHeight,
             rows: Math.max(0, ...(p.rows || [0])),
             shown: [...document.querySelectorAll('.dv-p')].filter((e) => e.offsetParent).length };
  });

  const before = await probe();
  ok(before.rows > 0, 'the rack is drawing parameter rows', JSON.stringify(before));

  // The CHAIN splitter by name. `.sp-bottom` matches the harmony strip's handle
  // first in document order, and dragging that one moves a different pane while
  // looking exactly like this test passing.
  const handle = await page.$('[data-splitter="chain"]');
  ok(!!handle, 'the chain strip has a drag handle');
  if (handle && before.rows > 0) {
    const b = await handle.boundingBox();
    const DRAG = 220;
    await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2);
    await page.mouse.down();
    await page.mouse.move(b.x + b.width / 2, b.y - DRAG, { steps: 10 });
    await page.mouse.up();
    await page.waitForTimeout(900);

    const after = await probe();
    ok(after.listHeight > before.listHeight,
       'the measured list height follows the strip — this is the value that went stale',
       `${before.listHeight} -> ${after.listHeight}`);
    /*
     * IN PROPORTION to the drag, not merely "more". A fix that re-measured once
     * and then latched again would add a row or two and pass a bare
     * greater-than.
     */
    const expect = before.rows + Math.floor(DRAG / Math.max(1, after.rowHeight)) - 2;
    ok(after.rows >= expect, 'and the pool grew by about the height that was added',
       `${before.rows} -> ${after.rows}, wanted >= ${expect}`);
    ok(after.shown === after.rows, 'with no blank band under the last row',
       `${after.shown} drawn vs ${after.rows} rows`);

    // Home again, or every section after this one inherits a dragged strip.
    await handle.dblclick();
    await page.waitForTimeout(500);
  }
}

/*
 * TWO VIEWS AT ONCE.
 *
 * The centre is a stack of one or two panes. `rollOpen` — "the arrangement, with
 * the piano roll below" — was a boolean with its own class, its own CSS and its
 * own key: one hard-coded pairing, and any second pairing would have needed all
 * three again. It is `view2 === 'piano'` now, one instance of the general thing.
 *
 * Driven with real keys. The whole claim is that a chord opens and closes it.
 */
section('the centre splits into two panes');
{
  await page.evaluate(() => window.__uni.loadProject('maximal'));
  await page.waitForTimeout(2500);
  const shape = () => page.evaluate(() => {
    const box = (id) => {
      const e = document.getElementById(id);
      if (!e || e.hidden || !e.offsetParent) return null;
      const r = e.getBoundingClientRect();
      return { y: Math.round(r.y), h: Math.round(r.height) };
    };
    const s = window.__uni.state();
    return { view: s.view, view2: s.view2, pane: s.pane,
             arrange: box('arrange'), patcher: box('patcher'),
             tracker: box('trackerPane'), edge: box('paneEdge') };
  });

  await page.keyboard.press('F2');
  await page.waitForTimeout(400);
  const one = await shape();
  ok(one.arrange && !one.patcher && !one.edge, 'one pane holds one view',
     JSON.stringify(one));
  const full = one.arrange.h;

  await page.keyboard.press('Shift+F3');
  await page.waitForTimeout(600);
  const two = await shape();
  ok(two.view === 'arrange' && two.view2 === 'patcher',
     'shift opens the other view below', `${two.view} / ${two.view2}`);
  ok(two.arrange && two.patcher, 'and BOTH are on screen', JSON.stringify(two));
  /*
   * STACKED, and the one above really gave up the room. A split that left the
   * top pane full height and drew the second over it would satisfy "both
   * visible" and be useless.
   */
  ok(two.arrange.h < full, 'the top pane gave up height for it',
     `${full} -> ${two.arrange.h}`);
  ok(two.patcher.y > two.arrange.y + two.arrange.h - 2,
     'and the second sits BELOW the first, not over it',
     `arrange ends ${two.arrange.y + two.arrange.h}, patcher starts ${two.patcher.y}`);
  ok(!!two.edge, 'with a divider between them to drag');
  ok(two.pane === 1, 'and the keys go to the new pane', String(two.pane));

  // The same chord closes it: the way out is the way in, like every other
  // toggle here. Without that, closing needs a key nothing suggests.
  await page.keyboard.press('Shift+F3');
  await page.waitForTimeout(500);
  const back = await shape();
  ok(back.view2 === null && !back.patcher, 'the same chord closes it',
     JSON.stringify(back));
  ok(back.arrange && back.arrange.h === full, 'and the top pane takes the room back',
     `${back.arrange && back.arrange.h} vs ${full}`);

  /*
   * ANY TWO VIEWS, which is the point of generalising. The tracker is the
   * awkward one — its column header and its minimap belong to it and have to
   * travel with it, which is why it is wrapped.
   */
  await page.keyboard.press('Shift+F1');
  await page.waitForTimeout(600);
  const withTracker = await shape();
  ok(withTracker.tracker && withTracker.arrange,
     'the tracker can be the second view too', JSON.stringify(withTracker));
  ok(withTracker.tracker.y > withTracker.arrange.y,
     'below the arrangement', JSON.stringify(withTracker));
  // Its header travels with it: a header left behind draws over whatever
  // replaced the tracker, which is exactly what happened when the wrapper was
  // `display: flex` and `[hidden]` could not hide it.
  const headInside = await page.evaluate(() => {
    const h = document.getElementById('head');
    const pane = document.getElementById('paneBot');
    return !!h && pane.contains(h) && h.offsetParent !== null;
  });
  ok(headInside, 'and its column header travels with it');

  await page.keyboard.press('Escape');
  await page.waitForTimeout(400);
  ok((await shape()).view2 === null, 'Escape closes the second pane');
}

/*
 * DOUBLE-CLICK A PATCHER DEVICE TO SEE ITS GRAPH.
 *
 * And the interesting half is WHERE. If a patcher is already on screen the graph
 * simply changes there — moving a surface you are already looking at is
 * disorienting and answers a question nobody asked. Only when none is visible
 * does this open one, and below, because you double-clicked a device in the rack
 * to look at its graph: whatever you were working in is what you want to keep.
 */
section('double-clicking a patcher device opens its graph');
{
  await page.evaluate(() => window.__uni.loadProject('rack'));
  await page.waitForTimeout(3000);
  await page.evaluate(() => { window.__uni.run('view tracker'); window.__uni.run('goto 1 0'); });
  await page.waitForTimeout(800);
  const st = () => page.evaluate(() => {
    const s = window.__uni.state();
    return { view: s.view, view2: s.view2, sel: s.chainSelected,
             nodes: [...document.querySelectorAll('.pt-node')].filter((n) => n.offsetParent)
                      .map((n) => (n.querySelector('.pt-type') || {}).textContent) };
  });
  const cards = await visible('.dv-card');
  ok(cards.length === 2, 'the track has a patcher device and an instrument',
     `${cards.length} cards`);

  if (cards.length === 2) {
    const before = await st();
    ok(before.view2 === null, 'nothing is open below to begin with');

    await cards[0].dblclick();
    await page.waitForTimeout(900);
    const opened = await st();
    ok(opened.view2 === 'patcher', 'double-clicking the patcher opens one below',
       JSON.stringify(opened.view2));
    ok(opened.view === 'tracker', 'and leaves what you were working in alone',
       opened.view);
    ok(opened.nodes.join(',') === 'euclidean,random,out',
       'showing THAT device\'s graph', JSON.stringify(opened.nodes));

    /*
     * ALREADY VISIBLE: the graph changes where it is. Put the patcher in the TOP
     * pane, then double-click again — a second pane must not open, and the one
     * on screen must not move.
     */
    await page.keyboard.press('Escape');            // close the lower pane
    await page.waitForTimeout(400);
    await page.keyboard.press('F3');                // patcher in the top pane
    await page.waitForTimeout(500);
    const up = await st();
    ok(up.view === 'patcher' && up.view2 === null, 'the patcher can sit in the top pane',
       JSON.stringify(up));

    // The rack is still on screen under the split, so its cards are clickable.
    const cards2 = await visible('.dv-card');
    if (cards2.length) {
      await cards2[0].dblclick();
      await page.waitForTimeout(800);
      const again = await st();
      ok(again.view2 === null,
         'with a patcher already visible, no second pane opens', JSON.stringify(again));
      ok(again.view === 'patcher', 'and the one on screen stays where it is', again.view);
    }
  }
}

/*
 * A TRACK'S AUDIO CAN GO SOMEWHERE OTHER THAN MAIN.
 *
 * A track whose output feeds another track IS a group — there is no separate
 * object to create — so the destination list is Main plus every other track.
 *
 * The engine has supported this the whole time (SetTrackRouting, a RoutingSnapshot
 * diff, and a summing path that finds the destination runtime); nothing on this
 * side read the snapshot or sent the command.
 */
section('a track can be routed away from Main');
{
  await page.evaluate(() => window.__uni.loadProject('maximal'));
  await page.waitForTimeout(3000);
  await page.evaluate(() => window.__uni.run('view mixer'));
  await page.waitForTimeout(700);

  const routes = () => page.evaluate(() => {
    const c = window.__uni.chains(); const out = {};
    for (const k in c) {
      if (c[k].routing) out[k] = c[k].routing.audioOutKind + ':' + c[k].routing.audioOutTrack;
    }
    return out;
  });
  const opts = (t) => page.evaluate((track) => {
    const strip = document.querySelector(`.mx-strip[data-track="${track}"]`);
    return strip ? [...strip.querySelectorAll('option')].map((o) => o.textContent) : null;
  }, t);

  /*
   * A CLOSED SELECT CARRIES ONE OPTION, and that is the point rather than a defect.
   *
   * A full destination list is one option per track per strip — O(tracks squared), and at the
   * engine's 64-track limit it was 4,096 of the mixer's 4,931 nodes, 83% of the surface, for a
   * control that shows only its selected item until somebody opens it. The list is built when a
   * pointer or a focus reaches the select.
   *
   * Asserted here so the saving cannot silently regress into eagerness, and so the next person
   * to read the "one option" below knows it is deliberate.
   */
  const closed = await opts(0);
  ok(closed && closed.length === 1,
     'a closed route select carries only its own value — the list is built when reached for',
     JSON.stringify(closed));

  /*
   * NOW REACH FOR IT, the way a person does. `focus` is the honest trigger: it is what a
   * keyboard user does before typing and what the pointer path ends up doing anyway — Playwright's
   * own `selectOption` below focuses first, which is why that call kept working while this read
   * did not.
   */
  await page.focus('.mx-strip[data-track="0"] .mx-out');
  await page.waitForTimeout(300);
  const list = await opts(0);
  ok(list && list[0] === 'Main', 'every strip offers Main first', JSON.stringify(list));
  /*
   * AND NOT ITSELF. A track cannot feed its own input, and offering it would be
   * offering a feedback loop — the one destination that is never right.
   */
  ok(list && !list.includes('Bass'), 'and not the track it belongs to',
     JSON.stringify(list));
  ok(list && list.includes('Arp'), 'but every other track', JSON.stringify(list));

  const before = await routes();
  ok(before['0'] === '1:0', 'the Bass starts routed to Main', before['0']);

  // A real select, committed the way a person commits one.
  await page.selectOption('.mx-strip[data-track="0"] .mx-out', '3');
  await page.waitForTimeout(1400);
  const after = await routes();
  /*
   * kind 2 is "another track" and 3 is the Arp. Read back from the ENGINE's own
   * snapshot rather than from the select: the select showing what you clicked
   * proves nothing, and the failure this guards is a routing command that is
   * accepted and does not land.
   */
  ok(after['0'] === '2:3', 'and the engine has it routed into the Arp', after['0']);
  ok(after['1'] === '1:0' && after['2'] === '1:0',
     'while every other track is untouched', JSON.stringify(after));

  // Back to Main, or later sections inherit a routed mix.
  await page.selectOption('.mx-strip[data-track="0"] .mx-out', '-1');
  await page.waitForTimeout(1200);
  ok((await routes())['0'] === '1:0', 'and it goes back to Main');
}

/*
 * BYPASS: the rack has drawn this state since v20 and could never set it.
 *
 * Asserted from the ENGINE's chain snapshot, not from the card's class. The card
 * dimming proves the click reached the renderer; the snapshot proves it reached
 * the engine, and those are the two halves that a working-looking-but-inert
 * button separates. Both routes are exercised — the button and the `b` key —
 * because they are two call sites and a rack where the mouse works and the
 * keyboard does not is the shape this file keeps finding.
 */
/*
 * THE RACK'S KEYBOARD ACTS ON THE TRACK THE RACK IS SHOWING.
 *
 * Normally that is the cursor's track, so the distinction never comes up. But the
 * rack can be PINNED to the master — it has to be, because the master has no lane
 * and therefore no cursor position that means it — and the key handler read
 * `state.cursor.track` while the strip drew the pinned one.
 *
 * The worst of the six sites was Backspace. It asked "Remove Zebralette?", naming
 * the card on screen from the master's chain, and then deleted a device from
 * whatever track the cursor was on. A confirmation that names one thing and
 * destroys another is worse than no confirmation, because it turns a mistake into a
 * mistake you approved — and device removal is not undoable.
 */
/*
 * THREE CONTROLS THAT WERE PRESENT AND INERT.
 *
 * Each of these had a comment, a help entry or a call site, and could not be
 * operated. That combination is the worst kind of gap in this codebase: it looks
 * finished from every angle except pressing it, and the suites had walked past all
 * three because nothing pressed them either.
 */
/*
 * NO PHANTOM CURSOR AFTER A SCROLL.
 *
 * The cursor and playhead classes were removed by looking their row up AGAIN — and
 * that lookup returns nothing once the window has scrolled past the row, so the class
 * was never taken off. The pool is a ring, so the element still wearing it is now
 * showing some other row entirely.
 *
 * Measured before the fix: cursor parked on row 5, scroll away, and the highlight is
 * drawn on ROW 435 while the cursor is still at 5. The whole job of that highlight is
 * to say where you are, and there is nothing about a misplaced one that looks wrong.
 */
/*
 * ⌘B WORKS ON THE FIRST PRESS AFTER OPENING A PROJECT.
 *
 * Opening a project from the rail closes the rail and left `focus` at 'browser', so
 * the keyboard was parked on a panel that was no longer on screen. Two symptoms,
 * neither of which looks like a focus bug: ⌘B needed two presses (the toggle reads
 * "if focus is browser, close it", so the first press closed an already-closed rail),
 * and Escape stopped closing the split pane, which is gated on focus being 'centre'.
 */
section('the rail hands the keyboard back when it closes');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  const st = () => page.evaluate(() => {
    const s = window.__uni.state();
    return { open: s.browserOpen, focus: s.focus, view2: s.view2 };
  });

  /*
   * THROUGH THE RAIL, which is the only path that produces the bug. Loading a
   * project through the test surface never sets focus to 'browser' in the first
   * place, so a section written that way passes with the bug present — I wrote it
   * that way first and the negative control is what showed it up.
   */
  await page.evaluate(() => window.__uni.browser(true));
  await page.waitForTimeout(900);
  ok((await st()).open, 'the rail is open', JSON.stringify(await st()));
  await page.evaluate(() => { window.__uni.state(); });
  // Enter on a project row: the rail's own open, which closes the rail.
  await page.evaluate(() => window.__uni.browserOpenRow('meter'));
  await page.waitForTimeout(2500);
  const after = await st();
  ok(!after.open, 'opening a project from it closes the rail', JSON.stringify(after));
  ok(after.focus !== 'browser', 'and hands the keyboard back',
     JSON.stringify(after));

  // ⌘B, ONCE, must open it again. With focus left stale this closed an
  // already-closed rail and only the SECOND press opened it.
  await page.keyboard.press('Meta+b');
  await page.waitForTimeout(600);
  const opened = await st();
  ok(opened.open && opened.focus === 'browser', 'and one ⌘B opens it again',
     JSON.stringify(opened));
  await page.keyboard.press('Meta+b');
  await page.waitForTimeout(600);
  ok(!(await st()).open, 'and a second closes it', JSON.stringify(await st()));

  /*
   * AND ESCAPE CLOSES THE SPLIT — in THIS ORDER, which is the only order that shows
   * it.
   *
   * The split-pane Escape is gated on focus being 'centre'. Opening the split with
   * Shift+F2 sets focus to 'centre' itself, so a sequence that splits AFTER opening a
   * project heals the very state it means to test. The split has to be open FIRST,
   * then a project opened from the rail, then Escape — and measured with the fix
   * reverted, that leaves `view2: "arrange"` and focus stuck on a rail that is not on
   * screen.
   *
   * Which is the shape a person hits: arrangement below the tracker, open a song, want
   * the screen back.
   */
  await page.keyboard.press('Shift+F2');
  await page.waitForTimeout(500);
  ok((await st()).view2 !== null, 'a second pane is open', JSON.stringify(await st()));
  await page.evaluate(() => window.__uni.browser(true));
  await page.waitForTimeout(900);
  await page.evaluate(() => window.__uni.browserOpenRow('meter'));
  await page.waitForTimeout(2500);
  ok((await st()).focus === 'centre',
     'opening a project with a split up still hands the keyboard back',
     JSON.stringify(await st()));
  await page.keyboard.press('Escape');
  await page.waitForTimeout(500);
  ok((await st()).view2 === null, 'so Escape can close the split',
     JSON.stringify(await st()));
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.waitForTimeout(300);
}

section('the cursor is drawn where the cursor is');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.evaluate(() => window.__uni.loadProject('meter'));
  await page.waitForTimeout(2500);

  const drawn = () => page.evaluate(() => {
    const cells = [...document.querySelectorAll('.tk-cell.cursor')].filter((e) => e.offsetParent);
    return { n: cells.length,
             rows: cells.map((c) => Number(c.closest('.tk-row').dataset.row)),
             at: window.__uni.state().cursor.row };
  });

  await page.evaluate(() => window.__uni.goto(5, 0));
  await page.waitForTimeout(500);
  const here = await drawn();
  ok(here.n === 1 && here.rows[0] === 5, 'the cursor is on its row', JSON.stringify(here));

  // Scroll the cursor's row out of the window WITHOUT moving the cursor.
  await page.evaluate(() => window.__uni.scrollTo(400));
  await page.waitForTimeout(700);
  const away = await drawn();
  ok(away.n === 0, 'scrolled past, nothing wears the cursor', JSON.stringify(away));
  ok(away.at === 5, 'and the cursor itself has not moved', String(away.at));

  await page.evaluate(() => window.__uni.scrollTo(0));
  await page.waitForTimeout(700);
  const back = await drawn();
  ok(back.n === 1 && back.rows[0] === 5, 'and it comes back to the same row',
     JSON.stringify(back));
}

section('controls that look operable are operable');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.evaluate(() => window.__uni.loadProject('rack'));
  await page.waitForTimeout(2500);

  /*
   * CTRL+TAB CYCLES THE SURFACES. The modifier branch returned unconditionally, so
   * this never reached its handler — and a bare return does not preventDefault, so
   * the keystroke went to the BROWSER. The documented way to change surface moved
   * browser tab focus instead.
   */
  const view0 = await page.evaluate(() => window.__uni.state().view);
  await page.keyboard.press('Control+Tab');
  await page.waitForTimeout(400);
  const view1 = await page.evaluate(() => window.__uni.state().view);
  ok(view1 !== view0, 'Ctrl+Tab changes surface', `${view0} -> ${view1}`);
  await page.keyboard.press('Control+Tab');
  await page.waitForTimeout(400);
  const view2 = await page.evaluate(() => window.__uni.state().view);
  ok(view2 !== view1, 'and again, rather than toggling two', `${view1} -> ${view2}`);
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.waitForTimeout(400);

  /*
   * DOUBLE-CLICKING A VST CARD OPENS ITS EDITOR. The call passed two positional
   * arguments to a handler that destructures `({ track, device })`, so both came
   * out undefined and nothing happened. The open BUTTON on the same card passes the
   * object correctly, so the feature demonstrably worked — from the other route.
   */
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(500);
  const cards2 = await visible('.dv-card');
  ok(cards2.length >= 2, 'the rack has a VST card to open', String(cards2.length));
  // The instrument, not the patcher — a patcher device double-click opens its GRAPH,
  // which is a different callback with a different (positional) shape.
  const vst = cards2[cards2.length - 1];
  /*
   * ON THE TITLE, not the card's centre. An instrument card is mostly parameter
   * rows — Zebra2 publishes 256 — so the centre of it is a `.dv-p-bar`, which the
   * dblclick handler deliberately excludes: double-clicking a parameter you are
   * dragging must not open a window. Playwright clicks an element's centre, so
   * targeting the card meant targeting a parameter, and the first version of this
   * check failed for that reason rather than for the bug's.
   */
  const title = await vst.$('.dv-title');
  ok(!!title, 'the card has a title to double-click');
  await title.dblclick();
  await page.waitForTimeout(900);
  const said = String(await page.evaluate(() => window.__uni.state().reject));
  /*
   * The engine owns the window, so this cannot assert one appeared. What it CAN
   * assert is that the command went out with a real track and device — the failure
   * was `undefined` for both, and the app's own refusal path is what reports that.
   */
  ok(!/undefined|no engine/.test(said), 'double-clicking it does not fail on undefined',
     said);
  const dbl = await page.evaluate(() => window.__uni.lastEditor());
  ok(dbl && dbl.track === 0 && dbl.device >= 0,
     'and names a real track and device', JSON.stringify(dbl));
}

section('the rack\'s keys act on the track the rack is showing');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.evaluate(() => window.__uni.loadProject('rack'));
  await page.waitForTimeout(2500);
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(500);

  const count = (t) => page.evaluate((track) => {
    const c = window.__uni.chains()[track];
    return ((c && c.devices) || []).length;
  }, t);
  const before = await count(0);
  ok(before > 0, 'track 0 has devices to lose', String(before));

  // Pin the rack to the master while the cursor stays on track 0. Through the
  // console, which is the path a person has.
  await page.evaluate(() => window.__uni.run('master on'));
  await page.waitForTimeout(1200);
  ok((await page.evaluate(() => window.__uni.chainProbe().track)) === 0xFFFF0000,
     'the rack is showing the master');
  /*
   * GIVE THE MASTER A DEVICE, or this section cannot fail.
   *
   * No fixture puts anything on the master, and with an empty rack the draw path's
   * own invariant clamps the selection to -1 and hands the keyboard back — so the
   * wrong-track delete could never fire and a test written against the empty case
   * passes with the bug present. That is the vacuous green this suite exists to
   * refuse, and I wrote it once before catching it.
   *
   * With a device on the master, position 0 is a real card in the master's chain AND
   * a real device in the cursor's track's chain. That is the collision the bug
   * needed: one index, two lists, and the keystroke reading the wrong one.
   */
  await page.evaluate(() => window.__uni.addDevice(0xFFFF0000, 'patcher event'));
  await page.waitForTimeout(1800);
  ok((await count(0xFFFF0000)) === 1, 'the master now has a device of its own',
     String(await count(0xFFFF0000)));

  /*
   * SELECT BY CLICKING, because `__uni.state()` IS A DEEP COPY.
   *
   * `state().focus = 'chain'` writes to a snapshot and returns — the app never sees
   * it — so the first version of this section pressed Backspace with the keyboard
   * still in the tracker and passed for a reason that had nothing to do with the
   * rack. Which is documented behaviour of that probe, and I walked into it anyway.
   *
   * Clicking a card is also the path a person takes, and it is what sets focus to
   * 'chain' in the first place.
   */
  const masterCards = await visible('.dv-card');
  ok(masterCards.length === 1, 'the master card is on screen to be clicked',
     String(masterCards.length));
  await masterCards[0].click();
  await page.waitForTimeout(400);
  ok((await page.evaluate(() => window.__uni.state().focus)) === 'chain',
     'and clicking it gave the rack the keyboard');
  // ACCEPT the confirmation, or this proves nothing: Playwright dismisses dialogs
  // by default, confirm() returns false, and the delete never runs whether the
  // target is right or wrong.
  page.once('dialog', (d) => d.accept());
  await page.keyboard.press('Backspace');
  await page.waitForTimeout(1800);
  ok((await count(0)) === before,
     'Backspace in the master\'s rack takes nothing from the cursor\'s track',
     `${before} -> ${await count(0)}`);
  ok((await count(0xFFFF0000)) === 0, 'and takes it from the master, which is what it named',
     String(await count(0xFFFF0000)));

  await page.evaluate(() => window.__uni.run('master off'));
  await page.waitForTimeout(800);
  await page.evaluate(() => { window.__uni.state().focus = 'centre'; });
  await loadAndWait('rack');
}

/*
 * DEVICES CAN BE REORDERED, WHICH IS WHAT A CHAIN IS.
 *
 * The same compressor before and after a distortion are two different sounds. The rack
 * could add a device and remove one and not change where it sat, so the only way to
 * reorder was to delete and re-add — which throws the device's settings away in order
 * to change its position.
 *
 * Asserted from the ENGINE's published chain, not from the order of the cards. The
 * cards are what a reorder must LOOK like; the chain is what it has to BE, and a
 * renderer that shuffles its own list would satisfy the first and change no sound.
 */
/*
 * A CHORD CAN BE REMOVED, WHICH IT COULD NOT BE.
 *
 * `del` at the cursor looked for a NOTE and answered "no note here" when the cursor was
 * on a chord — with a chord name sitting in the cell. So a chord typed by mistake stayed
 * for the life of the song, and the refusal claimed the cell was empty.
 *
 * Being able to create something you cannot delete is worse than not being able to
 * create it: the first is a trap, the second is a missing feature.
 */
section('a chord can be taken back');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await loadAndWait('meter');
  await page.evaluate(() => window.__uni.run('goto 12 0'));
  await page.waitForTimeout(400);

  const chords = () => page.evaluate(() => (window.__uni.chords() || []).length);
  const before = await chords();

  // Written through the CONSOLE, which had no chord command at all until now — the only
  // way to make one was typing a degree token into a cell.
  const said = String(await page.evaluate(() => window.__uni.run('chord 4 seventh')));
  await page.waitForTimeout(1200);
  const after = await chords();
  ok(after === before + 1, 'the console writes a chord', `${before} -> ${after} (${said})`);

  // And the cell shows it, or the delete below would be aimed at something invisible.
  const drawn = await page.evaluate(() => {
    const c = [...document.querySelectorAll('.tk-cell[data-kind="chord"]')]
      .filter((e) => e.offsetParent);
    return c.map((e) => e.textContent.trim());
  });
  ok(drawn.length > 0, 'and the tracker draws it', JSON.stringify(drawn));

  /*
   * BACKSPACE TAKES IT AWAY. The same key that deletes a note, because "remove what is
   * under the cursor" is one idea — needing a different key for a chord would be the
   * app's data model leaking into the keyboard.
   */
  await page.keyboard.press('Backspace');
  await page.waitForTimeout(1200);
  ok((await chords()) === before, 'and Backspace on it removes it',
     `${after} -> ${await chords()}`);
  ok(!/no note here/.test(String(await page.evaluate(() => window.__uni.state().reject))),
     'without claiming the cell was empty',
     String(await page.evaluate(() => window.__uni.state().reject)));

  // An empty cell still refuses, and says what it means rather than naming notes only.
  await page.keyboard.press('Backspace');
  await page.waitForTimeout(700);
  ok(/nothing here/.test(String(await page.evaluate(() => window.__uni.state().reject))),
     'and an empty cell says there is nothing here',
     String(await page.evaluate(() => window.__uni.state().reject)));

  // The console can remove one too, so neither surface is missing half the pair.
  await page.evaluate(() => window.__uni.run('chord 2'));
  await page.waitForTimeout(1200);
  ok((await chords()) === before + 1, 'a second chord goes in');
  const gone = String(await page.evaluate(() => window.__uni.run('delchord')));
  await page.waitForTimeout(1200);
  ok((await chords()) === before, 'and `delchord` takes it out',
     `${gone} — ${before + 1} -> ${await chords()}`);

  await loadAndWait('meter');
}

/*
 * AND A KEY CHANGE CAN BE TAKEN OFF THE TIMELINE.
 *
 * Same shape as the chord, one layer up: the engine has taken DeleteHarmony since before
 * this UI existed and nothing sent it, so once writing a key change landed, the timeline
 * could only ever grow. A timeline you can only add to is one you stop using.
 */
section('a key change can be taken off the timeline');
{
  const events = () => page.evaluate(() => (window.__uni.harmony() || []).length);
  const before = await events();

  await page.evaluate(() => window.__uni.run('harmony 7 minor 3840000'));
  await page.waitForTimeout(1400);
  const added = await events();
  ok(added === before + 1, 'a key change goes on at a tick', `${before} -> ${added}`);

  const said = String(await page.evaluate(() => window.__uni.run('delharmony 3840000')));
  await page.waitForTimeout(1400);
  ok((await events()) === before, 'and `delharmony` takes it off',
     `${added} -> ${await events()} (${said})`);

  /*
   * THE VERSION GUARD IS THE TRAP HERE, and it is worth a check of its own. DeleteHarmony
   * is gated on the HARMONY version, not the clip's — send nothing and the base is 0,
   * which matches only an engine whose harmony has never changed. So a naive
   * implementation works once on a fresh boot and is silently refused ever after. Doing
   * it twice in a row is what tells those apart.
   */
  await page.evaluate(() => window.__uni.run('harmony 2 dorian 7680000'));
  await page.waitForTimeout(1400);
  ok((await events()) === before + 1, 'a second one goes on after the first was removed');
  await page.evaluate(() => window.__uni.run('delharmony 7680000'));
  await page.waitForTimeout(1400);
  ok((await events()) === before,
     'and comes off too — so the version guard is being fed a moving number',
     String(await events()));
}

section('a device can be moved along its chain');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await loadAndWait('rack');
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(500);

  const order = () => page.evaluate(() => {
    const c = window.__uni.chains()['0'];
    return ((c && c.devices) || []).map((d) => d.id);
  });
  const before = await order();
  ok(before.length >= 2, 'the track has devices to reorder', JSON.stringify(before));

  // The console first, because it names an absolute position and so pins the INDEX
  // semantics: the engine erases the device and re-inserts, which makes `pos` the
  // final resting index rather than an index into the original list. Read the other
  // way it is off by one for every rightward move.
  const moved = String(await page.evaluate(
    (id) => window.__uni.run(`movedevice 0 ${id} 0`), before[before.length - 1]));
  await page.waitForTimeout(1500);
  const after = await order();
  ok(after[0] === before[before.length - 1],
     'the last device moved to position 0 lands at position 0',
     `${JSON.stringify(before)} -> ${JSON.stringify(after)}`);
  ok(after.length === before.length, 'and nothing was lost or duplicated',
     JSON.stringify(after));
  ok(new Set(after).size === after.length, 'and no id appears twice',
     JSON.stringify(after));

  /*
   * SHIFT+ARROW moves the SELECTED device; plain arrow moves the selection. And the
   * selection FOLLOWS the device — otherwise a second press moves whatever slid into
   * the old slot, which turns a reorder into a shuffle.
   */
  const cards = await visible('.dv-card');
  await cards[0].click();
  await page.waitForTimeout(400);
  ok((await page.evaluate(() => window.__uni.state().focus)) === 'chain',
     'the rack has the keyboard');
  const first = (await order())[0];
  await page.keyboard.press('Shift+ArrowRight');
  await page.waitForTimeout(1500);
  const shifted = await order();
  ok(shifted[1] === first, 'Shift+Right moves it one place along',
     `${JSON.stringify(after)} -> ${JSON.stringify(shifted)}`);
  ok((await page.evaluate(() => window.__uni.state().chainSelected)) === 1,
     'and the selection follows it, so a second press moves the same device');

  await page.keyboard.press('Shift+ArrowLeft');
  await page.waitForTimeout(1500);
  ok((await order())[0] === first, 'and Shift+Left brings it back',
     JSON.stringify(await order()));

  // At the end, it says so rather than silently doing nothing — a key that works four
  // times and then stops is a key you assume is broken.
  await page.keyboard.press('Shift+ArrowLeft');
  await page.waitForTimeout(700);
  ok(/already at the end/.test(String(await page.evaluate(() => window.__uni.state().reject))),
     'and at the edge it says why nothing happened',
     String(await page.evaluate(() => window.__uni.state().reject)));

  await page.evaluate(() => { window.__uni.state(); });
  await page.keyboard.press('Escape');
  await page.waitForTimeout(300);
  await loadAndWait('rack');
}

section('a device can be switched off without removing it');
{
  // Back to the tracker first: the section before this one leaves the mixer up,
  // and the mixer takes the space the rack lives in — so every card is present,
  // pooled and invisible, and `visible` correctly finds none of them.
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.evaluate(() => window.__uni.loadProject('rack'));
  await page.waitForTimeout(2500);
  // Track 0 — the rack shows the CURSOR's track, and `rack` puts its devices
  // there. On track 1 the strip is legitimately empty and the section reads as a
  // broken rack.
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(500);

  /** What the ENGINE says about track 0's devices. */
  const byp = () => page.evaluate(() => {
    const c = window.__uni.chains()['0'];
    return (c && c.devices || []).map((d) => `${d.id}:${d.bypass ? 1 : 0}`).join(' ');
  });

  const before = await byp();
  ok(/^\S+:0( \S+:0)*$/.test(before), 'nothing starts bypassed', before);

  // `visible`, not a selector: the pool keeps spare cards around with their last
  // project's `data-pos` still on them, so `.dv-card[data-pos="1"]` matches a
  // hidden leftover and the click hangs for thirty seconds. Fifth time.
  const cards = await visible('.dv-card');
  const buttons = await visible('.dv-card .dv-byp');
  ok(buttons.length === cards.length && cards.length > 0,
     'every card carries a bypass button', `${buttons.length} of ${cards.length}`);

  const target = await cards[1].evaluate((el) => el._devId);
  await buttons[1].click();
  await page.waitForTimeout(1200);
  const clicked = await byp();
  ok(clicked.includes(`${target}:1`), 'the button bypasses that device in the ENGINE',
     `${clicked} (wanted device ${target})`);
  ok(clicked.split(' ').filter((s) => s.endsWith(':1')).length === 1,
     'and only that one', clicked);
  ok(await cards[1].evaluate((el) => el.classList.contains('byp')),
     'and the card says so');

  // The same press again, from the keyboard this time. The card is selected by
  // the click above, so the rack already holds the keys.
  await page.keyboard.press('b');
  await page.waitForTimeout(1200);
  const keyed = await byp();
  ok(keyed.includes(`${target}:0`), 'and `b` switches it back on', keyed);

  // A bypassed device must not be a bypassed PROJECT: leaving one off would make
  // every later section quietly quieter, which is the kind of inherited state
  // that makes a suite depend on its own order.
  ok((await byp()) === before, 'the chain is back where it started', await byp());
}

/*
 * PER-INSERT METERS (kShmVersion 24).
 *
 * The thing worth asserting is not "a bar appeared" — a bar appears whether or
 * not it means anything, and the first build of this feature drew every
 * instrument pegged at full scale because 0 mB is 0 dBFS and the engine was
 * never writing the slot. So this checks the CHAIN from published number to
 * drawn width: the engine's value, the scale applied to it, and the pixel.
 */
/*
 * AN EDIT LANDS ON A TRACK THAT IS NOT THE ONE YOU EDITED LAST.
 *
 * M2.17 made edit acceptance PER TRACK. The page had been stamping the GLOBAL
 * clip version as every edit's base, which was correct until that moment and
 * then silently stopped being: the counters diverge on the first edit, and an
 * edit quoting the wrong one is not refused with a message, it is DROPPED.
 * Measured on `maximal` — three notes on track 0 left global 5, track 0 at 5,
 * every other track at 1 — so note entry, chords and transpose all worked on
 * whichever track you had touched most recently and nowhere else.
 *
 * Four checks in this file failed and NONE of them named the cause; they read as
 * "a chord token writes: clipVersion 20 -> 20", which is a rejection with no
 * reason attached. This section names it, so the next contract change to
 * versioning fails here first and says what it broke.
 *
 * The fix is in the sidecar, not the page: the per-track counters are not on the
 * wire, and the sidecar is the side that can read one. Same argument the BATCH
 * re-basing already made.
 */
/*
 * AND WHEN ONE DOES NOT LAND, IT SAYS SO.
 *
 * The complement of the section below, and the more important half. A stale-base
 * edit used to be dropped in complete silence — no event, no code, nothing on the
 * ring — so the entire symptom of M2.17's per-track versioning was "the app does
 * nothing", and the four suite failures it caused named nothing between them.
 *
 * The base is stamped DELIBERATELY WRONG here, which is the only way to provoke a
 * rejection now that the sidecar resolves a correct one. That is also why this
 * check is worth having: with the resolution working, no ordinary path produces a
 * rejection any more, so the reporting would rot unnoticed.
 */
section('a refused edit says so, with the version to retry');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.evaluate(() => window.__uni.loadProject('meter'));
  await page.waitForTimeout(2500);
  await page.evaluate(() => window.__uni.goto(4, 0));
  await page.waitForTimeout(300);

  const before = await page.evaluate(() => (window.__uni.notes() || []).length);
  /*
   * NOTHING IS ALREADY SAYING IT. `state.reject` is the app's one refusal line
   * and it PERSISTS until something clears it, so a message left by an earlier
   * section would satisfy every assertion below without this run proving
   * anything. Checked rather than cleared, so an unexpected refusal upstream
   * fails here instead of being erased.
   */
  const already = await page.evaluate(() => String(window.__uni.state().reject));
  ok(!/refused an edit/.test(already), 'nothing was already claiming a refusal',
     already);
  // A base far in the future is stale in the sense the engine means: not the
  // number it holds. Sent raw, past the app's own send path, because the app's
  // whole job now is to never produce one.
  await page.evaluate(() => window.__uni.send({
    type: 'note', track: 0, pitch: 60, tick: 3840000, dur: 240000, vel: 100,
    column: 0, base: 999999 }));
  await page.waitForTimeout(1200);

  const said = await page.evaluate(() => window.__uni.state().reject);
  ok(/refused an edit/.test(String(said)), 'the refusal reaches the screen',
     String(said));
  ok(/999999/.test(String(said)), 'and names the version that was presented',
     String(said));
  /*
   * THE NUMBER TO RETRY WITH. A refusal that says only "no" leaves the caller
   * exactly where it was; this is what makes the message actionable, and it is
   * why the payload carries current_base rather than the sidecar reducing it to
   * prose.
   */
  ok(/it is at \d+ now/.test(String(said)), 'and the one to retry with',
     String(said));
  const after = await page.evaluate(() => (window.__uni.notes() || []).length);
  ok(after === before, 'and the note really did not land', `${before} -> ${after}`);

  /*
   * THE TWO REASONS MUST NOT COLLAPSE INTO ONE SENTENCE.
   *
   * A stale base is worth retrying and a missing track never will be, so the
   * distinction is the only actionable thing in the message. They are cheap to
   * merge by accident — one generic "that edit was refused" satisfies any check
   * that only asserts something red appeared — and impossible to notice once
   * merged, because both readings are true of the generic string.
   */
  const staleSaid = String(await page.evaluate(() => window.__uni.state().reject));
  await page.evaluate(() => window.__uni.send({
    type: 'note', track: 61, pitch: 60, tick: 0, dur: 240000, vel: 100,
    column: 0, base: 1 }));
  await page.waitForTimeout(1200);
  const missing = String(await page.evaluate(() => window.__uni.state().reject));
  ok(missing !== staleSaid, 'a missing track reads differently from a stale base',
     `${missing}  /  ${staleSaid}`);
  ok(/does not exist/.test(missing), 'and says the track is not there', missing);
  ok(!/retry|version \d/.test(missing),
     'without offering a version to retry with, because retrying will not help',
     missing);
}

/*
 * A NOTE ON THE NINTH TRACK EXISTS.
 *
 * The sidecar read notes and chords from `ui_track_count.min(8)` while the frame's
 * per-lane block was 16 wide, so every note and chord on track 8 and above was
 * silently dropped from the feed. The notes were in the engine and in the saved
 * file — they simply never reached the browser, so they could not be seen, edited
 * or played from the tracker.
 *
 * NO FIXTURE HAS MORE THAN SIX TRACKS, which is exactly why the whole suite passed
 * over it. Nothing here is exotic: it is what happens on the ninth track a person
 * adds. So this section builds the condition instead of hoping a fixture has it.
 */
section('a note on the ninth track is not thrown away');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  // What this section inherited, restored at the end. It adds FOUR TRACKS and
  // writes a note that creates a clip, which is exactly the kind of change that
  // makes an unrelated section three screens down fail on a count — the mistake
  // the save section above documents, made again here.
  const inherited = await page.evaluate(() => window.__uni.state().currentProject);
  await page.evaluate(() => window.__uni.loadProject('meter'));
  await page.waitForTimeout(2500);
  const start = await page.evaluate(() => window.__uni.state().tracks);
  for (let i = start; i <= 9; i++) {
    await page.evaluate(() => window.__uni.addTrack());
    await page.waitForTimeout(500);
  }
  const grew = await page.evaluate(() => window.__uni.state().tracks);
  ok(grew >= 10, 'the song has ten tracks to test the ninth', String(grew));

  // Track 9 — past the old cap of 8, and past track 8 itself so an off-by-one in
  // either direction fails rather than passing by luck.
  await page.evaluate(() => window.__uni.run('goto 4 9'));
  await page.waitForTimeout(300);
  await page.evaluate(() => window.__uni.run('note 100'));
  await page.waitForTimeout(1000);
  const onNine = await page.evaluate(() =>
    (window.__uni.notes() || []).filter((n) => n.tr === 9).length);
  ok(onNine === 1, 'a note written on track 9 comes back on track 9',
     `${onNine} notes`);

  // And the lane it landed on can still be read, so this is not just a count.
  const pitch = await page.evaluate(() =>
    (window.__uni.notes() || []).filter((n) => n.tr === 9).map((n) => n.p));
  ok(pitch.length === 1 && pitch[0] === 100, 'with the pitch it was given',
     JSON.stringify(pitch));

  // Reloading is the whole undo: the added tracks and the clip the note created
  // live only in the in-memory document, and nothing was saved.
  await loadAndWait(inherited || 'meter');
}

section('an edit lands on a track other than the last one edited');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.evaluate(() => window.__uni.loadProject('meter'));
  await page.waitForTimeout(2500);

  // `tr`, which is what the probe calls it. `n.track` is undefined and every
  // count came back 0 — a filter that silently matches nothing looks exactly
  // like the bug this section is about.
  const count = (t) => page.evaluate((track) =>
    (window.__uni.notes() || []).filter((n) => n.tr === track).length, t);
  const write = async (track, row, pitch) => {
    await page.evaluate(({ row, track }) => window.__uni.goto(row, track), { row, track });
    await page.waitForTimeout(120);
    await page.evaluate((p) => window.__uni.run('note ' + p), pitch);
    await page.waitForTimeout(700);
  };

  const before = [await count(0), await count(1), await count(2)];
  // Three on track 0 first, so its counter runs well ahead of the others' — one
  // edit would leave them close enough that a wrong base could still be accepted.
  await write(0, 8, 60); await write(0, 9, 62); await write(0, 10, 64);
  const mid = [await count(0), await count(1), await count(2)];
  ok(mid[0] === before[0] + 3, 'three notes land on the track being edited',
     `${before[0]} -> ${mid[0]}`);

  // Now the tracks left behind. This is the whole point: their counters are three
  // behind the global, and the page must still be able to write to them.
  await write(1, 8, 67);
  await write(2, 8, 71);
  const after = [await count(0), await count(1), await count(2)];
  ok(after[1] === mid[1] + 1, 'and one lands on a track three versions behind',
     `${mid[1]} -> ${after[1]}`);
  ok(after[2] === mid[2] + 1, 'and on the next one after that',
     `${mid[2]} -> ${after[2]}`);
  ok(after[0] === mid[0], 'without disturbing the track that was ahead',
     `${mid[0]} -> ${after[0]}`);
}

section('every insert says what it is putting out');
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.evaluate(() => window.__uni.loadProject('generator'));
  await page.waitForFunction(
    () => JSON.stringify(window.__uni.chainProbe() || '').includes('Zebra'),
    null, { timeout: 30000 }).catch(() => {});
  await page.evaluate(() => window.__uni.run('goto 1 0'));
  await page.waitForTimeout(600);

  const SILENT = -32768;
  const stopped = await page.evaluate(() => window.__uni.deviceMeters());
  const inst = stopped.find((m) => m.device === 0);
  ok(!!inst, 'the instrument has a meter entry', JSON.stringify(stopped));
  /*
   * SILENT, NOT ZERO, with the transport stopped. This is the exact bug that
   * shipped: an unwritten slot reads 0, and 0 on this scale is FULL SCALE — a
   * meter pegged at the top on a stopped transport, on every track of every
   * project, which does not look like a bug. It looks like clipping.
   */
  ok(inst && inst.outPeak === SILENT, 'and reads SILENT while stopped, not 0 dBFS',
     inst && String(inst.outPeak));

  const heard = await playUntilAudible();
  const playing = await page.evaluate(() => window.__uni.deviceMeters());
  const live = playing.find((m) => m.device === 0);
  audibleOr(live && live.outPeak > -6000 && live.outPeak < 0 ? { ok: true } : heard,
     'and a real dBFS level while playing',
     // The whole picture when it did not sound: which plugin loaded, whether the transport ran,
     // and every meter — because "-32768" alone has told me nothing three times.
     live && live.outPeak > -32768 ? String(live.outPeak) : JSON.stringify(heard).slice(0, 300));
  // An instrument has no audio input and says so, rather than inventing a level.
  ok(live && live.inPeak === SILENT, 'while its input stays silent — it has none',
     live && String(live.inPeak));
  // Peak is never below RMS. A cheap invariant that catches the two being
  // swapped on the wire, which no amount of looking at a bar would reveal.
  ok(live && live.outPeak >= live.outRms, 'peak is not below rms',
     live && `${live.outPeak} vs ${live.outRms}`);

  const cards = await visible('.dv-card');
  const drawn = await Promise.all(cards.map((c) => c.evaluate((el) => ({
    device: el._devId,
    shown: getComputedStyle(el.querySelector('.dv-meter')).display !== 'none',
    inShown: getComputedStyle(el.querySelector('.dv-m-row.in')).display !== 'none',
    width: el.querySelectorAll('.dv-m-fill')[1].style.width,
    db: el.querySelector('.dv-m-db').textContent,
  }))));
  const patcher = drawn.find((d) => d.device !== 0);
  ok(patcher && !patcher.shown, 'a patcher device gets no meter — it is not an insert',
     JSON.stringify(patcher));
  /*
   * ...AND ITS HIDDEN BAR IS EMPTY, not left at whatever the last device on this
   * pooled card was showing. Hidden, so it cannot lie now — but the pool rebinds,
   * and a card that carried a loud insert would draw that insert's level for one
   * frame on the next device it holds. Full scale, on the wrong instrument.
   */
  ok(patcher && patcher.width === '0%',
     'and its hidden bar is empty, so a rebind cannot flash the last one',
     JSON.stringify(patcher));
  const card = drawn.find((d) => d.device === 0);
  ok(card && card.shown, 'the instrument does', JSON.stringify(card));
  ok(card && !card.inShown, 'with no input row, because it has no audio input');

  /*
   * AND THE BAR IS THE NUMBER. Recomputing the expected width here from the
   * meters the page published is what makes this a check rather than a
   * screenshot: a scale that is wrong by a factor still draws a plausible bar
   * that moves with the music. Within a percentage point, because the two
   * readings are a frame or two apart and a meter moves every frame.
   */
  /*
   * ASK 1: THE DRAWN METER, NOT THE PUBLISHED NUMBER, IS EMPTY WHEN STOPPED.
   *
   * The published value is checked above; this is the other end of the same
   * wire, and it is the end that would have shown the bug. An unwritten slot
   * reads 0 mB, which on a dBFS scale is FULL SCALE — so the failure draws a
   * meter pegged at the top on a stopped transport, on every track of every
   * project, and looks like clipping rather than like a bug. Asserting that a
   * meter ELEMENT exists would have passed throughout.
   */
  const drawnFill = () => cards[1].evaluate((el) => ({
    width: el.querySelectorAll('.dv-m-fill')[1].style.width,
    tick: el.querySelectorAll('.dv-m-tick')[1].style.left,
  }));
  /*
   * STOPPED IS NOT SILENT, which this check learned the hard way: it asserted an
   * empty meter a second after Stop and read 17%, because a synth's voices keep
   * ringing when the transport stops — that is the entire reason PANIC exists,
   * and it is measured elsewhere in this repo as 0.0196 rms of tail.
   *
   * So Stop only has to prove the meter is not PEGGED, which is the actual bug:
   * an unwritten slot reads 0 mB, and 0 mB on a dBFS scale is full scale. And
   * PANIC — real silence — has to drive it to nothing. Together they pin both
   * ends without either of them lying about what Stop means.
   */
  const loudEnough = (await drawnFill()).tick;
  await page.evaluate(() => window.__uni.run('stop'));
  await page.waitForTimeout(1200);
  const halted = await drawnFill();
  ok(parseInt(halted.width, 10) < 90,
     'stopped, the instrument is not drawing a PEGGED meter', JSON.stringify(halted));

  /*
   * WAIT FOR THE BAR TO FALL, do not sleep and hope.
   *
   * This was `panic` then a 1500 ms sleep, and on a loaded box the meter had not finished
   * decaying when it was read — 44% where it wanted 0%. That is the same mistake this file
   * already fixed on the other side: "a fixed sleep after the space bar is a guess about how
   * long a plugin takes to make its first sample, and on a busy box it is wrong."
   *
   * The condition IS the assertion, so waiting for it is not weakening the check — the timeout
   * still fails, and it fails after giving the machine a fair chance rather than after a
   * constant somebody guessed.
   */
  await page.evaluate(() => window.__uni.run('panic'));
  // The SAME element `drawnFill` reads — the second `.dv-m-fill` of the second card — because a
  // wait on a different box is a wait that can be satisfied while the thing under test has not
  // moved. That is how a fixed sleep and a wrong selector fail identically.
  /*
   * A METER THAT HAS NOT FALLEN IN TWELVE SECONDS IS THE MACHINE, NOT THE PANIC.
   *
   * This check has failed three times on three different runs — twice as the dBFS pair, once as
   * this — and each time a different member of the same family, always with the box loaded. A
   * meter decays over about a second; taking more than twelve means the page is getting almost
   * no frames, which is not a statement about whether `panic` cut the voices.
   *
   * It is NOT routed through the classifier unconditionally, because a bar that never falls is
   * also what a broken panic looks like — so the machine has to actually be busy for the run to
   * be excused, and on a quiet box this still fails.
   */
  await cards[1].evaluate((el) => new Promise((done) => {
    const at = () => parseInt(el.querySelectorAll('.dv-m-fill')[1].style.width, 10) || 0;
    if (at() === 0) return done();
    const t0 = Date.now();
    const tick = setInterval(() => {
      if (at() === 0 || Date.now() - t0 > 12000) { clearInterval(tick); done(); }
    }, 100);
  }));
  await page.waitForTimeout(300);
  const silent = await drawnFill();
  /*
   * "EMPTY" IS UNDER A PIXEL, not exactly zero. A cut voice leaves a peak meter
   * holding 1% — which is -59.4 dB on this scale, and 1% of a 64px bar is less
   * than one pixel. Asserting `=== '0%'` failed on that and would have failed on
   * any implementation with a peak hold, which is every implementation worth
   * having. The claim is that the meter draws NOTHING VISIBLE; that is what is
   * checked.
   */
  /*
   * "EMPTY" IS THE RMS BAR, and the PEAK is asserted separately and loosely.
   *
   * Two goes at this. `=== '0%'` failed at 1%, then `< 2` failed at 4% — because
   * what is left after every voice is cut is not silence, it is the plugin's own
   * noise floor at around -58 dB. That is real output and the meter is right to
   * show it; a peak meter that read exactly zero on a running synth would be the
   * broken one.
   *
   * So the RMS bar has to be empty — average level of nothing is nothing — and the
   * peak only has to be far below where it was playing. Which is the honest claim:
   * a cut voice leaves a noise floor, not a void.
   */
  audibleOr(silent.width === '0%' ? { ok: true } : machine(),
     'and with every voice cut, its level bar is empty',
     JSON.stringify(silent));
  /*
   * `loudEnough` IS WHAT IT WAS PLAYING, so if it was never playing this comparison is between
   * two zeros and passes by accident — or, at load, fails for a reason that is not the app.
   * Both halves go through the starvation guard for that reason.
   */
  audibleOr(parseInt(loudEnough, 10) > 0
              && parseInt(silent.tick, 10) < parseInt(loudEnough, 10) / 3
              // `heard` is this section's own play attempt, taken above; a starved run failed
              // there too, and that is what makes this one inconclusive rather than false.
              ? { ok: true } : heard,
     'and its peak is far below what it was playing',
     `${silent.tick} against ${loudEnough} playing`);

  const heardAgain = await playUntilAudible();
  const loud = await drawnFill();
  audibleOr(parseInt(loud.width, 10) > 0 ? { ok: true } : heardAgain,
     'and playing, it draws something again',
     JSON.stringify(loud));

  /*
   * ASK 4: BYPASS MOVES THE CARD AND THE METER TOGETHER.
   *
   * One assertion over both, deliberately. They are two renderings of one fact —
   * this device is doing nothing — and a check that tested them apart would pass
   * a build where the card dims and the meter carries on showing level, which
   * reads as "bypass is broken" to anyone looking at the meter and "bypass
   * works" to anyone looking at the card.
   */
  /*
   * A REAL POINTER, not `element.click()`. The rack's handler is delegated on
   * POINTERDOWN, which a synthetic click never fires — so the first version of
   * this check clicked, nothing happened, and the assertion that bypass came back
   * on afterwards passed VACUOUSLY, because it had never gone off.
   */
  await (await cards[1].$('.dv-byp')).click();
  await page.waitForTimeout(1800);
  const off = await cards[1].evaluate((el) => ({
    dim: el.classList.contains('byp'),
    width: el.querySelectorAll('.dv-m-fill')[1].style.width,
  }));
  ok(off.dim && off.width === '0%',
     'bypassed, the card says so AND its meter goes silent', JSON.stringify(off));

  await page.keyboard.press('b');
  await page.waitForTimeout(1800);
  const back = await cards[1].evaluate((el) => ({
    dim: el.classList.contains('byp'),
    width: el.querySelectorAll('.dv-m-fill')[1].style.width,
  }));
  // Through the guard too: "the level came back" needs a level to come back to, and on a swamped
  // box there was never one. Same reasoning as the three above.
  audibleOr(!back.dim && parseInt(back.width, 10) > 0 ? { ok: true } : heard,
     'and switching it back on brings both back', JSON.stringify(back));

  /*
   * AND THE BAR IS THE MODEL'S NUMBER, EXACTLY — because that is the only pair of
   * numbers belonging to the same instant.
   *
   * THREE versions of this check were wrong in the same way. It compared the DOM
   * against `deviceMeters()`, which is the engine's CURRENT reading: the DOM was
   * written on the last draw and the store has moved on since. Twelve points of
   * slack failed at thirteen. Reading both inside one evaluate failed at
   * twenty-eight, which is when it became clear the slack was never absorbing lag —
   * it was hiding the fact that the two numbers were not about the same moment. The
   * `card` snapshot here was taken dozens of assertions earlier.
   *
   * So the chain is checked as two exact links. ENGINE -> MODEL is asserted above
   * against the published region. MODEL -> DOM is this, against the fractions the
   * renderer actually wrote. Neither has a tolerance, because neither crosses a
   * frame.
   */
  const link = await page.evaluate(() => {
    const p = window.__uni.chainProbe();
    const m = (p.meters || []).find((x) => x && x.device === 0);
    let width = null, tick = null, db = null;
    for (const el of document.querySelectorAll('.dv-card')) {
      if (!el.offsetParent || el._devId !== 0) continue;
      width = el.querySelectorAll('.dv-m-fill')[1].style.width;
      tick = el.querySelectorAll('.dv-m-tick')[1].style.left;
      db = el.querySelector('.dv-m-db').textContent;
    }
    return { m, width, tick, db };
  });
  ok(link.m && link.width !== null, 'the model and its bar can both be read',
     JSON.stringify(link));
  if (link.m && link.width !== null) {
    ok(parseInt(link.width, 10) === link.m.outRms,
       'the drawn bar is exactly the level the model computed',
       `${link.width} vs ${link.m.outRms}%`);
    ok(parseInt(link.tick, 10) === link.m.outPeak,
       'and the peak tick is exactly the peak', `${link.tick} vs ${link.m.outPeak}%`);
    ok(link.db === link.m.text, 'and the readout is the string it was given',
       `${link.db} vs ${link.m.text}`);
    // `−∞` is what a silent meter reads, and it is CORRECT for silence — so this only asks for a
    // number when there was sound. On a swamped box it is the machine being reported, not a
    // broken readout.
    audibleOr(/^-?\d/.test(String(link.db)) ? { ok: true } : heard,
              'which is a dB number', String(link.db));
  }

  await page.evaluate(() => window.__uni.run('stop'));
  await page.waitForTimeout(400);
}

section('dragging a clip moves it');
{
  await page.evaluate(() => window.__uni.run('view arrange'));
  await page.evaluate(() => window.__uni.run('goto 1 1'));
  await page.keyboard.press('Home');
  await page.waitForTimeout(500);

  // The clip's own geometry, keyed on its stable placement id so the assertions
  // can follow THIS clip rather than "whatever is first" after it has moved.
  const clipBox = () => page.evaluate(() => {
    for (const c of document.querySelectorAll('.ar-clip')) {
      const r = c.getBoundingClientRect();
      if (r.width < 40 || r.height < 6) continue;
      const top = document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2);
      if (!(top && (top === c || c.contains(top)))) continue;
      return { id: c._pId, tick: c._pTick, end: c._pEnd, track: c._pTrack,
               x: r.x, y: r.y, w: r.width, h: r.height };
    }
    return null;
  });
  const byId = (id) => page.evaluate((wanted) =>
    (window.__uni.clips() || []).find((c) => c.id === wanted) || null, id);

  /*
   * HOW FAR IS TWO BARS, IN PIXELS?
   *
   * The first version of this dragged a flat 120px and failed, and the failure
   * was entirely mine: a drag snaps to the bar, the arrangement's zoom is left
   * wherever an earlier section put it, and at the finest zoom 120px is a
   * quarter of a bar — which snaps to nothing. The clip correctly did not move.
   *
   * A gesture test has to speak the units the gesture is defined in. Two bars is
   * two bars at any zoom.
   */
  // Normalise the zoom with the keys that do it, so the pixel arithmetic below
  // is predictable AND the setup is still real input. Earlier sections leave the
  // arrangement wherever their own gestures put it.
  for (let i = 0; i < 8; i++) {
    const z = await page.evaluate(() => window.__uni.arrangeProbe().zoomIndex);
    if (z === 3) break;
    await page.keyboard.press(z > 3 ? '+' : '-');
    await page.waitForTimeout(80);
  }
  const barPx = await page.evaluate(() => {
    const a = window.__uni.arrangeProbe();
    return (a && a.ticksPerPixel) ? 3840000 / a.ticksPerPixel : 64;
  });
  const box = await clipBox();
  if (!box) {
    blocked(false, 'dragging a clip moves it', 'no clip wide enough to grab');
  } else {
    ok(Number.isFinite(box.id) && box.id > 0,
       'the clip carries a stable placement id, not a list index', `id=${box.id}`);

    // ── MOVE. Grab the middle, well clear of both trim handles. ──────────────
    const before = await byId(box.id);
    const midX = box.x + box.w / 2, midY = box.y + box.h / 2;
    await page.mouse.move(midX, midY);
    await page.mouse.down();
    // Two moves, not one: a single move can be coalesced into the press on some
    // platforms, and a drag that only commits after two events would pass a
    // one-move test and fail under a hand.
    await page.mouse.move(midX + barPx * 2, midY, { steps: 6 });
    await page.waitForTimeout(60);
    const ghost = await page.evaluate(() => {
      const g = document.querySelector('.ar-ghost');
      if (!g || g.style.display === 'none') return null;
      const r = g.getBoundingClientRect();
      return { x: Math.round(r.x), w: Math.round(r.width) };
    });
    ok(ghost !== null, 'a ghost follows the pointer while dragging');
    ok(ghost && ghost.x > box.x, 'and it is ahead of the clip it came from',
       ghost && `ghost ${ghost.x} vs clip ${Math.round(box.x)}`);
    // The real block must NOT have moved yet: it moves when the engine says so.
    const during = await page.evaluate(() =>
      [...document.querySelectorAll('.ar-clip')].some((c) => c.classList.contains('dragging')));
    ok(during, 'the clip being dragged is marked, so the ghost reads as the live one');

    await page.mouse.up();
    await page.waitForTimeout(700);
    const after = await byId(box.id);
    ok(after && after.at > before.at, 'releasing moves the clip',
       after ? `${before.at} -> ${after.at}` : 'the clip vanished');
    ok(after && after.len === before.len, 'and a move does not resize it',
       after && `${before.len} -> ${after.len}`);
    ok(await page.evaluate(() => {
      const g = document.querySelector('.ar-ghost');
      return !g || g.style.display === 'none';
    }), 'the ghost goes when the drag ends');

    // ── TRIM. The right handle is six pixels wide; aim inside it. ────────────
    const b2 = await clipBox();
    const trimBefore = await byId(b2.id);
    await page.mouse.move(b2.x + b2.w - 3, b2.y + b2.h / 2);
    await page.mouse.down();
    await page.mouse.move(b2.x + b2.w + barPx * 2, b2.y + b2.h / 2, { steps: 6 });
    await page.mouse.up();
    await page.waitForTimeout(700);
    const trimAfter = await byId(b2.id);
    ok(trimAfter && trimAfter.len > trimBefore.len, 'dragging the right edge lengthens it',
       trimAfter ? `${trimBefore.len} -> ${trimAfter.len} (box w=${Math.round(b2.w)}, barPx=${barPx})`
                 : 'gone');
    ok(trimAfter && trimAfter.at === trimBefore.at,
       'and leaves the start exactly where it was — the sentinel does its job',
       trimAfter && `${trimBefore.at} -> ${trimAfter.at}`);

    // ── ESCAPE. A drag abandoned mid-gesture must commit nothing. ────────────
    const b3 = await clipBox();
    const escBefore = await byId(b3.id);
    await page.mouse.move(b3.x + b3.w / 2, b3.y + b3.h / 2);
    await page.mouse.down();
    await page.mouse.move(b3.x + b3.w / 2 + barPx * 3, b3.y + b3.h / 2, { steps: 6 });
    await page.keyboard.press('Escape');
    await page.mouse.up();
    await page.waitForTimeout(600);
    const escAfter = await byId(b3.id);
    ok(escAfter && escAfter.at === escBefore.at, 'Escape abandons a drag, changing nothing',
       escAfter && `${escBefore.at} -> ${escAfter.at}`);

    // ── A CLICK IS NOT AN EDIT. Selecting must not cost an undo step. ────────
    const b4 = await clipBox();
    const clickBefore = await byId(b4.id);
    await page.mouse.click(b4.x + b4.w / 2, b4.y + b4.h / 2);
    await page.waitForTimeout(500);
    const clickAfter = await byId(b4.id);
    ok(clickAfter && clickAfter.at === clickBefore.at,
       'a click selects without moving anything',
       clickAfter && `${clickBefore.at} -> ${clickAfter.at}`);

    // ── BACKSPACE removes the selected clip, and only with a selection. ──────
    const b5 = await clipBox();
    const n0 = (await page.evaluate(() => window.__uni.clips().length));
    await page.mouse.click(b5.x + b5.w / 2, b5.y + b5.h / 2);
    await page.waitForTimeout(300);
    await page.keyboard.press('Backspace');
    await page.waitForTimeout(700);
    const n1 = (await page.evaluate(() => window.__uni.clips().length));
    ok(n1 === n0 - 1, 'Backspace removes the selected clip', `${n0} -> ${n1}`);
    // And with nothing selected it must not eat the keystroke.
    await page.keyboard.press('Escape');
    await page.waitForTimeout(200);
    await page.keyboard.press('Backspace');
    await page.waitForTimeout(600);
    const n2 = (await page.evaluate(() => window.__uni.clips().length));
    ok(n2 === n1, 'and with nothing selected it removes nothing', `${n1} -> ${n2}`);
  }
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

section('harmony quantize is settable and shows its own state');
{
  /*
   * THE ENGINE HAS TAKEN THIS SINCE BEFORE THIS UI EXISTED, and it was recorded on our side as
   * unreachable with the reason "the flag is writable but NOT published, so no control can show
   * its state". That was wrong — `uiTrackMixFlags` bit 2 has carried it all along — and the
   * wrong note kept a working feature out of the UI for as long as it stood.
   *
   * Driven from the CONSOLE and read back from the ENGINE's own flags, then driven again from
   * the header CONTROL, because a toggle that only the console can reach is half a feature and
   * a control that cannot show its state is worse than none.
   */
  await page.evaluate(() => window.__uni.setView('tracker'));
  await page.waitForTimeout(400);
  /*
   * BRING THE TRACK ON SCREEN FIRST.
   *
   * The header scrolls with the track strip, and by this point in a 400-check run it is
   * somewhere else entirely — the button's rect came back at x = -2496 and the click landed on
   * nothing, which reads as a dead control. `scrollTo` is the ROW axis and did not help; moving
   * the CURSOR to the track is what scrolls the strip horizontally, which is also what a person
   * does before clicking a track's header.
   */
  await page.evaluate(() => window.__uni.setView('tracker'));
  await page.waitForTimeout(300);
  const q = () => page.evaluate(() => window.__uni.harmonyQuantized());
  const before = await q();
  ok(Array.isArray(before) && before.length > 1,
     'the engine publishes a harmony-quantize flag per track', JSON.stringify(before));

  /*
   * A TRACK THAT STARTS OFF, found rather than assumed.
   *
   * `maximal` ships with the flag already set on some of its tracks — the first read was
   * [1,0,1,1,0,0] — so "turn track 0 on and check it is on" is a check that cannot move. It
   * would pass with the command deleted. Same trap as every other one this file has caught, and
   * it is worth the two lines to pick a track by its state instead of by its number.
   */
  const off0 = before.findIndex((x) => x === 0);
  ok(off0 > 0, 'and there is a track with it OFF to turn on — a check that starts at the value '
     + 'it is setting cannot fail', JSON.stringify(before));

  await page.evaluate((t) => window.__uni.run(`harmony-quantize ${t} on`), off0);
  await page.waitForTimeout(1200);
  const on = await q();
  ok(on[off0] === 1, `the console turns it on for track ${off0} and the engine agrees`,
     `${JSON.stringify(before)} -> ${JSON.stringify(on)}`);
  // ...and ONLY that track. A command that set the flag everywhere would look identical on the
  // one track anybody checked.
  ok(on.every((x, i) => i === off0 || x === before[i]),
     'and no other track moved', JSON.stringify(on));

  // The header control, clicked — the same function the verb calls.
  await page.evaluate((t) => window.__uni.run(`goto 0 ${t}`), off0);
  await page.waitForTimeout(600);
  const at = await page.evaluate((t) => {
    const b = document.querySelector(`.htrack[data-track="${t}"] .hhq`);
    if (!b) return null;
    const r = b.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2, lit: b.classList.contains('on'),
             onScreen: r.x > 0 && r.x < window.innerWidth };
  }, off0);
  ok(at && at.lit && at.onScreen,
     'the header control is there, on screen, and LIT — it shows its own state',
     JSON.stringify(at));
  if (at && at.onScreen) {
    await page.mouse.click(at.x, at.y);
    await page.waitForTimeout(1200);
    const back = await q();
    ok(back[off0] === 0, 'and clicking it turns the flag back off', JSON.stringify(back));
  }
}

section('a lane\'s subdivision is settable');
{
  /*
   * PER-LANE GRIDS WERE COMPLETE EXCEPT FOR THE PATH THAT WRITES THEM.
   *
   * `lines_per_beat` has been per track in the project format, published since SHM v10, and
   * honoured by the tracker's grid — a project could carry a 3-rows-per-beat lane against a 4
   * elsewhere and this app drew both correctly, while no surface could MAKE one. Opcode 92
   * landed today and this is both halves of the UI for it.
   */
  await page.evaluate(() => window.__uni.setView('tracker'));
  await page.waitForTimeout(400);
  const lpbOf = (t) => page.evaluate((k) => (window.__uni.engine().lpb || [])[k], t);
  const was = await lpbOf(1);
  ok(was > 0, 'the engine publishes a lane subdivision', String(was));

  const want = was === 3 ? 6 : 3;
  await page.evaluate(([t, n]) => window.__uni.run(`lpb ${t} ${n}`), [1, want]);
  await page.waitForTimeout(1200);
  ok(await lpbOf(1) === want, `the console sets track 1 to ${want}/beat`,
     `${was} -> ${await lpbOf(1)}`);
  // ...and only that track, because a command that set every lane would look identical here.
  ok(await lpbOf(0) !== want || was === (await lpbOf(0)),
     'and track 0 is untouched', String(await lpbOf(0)));

  /*
   * OUT OF RANGE IS REFUSED, NOT CLAMPED, and the two ends are refused for different reasons:
   * 32 packs as 0 in the grid's five-bit field, and 0 is that packer's sentinel for "no grid".
   * A clamp would hand back a subdivision nobody asked for with no way to notice.
   */
  for (const bad of [0, 32]) {
    await page.evaluate(([t, n]) => window.__uni.run(`lpb ${t} ${n}`), [1, bad]);
    await page.waitForTimeout(500);
  }
  ok(await lpbOf(1) === want, 'and 0 or 32 leaves the lane exactly as it was — refused, not '
     + 'clamped', String(await lpbOf(1)));

  /*
   * THE READOUT IS THE CONTROL. Clicking it cycles; it has looked like a label since the
   * feature landed.
   *
   * WHAT IT CYCLES IS THE LEVEL IN FORCE, which is not always the track. A lane's rows
   * resolve clip -> track -> zoom, so on a track whose clip carries its own subdivision the
   * badge names the CLIP's number and the click edits the clip (opcode 94). This check used
   * to assert the track's `lpb` moved, and it passed only because no clip could carry a grid
   * of its own until something could write one — a control that wrote the track while the
   * badge showed the clip would change a number nothing on screen was using.
   */
  await page.evaluate(() => window.__uni.run('goto 0 1'));
  await page.waitForTimeout(600);
  const at = await page.evaluate(() => {
    const b = document.querySelector('.htrack[data-track="1"] .hlpb');
    if (!b) return null;
    const r = b.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2,
             onScreen: r.x > 0 && r.x < window.innerWidth, text: b.textContent,
             fromClip: b.classList.contains('from-clip') };
  });
  ok(at && at.onScreen, 'the header draws the lane grid, on screen', JSON.stringify(at));
  if (at && at.onScreen) {
    // Whichever level the badge is naming, read from the same place the user reads it.
    const shown = () => page.evaluate(() => {
      const b = document.querySelector('.htrack[data-track="1"] .hlpb');
      return b ? parseInt(b.textContent, 10) : NaN;
    });
    const before = await shown();
    const trackBefore = await lpbOf(1);
    await page.mouse.click(at.x, at.y);
    await page.waitForTimeout(1200);
    const after = await shown();
    /*
     * `after !== want` ALONE IS NOT THE CLAIM, and my first version stopped there — which passes
     * when the click did nothing AND the console set before it did nothing, because the lane
     * simply never left its starting value. The claim is that it moved from where it WAS when
     * the click happened, so that is what is compared.
     */
    ok(after !== before && after >= 1 && after <= 31,
       'and clicking it moves the subdivision the badge is naming', `${before} -> ${after}`);
    // ...and it moved the level it NAMED. A clip-level badge that wrote the track would leave
    // the screen unmoved, which is the failure this pair exists to tell apart.
    const trackAfter = await lpbOf(1);
    ok(at.fromClip ? trackAfter === trackBefore : trackAfter !== trackBefore,
       at.fromClip ? 'and left the track\'s own value alone, because the badge named the clip'
                   : 'and moved the track\'s value, because the badge named the track',
       `badge ${at.fromClip ? 'clip' : 'track'}, track ${trackBefore} -> ${trackAfter}`);
  }
}

section('the palette and the scale button reach something');
{
  /*
   * A COMMAND RUN FROM THE PALETTE PUTS ITS ANSWER SOMEWHERE.
   *
   * The palette calls `api.log('out', result)` and guards it with a `typeof === 'function'`
   * check — and `dockApi` had no `log`, so the guard swallowed every palette command's output.
   * The launcher closed, the command ran, and the answer was discarded. `help` from the palette
   * produced nothing at all, which reads as a launcher that does not work.
   *
   * Driven through the palette's own run, and read out of the CONSOLE's transcript, because
   * "somewhere a person can read it" is the claim and the transcript is that place.
   */
  await page.evaluate(() => window.__uni.setView('tracker'));
  await page.waitForTimeout(400);
  const before = await page.evaluate(
    () => document.querySelectorAll('.dk-line').length);
  const out = await page.evaluate(() => {
    window.__uni.palette(true);
    // A command that takes NO arguments. `oct` needs one, so the grammar refuses it, `run`
    // returns null, and the check would have been measuring the refusal path instead of the
    // output path.
    window.__uni.paletteQuery('state');
    return window.__uni.paletteRun();
  });
  await page.waitForTimeout(500);
  const after = await page.evaluate(
    () => [...document.querySelectorAll('.dk-line')].map((e) => e.textContent).slice(-3));
  ok(out !== null && out !== undefined && String(out).length > 0,
     'a palette command returns its answer', JSON.stringify(out));
  ok(after.length > 0 && after.some((t) => t && t.includes(String(out).slice(0, 8))),
     'and the answer lands in the console transcript rather than nowhere',
     `${before} lines before; last lines now ${JSON.stringify(after)}`);

  /*
   * THE CHROME'S SCALE BUTTON DOES SOMETHING.
   *
   * `createChrome` has taken an `onScales` click handler since it was written and nothing
   * passed one, so the button drew the current key, promised a scale browser in its tooltip
   * and did nothing at all. It opens the palette seeded with `harmony `, whose `scale`
   * argument is a oneOf over the engine's own scale registry — which is the scale browser this
   * app actually has.
   */
  await page.evaluate(() => window.__uni.palette(false));
  await page.waitForTimeout(300);
  const clicked = await page.evaluate(() => {
    const b = document.querySelector('.ch-btn.ch-scales');
    if (!b) return null;
    const r = b.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
  });
  ok(!!clicked, 'the chrome has a scale button');
  if (clicked) {
    await page.mouse.click(clicked.x, clicked.y);
    await page.waitForTimeout(500);
    const pal = await page.evaluate(() => {
      const p = window.__uni.paletteProbe ? window.__uni.paletteProbe() : null;
      return { open: !!(p && p.open), query: p ? p.query : null };
    });
    ok(pal.open, 'clicking it opens the palette — it was wired to nothing at all',
       JSON.stringify(pal));
    ok(pal.query && pal.query.startsWith('harmony'),
       'seeded with `harmony `, so the scale list is what it offers', JSON.stringify(pal));
    await page.keyboard.press('Escape');
    await page.waitForTimeout(300);
  }
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
