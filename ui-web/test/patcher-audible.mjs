#!/usr/bin/env node
/**
 * DOES THE PATCHER MAKE A SOUND? Nothing in this repo asked.
 *
 * The patcher is one of the eight things on the demo runbook, and every test it has checks what
 * it WRITES: the nodes go in, the link lands, the device's own graph saves rather than the pool.
 * All structural, all true, and none of them would notice a patcher that built a perfect graph
 * and emitted nothing — which is exactly the shape `chord-render.mjs` found for chords ("it is
 * in the model, the scheduler reaches it, it emits note-ons, and the peak is 0").
 *
 * A patcher is an EVENT graph: it generates notes and the track's instrument sounds them. So it
 * needs an instrument after it, and the thing to measure is whether the instrument speaks.
 *
 * THE CONTROL IS THE POINT, and it is the second render.
 *
 * "The render made a noise" proves nothing here — the track carries a sampler, and a sampler
 * with a sample in it can be made to sound by all sorts of things that are not the patcher. So
 * the same song is rendered TWICE: once with the euclidean's hits as authored, and once with the
 * pattern emptied. If the second is silent, the sound in the first came from the patcher and
 * from nowhere else. If both sound, something other than the graph is making the noise and the
 * first render was never evidence.
 *
 * That ordering matters too: the second render is the one that can only be explained one way.
 *
 * OFFLINE, because on this machine a live capture cannot answer anything — the device opens and
 * the callback never fires. A render has no device, no origin to locate and nothing to starve.
 */

import { chromium } from 'playwright';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startStack } from './stack.mjs';
import { readWav, envelope } from './wav.mjs';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));
let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const SONG = 'patchaud';
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

console.log('\ndoes a patcher graph reach the speakers\n');

await run(`new ${SONG}`);
await settle(1400);

/*
 * A KEY, because the euclidean emits DEGREES and a degree is resolved against the harmony
 * timeline. Set before anything else so nothing below has to wonder about it.
 */
await run('harmony 0 major 0');
await settle(900);

/*
 * A SAMPLER FOR THE PATCHER TO PLAY, and a real sample in it.
 *
 * `demo_pluck_c4.wav` because its attack is in the first millisecond — the probe assets are
 * silent for their first second and a sampler stretches that when transposing down, which has
 * twice been mistaken here for an engine fault. An asset that speaks immediately means a silent
 * window is about the patcher.
 *
 * A patcher EVENT device is not an instrument by the engine's rule, so the two coexist on one
 * track; that is the arrangement the runbook describes.
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
    if (!chip || chip.disabled) return `${c} unavailable`;
    chip.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    await new Promise((r) => setTimeout(r, 400));
    const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
    const row = rows.find((el) => (el.textContent || '').toLowerCase().includes(w.toLowerCase()));
    if (!row) return `no "${w}" in ${c}`;
    row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    return true;
  }, [cat, want]);
};

/*
 * THE PATCHER FIRST, THEN THE INSTRUMENT — and the order is the whole thing, not tidiness.
 *
 * A chain is a signal path. An event graph generates notes for what comes AFTER it, so a patcher
 * placed behind the sampler emits into nothing and the track is silent with every structural
 * check green. The runbook says this in one line — "it needs an instrument after it" — and I
 * built this suite the other way round and spent two renders looking for an engine defect.
 */
check(await pick('devs', 'patcher event') === true, 'an event patcher goes on the track FIRST');
await settle(1500);
check(await pick('devs', 'sampler') === true, 'then a sampler for it to play');
await settle(1500);
check(await pick('smpl', 'demo_pluck') === true, 'and a sample into that');
await settle(2500);

const order = await page.evaluate(() => {
  const c = window.__uni.chains()[0];
  return c && c.devices ? c.devices.map((d) => `${d.pos}:${d.kind}`) : [];
});
check(String(order[0] || '').includes(':0'), 'the patcher really is ahead of the instrument',
      JSON.stringify(order));

/*
 * OPEN THE PATCHER'S OWN CARD BEFORE EDITING. Without a device open every edit is POOL-scoped:
 * the published graph looks perfectly correct and the device saves with no nodes, so the render
 * would be silent for a reason that has nothing to do with the audio path.
 */
const devId = await page.evaluate(() => {
  const cards = [...document.querySelectorAll('.dv-card')].filter((el) => el.style.display !== 'none');
  const card = cards.find((el) => /patcher/i.test(el.textContent || ''));
  if (!card) return null;
  card.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
  return card._devId;
});
await settle(900);
const target = await page.evaluate(() => window.__uni.patchTarget());
check(devId !== null && target.device === devId,
      'the patcher card is open, so the edits land in ITS graph',
      `${JSON.stringify(target)} vs device ${devId}`);

