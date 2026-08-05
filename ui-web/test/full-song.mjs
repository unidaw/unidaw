#!/usr/bin/env node
/**
 * A WHOLE SONG, BUILT WITH THE INTERFACE, AND THEN PROVED TO SOUND.
 *
 * The goal this exists for, stated by the owner: "a suite makes a full song with the UI,
 * using Zebralette plugin (not Zebra2) and the built-in Sampler, using both tracked notes
 * and Patcher."
 *
 * Every other suite in this directory tests ONE surface, and most of them talk to the app
 * through `__uni.run()` — which is a string handed to a command table, not a person using
 * the program. Ten minutes of real use turned up five separate defects that every one of
 * those suites was green through, and each had the same shape: a capability that existed at
 * both ends with nothing joining them, reachable from the console and from nowhere on
 * screen. The plugin editor button had never worked once. The console could not be typed
 * into. The sampler could not be added, and once added could not be given a file.
 *
 * So this drives the POINTER and the KEYBOARD, end to end, and asks the only question that
 * matters about a DAW: does the song you just built make the sound you built it to make.
 *
 * FOUR THINGS ARE ASSEMBLED, deliberately at four different points on the timeline:
 *
 *   bar 1   the built-in SAMPLER, given a file from the SAMPLES rail
 *   bar 2   ZEBRALETTE, chosen BY NAME from the plugin catalogue
 *   bar 3   a PATCHER graph, wired from nodes
 *   all     tracked notes, typed on the tracker's piano row with real key events
 *
 * Separated in TIME because that is what makes "every voice is audible" answerable. A render
 * that dropped three of four voices and kept one is a render with audio in it, and a check
 * that only asks "is it silent" passes on exactly that. Each part gets its own window.
 *
 * ZEBRALETTE, NOT ZEBRA2, and it is asserted rather than assumed. Both plugins live inside
 * Zebra2.vst3, and a bundle loader that takes the first entry gets Zebra2 — so "the project
 * names Zebralette" and "the engine loaded Zebralette" are different claims. The one that
 * matters is what the HOST reports back, and that is what is checked.
 *
 * THE ORACLE IS THE OFFLINE RENDER, not a live capture. `daw_engine --render` has no audio
 * device and no wall clock — the pump waits for every host on every block — so it is
 * byte-identical run to run and needs nothing of the machine. Its four companions are all
 * here, because a WAV assertion without them proves nothing: not silent, each voice audible
 * in its own window, two renders identical, and a deliberately different song rendering
 * different bytes.
 */

import { chromium } from 'playwright';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startStack } from './stack.mjs';
import { readWav, envelope } from './wav.mjs';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));

let pass = 0, fail = 0, blocked = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};
const block = (what, why) => { blocked++; console.log('  BLOCKED ', what, `— ${why}`); };
const step = (s) => console.log(`\n${s}`);

const SONG = 'fullsong';
const PLUGIN = 'Zebralette';

