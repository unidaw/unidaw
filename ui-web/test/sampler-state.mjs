/**
 * EVERY SAMPLER SLOT FIELD, DRIVEN FROM THE BROWSER AND READ BACK AS STATE.
 *
 * The sampler suites so far prove SOUND — chop-audible, note-off-cuts, sound-op-audible — and
 * sound is expensive to assert and coarse when you do. What they do not cover is the surface
 * area: `slot <track> <device> <slot> <field> <value>` reaches twenty-nine fields, and before
 * this file about six of them had ever been sent from the page. A verb with twenty-three
 * untested branches is twenty-three chances for the failure mode this project keeps finding —
 * the command reports success and nothing changes.
 *
 * SO EVERY FIELD IS SET, AND EVERY FIELD IS READ BACK, THROUGH TWO INDEPENDENT WITNESSES:
 *
 *   - The SHM read-back (`samplerKit`), which backend publishes FROM THE SNAPSHOT THE AUDIO
 *     PRODUCER READS rather than from the document. It sees thirteen of the fields, and it is
 *     the only one that can say the audio thread agrees.
 *   - The SAVED PROJECT, which sees all twenty-nine. It is the only one that can say the edit
 *     will still be there tomorrow — and this project has already shipped a knob that was heard,
 *     drawn, and silently dropped at save (see patchsave.mjs), so "it took" and "it was kept"
 *     are genuinely different questions.
 *
 * A field with no read-back witness is REPORTED, not skipped quietly: the list is printed, so
 * the limit of this file is visible in its own output rather than in a comment nobody reads.
 *
 * AND THE VALUES ARE ALL DIFFERENT FROM THE DEFAULTS. A check that sets a field to what it
 * already holds passes whether or not the command does anything — the exact shape that made
 * backend's SliceSelect fixture green with the feature off. Every value below is asserted to
 * differ from what was read BEFORE it was written.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { writeFileSync, readFileSync, existsSync, statSync } from 'node:fs';
import { resolve } from 'node:path';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8, keepDir: true });
const Q = 960000;
const WAV = resolve('presets/audio/waveform_probe.wav');
const TRACK = 0, DEVICE = 9, SLOT = 1, SLOT2 = 2;

/*
 * THE FIXTURE, hand-written, and carrying MORE THAN ONE OF EVERYTHING THAT IS ADDRESSED BY ID.
 *
 * Two sources, two slots, a mod set and a slice set with two markers. That is not decoration:
 * `source`, `slice` and `modset` are the three fields the engine REFUSES rather than clamps,
 * and a refusal check needs a valid id to contrast with the invalid one. A fixture with one
 * source cannot tell "it moved the slot to source 2" from "it left it on the only source there
 * is" — the same blind spot that let a one-device project publish patcher owner 0 and look
 * right.
 */