/*
 * euclidean -> random_degree -> event out, AND THE MIDDLE NODE IS REQUIRED BY DESIGN.
 *
 * A euclidean emits GATES — a rhythm with no pitch — and the note-resolution path skips gates
 * deliberately. Something has to promote a gate to a degree before a note exists: random_degree
 * does, and so does SliceSelect. So a direct `euclidean -> event_out` renders silence, correctly.
 *
 * I measured that silence and reported it as a defect. It is documented in
 * `tools/patcher_plays_sampler_check.sh`, whose header says the first version of THAT fixture
 * made the same mistake — "a fixture that cannot produce the input makes the check agree with you
 * for the wrong reason". Two of us fell in it, which is why the runbook now warns about it.
 */
await run('addnode euclidean');
await settle(700);
await run('addnode random');
await settle(700);
await run('addnode out');
await settle(700);
const nodes0 = await page.evaluate(() => window.__uni.nodes() || []);
const ids = nodes0.map((n) => n.id);
check(ids.length >= 2, 'a euclidean and an out are in the graph', JSON.stringify(ids));

/*
 * A FRESHLY ADDED EUCLIDEAN ARRIVES WITH SETTINGS — this used to read the other way round.
 *
 * What it said, and it was true: `addnode euclidean` minted the node with every published
 * field at ZERO, because `AddPatcherNode` attaches no config struct at all
 * (apps/patcher_graph.cpp `addPatcherNode`) and the published `hasConfig` byte is 0. The box
 * said "no config published" and the eight settings could not be touched — reported from
 * live use, and the whole of building a patch from scratch.
 *
 * The app now sends the defaults for a node it adds, as soon as the engine publishes the
 * node it made (index.html `seedNewNodes`). The values are the engine's own — the member
 * initialisers in apps/patcher_abi.h, which are also what the Rust kernel falls back to for
 * a null config block — so this changed what the node SAYS, not what it does.
 *
 * Asserted as the exact eight, because "not all zero" would pass on a node seeded with one
 * field set, and because these numbers are a mirror of the engine's.
 */
const euclid = nodes0.find((n) => String(n.type).includes('euclid'));
check(!!euclid, 'the euclidean is identifiable in the graph', JSON.stringify(nodes0.map((n) => n.type)));
if (euclid) {
  const cfg = euclid.config || [];
  // steps 16, hits 5, offset 0, degree 1, oct 0, vel 100, base octave 4, dur 0 (half a step).
  const want = [16, 5, 0, 1, 0, 100, 4, 0];
  check(want.every((v, i) => cfg[i] === v),
        'and it arrives with the ENGINE\'S OWN defaults, not "no config published"',
        `config ${JSON.stringify(cfg)} vs ${JSON.stringify(want)} — all zeros means the seed `
        + 'never landed and the node has eight settings nobody can edit');

  // steps/hits/vel, because a pattern of no hits at no velocity is not a pattern.
  await run(`patch ${euclid.id} steps 8`);
  await settle(300);
  await run(`patch ${euclid.id} hits 4`);
  await settle(300);
  await run(`patch ${euclid.id} vel 100`);
  await settle(300);
  /*
   * AND THE BASE OCTAVE, which defaults to 0 and is not a detail.
   *
   * Octave 0 puts the emitted note around MIDI 12 — four octaves below the sample's root of 60 —
   * and a sampler transposing down by four octaves plays at a SIXTEENTH of the rate, so a 1.6
   * second pluck becomes twenty-five seconds of near-silence and a ten-second render catches
   * none of it. Silent for a reason that has nothing to do with the graph.
   */
  await run(`patch ${euclid.id} base 4`);
  await settle(300);
  /*
   * AND THE DEGREE, which also mints at 0. Degrees are 1-based everywhere else in this program —
   * `add_chords` refuses 0 outright — so a node emitting "degree 0" may be emitting nothing at
   * all. Set explicitly rather than left to a coercion nobody has verified for this node type.
   */
  await run(`patch ${euclid.id} degree 1`);
  await settle(500);
  const after = await page.evaluate((id) =>
    (window.__uni.nodes() || []).find((n) => n.id === id), euclid.id);
  console.log(`  euclidean config after setting it: ${JSON.stringify(after && after.config)}`);
  check(after && after.config && after.config[0] === 8 && after.config[1] === 4,
        'setting steps and hits from the console takes',
        JSON.stringify(after && after.config));
}