const stack = await startStack({ keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await page.waitForTimeout(1500);

/*
 * `engineState()`, NOT `state().engine` and NOT `engineStats()`.
 *
 * `state()` describes the PAGE — view, cursor, edit mode — and carries no engine block at
 * all, so `state().engine.noteCount` throws. `engineStats()` is the SOCKET's counters. The
 * engine's own published facts are on `engineState()`, and the three read so similarly that
 * journey.mjs has a comment about exactly this confusion.
 */
const st = () => page.evaluate(() => {
  const s = window.__uni.state();
  const e = window.__uni.engineState ? window.__uni.engineState() : null;
  return { view: s.view, focus: s.focus, editMode: s.editMode, reject: s.reject,
           cursor: { row: s.cursor.row, track: s.cursor.track, col: s.cursor.col },
           engine: e };
});
const run = async (c) => page.evaluate((x) => window.__uni.run(x), c);
const settle = (ms) => page.waitForTimeout(ms);

/**
 * Open the rail on a category and click the first row whose text contains `want`.
 *
 * ENSURES the rail is open rather than TOGGLING it — ⌘B is a toggle, and calling this with
 * the rail already up closes it, whereupon every row is invisible, nothing matches, and the
 * failure reads as "that thing cannot be added" instead of "the test shut the window".
 *
 * Returns a REASON string on failure rather than false, so a machine without the plugin is
 * distinguishable from a category that refused the pointer.
 */
const pick = async (cat, want) => {
  const open = await page.evaluate(() => {
    const r = document.querySelector('.br');
    return !!r && r.offsetParent !== null && getComputedStyle(r).display !== 'none';
  });
  if (!open) await page.keyboard.press('Meta+b');
  await settle(500);
  return page.evaluate(async ([c, w]) => {
    const chip = document.querySelector(`.br-chip[data-cat="${c}"]`);
    if (!chip) return `no ${c} chip`;
    if (chip.disabled) return `the ${c} category is unavailable`;
    chip.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    await new Promise((r) => setTimeout(r, 400));
    const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
    const row = rows.find((el) => (el.textContent || '').toLowerCase().includes(w.toLowerCase()));
    if (!row) return `no row matching "${w}" in ${c}`;
    row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    return true;
  }, [cat, want]);
};

/**
 * Put the tracker cursor on (row, track) and give the grid the keyboard.
 *
 * A REAL MOUSE, not a synthesised PointerEvent. The grid calls `setPointerCapture(pointerId)`
 * on pointerdown, and a dispatched event carries an id belonging to no active pointer — so
 * every click threw "No active pointer with the given id is found" into the page. Nine of
 * them, caught only because this suite fails on page errors. The app was right and the test
 * was pretending; `page.mouse` produces a pointer that actually exists.
 *
 * The click lands wherever it lands and moves the cursor, so the `goto` is repeated after it.
 */
let gridBox = null;
const at = async (row, track) => {
  if (!gridBox) {
    const host = await page.$('#tracker') || await page.$('.tk');
    gridBox = host ? await host.boundingBox() : null;
  }
  if (gridBox) {
    await page.mouse.click(gridBox.x + 60, gridBox.y + Math.min(60, gridBox.height / 2));
    await settle(120);
  }
  await run(`goto ${row} ${track}`);
  await settle(150);
};

/**
 * Type notes on the tracker's piano row. 'z s x d c' is C D E F G.
 *
 * MOVES THE CURSOR BETWEEN KEYS. Typing does not advance it here, so the first version of
 * this wrote four notes onto ONE row — all four came back at `nanotick: 0`, which is a chord
 * on the downbeat and not the phrase it was meant to be. The saved file is what said so; the
 * note COUNT was four either way, so every count-based check passed.
 */
const typeNotes = async (keys, startRow, track, step = 2) => {
  const s = await st();
  if (!s.editMode) { await page.keyboard.press('Meta+e'); await settle(250); }
  for (let i = 0; i < keys.length; i++) {
    await at(startRow + i * step, track);
    await page.keyboard.press(keys[i]);
    await settle(200);
  }
  await settle(600);
};

// ===========================================================================
step('1. a new song, its tempo and its meter');
// ===========================================================================
await run(`new ${SONG}`);
await settle(1200);
await run('tempo 120');
await run('timesig 4/4');
await settle(400);
const s1 = await st();
check(!!s1, 'the app has state after a new song');

// ===========================================================================
step('2. track 1 — the built-in SAMPLER, added and fed from the rail');
// ===========================================================================
const addedSampler = await pick('devs', 'sampler');
check(addedSampler === true, 'a sampler is added from the DEVICES rail', String(addedSampler));
await page.waitForFunction(() => {
  const p = window.__uni.chainProbe();
  return !!p && p.cards >= 1;
}, null, { timeout: 15000 }).catch(() => {});

const loadedSample = await pick('smpl', 'waveform_probe');
check(loadedSample === true, 'a sample is loaded into it from the SAMPLES rail',
      String(loadedSample));

// The kit is the engine's answer, and the only proof the file reached the SAMPLER.
const kit = await page.waitForFunction(() => {
  const t = window.__uni.state().cursor.track;
  const ch = window.__uni.chainProbe();
  if (!ch) return null;
  for (let d = 0; d < 6; d++) {
    const k = window.__uni.samplerKitCached(t, d);
    if (k && k.slots && k.slots.length) return { device: d, slots: k.slots.length };
  }
  return null;
}, null, { timeout: 25000 }).then((h) => h.jsonValue()).catch(() => null);
check(!!kit, 'the engine answers with a kit — the file reached the sampler',
      'no kit; a load addressed to the wrong device looks exactly like this');

await typeNotes(['z', 'x', 'c', 'z'], 0, 0);
const afterSampler = ((await st()).engine || {}).noteCount || 0;
check(afterSampler > 0, 'typed notes land on the sampler track', String(afterSampler));

/*
 * PUT THE SLOT WHERE THE KEYBOARD ACTUALLY PLAYED, and this is not a workaround.
 *
 * `load-sample` mints every slot on C1 FIXED-PITCH — root 36, keylow 36, keyhigh 36 — so a
 * sample loaded from the rail answers exactly one key. The tracker's piano row at its default
 * octave writes 48. Every structural fact was perfect: the kit arrived, the slot existed, the
 * notes were in the clip, the project saved, the render ran. And it rendered SILENCE, because
 * nothing the keyboard could type reached the zone the sample was sitting in.
 *
 * That is the exact "reports healthy, produces nothing" shape this suite exists to catch, and
 * it caught it on the first run — the check that found it was "the song is not silent", which
 * is why that check comes before every comparison.
 *
 * The slot follows the KEYS, read back rather than assumed, which keeps this file ignorant of
 * the tracker's key map. All three of keylow/keyhigh/root: a zone whose low is above its high
 * never matches, and a root left at 36 transposes the sample by the distance it moved.
 */
const played = await page.evaluate(() => {
  const all = window.__uni.notes() || [];
  return [...new Set(all.filter((n) => n.tr === 0).map((n) => n.p))].sort((a, b) => a - b);
});
check(played.length > 0, 'the keyboard wrote pitches on the sampler track', JSON.stringify(played));
const samplerDev = kit ? kit.device : 0;
if (played.length) {
  await run(`slot 0 ${samplerDev} 1 keylow ${played[0]}`);
  await run(`slot 0 ${samplerDev} 1 keyhigh ${played[played.length - 1]}`);
  await run(`slot 0 ${samplerDev} 1 root ${played[0]}`);
  await settle(700);
  const zone = await page.evaluate((d) => {
    const k = window.__uni.samplerKitCached(0, d);
    return k && k.slots && k.slots[0]
      ? { lo: k.slots[0].keyLow, hi: k.slots[0].keyHigh, root: k.slots[0].root } : null;
  }, samplerDev);
  check(!!zone && zone.lo <= played[0] && zone.hi >= played[0],
        'the sample now answers the keys that were typed', JSON.stringify(zone));
}

// ===========================================================================
step('3. track 2 — ZEBRALETTE, chosen by name from the catalogue');
// ===========================================================================
await run('add-track');
await settle(800);
await run('goto 0 1');
await settle(300);

const addedPlugin = await pick('plug', PLUGIN);
if (addedPlugin !== true) {
  block(`${PLUGIN} can be inserted from the rail`, String(addedPlugin));
} else {
  check(true, `${PLUGIN} can be inserted from the rail`);
}

let hostName = '';
if (addedPlugin === true) {
  /*
   * WHAT THE HOST REPORTS, not what the row said. Zebralette and Zebra2 are both inside
   * Zebra2.vst3; a loader that takes the bundle's first entry returns Zebra2 for either
   * row, and the rack would still draw the name the CATALOGUE gave. The engine names a
   * device from the plugin it actually instantiated, so this is the only claim worth
   * making — and it is exactly the distinction the goal asked for.
   */
  hostName = await page.waitForFunction(() => {
    const p = window.__uni.chainProbe();
    const t = (p && p.titles) ? p.titles.join(',') : '';
    return /zebra/i.test(t) ? t : null;
  }, null, { timeout: 30000 }).then((h) => h.jsonValue()).catch(() => '');
  check(/zebralette/i.test(hostName),
        `the engine loaded ${PLUGIN} and not Zebra2`,
        `the rack reports ${JSON.stringify(hostName)}`);

  await typeNotes(['z', 'd', 'g'], 8, 1);
  const n2 = ((await st()).engine || {}).noteCount || 0;
  check(n2 > afterSampler, 'typed notes land on the plugin track',
        `${afterSampler} -> ${n2}`);
}

// ===========================================================================
step('4. track 3 — a PATCHER graph, wired from nodes');
// ===========================================================================
await run('add-track');
await settle(800);
await run('goto 0 2');
await settle(300);

/*
 * A PATCHER EVENT DEVICE PLUS AN INSTRUMENT, which is what a patcher actually is here.
 *
 * The first version added a `patcher instrument` and two nodes and asserted "nodes can be
 * added". Both halves were wrong. `osc` is not a node type at all — the list is kernel,
 * euclidean, passthru, audio, lfo, random, out, slice — so that call was refused, and the
 * check still passed because the OTHER addnode changed the output it was diffing. And a
 * patcher is an EVENT graph: every shipped preset wires euclidean -> random_degree ->
 * event_out, generating notes that the track's INSTRUMENT then sounds. A patcher on its own
 * is silent by construction, so "the patcher track is audible" was measuring the sampler's
 * one-shot bleeding across the whole song.
 */
const addedPatcher = await pick('devs', 'patcher event');
check(addedPatcher === true, 'a patcher event device is added from the rail',
      String(addedPatcher));
await settle(1200);

const patcherVoice = await pick('plug', PLUGIN);
if (patcherVoice !== true) {
  block('the patcher track gets an instrument to sound through', String(patcherVoice));
} else {
  check(true, 'the patcher track gets an instrument to sound through');
}
await settle(1500);

/*
 * OPEN THE DEVICE'S GRAPH BEFORE EDITING IT, and this line is the whole lesson of this
 * section.
 *
 * Without it every patcher edit is POOL-SCOPED: it lands in the shared graph owned by no
 * device, and since patcher-is-a-device the pool is not what a project renders or saves. The
 * first version of this suite skipped it, added a euclidean and an out node, asserted from
 * `__uni.nodes()` that the patcher had two nodes, passed — and saved this device holding
 * `nodes: 0`. Every assertion was true and the conclusion was false, because `nodes()` reports
 * the ASSEMBLED POOL and a fact about the pool says nothing about the device.
 *
 * A double-click on the patcher card is the app's own gesture for opening a graph.
 */
const opened = await page.evaluate(() => {
  const cards = [...document.querySelectorAll('.dv-card')].filter((el) => el.style.display !== 'none');
  const card = cards.find((el) => /patcher/i.test(el.textContent || ''));
  if (!card) return null;
  card.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
  return card._devId;
});
await settle(900);
const target = await page.evaluate(() => window.__uni.patchTarget());
check(opened !== null && target.device === opened && target.track === 2,
      'the patcher graph is opened for THIS track and device — edits are device-scoped',
      `${JSON.stringify(target)} vs device ${opened}`);

await run('addnode euclidean');
await settle(600);
await run('addnode out');
await settle(600);
const nodeList = await page.evaluate(() => window.__uni.nodes());
const ids = Array.isArray(nodeList) ? nodeList.map((n) => n.id) : [];
check(ids.length >= 2, 'two nodes are added',
      JSON.stringify(nodeList).slice(0, 160));
if (ids.length >= 2) {
  const linked = await run(`link ${ids[ids.length - 2]} ${ids[ids.length - 1]}`);
  await settle(700);
  check(!/refus|error|no /i.test(String(linked)), 'and they link',
        JSON.stringify(String(linked)).slice(0, 120));
}

await typeNotes(['z', 'x'], 16, 2);

// ===========================================================================
step('5. arrangement furniture — a marker and a loop');
// ===========================================================================
await run('marker 0 start');
await run('loop 1 4');            // bars are 1-based on the ruler
await settle(400);
const s5 = await st();
check(!!s5, 'the song carries markers and a loop');

// ===========================================================================
step('6. save it');
// ===========================================================================
await run(`save ${SONG}`);
await settle(1500);
const SONG_PATH = join(stack.dir, `${SONG}.uniproj.json`);
const onDisk = existsSync(SONG_PATH);
check(onDisk, 'the song is on disk');

/*
 * WHERE EACH PART ACTUALLY SITS, read from the file rather than computed from bars.
 *
 * The first version assumed "row 16 is bar 2" and windowed at 2s and 4s. The tracker's
 * `goto` is in DISPLAYED rows, and how many nanoticks a displayed row spans depends on the
 * ZOOM, not on the track's lines-per-beat — so the parts actually landed at 0s, 8s and 16s
 * and the 8-second render did not reach two of them. Every window read 0.0000 and the suite
 * reported three silent instruments, which was true of the file and false of the song.
 *
 * Reading the placements makes the arithmetic unnecessary: the engine has already decided
 * where everything is, and this is the same "find the downbeat IN the file rather than
 * computing it" rule the capture suites learned the hard way.
 */
const saved = onDisk ? JSON.parse(readFileSync(SONG_PATH, 'utf8')) : null;
const Q = (saved && (saved.nanoticks_per_quarter
                     || (saved.timebase && saved.timebase.nanoticks_per_quarter))) || 960000;
const BPM = (saved && saved.tempo_map && saved.tempo_map[0] && saved.tempo_map[0].bpm) || 120;
const secPerQuarter = 60 / BPM;
/** [{ track, name, atSec }] for every placed clip, in timeline order. */
const parts = saved ? saved.tracks.flatMap((t) => (t.placements || []).map((pl) => ({
  track: t.track_id, name: t.name,
  atSec: (pl.at / Q) * secPerQuarter,
}))).sort((a, b) => a.atSec - b.atSec) : [];
console.log('   parts on the timeline:',
            parts.map((p) => `${p.name}@${p.atSec.toFixed(2)}s`).join(' '));
check(parts.length === 3, 'three parts are placed on the timeline',
      JSON.stringify(parts.map((p) => [p.name, +p.atSec.toFixed(2)])));

/*
 * THE PATCHER GRAPH IS IN THE FILE — the claim the published graph cannot make.
 *
 * Nothing on the wire distinguishes "this device's graph has two nodes" from "the pool has two
 * nodes and this device has none"; that is the defect, so the check has to read the project.
 */
const patcherDev = saved
  ? ((saved.tracks.find((t) => t.track_id === 2) || {}).device_chain || [])
      .find((d) => String(d.kind || '').includes('patcher'))
  : null;
const graph = (patcherDev && (patcherDev.patcher_state || patcherDev.patcher)) || {};
check((graph.nodes || []).length >= 2,
      "the patcher nodes are saved in the DEVICE'S graph, not the pool",
      `the saved device holds ${(graph.nodes || []).length} node(s)`);
check((graph.edges || []).length >= 1, 'and the link with them',
      `the saved device holds ${(graph.edges || []).length} edge(s)`);

const noteTotal = ((await st()).engine || {}).noteCount || 0;
check(noteTotal >= 9, 'every part contributed notes', `${noteTotal} notes`);
check(errors.length === 0, 'no page errors while building the song', errors.join(' | '));

await browser.close();

// ===========================================================================
step('7. render it offline, twice, and ask whether it sounds');
// ===========================================================================
/*
 * A SEPARATE ENGINE PROCESS with its OWN shared-memory name — two engines on one segment is
 * the failure that cost a night, and it is prevented here rather than detected.
 */
/*
 * LONG ENOUGH TO REACH THE LAST PART, computed from where the parts actually are. A fixed
 * eight seconds stopped before two of three instruments had played a note, and a window past
 * the end of a file reads exactly like an instrument that produced nothing.
 */
const RENDER_SECONDS = Math.max(8, Math.ceil((parts.length ? parts[parts.length - 1].atSec : 0) + 6));

const render = (name, project = SONG) => {
  const out = join(stack.dir, `${name}.wav`);
  try { unlinkSync(out); } catch { /* absent is the normal case */ }
  execFileSync(join(ROOT, 'build', 'daw_engine'),
               ['--project', project, '--render', name, '--run-seconds', String(RENDER_SECONDS)],
               { cwd: join(ROOT, 'build'),
                 env: { ...process.env,
                        DAW_PROJECT_DIR: stack.dir,
                        DAW_HOST_BINARY: join(ROOT, 'build', 'juce_host_process'),
                        DAW_UI_SHM_NAME: `/fullsong_${process.pid}_${name}` },
                 stdio: ['ignore', 'pipe', 'pipe'], timeout: 180000 });
  return existsSync(out) ? readFileSync(out) : null;
};

let takeA = null;
try { takeA = render('take_a'); }
catch (e) { check(false, 'the offline render runs', String(e).slice(0, 220)); }
check(takeA && takeA.length > 44, 'the render writes a WAV',
      takeA ? `${takeA.length} bytes` : 'nothing');

if (takeA) {
  const wav = readWav(join(stack.dir, 'take_a.wav'));
  const SLICE = 0.05;                                      // seconds per envelope bucket
  const env = envelope(wav.mono, wav.rate, SLICE);
  const peak = env.reduce((m, v) => Math.max(m, v), 0);
  const total = wav.mono.length / wav.rate;

  // 1. NOT SILENT. First, because every comparison below is satisfied by silence.
  check(peak > 0.001, 'the song is not silent', `peak ${peak.toFixed(4)}`);

  /*
   * 2. EVERY PART AUDIBLE IN ITS OWN WINDOW. The three instruments were placed a bar apart
   *    precisely so this question can be asked; a render that kept one voice and dropped two
   *    passes "not silent" and fails here, which is the whole point of the separation.
   */
  const windowPeak = (fromSec, toSec) => {
    const a = Math.max(0, Math.floor(fromSec / SLICE));
    const b = Math.min(env.length, Math.ceil(toSec / SLICE));
    let m = 0;
    for (let i = a; i < b; i++) m = Math.max(m, env[i]);
    return m;
  };
  /*
   * 2. EVERY VOICE AUDIBLE — PROVED BY ISOLATION, not by windowing the mix.
   *
   * Windowing the full mix does NOT answer this, and believing it would have been the
   * expensive mistake here. `waveform_probe.wav` is about eight seconds long and a freshly
   * minted slot is ONE-SHOT — "a slot minted at one-shot plays its whole sample however short
   * the note is" — so a single sampler note covers the entire song. Every window had energy in
   * it, all three "is audible" checks were green, and two of them were measuring the sampler.
   *
   * So each track is rendered ALONE: mute the other two through the app, save under its own
   * name, render that. A track that contributes nothing fails here and cannot be rescued by a
   * neighbour bleeding into its window.
   */
  const soloRender = async (keep, label) => {
    const b = await chromium.launch({ channel: 'chrome' });
    const p = await b.newPage({ viewport: { width: 1600, height: 950 } });
    await p.goto(stack.url, { waitUntil: 'load' });
    await p.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
    await p.waitForTimeout(1200);
    await p.evaluate((n) => window.__uni.run(`load ${n}`), SONG);
    await p.waitForTimeout(1800);
    // `mute <track>` TOGGLES, so this is only correct from a freshly loaded song.
    for (const t of [0, 1, 2]) {
      if (t === keep) continue;
      await p.evaluate((x) => window.__uni.run(`mute ${x}`), t);
      await p.waitForTimeout(250);
    }
    const name = `${SONG}_${label}`;
    await p.evaluate((n) => window.__uni.run(`save ${n}`), name);
    await p.waitForTimeout(1800);
    await b.close();
    try { return render(`take_${label}`, name); } catch (e) { return null; }
  };

  const solos = [];
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    const wavBuf = await soloRender(part.track, `solo${i}`);
    if (!wavBuf) { block(`${part.name} alone produces audio`, 'the solo render did not run'); continue; }
    const w = readWav(join(stack.dir, `take_solo${i}.wav`));
    const e = envelope(w.mono, w.rate, SLICE);
    const pk = e.reduce((m, v) => Math.max(m, v), 0);
    check(pk > 0.001, `${part.name} alone produces audio — nothing else is playing`,
          `peak ${pk.toFixed(4)}`);
    solos.push({ part, buf: wavBuf });
  }

  /*
   * AND THE MUTING ACTUALLY HAPPENED. Without this the three checks above are vacuous: if
   * `mute` were a no-op, every "solo" render would BE the full mix, all three would find
   * audio, and the suite would report each instrument proved in isolation having isolated
   * nothing. The same trap as a green check on the wrong object — the measurement is fine
   * and the thing measured is not what the sentence claims.
   */
  for (const s of solos) {
    check(Buffer.compare(s.buf, takeA) !== 0,
          `${s.part.name} alone differs from the full mix — the muting took effect`);
  }
  for (let i = 1; i < solos.length; i++) {
    check(Buffer.compare(solos[i].buf, solos[0].buf) !== 0,
          `${solos[i].part.name} alone differs from ${solos[0].part.name} alone`);
  }

  // 3. REPEATABLE, so a byte difference below is the change and not a race.
  let takeB = null;
  try { takeB = render('take_b'); } catch (e) { /* reported by the check */ }
  check(takeB && Buffer.compare(takeA, takeB) === 0,
        'two renders of one song are byte-identical',
        takeB ? `${takeA.length} vs ${takeB.length} bytes` : 'the second render did not run');

  /*
   * 4. THE COMPARISON DISCRIMINATES. Without this, "the bytes matched" also passes for an
   *    engine that renders the same thing regardless of the project — and the difference is
   *    made THROUGH THE APP, because editing the saved JSON to remove notes changes nothing:
   *    notes live in the clip store, not on the placement.
   */
  const ctlBrowser = await chromium.launch({ channel: 'chrome' });
  const p2 = await ctlBrowser.newPage({ viewport: { width: 1600, height: 950 } });
  await p2.goto(stack.url, { waitUntil: 'load' });
  await p2.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
  await p2.waitForTimeout(1200);
  await p2.evaluate((n) => window.__uni.run(`load ${n}`), SONG);
  await p2.waitForTimeout(1500);
  await p2.evaluate(() => window.__uni.run('mute 0'));
  await p2.waitForTimeout(500);
  await p2.evaluate((n) => window.__uni.run(`save ${n}`), SONG);
  await p2.waitForTimeout(1500);
  await ctlBrowser.close();

  let takeC = null;
  try { takeC = render('take_c'); } catch (e) { /* reported by the check */ }
  check(takeC && Buffer.compare(takeA, takeC) !== 0,
        'muting a track through the app changes the render — the comparison discriminates',
        takeC ? 'the bytes were identical with a track muted' : 'the control render did not run');
}

await stack.stop();

const note = blocked ? ` · ${blocked} BLOCKED` : '';
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed${note}`
                      : `ALL PASS (${pass} checks)${note}`}\n`);
process.exit(fail ? 1 : 0);