writeFileSync(`${stack.dir}/slotfix.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'slotfix', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [], clips: [],
  tracks: [{
    track_id: 0, name: 'Kit', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
    device_chain: [{
      device_id: DEVICE, kind: 'sampler', capability_mask: 5, patcher_node_id: 0,
      host_slot_index: 4294967295, bypass: false, vst_ref: {},
      sampler: {
        next_slot_id: 3, next_source_id: 3, next_mod_set_id: 3,
        voice_cap: 16, default_gate: 0, default_view: 0,
        sources: [{ local_id: 1, path: WAV }, { local_id: 2, path: WAV }],
        // Markers on SOURCE 1, because a slice id is validated against the slot's own source —
        // slice 7 of another file would play the wrong region, so the engine checks the set
        // whose source_local_id matches the slot's. Two markers, so `slice 2` is a move.
        slice_sets: [{ source_local_id: 1, next_marker_id: 3,
                       markers: [{ id: 1, frame: 0, tune_cents: 0, reverse: 0, mod_set_id: 0 },
                                 { id: 2, frame: 1000, tune_cents: 0, reverse: 0, mod_set_id: 0 }] }],
        /*
         * TWO mod sets, because the loader gives a slot with no `mod_set_id` the FIRST one —
         * slot 1 comes up already pointing at set 1. Setting it to 1 would be a check that
         * cannot move, which the "none of them was set to the value it already held" assertion
         * below exists to catch, and did.
         */
        mod_sets: [{ id: 1, name: 'a', filter_type: 0, cutoff_milli: 0, resonance_milli: 0,
                     next_modulator_id: 1, modulators: [] },
                   { id: 2, name: 'b', filter_type: 0, cutoff_milli: 0, resonance_milli: 0,
                     next_modulator_id: 1, modulators: [] }],
        slots: [
          { id: SLOT, source_local_id: 1, key_low: 36, key_high: 36, root_key: 36 },
          { id: SLOT2, source_local_id: 1, key_low: 38, key_high: 40, root_key: 38 },
        ],
      },
    }],
    mod_links: [], placements: [],
  }],
}));

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => { if (!errors.includes(e.message)) errors.push(e.message); });

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1200);
await page.evaluate(() => window.__uni.loadProject('slotfix'));
await page.waitForTimeout(3000);

/** Ask, then wait for the answer to land. A receipt is not an outcome. */
const ask = async () => {
  await page.evaluate(([t, d]) => window.__uni.samplerKit(t, d), [TRACK, DEVICE]);
  await page.waitForFunction(([t, d]) => !!window.__uni.samplerKitCached(t, d),
                             [TRACK, DEVICE], { timeout: 4000 }).catch(() => {});
  return page.evaluate(([t, d]) => window.__uni.samplerKitCached(t, d), [TRACK, DEVICE]);
};
const run = (line) => page.evaluate((s) => window.__uni.run(s), line);

/*
 * WHAT THE ENGINE SAID, which on this wire is the only place a refusal reliably appears.
 *
 * `samplerSlot` reports success as soon as the message is on the ring — the engine's answer
 * comes back as a log event and an SHM diff, and a command that was refused looks exactly like
 * one that was applied from the console's side. This project's own rule, learned the hard way:
 * grep the log for `*_rejected` before believing the surface.
 *
 * Printed with every failure below rather than asserted on, because the reason belongs in the
 * output of the check that noticed, not in a separate check nobody reads.
 */
const LOG = `${stack.dir}/../engine.log`;
const rejects = (needle) => {
  if (!existsSync(LOG)) return `no engine log at ${LOG}`;
  return readFileSync(LOG, 'utf8').split('\n')
    .filter((l) => l.includes('rejected') && (!needle || l.includes(needle)))
    .slice(-4).join(' ; ') || 'the engine logged no refusal at all';
};

{
  const kit = await ask();
  check(kit && kit.found === true && kit.slots.length === 2,
        'the fixture loads as a sampler with two slots', JSON.stringify(kit && kit.slots));
  const a = kit && kit.slots.find((s) => s.slot === SLOT);
  check(a && a.frames > 0 && (a.flags & 4) === 0,
        'and slot 1 resolved its source — a slot flagged source-missing would clamp everything '
        + 'below to nothing and still report success', a && JSON.stringify(a));
}

/*
 * THE TABLE. One row per SamplerSlotField, in wire order, so a field added to the engine and
 * not to this file leaves a hole that the count check below names.
 *
 *   verb  — the token the console takes, which is SLOT_FIELDS[id] in chainmodel.js
 *   json  — the key in the saved project (sampler_serialize.h)
 *   kit   — how to read it out of the SHM read-back, or null when it is not published
 *   set   — the value to write. Every one differs from the fixture's default, which the
 *           "actually moved" check below proves rather than assumes.
 */
const FIELDS = [
  { id: 0,  verb: 'voicegroup', json: 'voice_group',       kit: (s) => s.group,  set: 3 },
  { id: 1,  verb: 'nna',        json: 'nna',               kit: (s) => s.nna,    set: 2 },
  { id: 2,  verb: 'gate',       json: 'gate',              kit: (s) => s.flags & 1, set: 1 },
  { id: 3,  verb: 'reverse',    json: 'reverse',           kit: (s) => (s.flags >> 1) & 1, set: 1 },
  { id: 4,  verb: 'gain',       json: 'gain_millibels',    kit: (s) => s.gainMb, set: -600 },
  { id: 5,  verb: 'pan',        json: 'pan_thousandths',   kit: (s) => s.panTh,  set: -250 },
  { id: 6,  verb: 'tune',       json: 'tune_cents',        kit: null,            set: 700 },
  { id: 7,  verb: 'pitchtrack', json: 'pitch_track_milli', kit: null,            set: 500 },
  { id: 8,  verb: 'root',       json: 'root_key',          kit: (s) => s.root,   set: 48 },
  { id: 9,  verb: 'keylow',     json: 'key_low',           kit: (s) => s.keyLow, set: 30 },
  { id: 10, verb: 'keyhigh',    json: 'key_high',          kit: (s) => s.keyHigh, set: 90 },
  { id: 11, verb: 'vellow',     json: 'vel_low',           kit: (s) => s.velLow, set: 10 },
  { id: 12, verb: 'velhigh',    json: 'vel_high',          kit: (s) => s.velHigh, set: 120 },
  { id: 13, verb: 'selectmode', json: 'select_mode',       kit: null,            set: 2 },
  { id: 14, verb: 'polyphony',  json: 'polyphony',         kit: null,            set: 4 },
  { id: 15, verb: 'chokefade',  json: 'choke_fade_us',     kit: null,            set: 5000 },
  { id: 16, verb: 'modset',     json: 'mod_set_id',        kit: (s) => s.modSet, set: 2 },
  { id: 17, verb: 'stem',       json: 'output_stem',       kit: (s) => s.stem,   set: 2 },
  { id: 18, verb: 'quality',    json: 'quality',           kit: (s) => s.quality, set: 2 },
  { id: 19, verb: 'layergroup', json: 'layer_group',       kit: null,            set: 7 },
  { id: 20, verb: 'loopmode',   json: 'loop_mode',         kit: null,            set: 2 },
  { id: 21, verb: 'sustainloop', json: 'sustain_loop',     kit: null,            set: 1 },
  { id: 22, verb: 'loopstart',  json: 'loop_start_frame',  kit: null,            set: 100 },
  { id: 23, verb: 'loopend',    json: 'loop_end_frame',    kit: null,            set: 900 },
  { id: 24, verb: 'loopxfade',  json: 'loop_xfade_frames', kit: null,            set: 32 },
  { id: 25, verb: 'startframe', json: 'start_frame',       kit: null,            set: 50 },
  { id: 26, verb: 'endframe',   json: 'end_frame',         kit: null,            set: 800 },
  /*
   * `slice` BEFORE `source`, deliberately. A slice id is validated against the slot's CURRENT
   * source, and the markers in this fixture belong to source 1 — so moving the slot to source 2
   * first would make `slice 2` a legitimate refusal and the row would fail for the right reason
   * at the wrong time. Ordering is part of the contract here, not an implementation detail.
   */
  { id: 28, verb: 'slice',      json: 'slice_id',          kit: (s) => s.slice,  set: 2 },
  { id: 27, verb: 'source',     json: 'source_local_id',   kit: (s) => s.source, set: 2 },
];

// ---------------------------------------------------------------------------
// EVERY FIELD IS REACHABLE, AND EVERY FIELD IS COVERED HERE.
// ---------------------------------------------------------------------------
{
  /*
   * The console's own help line for `slot`, which spells the oneOf list literally — so the
   * tokens are read out of the RUNNING APP rather than re-imported from chainmodel.js. A check
   * that imports the same table it is checking proves the table equals itself.
   */
  /*
   * THE REFUSAL SPELLS THE LIST, which is the only way to read the accepted tokens back out of
   * the RUNNING APP.
   *
   * Two wrong routes first, both worth recording. `run` returns `probe().last` — the last SIX
   * transcript lines — so `help` prints ninety lines and returns the tail, and `slot` sorts far
   * enough up the alphabet to be long gone; that version reported every field missing while the
   * help string was perfectly correct. And importing SLOT_FIELDS here would only prove the table
   * equals itself.
   *
   * A bad token is one line, it comes back, and the enum validator writes the whole `oneOf` list
   * into it — so the app tells this file which tokens it takes.
   */
  const names = String(await page.evaluate(
    () => JSON.stringify(window.__uni.run('slot 0 9 1 notafield 1'))));
  const covered = new Set(FIELDS.map((f) => f.verb));
  const missing = FIELDS.filter((f) => !names.includes(f.verb)).map((f) => f.verb);
  check(missing.length === 0, 'every field this file drives is a token the console accepts',
        JSON.stringify(missing));
  check(covered.size === 29,
        'and all twenty-nine slot fields are driven — a field added to the engine without a row '
        + 'here would be untested and look tested', `${covered.size} covered`);
}

// ---------------------------------------------------------------------------
// SET EVERY FIELD, THEN READ IT BACK OUT OF THE SNAPSHOT THE PRODUCER READS.
// ---------------------------------------------------------------------------
const before = await ask();
const slotBefore = before.slots.find((s) => s.slot === SLOT);
const noWitness = [];
const unmoved = [];
{
  for (const f of FIELDS) {
    const said = await run(`slot ${TRACK} ${DEVICE} ${SLOT} ${f.verb} ${f.set}`);
    f.said = String(said);
  }
  await page.waitForTimeout(600);
  const after = await ask();
  const slotAfter = after.slots.find((s) => s.slot === SLOT);

  const wrong = [];
  for (const f of FIELDS) {
    if (!f.kit) { noWitness.push(f.verb); continue; }
    const was = f.kit(slotBefore), now = f.kit(slotAfter);
    // TWO claims per field, and the second is the one that makes the first mean anything: the
    // value is what was asked for, AND it is not what it already was.
    if (now !== f.set) wrong.push(`${f.verb}: wanted ${f.set}, read ${now} (${f.said})`);
    else if (was === f.set) unmoved.push(`${f.verb} was already ${f.set}`);
  }
  check(wrong.length === 0,
        `every published slot field reads back what the console set — ${FIELDS.length - noWitness.length} of them`,
        `${wrong.slice(0, 6).join(' | ')} :: engine said ${rejects('set_slot')}`);
  check(unmoved.length === 0,
        'and none of them was set to the value it already held — a check that cannot move is a '
        + 'check that cannot fail', unmoved.join(' | '));
  console.log(`  ${noWitness.length} fields have no SHM witness and are checked only against the `
              + `saved file: ${noWitness.join(' ')}`);
}

// ---------------------------------------------------------------------------
// ...AND THEY SURVIVE A SAVE. Heard, drawn, and dropped at save is a real shape here.
// ---------------------------------------------------------------------------
{
  const path = `${stack.dir}/slotfix.uniproj.json`;
  const mtimeBefore = existsSync(path) ? statSync(path).mtimeMs : 0;
  const saved = await run('save slotfix');
  await page.waitForTimeout(2500);
  const mtimeAfter = existsSync(path) ? statSync(path).mtimeMs : 0;
  // PROVE THE SAVE HAPPENED before reading anything into the comparison. An unchanged file is
  // a different failure from "the value was not kept", and patchsave.mjs once reported the
  // second when the first had happened — `save` with no argument is refused and writes nothing.
  check(mtimeAfter > mtimeBefore, 'the project is actually written to disk',
        `${mtimeBefore} -> ${mtimeAfter}; ${String(saved).slice(0, 60)}`);

  const doc = JSON.parse(readFileSync(path, 'utf8'));
  const dev = (doc.tracks || []).flatMap((t) => t.device_chain || [])
    .find((d) => d.device_id === DEVICE);
  const slot = dev && dev.sampler && (dev.sampler.slots || []).find((s) => s.id === SLOT);
  check(!!slot, 'the saved file has the slot', JSON.stringify(dev && Object.keys(dev)));
  if (slot) {
    const wrong = FIELDS.filter((f) => Number(slot[f.json]) !== f.set)
      .map((f) => `${f.verb}/${f.json}: wanted ${f.set}, file says ${slot[f.json]}`);
    check(wrong.length === 0,
          `all ${FIELDS.length} fields are in the saved project with the values the console set`,
          wrong.slice(0, 6).join(' | '));
  }
}

// ---------------------------------------------------------------------------
// REFUSED, NOT CLAMPED — the three fields where a wrong value means SILENCE.
//
// modset, source and slice address things BY ID, and an id that does not exist would leave the
// slot pointing at nothing and playing nothing. Backend refuses all three rather than clamping,
// on the stated grounds that silence is not a near-miss of anything a caller asked for. The
// check that matters is not that the command is refused — it is that the slot is UNCHANGED
// afterwards, because a refusal that has already written half of itself is worse than a clamp.
// ---------------------------------------------------------------------------
{
  const kitBefore = await ask();
  const s0 = kitBefore.slots.find((s) => s.slot === SLOT);
  for (const [verb, bad] of [['modset', 999], ['source', 998], ['slice', 997]]) {
    await run(`slot ${TRACK} ${DEVICE} ${SLOT} ${verb} ${bad}`);
  }
  await page.waitForTimeout(600);
  const kitAfter = await ask();
  const s1 = kitAfter.slots.find((s) => s.slot === SLOT);
  check(s1 && s0 && s1.modSet === s0.modSet && s1.source === s0.source && s1.slice === s0.slice,
        'a modset, source or slice id that does not exist leaves the slot exactly as it was',
        `${JSON.stringify({ modSet: s0.modSet, source: s0.source, slice: s0.slice })} -> `
        + `${JSON.stringify({ modSet: s1.modSet, source: s1.source, slice: s1.slice })}`);

  // ...and the CONTROL, which is what makes the line above a finding rather than a tautology: a
  // VALID id of the same kind must still take. Otherwise "unchanged" would also be the answer if
  // the command had stopped working entirely.
  await run(`slot ${TRACK} ${DEVICE} ${SLOT} source 1`);
  await page.waitForTimeout(500);
  const s2 = (await ask()).slots.find((s) => s.slot === SLOT);
  check(s2 && s2.source === 1,
        'while a source id that DOES exist still moves the slot — the refusal is about the id, '
        + 'not about the command being dead', s2 && `source ${s2.source}`);
}

// ---------------------------------------------------------------------------
// CLAMPED, NOT REFUSED, for the range fields — and the clamp lands on the LIMIT.
//
// The engine's own comment: a value out of range is almost always a caller's arithmetic rather
// than an intent, and refusing would leave the kit in a state the caller thinks it changed. So
// the contract is that an absurd value produces the boundary, not the old value and not the
// absurd one. All three are distinguishable and only one is right.
// ---------------------------------------------------------------------------
{
  const CLAMPS = [
    { verb: 'gain', set: 99999, want: 2400, kit: (s) => s.gainMb },
    { verb: 'gain', set: -99999, want: -9600, kit: (s) => s.gainMb },
    { verb: 'pan', set: 5000, want: 1000, kit: (s) => s.panTh },
    { verb: 'root', set: 300, want: 127, kit: (s) => s.root },
    { verb: 'keylow', set: 300, want: 127, kit: (s) => s.keyLow },
    { verb: 'quality', set: 9, want: 2, kit: (s) => s.quality },
    { verb: 'nna', set: 9, want: 2, kit: (s) => s.nna },
  ];
  const wrong = [];
  for (const c of CLAMPS) {
    await run(`slot ${TRACK} ${DEVICE} ${SLOT} ${c.verb} ${c.set}`);
    await page.waitForTimeout(250);
    const s = (await ask()).slots.find((x) => x.slot === SLOT);
    const got = s ? c.kit(s) : null;
    if (got !== c.want) wrong.push(`${c.verb} ${c.set} -> ${got}, expected ${c.want}`);
  }
  check(wrong.length === 0,
        `an out-of-range value lands on the LIMIT, not on the old value and not on itself — `
        + `${CLAMPS.length} bounds`, wrong.join(' | '));
}

// ---------------------------------------------------------------------------
// `slot 0` MATCHES NOTHING, and the help used to say it matched everything.
//
// deviceId 0 and modSetId 0 ARE wildcards on this wire, which is exactly what makes this worth
// pinning: the sentinel is per field, not per protocol. The engine compares `slot.id != slotId`,
// so 0 addresses a slot whose id is 0 — and no kit mints one.
// ---------------------------------------------------------------------------
{
  const kitBefore = await ask();
  const g0 = kitBefore.slots.map((s) => s.group).join(',');
  await run(`slot ${TRACK} ${DEVICE} 0 voicegroup 7`);
  await page.waitForTimeout(600);
  const g1 = (await ask()).slots.map((s) => s.group).join(',');
  check(g0 === g1, 'slot 0 changes nothing — it is not a wildcard for "every slot"',
        `${g0} -> ${g1}`);
  const help = String(await run('help slot'));
  check(!/0 means all|all of them/.test(help),
        'and the help does not claim otherwise', help.slice(0, 120));
}

// ---------------------------------------------------------------------------
// THE BANK'S OWN THREE FIELDS (opcode 88), which are not slot fields and were reachable by
// nothing until backend added the opcode: voice cap, default gate, default view.
// ---------------------------------------------------------------------------
{
  const was = await ask();
  await run(`bank ${TRACK} ${DEVICE} voice-cap 24`);
  await run(`bank ${TRACK} ${DEVICE} default-gate 1`);
  // `default-view` takes the view's INDEX on the wire — 0 kit, 1 sample. The console
  // prints the name back, which is why the reply reads `default-view = sample`.
  await run(`bank ${TRACK} ${DEVICE} default-view 1`);
  await page.waitForTimeout(700);
  const now = await ask();
  check(now.voiceCap === 24 && was.voiceCap !== 24,
        'the voice cap is settable and moved', `${was.voiceCap} -> ${now.voiceCap}`);
  check(now.defaultGate === 1 && was.defaultGate !== 1,
        'the bank default gate is settable and moved',
        `${was.defaultGate} -> ${now.defaultGate}`);
  check(now.defaultView === 1 && was.defaultView !== 1,
        'the remembered view is settable and moved',
        `${was.defaultView} -> ${now.defaultView}`);
  /*
   * AND `default-gate` IS A SEED, NOT A LIVE OVERRIDE. Slot 1's gate was set to 1 far above and
   * the bank default is now also 1, so this cannot tell them apart — it sets the slot back to 0
   * first, and then the bank default must NOT drag it along. A default that reaches back into
   * existing slots would silently rewrite a kit somebody had already voiced.
   */
  await run(`slot ${TRACK} ${DEVICE} ${SLOT} gate 0`);
  await run(`bank ${TRACK} ${DEVICE} default-gate 1`);
  await page.waitForTimeout(600);
  const s = (await ask()).slots.find((x) => x.slot === SLOT);
  check(s && (s.flags & 1) === 0,
        'and it seeds new slots without reaching back into existing ones',
        s && `slot ${SLOT} gate ${s.flags & 1}`);
}

// ---------------------------------------------------------------------------
// A SLOT'S NAME (kShmVersion 36 / opcode 90) — persisted since the sampler shipped, published
// by nothing until now, so it survived save and reload perfectly and was invisible to every
// reader. This repo's most-repeated defect wearing its other face: usually a persisted field
// cannot be WRITTEN, this one could not be READ.
// ---------------------------------------------------------------------------
{
  const before = (await ask()).slots.find((s) => s.slot === SLOT);
  check(before && before.name === '', 'a slot starts with no name', JSON.stringify(before && before.name));

  await run(`slot-name ${TRACK} ${DEVICE} ${SLOT} kick 01`);
  await page.waitForTimeout(700);
  const named = (await ask()).slots.find((s) => s.slot === SLOT);
  check(named && named.name === 'kick 01',
        'naming a slot reaches the engine and reads back — including the space, so the verb '
        + 'takes the rest of the line rather than one word', JSON.stringify(named && named.name));

  /*
   * NEVER TRUNCATED, ALWAYS REFUSED. A name that does not fit is rejected rather than stored
   * short, so what reads back is byte-for-byte what was sent or the write did not happen — a
   * truncated name is the worst of the three outcomes because it LOOKS like it worked.
   *
   * Checked at 60 bytes against a 40-byte array, and the claim is not "it was refused" but "the
   * old name is still there": a refusal that had already written half of itself would leave a
   * name nobody chose.
   */
  const said = await run(`slot-name ${TRACK} ${DEVICE} ${SLOT} ${'x'.repeat(60)}`);
  await page.waitForTimeout(700);
  const kept = (await ask()).slots.find((s) => s.slot === SLOT);
  check(kept && kept.name === 'kick 01',
        'a name too long to fit is refused and the old one is untouched — not shortened',
        `${JSON.stringify(kept && kept.name)} :: ${JSON.stringify(said).slice(-120)}`);

  // An empty name clears it, which is the only way back to an unnamed pad.
  await run(`slot-name ${TRACK} ${DEVICE} ${SLOT}`);
  await page.waitForTimeout(700);
  const cleared = (await ask()).slots.find((s) => s.slot === SLOT);
  check(cleared && cleared.name === '', 'and an empty name clears it',
        JSON.stringify(cleared && cleared.name));
}

// ---------------------------------------------------------------------------
// WHERE A SLICE ACTUALLY IS (kShmVersion 35). The chop played and could not be SEEN: every slot
// reported the SOURCE's length, so nothing could draw where a slice begins or how long it is.
// ---------------------------------------------------------------------------
{
  const k = await ask();
  const s1 = k.slots.find((s) => s.slot === SLOT);
  const s2 = k.slots.find((s) => s.slot === SLOT2);
  check(s1 && typeof s1.begin === 'number' && typeof s1.end === 'number',
        'the read-back carries a slice extent per slot', JSON.stringify(s1 && [s1.begin, s1.end]));
  /*
   * A SLOT WITH NO SLICE GETS THE WHOLE SOURCE — (0, frames), not zeroes. Backend's rule, and
   * worth pinning rather than assuming: "0,0 means the whole thing" would be a sentinel that
   * reads as a bug at the exact moment somebody is looking for one.
   *
   * Slot 2 has no slice in this fixture, so it is the one that can make that claim.
   */
  check(s2 && s2.slice === 0 && s2.begin === 0 && s2.end === s2.frames,
        'and a slot with NO slice reports the whole source, not zeroes',
        JSON.stringify(s2 && { slice: s2.slice, begin: s2.begin, end: s2.end, frames: s2.frames }));
  /*
   * ...AND A SLICED SLOT REPORTS SOMETHING SMALLER. Slot 1 was pointed at slice 2 far above,
   * whose marker sits at frame 1000 of a source thousands of frames long — so its extent must
   * be a PROPER SUBSET of the source. Without this the check above passes on an engine that
   * writes (0, frames) for everything, which is exactly what a stubbed field looks like.
   */
  check(s1 && s1.slice > 0 && s1.begin > 0 && s1.end <= s1.frames && s1.end > s1.begin,
        'while a SLICED slot reports a proper region inside it — the control that says the '
        + 'extent is computed and not stubbed',
        JSON.stringify(s1 && { slice: s1.slice, begin: s1.begin, end: s1.end, frames: s1.frames }));
}

// ---------------------------------------------------------------------------
// VINTAGE (opcode 91) — bit depth and sample-rate reduction on a MOD SET, not a slot, because
// a chopped break wants one character and sixteen copies would be sixteen edits.
//
// TWO WITNESSES, like every other field here: the SAVED FILE says the edit will still be there
// tomorrow, and the KIT READ-BACK says the audio thread agrees. Backend publishes the pair per
// SLOT, resolved from its mod set — riding the entry's two reserved words, so no offset moved
// and kShmVersion did not change. A slot is what a kit grid draws, and "which mod set is this
// pad on" should not be a question the grid has to answer to say whether the pad is crushed.
// ---------------------------------------------------------------------------
{
  const path = `${stack.dir}/slotfix.uniproj.json`;
  const modSets = () => {
    const doc = JSON.parse(readFileSync(path, 'utf8'));
    const dev = (doc.tracks || []).flatMap((t) => t.device_chain || [])
      .find((d) => d.device_id === DEVICE);
    return (dev && dev.sampler && dev.sampler.mod_sets) || [];
  };

  /*
   * ON THE SET THE SLOT IS ACTUALLY ON, read back rather than assumed.
   *
   * I wrote `1` here — the loader points a slot with no `mod_set_id` at the first set, so slot 1
   * starts there. It does not stay: the field sweep far above sets `modset` to 2 as one of its
   * twenty-nine, so by the time this runs slot 1 is on set 2. Setting vintage on set 1 stored it
   * perfectly and the slot saw nothing, which read as the read-back not carrying the field.
   *
   * Asking the kit is both correct and durable — a check that hardcodes a relationship another
   * block is free to change is a check that will break again for a different reason.
   */
  const onSet = (await ask()).slots.find((x) => x.slot === SLOT).modSet;
  await run(`vintage ${TRACK} ${DEVICE} 12 22050 ${onSet}`);
  await page.waitForTimeout(600);
  await run('save slotfix');
  await page.waitForTimeout(2500);
  const one = modSets().find((m) => m.id === onSet);
  check(one && Number(one.bit_depth) === 12 && Number(one.rate_hz) === 22050,
        `vintage sets bit depth and rate on one mod set (${onSet})`,
        JSON.stringify(one && { bits: one.bit_depth, rate: one.rate_hz }));
  /*
   * ...AND THE SLOT ON THAT SET SEES IT. The file proves it was stored; this proves the RUNTIME
   * resolved it onto the slot the producer plays, which is the half a saved file cannot say.
   */
  const vslot = (await ask()).slots.find((x) => x.slot === SLOT);
  check(vslot && vslot.bits === 12 && vslot.rate === 22050,
        'and the read-back carries it per slot, resolved from that slot\'s mod set',
        JSON.stringify(vslot && { bits: vslot.bits, rate: vslot.rate }));

  /*
   * ONE HALF AT A TIME, which is the whole reason the payload carries flags.
   *
   * 0 is a LEGAL value for both — it means off — so "crush the bits and leave the rate" cannot
   * be said with a value, and a caller who filled the other field in for completeness would
   * silently turn it off. This sets only the bits and asserts the RATE SURVIVED, which is the
   * assertion that fails if the flags are ever dropped or defaulted.
   */
  /*
   * NO `modset` ARGUMENT, so it defaults to 0 — every mod set on the sampler. A positional
   * optional cannot be SKIPPED in the middle: `vintage 0 9 8 2` reads the 2 as the RATE, which
   * is exactly what my first version did and it set the rate to 2 Hz while claiming to test that
   * the rate was left alone. The wildcard is the honest way to say "bits only", and it makes
   * this check prove the flags AND the wildcard at once.
   */
  await run(`vintage ${TRACK} ${DEVICE} 8`);
  await page.waitForTimeout(600);
  await run('save slotfix');
  await page.waitForTimeout(2500);
  const two = modSets().find((m) => m.id === onSet);
  check(two && Number(two.bit_depth) === 8 && Number(two.rate_hz) === 22050,
        'naming only `bits` leaves the rate alone — the flags say which half the call is about',
        JSON.stringify(two && { bits: two.bit_depth, rate: two.rate_hz }));

  // ...and mod set 0 IS a wildcard here, unlike a slot id. Set 2 was untouched above.
  await run(`vintage ${TRACK} ${DEVICE} 4 0 0`);
  await page.waitForTimeout(600);
  await run('save slotfix');
  await page.waitForTimeout(2500);
  const all = modSets();
  check(all.length >= 2 && all.every((m) => Number(m.bit_depth) === 4),
        'and mod set 0 reaches every mod set on the sampler — a real wildcard, unlike slot 0',
        JSON.stringify(all.map((m) => [m.id, m.bit_depth])));

  // A call naming NEITHER is refused rather than accepted as a no-op.
  const neither = await run(`vintage ${TRACK} ${DEVICE}`);
  check(/bits, rate, or both/.test(String(neither)),
        'and a call naming neither is refused, not accepted as a command that does nothing',
        JSON.stringify(neither).slice(-90));
}

check(errors.length === 0, 'and nothing threw', errors.slice(0, 3).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