// Chain them in order: each node to the next.
for (let i = 0; i + 1 < ids.length; i++) {
  await run(`link ${ids[i]} ${ids[i + 1]}`);
  await settle(500);
}

/*
 * A CLIP FOR THE TRACK TO BE SCHEDULED OVER — made by TYPING a note and then deleting it.
 *
 * A track with no placement is not scheduled at all, so its generator never runs; the graph
 * saved perfectly and the render was silent with `placements: 0`.
 *
 * `add-clip <clip> ...` was the obvious way and is the wrong one here: it PLACES an existing
 * clip, and a project made with `new` has none — so it wrote a placement pointing at clip 1,
 * which did not exist, and every note typed afterwards went nowhere. Both the patcher render and
 * the note control came back silent and neither was about the patcher. Typing a note is what
 * MAKES a clip.
 *
 * Then the note is deleted, leaving the clip and the placement behind: anything left in it would
 * sound, and the render could not tell that from the pattern.
 */
const grid0 = await page.$('#tracker') || await page.$('.tk');
const box0 = grid0 ? await grid0.boundingBox() : null;
if (box0) await page.mouse.click(box0.x + 60, box0.y + Math.min(60, box0.height / 2));
await settle(300);
if (!(await page.evaluate(() => window.__uni.state().editMode))) {
  await page.keyboard.press('Meta+e');
  await settle(300);
}
await run('goto 0 0');
/*
 * ONE LONG NOTE — 30 quarters, 15 seconds — and its length is the CLIP's length.
 *
 * A quarter-note version made a 0.5s placement, which LOOPS: the note retriggered every half
 * second forever and the "patcher" window was measuring it. The control caught that — pattern on
 * and pattern off gave the identical 0.2658 — which is exactly what a control is for.
 *
 * The sample is a one-shot and decays in about 1.6s whatever the note length, so a long note
 * gives the clip its extent without sounding through the window being measured.
 */
await run('note 60 28800000');
await settle(800);
/*
 * THE NOTE STAYS. Deleting it left the placement with LENGTH 0 — `placements: [(1, 3840000, 0)]`
 * — and a placement covering no time schedules nothing, so the generator never ran and the render
 * was silent. That cost a wrong report to backend: the patcher was fine, my clip had no duration.
 *
 * So the note stays and gives the clip its extent, and the patcher is measured in a window WELL
 * AFTER it has decayed. The sample rings for about 1.6s, so from 3s on, anything sounding is the
 * pattern. The control below re-renders with the pattern emptied and that same window must go
 * silent — which is what separates "the patcher played" from "the note is still ringing".
 */
const clipInfo = await page.evaluate(() => (window.__uni.clips() || [])
  .map((c) => ({ at: c.at, len: c.len })));
console.log(`  placements: ${JSON.stringify(clipInfo)}`);
check(clipInfo.length > 0 && clipInfo[0].len > 0,
      'the clip has a real LENGTH — a zero-length placement schedules nothing at all',
      JSON.stringify(clipInfo));

await run(`save ${SONG}`);
await settle(2200);

/**
 * Render the saved project and return the peak of a WINDOW, or -1.
 *
 * From 3 seconds by default: the clip's one typed note is at tick 0 and rings for about 1.6s, so
 * everything after that is the generator. Measuring the whole file would credit the patcher with
 * the note that gives the clip its length.
 */
const renderPeak = (name, out, from = 3.0) => {
  const wav = join(stack.dir, `${out}.wav`);
  try { unlinkSync(wav); } catch { /* absent is normal */ }
  try {
    execFileSync(join(ROOT, 'build', 'daw_engine'),
                 ['--project', name, '--render', out, '--run-seconds', '10'],
                 { cwd: join(ROOT, 'build'),
                   env: { ...process.env, DAW_PROJECT_DIR: stack.dir,
                          DAW_HOST_BINARY: join(ROOT, 'build', 'juce_host_process'),
                          DAW_UI_SHM_NAME: `/patchaud_${process.pid}` },
                   stdio: ['ignore', 'pipe', 'pipe'], timeout: 180000 });
  } catch (e) {
    console.log(`  render ${out} failed: ${String(e).slice(0, 160)}`);
    return -1;
  }
  if (!existsSync(wav)) return -1;
  const w = readWav(wav);
  const per = 0.05;
  const env = envelope(w.mono, w.rate, per);
  let m = 0;
  for (let i = Math.round(from / per); i < env.length; i++) m = Math.max(m, env[i]);
  return m;
};

/*
 * THE POSITIVE CONTROL, AND IT IS NOT OPTIONAL — chord-render.mjs learned this the hard way.
 *
 * "The patcher window is silent" and "this whole track is silent" are the same observation.
 * Only a NOTE that sounds in the same project, through the same sampler, the same chain and the
 * same render tells them apart. Without it a silent patcher render is equally good evidence for
 * a broken patcher, a broken sampler, a sample that never resolved, or a muted track — and I
 * would be reporting whichever one I happened to suspect.
 *
 * Typed into the clip AFTER the patcher render, so the first render measures the graph alone.
 */
/*
 * THE KEYMAP QUESTION, ASKED BEFORE THE ROUTING ONE.
 *
 * The engine publishes `unmapped` on the kit read-back — notes that hit no slot — and it splits
 * a silent render into two completely different investigations:
 *
 *     silent, unmapped > 0   the notes arrived and no slot answers their pitch (a KEYMAP miss)
 *     silent, unmapped == 0  no notes arrived at all (a ROUTING failure)
 *
 * Backend's A/B showed a patcher into a slot pinned at key 36 is silent while the same graph into
 * a slot spanning 0-127 peaks at 0.4027 — a euclidean emits degrees around 48-60 and a one-key
 * slot matches none of them. So this reading is what tells my fixture's case from theirs, and it
 * is one line rather than another round of renders.
 */
const kitAfter = await page.evaluate(() => {
  for (let d = 0; d < 6; d++) {
    const k = window.__uni.samplerKitCached(0, d);
    if (k && k.slots && k.slots.length) {
      return { unmapped: k.unmapped, voices: k.activeVoices,
               slots: k.slots.map((s) => ({ lo: s.keyLow, hi: s.keyHigh, root: s.root })) };
    }
  }
  return null;
});
console.log(`  kit: ${JSON.stringify(kitAfter)}`);
check(kitAfter && kitAfter.slots.some((s) => s.lo <= 48 && s.hi >= 60),
      'the slot spans the pitches a euclidean emits (degrees near 48-60)',
      `${JSON.stringify(kitAfter && kitAfter.slots)} — a slot pinned to one key is silent for a `
      + `generated pattern and sounds for a typed note, which reads as a routing failure`);

const live = renderPeak(SONG, 'take');
console.log(`  with the pattern:  peak ${live < 0 ? 'RENDER FAILED' : live.toFixed(4)}`);
check(live > 0.004, 'THE PATCHER MAKES A SOUND — its notes reach the instrument and the master',
      `peak ${live.toFixed(4)}; a graph that builds correctly and emits nothing looks exactly `
      + `like this and every structural test still passes`);

/*
 * THE CONTROL. The euclidean's HITS go to zero — the graph, the link, the sampler and the sample
 * all stay exactly as they are, and the only thing that changes is whether the generator fires.
 *
 * Edited in the SAVED FILE rather than through the UI on purpose: it changes one number and
 * touches nothing else, so a difference between the two renders cannot be some other edit's
 * doing. `hits` is the euclidean's second config field.
 */
const path = join(stack.dir, `${SONG}.uniproj.json`);
let silenced = false;
try {
  const doc = JSON.parse(readFileSync(path, 'utf8'));
  for (const t of doc.tracks || []) {
    for (const d of t.device_chain || []) {
      const g = d.patcher_state || d.patcher;
      for (const n of (g && g.nodes) || []) {
        // The saved node carries its config under its TYPE name, and only when non-zero — a
        // node with everything at zero saves as a bare {id, type}, which is how the first run
        // of this suite came to look for a key that was not there.
        if (n.euclidean && n.euclidean.hits) { n.euclidean.hits = 0; silenced = true; }
      }
    }
  }
  doc.meta = { ...(doc.meta || {}), name: `${SONG}_mute` };
  const { writeFileSync } = await import('node:fs');
  writeFileSync(join(stack.dir, `${SONG}_mute.uniproj.json`), JSON.stringify(doc));
} catch (e) {
  check(false, 'the saved project can be edited for the control', String(e).slice(0, 160));
}
check(silenced, 'the control found the euclidean node to silence',
      'no node with a euclidean config in the saved chain — if the graph did not save, the '
      + 'render above was measuring something else entirely');

if (silenced && live > 0.004) {
  const quiet = renderPeak(`${SONG}_mute`, 'ctrl');
  console.log(`  hits = 0:          peak ${quiet < 0 ? 'RENDER FAILED' : quiet.toFixed(4)}`);
  check(quiet >= 0 && quiet < 0.004,
        'and with the pattern emptied it is SILENT — so the sound came from the graph',
        `peak ${quiet.toFixed(4)} against ${live.toFixed(4)}; both sounding means something other `
        + `than the patcher is making the noise and the first render proved nothing`);
} else if (silenced) {
  /*
   * NOT RUN, AND SAID SO. With the live render silent the control is guaranteed to be silent
   * too, and it would "pass" while establishing nothing — a check that cannot fail is worse
   * than a missing one, because it reads as evidence in the summary.
   */
  console.log('  BLOCK the control — the live render was silent, so a silent control says nothing');
}

/*
 * Now the control: the same project with one typed note in it.
 */
const grid = await page.$('#tracker') || await page.$('.tk');
const box = grid ? await grid.boundingBox() : null;
if (box) await page.mouse.click(box.x + 60, box.y + Math.min(60, box.height / 2));
await settle(300);
if (!(await page.evaluate(() => window.__uni.state().editMode))) {
  await page.keyboard.press('Meta+e');
  await settle(300);
}
await run('goto 4 0');
/*
 * ONE LONG NOTE — 30 quarters, 15 seconds — and its length is the CLIP's length.
 *
 * A quarter-note version made a 0.5s placement, which LOOPS: the note retriggered every half
 * second forever and the "patcher" window was measuring it. The control caught that — pattern on
 * and pattern off gave the identical 0.2658 — which is exactly what a control is for.
 *
 * The sample is a one-shot and decays in about 1.6s whatever the note length, so a long note
 * gives the clip its extent without sounding through the window being measured.
 */
await run('note 60 28800000');
await settle(700);
await run(`save ${SONG}_note`);
await settle(2000);
const withNote = renderPeak(`${SONG}_note`, 'ctl2');
console.log(`  plus a typed note:  peak ${withNote < 0 ? 'RENDER FAILED' : withNote.toFixed(4)}`);
check(withNote > 0.004,
      'A TYPED NOTE ON THIS TRACK SOUNDS — so the sampler, the sample, the chain and the render '
      + 'all work, and a silent patcher render is about the PATCHER',
      `peak ${withNote.toFixed(4)}; if this is silent too the fault is somewhere in the setup and `
      + `the patcher result above establishes nothing`);

/*
 * THE SECOND POSITIVE CONTROL, AND THE ONE THAT LOCATES THE FAULT.
 *
 * `presets/projects/generator.uniproj.json` is this same graph — euclidean -> event_out, patcher
 * device AHEAD of the instrument, one placement — feeding a VST INSTRUMENT instead of the
 * built-in sampler. If that sounds and the sampler version does not, the patcher, the scheduler,
 * the graph, the placement and the render are all fine and the gap is between a generated event
 * and the BUILT-IN SAMPLER specifically.
 *
 * Rendered straight from the preset rather than built through the UI, because the point is that
 * it is not this suite's construction that differs.
 */
const genDir = join(stack.dir, 'gen');
try {
  const { mkdirSync, copyFileSync } = await import('node:fs');
  mkdirSync(genDir, { recursive: true });
  copyFileSync(join(ROOT, 'presets/projects/generator.uniproj.json'),
               join(genDir, 'generator.uniproj.json'));
  execFileSync(join(ROOT, 'build', 'daw_engine'),
               ['--project', 'generator', '--render', 'gen', '--run-seconds', '10'],
               { cwd: join(ROOT, 'build'),
                 env: { ...process.env, DAW_PROJECT_DIR: genDir,
                        DAW_HOST_BINARY: join(ROOT, 'build', 'juce_host_process'),
                        DAW_UI_SHM_NAME: `/patchgen_${process.pid}` },
                 stdio: ['ignore', 'pipe', 'pipe'], timeout: 180000 });
  const gw = join(genDir, 'gen.wav');
  const vstPeak = existsSync(gw)
    ? envelope(readWav(gw).mono, readWav(gw).rate, 0.05).reduce((m, v) => Math.max(m, v), 0) : -1;
  console.log(`  patcher -> VST:     peak ${vstPeak < 0 ? 'RENDER FAILED' : vstPeak.toFixed(4)}`);
  check(vstPeak > 0.004,
        'the SAME graph feeding a VST instrument sounds — so the patcher itself works',
        `peak ${vstPeak.toFixed(4)}; if this is silent too the fault is in the patcher and not `
        + `in what it feeds`);
} catch (e) {
  check(false, 'the generator preset renders', String(e).slice(0, 160));
}

check(errors.length === 0, 'nothing threw', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
