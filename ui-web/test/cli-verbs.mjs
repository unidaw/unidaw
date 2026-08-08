#!/usr/bin/env node
/**
 * THE SEVEN CLI VERBS THE PARITY LIST COUNTED AS COVERED AND NOTHING HAD EVER RUN.
 *
 * `unit.mjs` checks the registry's CLI column against daw-cli's source by grepping its dispatch
 * for `Some(&"verb")`. That proves an ARM EXISTS. It does not prove the arm works, and it cannot:
 * a verb that parses its flags, builds a payload with a field in the wrong place and sends it is
 * indistinguishable, from the source, from one that works. The engine answers a malformed
 * command by ignoring it, so the failure mode is a well-formed no-op — which is this wire's
 * usual way of failing, and the reason `sent: true` is not evidence anywhere in this repo.
 *
 * Auditing the registry's 63 CLI verbs against every test and tool script found SEVEN that no
 * file in the repository has ever invoked:
 *
 *     macro  move-device  open-editor  patcher-connect  patcher-unnode  remove-device  set-tempo
 *
 * They were green on the parity list the whole time. This file runs them against a live engine
 * and asserts the ANSWER — a field in the saved project, or a count in the published state —
 * rather than the exit code, because daw-cli exits 0 for a command the engine then drops.
 *
 * ── WHY THESE SEVEN ARE WORTH THE ENGINE TIME ────────────────────────────────────────────────
 *
 * They are not obscure. `remove-device` and `move-device` are the device rack; `patcher-connect`
 * and `patcher-unnode` are two of the four patcher edits; `set-tempo` is in the demo runbook as
 * an AI prompt. If any of them had the open-editor defect — a payload built as the wrong 40-byte
 * struct, so the engine reads a different field and refuses every call — nothing here would have
 * noticed. That defect was real, in this repo, on this branch's own name.
 */

import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { startStack } from './stack.mjs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ROOT = resolve(new URL('../..', import.meta.url).pathname);
const Q = 960000;

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8, keepDir: true });

/** daw-cli against THIS stack's segment. Returns {ok, out} — never throws, so one dead verb
 *  does not take the other six with it. */
const cli = (...args) => {
  try {
    return { ok: true, out: execFileSync(join(ROOT, 'ui/target/release/daw-cli'), args,
      { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm, DAW_PROJECT_DIR: stack.dir }, encoding: 'utf8', timeout: 15000 }) };
  } catch (e) {
    return { ok: false, out: String(e.stdout || '') + String(e.stderr || e.message || '') };
  }
};

/*
 * A project with TWO devices on track 0 and an event patcher on track 1.
 *
 * Two devices because `move-device` and `remove-device` both need a chain that can be observed
 * to CHANGE — with one device, a remove that removed the wrong thing and a remove that worked
 * leave the same empty chain, and a move is unobservable. Their ids are 3 and 7, deliberately
 * not 1 and 2: an off-by-one in an index-vs-id mixup lands on a real device when the ids are
 * consecutive, and on nothing when they are not.
 */
const NAME = 'cliverbs';
writeFileSync(`${stack.dir}/${NAME}.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: NAME, created_utc: 0, modified_utc: 0 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [],
  /*
   * TWO CLIPS, PLACED THREE TIMES — the fixture `get shared` needs, and it is built to be able
   * to give a WRONG answer.
   *
   * Clip 1 appears three times (twice on track 0, once on track 1); clip 2 appears once. A test
   * against a project where everything is shared cannot tell "counts appearances" from "returns
   * the number of placements", and one where nothing is shared cannot tell it from a constant 1.
   * Both numbers have to be present for either to mean anything.
   */
  clips: [
    { id: 1, name: 'twice', length: Q * 4, lines_per_beat: 4, kind: 'symbolic',
      time_sig_numerator: 4, time_sig_denominator: 4, chords: [],
      notes: [{ nanotick: 0, duration: Q, pitch: 60, velocity: 100, column: 0, note_id: 1 }] },
    { id: 2, name: 'once', length: Q * 4, lines_per_beat: 4, kind: 'symbolic',
      time_sig_numerator: 4, time_sig_denominator: 4, chords: [],
      notes: [{ nanotick: 0, duration: Q, pitch: 67, velocity: 100, column: 0, note_id: 2 }] },
  ],
  tracks: [
    {
      track_id: 0, name: 'Rack', harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
      device_chain: [
        { device_id: 3, kind: 'sampler', patcher_node_id: 0, bypass: false,
          sampler: { slots: [{ id: 1, name: 'a', key_low: 0, key_high: 127, root_key: 60, gate: 0 }] } },
        { device_id: 7, kind: 'patcher_event', patcher_node_id: 0, bypass: false },
      ],
      mod_links: [],
      placements: [
        { clip_id: 1, at: 0, length: Q * 4, notes: [], chords: [], mutes: [] },
        { clip_id: 1, at: Q * 8, length: Q * 4, notes: [], chords: [], mutes: [] },
        { clip_id: 2, at: Q * 16, length: Q * 4, notes: [], chords: [], mutes: [] },
      ],
    },
    {
      track_id: 1, name: 'Patch', harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
      device_chain: [{ device_id: 1, kind: 'patcher_event', patcher_node_id: 0, bypass: false }],
      mod_links: [],
      placements: [{ clip_id: 1, at: 0, length: Q * 4, notes: [], chords: [], mutes: [] }],
    },
  ],
}, null, 2));

/*
 * THE LOAD IS ASSERTED BY ITS CONTENT, NOT BY THE EXIT CODE — and that distinction cost this
 * file its first run.
 *
 * The fixture originally gave one device `kind: 'gain'`. There is no such DeviceKind, and
 * `loadProject` answers an unknown kind by ABANDONING THE WHOLE PARSE — `setError(...); return
 * false` — so nothing loaded at all. `do load` had already exited 0, because the command was
 * accepted by the ring; the engine's refusal happens later and elsewhere. Every subsequent check
 * then ran against the engine's DEFAULT project and reported that seven CLI verbs were broken.
 *
 * That is exactly the shape this suite exists to catch, arriving first in the suite's own fixture.
 * So the load is confirmed by finding the fixture's own devices, and `cli.ok` is never treated as
 * evidence that anything happened.
 */
const load = cli('do', 'load', NAME);
check(load.ok, 'do load is accepted (this alone proves nothing — see below)', load.out.slice(0, 200));

/** Save and read the project back — the ground truth for every structural assertion here. */
const saved = async (tag) => {
  const s = cli('do', 'save', tag);
  if (!s.ok) return null;
  const p = `${stack.dir}/${tag}.uniproj.json`;
  for (let i = 0; i < 40; i++) {
    if (existsSync(p)) {
      try { return JSON.parse(readFileSync(p, 'utf8')); } catch { /* mid-write */ }
    }
    await sleep(150);
  }
  return null;
};
const chainOf = (doc, track) =>
  (doc?.tracks?.find((t) => t.track_id === track)?.device_chain ?? []).map((d) => d.device_id);

const ENGINE_LOG = join(stack.root, 'engine.log');

let before = null;
for (let i = 0; i < 30; i++) {
  before = await saved(`${NAME}_a${i}`);
  if (JSON.stringify(chainOf(before, 0)) === '[3,7]') break;
  await sleep(200);
}
check(JSON.stringify(chainOf(before, 0)) === '[3,7]',
      'THE FIXTURE ACTUALLY LOADED: devices [3,7] are on track 0',
      `${JSON.stringify(chainOf(before, 0))} — an unknown device kind makes loadProject abandon `
      + 'the whole parse while `do load` still exits 0');

/* ── set-tempo ──────────────────────────────────────────────────────────────────────────────
 * POSITIONAL, not flagged: `do set-tempo <bpm> [position]`, and with no position it FLATTENS
 * (flags=1) rather than adding a point. Both halves are asserted, because a flatten that
 * appended instead would leave 120 in the map and still read as "the tempo changed".
 */
const tempo = cli('do', 'set-tempo', '96');
check(tempo.ok, 'do set-tempo runs', tempo.out.slice(0, 160));
const afterTempo = await saved(`${NAME}_b`);
const map = afterTempo?.tempo_map ?? [];
check(map.length === 1 && Math.abs(map[0]?.bpm - 96) < 0.01,
      'set-tempo with no position FLATTENS the map to one point at 96',
      JSON.stringify(map));

/* ── move-device ────────────────────────────────────────────────────────────────────────────
 * The rack's reorder. Asserted as the ORDER OF IDS, not a count: a move that dropped the device
 * and re-added it would keep the count and lose the identity, and a move that did nothing keeps
 * both — only the sequence tells the three apart.
 */
const move = cli('do', 'move-device', '--track', '0', '--device', '7', '--index', '0');
check(move.ok, 'do move-device runs', move.out.slice(0, 160));
const afterMove = await saved(`${NAME}_c`);
check(JSON.stringify(chainOf(afterMove, 0)) === '[7,3]',
      'move-device puts device 7 first', JSON.stringify(chainOf(afterMove, 0)));

/* ── macro ──────────────────────────────────────────────────────────────────────────────────
 * A mod SOURCE value. This one cannot be read back from the project file — a macro's live value
 * is runtime state, not a persisted field — so the assertion is that the command is ACCEPTED and
 * the engine does not name it in a rejection. That is weaker than the others and is labelled as
 * such rather than dressed up: what it rules out is the open-editor defect (a payload the engine
 * refuses outright), not a wrong value.
 */
const macro = cli('do', 'macro', '--track', '0', '--device', '7', '--source-id', '0',
                  '--value', '0.75');
check(macro.ok && /"sent"/.test(macro.out), 'do macro is accepted by the engine',
      macro.out.slice(0, 160));

/* ── patcher-unnode and patcher-connect ─────────────────────────────────────────────────────
 * Two of the four patcher edits. `--device` matters: without it the command takes the legacy
 * whole-pool path, which for a project with per-device graphs edits a graph that is never saved.
 * So both are addressed at device 1 on track 1, and the assertion reads the DEVICE's graph.
 */
/*
 * A PAIR THAT CAN LEGALLY CONNECT. Two euclideans cannot: a euclidean emits gates and has no
 * event INPUT, so the engine answers `patcher_device_edit.rejected ... reason=invalid_port` and
 * the graph saves with two nodes and no edge — which reads exactly like the verb being broken.
 * euclidean -> event-out is the shape the app itself builds.
 */
const addA = cli('do', 'patcher-node', '--track', '1', '--device', '1', '--type', 'euclidean');
const addB = cli('do', 'patcher-node', '--track', '1', '--device', '1', '--type', 'event-out');
check(addA.ok && addB.ok, 'two patcher nodes are added to work on', (addA.out + addB.out).slice(0, 160));

const nodesOf = (doc) => {
  const dev = doc?.tracks?.find((t) => t.track_id === 1)?.device_chain?.[0];
  return dev?.patcher?.nodes ?? dev?.graph?.nodes ?? [];
};
const withNodes = await saved(`${NAME}_d`);
const nodeCount = nodesOf(withNodes).length;
check(nodeCount >= 2, 'the device graph holds the two new nodes', String(nodeCount));

// The node ids the ENGINE assigned, not 0 and 1 — the same lesson as the clip ids: what a
// fixture or a caller names is not what the runtime holds.
const ids = nodesOf(withNodes).map((n) => n.id);
const conn = cli('do', 'patcher-connect', '--track', '1', '--device', '1',
                 '--src', String(ids[0]), '--dst', String(ids[1]), '--kind', 'event');
check(conn.ok, 'do patcher-connect runs', conn.out.slice(0, 160));
const afterConn = await saved(`${NAME}_e`);
// `edges`, not `connections` — project_file.cpp writes `writer.beginArray("edges")`. The first
// version of this guessed three key names and not that one, so it read [] off a graph that had
// the edge in it and reported patcher-connect broken. Guessing at a schema is how a test comes to
// disagree with a working feature.
const edgesOf = (doc) => {
  const dev = doc?.tracks?.find((t) => t.track_id === 1)?.device_chain?.[0];
  return dev?.patcher?.edges ?? dev?.graph?.edges ?? [];
};
check(edgesOf(afterConn).length >= 1,
      'patcher-connect leaves a connection in the SAVED device graph',
      JSON.stringify(edgesOf(afterConn)).slice(0, 120)
      + ' :: ' + ((readFileSync(ENGINE_LOG, 'utf8')
                    .match(/[^\n]*patcher_device_edit.rejected[^\n]*/g) || []).slice(-1).join('')));

const unnode = cli('do', 'patcher-unnode', '--track', '1', '--device', '1',
                   '--node', String(ids[1]));
check(unnode.ok, 'do patcher-unnode runs', unnode.out.slice(0, 160));
const afterUnnode = await saved(`${NAME}_f`);
check(nodesOf(afterUnnode).length === nodeCount - 1,
      'patcher-unnode removes exactly one node',
      `${nodeCount} -> ${nodesOf(afterUnnode).length}`);

/* ── open-editor ────────────────────────────────────────────────────────────────────────────
 * THE ONE WITH A KNOWN HISTORY, and the one whose positive case cannot be asserted here.
 *
 * The bug this branch is named for: it was built as a UiChainCommandPayload where the engine reads
 * a UiCommandPayload. Both are 40 bytes, so the size check passed and the engine read `deviceKind`
 * (offset 16) as the device id — 0 for every call, every device, every track.
 *
 * `handleOpenPluginEditor` resolves a host index by walking the chain for VstInstrument and
 * VstEffect devices ONLY. A sampler has no plugin editor, so it is refused, correctly — this
 * suite's fixture has no hosted plugin and cannot have one that is guaranteed present on any
 * machine. So the positive case is out of reach and is NOT faked with a weaker assertion.
 *
 * What IS asserted is that the command REACHES the engine and is decoded: the refusal names the
 * device id that was sent. Under the original bug the id was always 0 no matter what was asked
 * for, so "asked for 4242, refused 4242" is precisely the evidence that the payload arrives
 * intact — the one thing the wire bug destroyed, testable without a plugin.
 */
const badEditor = cli('do', 'open-editor', '--track', '0', '--device', '4242');
check(badEditor.ok, 'do open-editor runs', badEditor.out.slice(0, 160));
await sleep(1200);
const log = existsSync(ENGINE_LOG) ? readFileSync(ENGINE_LOG, 'utf8') : '';
check(/OpenPluginEditor failed - device 4242/.test(log),
      'open-editor DELIVERS THE DEVICE ID IT WAS GIVEN — the wire bug always sent 0',
      (log.match(/OpenPluginEditor failed[^\n]*/) || ['no refusal line at all'])[0]);
// And the id is read from the right offset in both directions: a different id must be echoed
// differently, or the check above would pass on any constant.
const badEditor2 = cli('do', 'open-editor', '--track', '0', '--device', '777');
check(badEditor2.ok, 'open-editor runs a second time', badEditor2.out.slice(0, 120));
await sleep(1200);
const log2 = existsSync(ENGINE_LOG) ? readFileSync(ENGINE_LOG, 'utf8') : '';
check(/OpenPluginEditor failed - device 777/.test(log2),
      'CONTROL: a different device id is echoed differently, so the check above is not constant',
      (log2.match(/OpenPluginEditor failed - device \d+/g) || []).join(' | '));

/* ── get shared ─────────────────────────────────────────────────────────────────────────────
 * "IF I EDIT HERE, WHAT ELSE CHANGES?" — the question shared clips make possible and surprising.
 *
 * The agent has had `shared_clips` since the scratch-placement work; only the CLI lacked it, so
 * this was a plain missing arm rather than a design question. It is asserted here the way two
 * surfaces answering ONE question have to be: against the rule, not against a number I typed.
 *
 * The fixture places clip 1 three times and clip 2 once, and the checks below are chosen so that
 * each rules out a specific wrong implementation:
 *
 *   self-consistency  — kills "appearances = the number of rows returned"
 *   3 vs 1            — kills "appearances = 1" and "appearances = total placements"
 *   the --track filter — kills the one that actually matters, below
 */
{
  const all = cli('get', 'shared');
  check(all.ok, 'do `get shared` runs', all.out.slice(0, 200));
  let rows = null;
  try { rows = JSON.parse(all.out); } catch { /* reported by the next check */ }
  check(Array.isArray(rows) && rows.length === 4,
        '`get shared` answers PARSEABLE JSON, one row per placement',
        `got ${JSON.stringify(all.out.slice(0, 200))}`);

  if (Array.isArray(rows) && rows.length) {
    // The definition, checked against the list itself rather than a number I chose.
    const seen = new Map();
    for (const r of rows) seen.set(r.clip, (seen.get(r.clip) || 0) + 1);
    const disagree = rows.filter((r) => r.appearances !== seen.get(r.clip));
    check(disagree.length === 0,
          'APPEARANCES agrees with the placements actually sharing each clip',
          JSON.stringify(disagree.slice(0, 3)));

    const c1 = rows.filter((r) => r.clip === 1);
    const c2 = rows.filter((r) => r.clip === 2);
    check(c1.length === 3 && c1.every((r) => r.appearances === 3),
          'the clip placed three times reports THREE appearances', JSON.stringify(c1));
    check(c2.length === 1 && c2[0].appearances === 1,
          'and the clip placed once reports ONE — a constant would pass the check above',
          JSON.stringify(c2));
  }

  /*
   * THE FILTER NARROWS THE ROWS, NOT THE COUNT — and this is the check the verb was written for.
   *
   * Clip 1 has three appearances: two on track 0 and one on track 1. Asking about track 1 must
   * still say THREE, because a clip is shared whether or not its other appearances are on the
   * track you asked about. An implementation that counts the filtered set answers 1 — and 1 means
   * "safe to edit, nothing else changes", which is the exact opposite of the truth.
   *
   * The agent's `shared_clips` counts over all extents. If these two ever disagree, one of them is
   * telling somebody it is safe to edit a clip that is not.
   */
  const one = cli('get', 'shared', '--track', '1');
  check(one.ok, '`get shared --track 1` runs', one.out.slice(0, 200));
  let only = null;
  try { only = JSON.parse(one.out); } catch { /* reported below */ }
  check(Array.isArray(only) && only.length === 1 && only[0].track === 1,
        '--track 1 returns only track 1\'s placement', JSON.stringify(only));
  check(Array.isArray(only) && only.length === 1 && only[0].appearances === 3,
        'AND IT STILL REPORTS THREE APPEARANCES — the filter cannot change what "shared" means',
        `${JSON.stringify(only)} — an answer of 1 here reads as "nothing else changes", which is `
        + 'false: two more placements of this clip sit on track 0');
}

/* ── remove-device ──────────────────────────────────────────────────────────────────────────
 * Last, because it destroys the fixture. By ID, and the surviving id is asserted rather than the
 * count: removing the wrong device leaves a chain of one either way.
 */
const rm = cli('do', 'remove-device', '--track', '0', '--device', '3');
check(rm.ok, 'do remove-device runs', rm.out.slice(0, 160));
const afterRm = await saved(`${NAME}_g`);
check(JSON.stringify(chainOf(afterRm, 0)) === '[7]',
      'remove-device removes device 3 and leaves 7', JSON.stringify(chainOf(afterRm, 0)));

/* ── do transpose ───────────────────────────────────────────────────────────────────────────
 * A RANGE, not a selection. Same arithmetic as the agent's tool — both call plan_transpose.
 *
 * THIS FIXTURE IS THE HARD CASE, by accident: clip 1 is placed twice on track 0 AND once on track
 * 1, because `get shared` needed something to count. Two separate things happen here and the test
 * has to tell them apart.
 *
 *  1. A flattened track repeats a shared clip's notes once per appearance, so a naive transpose
 *     writes the same clip cell twice. `dedupe_by_clip_cell` folds them — reported as
 *     `shared_appearances_folded`.
 *  2. THE ENGINE FORKS a clip whose edit would reach ANOTHER TRACK. Measured: track 0's placements
 *     come back pointing at new clips while track 1 keeps the original, untouched.
 *
 * I first asserted "the number of clip notes does not change" and it failed at 2 -> 3. That was
 * the FORK, which is correct and desirable, not the repeated write. The count is the wrong
 * observable here; what matters is that track 0 moved and TRACK 1 DID NOT.
 */
{
  // Notes as each TRACK hears them: follow its placements to the clips they name.
  const heard = (doc, track) => {
    if (!doc) return null;
    const clips = new Map((doc.clips || []).map((c) => [c.id, c]));
    const t = (doc.tracks || []).find((x) => x.track_id === track);
    if (!t) return null;
    return (t.placements || [])
      .flatMap((pl) => ((clips.get(pl.clip_id) || {}).notes || []).map((n) => n.pitch))
      .sort((a, b) => a - b);
  };

  const before = await saved(`${NAME}_t0`);
  const t0before = heard(before, 0);
  const t1before = heard(before, 1);
  check(JSON.stringify(t0before) === '[60,60,67]',
        'track 0 hears the shared clip twice plus its own', JSON.stringify(t0before));
  check(JSON.stringify(t1before) === '[60]',
        'and track 1 hears the SAME shared clip', JSON.stringify(t1before));

  const up = cli('do', 'transpose', '--track', '0', '--semitones', '12');
  check(up.ok, 'do transpose runs on a track with a SHARED clip', up.out.slice(0, 220));
  let rep = null;
  try { rep = JSON.parse(up.out); } catch { /* reported next */ }
  check(rep && rep.transposed === 2,
        'it writes TWO edits, not three — the repeated appearance is folded',
        up.out.slice(0, 220));
  check(rep && rep.shared_appearances_folded === 1,
        'AND SAYS SO, because "transposed 2 of your 3 notes" is alarming until you know the '
        + 'third was the same music seen again',
        up.out.slice(0, 220));

  await sleep(1400);
  const after = await saved(`${NAME}_t1`);
  check(JSON.stringify(heard(after, 0)) === '[72,72,79]',
        'EVERY NOTE TRACK 0 HEARS IS AN OCTAVE UP, both appearances of the shared clip included',
        JSON.stringify(heard(after, 0)));
  check(JSON.stringify(heard(after, 1)) === '[60]',
        'AND TRACK 1 IS UNTOUCHED — the engine forks a clip whose edit would reach another '
        + 'track, so transposing one track cannot silently retune another',
        `${JSON.stringify(t1before)} -> ${JSON.stringify(heard(after, 1))}`);

  // The argument refusals, which do not depend on the fixture's shape.
  const zero = cli('do', 'transpose', '--track', '0', '--semitones', '0');
  check(!zero.ok, '0 semitones is REFUSED as a non-edit', zero.out.slice(0, 160));
  const backwards = cli('do', 'transpose', '--track', '0', '--semitones', '1',
                        '--from', '9600000', '--to', '9600000');
  check(!backwards.ok, 'an empty range is REFUSED, not reported as 0 transposed',
        backwards.out.slice(0, 160));
}

/* ── do copy / cut / paste ──────────────────────────────────────────────────────────────────
 * The last rows on the parity lists, and the obstacle was never the ops: a clipboard is STATE and
 * daw-cli exits between the copy and the paste. It lives in a file beside the projects now, which
 * also means this surface and the agent share one — copy with either, paste with the other.
 *
 * ASSERTED ON THE SAVED PROJECT, and on the PITCHES rather than the count. A paste that lands the
 * wrong notes in the right number is exactly what dropping a field looks like.
 */
{
  const pitchesAt = (doc, lo, hi) => {
    if (!doc) return null;
    const clips = new Map((doc.clips || []).map((c) => [c.id, c]));
    const t = (doc.tracks || []).find((x) => x.track_id === 0);
    if (!t) return null;
    const out = [];
    for (const pl of (t.placements || [])) {
      for (const n of ((clips.get(pl.clip_id) || {}).notes || [])) {
        const tick = pl.at + n.nanotick;
        if (tick >= lo && tick < hi) out.push(n.pitch);
      }
    }
    return out.sort((a, b) => a - b);
  };

  const before = await saved(`${NAME}_c0`);
  const src = pitchesAt(before, 0, Q * 4);
  check(src && src.length > 0, 'there is a phrase to copy', JSON.stringify(src));

  const cp = cli('do', 'copy', '--track', '0', '--from', '0', '--to', String(Q * 4));
  check(cp.ok, 'do copy runs', cp.out.slice(0, 200));
  let rep = null;
  try { rep = JSON.parse(cp.out); } catch { /* next check reports it */ }
  check(rep && rep.copied === src.length,
        'and copies every note in the range', `${cp.out.slice(0, 160)} vs ${JSON.stringify(src)}`);
  check(rep && typeof rep.clipboard === 'string' && rep.clipboard.length > 0,
        'and says WHERE the clipboard is — a file, so it survives this process exiting',
        cp.out.slice(0, 200));

  const ps = cli('do', 'paste', '--track', '0', '--at', String(Q * 40));
  check(ps.ok, 'do paste runs', ps.out.slice(0, 200));
  await sleep(1400);
  const after = await saved(`${NAME}_c1`);
  const landed = pitchesAt(after, Q * 40, Q * 48);
  check(JSON.stringify(landed) === JSON.stringify(src),
        'AND THE SAME PITCHES LAND WHERE IT WAS PASTED',
        `${JSON.stringify(landed)} expected ${JSON.stringify(src)} — asserting the COUNT would `
        + 'pass on a paste that put the wrong notes there in the right number');
  check(JSON.stringify(pitchesAt(after, 0, Q * 4)) === JSON.stringify(src),
        'and the ORIGINAL is still where it was — copy is not cut',
        JSON.stringify(pitchesAt(after, 0, Q * 4)));

  /*
   * CUT, which is copy with the delete. Driven here because the parity ratchet noticed the row was
   * CLAIMED and never run — the exact failure the owner named: an unexercised arm turns the list
   * green while proving nothing.
   *
   * Asserted on both halves. A cut that removes the notes and loses them is a delete; a cut that
   * keeps them and removes nothing is a copy.
   */
  {
    const had = pitchesAt(await saved(`${NAME}_c2`), Q * 40, Q * 48);
    check(had.length > 0, 'the pasted phrase is there to cut', JSON.stringify(had));

    const ct = cli('do', 'cut', '--track', '0', '--from', String(Q * 40), '--to', String(Q * 48));
    check(ct.ok, 'do cut runs', ct.out.slice(0, 200));
    let cr = null;
    try { cr = JSON.parse(ct.out); } catch { /* next check reports it */ }
    check(cr && cr.deleted === had.length,
          'and it DELETES what it took', `${ct.out.slice(0, 160)} vs ${JSON.stringify(had)}`);

    await sleep(1400);
    const gone = pitchesAt(await saved(`${NAME}_c3`), Q * 40, Q * 48);
    check(gone.length === 0, 'the range really is empty afterwards', JSON.stringify(gone));

    // AND THE CLIPBOARD SURVIVED IT — a cut that loses what it cut is just a delete.
    const back = cli('do', 'paste', '--track', '0', '--at', String(Q * 60));
    check(back.ok, 'the cut phrase pastes back', back.out.slice(0, 160));
    await sleep(1400);
    const restored = pitchesAt(await saved(`${NAME}_c4`), Q * 60, Q * 68);
    check(JSON.stringify(restored) === JSON.stringify(had),
          'AND IT IS THE SAME PHRASE — cut keeps what it took',
          `${JSON.stringify(restored)} expected ${JSON.stringify(had)}`);
  }

  // An empty clipboard is a refusal. Emptied by pointing at a directory with no clipboard file.
  const nowhere = (() => {
    try {
      return { ok: true, out: execFileSync(join(ROOT, 'ui/target/release/daw-cli'),
        ['do', 'paste', '--track', '0', '--at', '0'],
        { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm,
                 DAW_PROJECT_DIR: `${stack.dir}/no_clipboard_here` },
          encoding: 'utf8', timeout: 15000 }) };
    } catch (e) { return { ok: false, out: String(e.stdout || '') + String(e.stderr || '') }; }
  })();
  check(!nowhere.ok, 'pasting with NOTHING COPIED is refused, not reported as 0 pasted',
        nowhere.out.slice(0, 160));
}

/* ── do new ─────────────────────────────────────────────────────────────────────────────────
 * LAST, because it REPLACES the loaded document — everything above needs the fixture.
 *
 * `new` was reachable only from the browser: it is a sidecar websocket message, because the
 * browser cannot write files. daw-cli can, so the limitation was inherited rather than required.
 *
 * Three assertions, and the third is the one that matters. A `new` that writes the file is easy;
 * a `new` that refuses to overwrite one is the behaviour that exists because this project lost a
 * song to a `new` that was too willing — and a check for "the file appeared" passes on a build
 * that clobbers.
 */
{
  const fresh = `${NAME}_new`;
  const made = cli('do', 'new', fresh);
  check(made.ok, 'do new runs', made.out.slice(0, 200));
  const path = `${stack.dir}/${fresh}.uniproj.json`;
  check(existsSync(path), 'and WRITES the project file', path);

  let doc = null;
  try { doc = JSON.parse(readFileSync(path, 'utf8')); } catch { /* reported next */ }
  check(doc && Array.isArray(doc.tracks) && doc.tracks.length === 1,
        'the new document is empty but usable — exactly one track',
        JSON.stringify(doc && doc.tracks));
  check(doc && (doc.clips || []).length === 0, 'and no clips', JSON.stringify(doc && doc.clips));

  // IT WAS ACTUALLY LOADED, not merely written. `new` sends LoadProject after the write, and a
  // file on disk the engine never opened is this verb's most likely failure — the writer and the
  // reader would simply disagree about the directory, silently.
  await sleep(1200);
  const back = await saved(`${fresh}_rt`);
  // ORDINARY TRACKS, not `tracks.length`. A save always emits the MASTER track alongside them —
  // id 4294901760 — so the document on disk has two where the one `new` wrote has one. The first
  // version of this asserted the raw count and failed on a build that was working perfectly: it
  // was asserting a fact about the save format, and the fact was wrong.
  const ordinary = (back && back.tracks || []).filter((t) => !t.is_master);
  check(ordinary.length === 1,
        'AND THE ENGINE IS IN IT — saving now writes one empty track, not the fixture',
        JSON.stringify(back && (back.tracks || []).map((t) => [t.track_id, !!t.is_master])));
  check(back && chainOf(back, 0).length === 0,
        'the fixture\'s devices are gone, which is what "new" means',
        JSON.stringify(back && chainOf(back, 0)));

  // THE REFUSAL. Same name, twice.
  const again = cli('do', 'new', fresh);
  check(!again.ok, 'a SECOND `new` with the same name is REFUSED, not silently obeyed',
        `exit ok=${again.ok}, out=${again.out.slice(0, 160)} — overwriting a song is not a thing `
        + 'to do as a side effect of the shortcut for "start something"');
  check(/already exists/i.test(again.out),
        'and it says why', again.out.slice(0, 200));
}

/*
 * ── A BATCH OF VERBS THAT HAD NEVER BEEN RUN ────────────────────────────────────────────────
 *
 * unit.mjs pins CLI_NEVER_EXERCISED: of 69 verbs the registry claims, 37 were not so much as
 * MENTIONED by any suite that invokes daw-cli. The parity check greps daw-cli's SOURCE, so those
 * rows were green on the strength of an arm existing, which is not the same as an arm working —
 * the agent side learned that when `patcher_node` sent every link to port 0 and reported success.
 *
 * These three are the first batch off that list. Each is asserted from the SAVED DOCUMENT rather
 * than from the exit code, because daw-cli exits 0 for a command the engine then drops — this
 * file's own header says so, and it is the reason a green row proved nothing in the first place.
 */
{
  /*
   * RELOAD THE FIXTURE FIRST. This batch is appended to a suite whose sixty preceding checks have
   * already removed devices, moved them and edited clips — so "the fixture has a sampler at device
   * 3" is true of the file and false of the engine by the time we get here. The first version of
   * this block asserted against the original fixture and read `bypass=undefined` from a chain that
   * no longer had any devices in it, which looked like set-bypass failing.
   *
   * A batch that depends on state has to establish it.
   */
  const reload = cli('do', 'load', NAME);
  check(reload.ok, 'the fixture reloads, so this batch starts from known state',
        reload.out.slice(0, 120));
  await sleep(1200);

  const before = await saved('cliv_batch_before');
  const tracksBefore = (before?.tracks ?? []).length;
  check(tracksBefore >= 2, 'two or more tracks to work with', String(tracksBefore));

  // ── time-sig, song-scoped ────────────────────────────────────────────────────────────────
  const ts = cli('do', 'time-sig', '--sig', '7/8');
  check(ts.ok, 'do time-sig is accepted', ts.out.slice(0, 120));
  const afterTs = await saved('cliv_batch_ts');
  /*
   * `timebase.time_sig_*`, which is where project_file.cpp writes the SONG signature (its
   * beginChildObject("timebase") at line 553). My first version guessed the clip's own pair and
   * read undefined — a wrong PATH reporting as a product failure, which is the shape this whole
   * batch exists to avoid. A clip carries its own time_sig_* for a clip-level override; the song
   * default is not there.
   */
  const num = afterTs?.timebase?.time_sig_numerator;
  const den = afterTs?.timebase?.time_sig_denominator;
  check(num === 7 && den === 8,
        'TIME-SIG REACHES THE DOCUMENT — 7/8 is stored, not just accepted',
        `numerator=${num} denominator=${den}`);

  // ── lines-per-beat, per track ────────────────────────────────────────────────────────────
  const lpb = cli('do', 'lines-per-beat', '--track', '0', '--lines', '8');
  check(lpb.ok, 'do lines-per-beat is accepted', lpb.out.slice(0, 120));
  const afterLpb = await saved('cliv_batch_lpb');
  const lines = afterLpb?.tracks?.find((t) => t.track_id === 0)?.lines_per_beat;
  check(lines === 8,
        'LINES-PER-BEAT REACHES THE TRACK — 8 is stored',
        `track 0 lines_per_beat=${lines}`);

  // ── harmony-quantize, a per-track flag ───────────────────────────────────────────────────
  const hq = cli('do', 'harmony-quantize', '--track', '0', '--on', '1');
  check(hq.ok, 'do harmony-quantize is accepted', hq.out.slice(0, 120));
  const afterHq = await saved('cliv_batch_hq');
  const hqOn = afterHq?.tracks?.find((t) => t.track_id === 0)?.harmony_quantize;
  check(hqOn === true || hqOn === 1,
        'HARMONY-QUANTIZE REACHES THE TRACK — the flag is stored on',
        `track 0 harmony_quantize=${JSON.stringify(hqOn)}`);

  // ── clip-name ────────────────────────────────────────────────────────────────────────────
  //
  // --clip is REQUIRED by the verb (its own comment calls an absent one a silent-wrong-target
  // trap), so the id comes from the document rather than being assumed to be 1.
  const clipId = afterHq?.clips?.[0]?.id;
  check(clipId !== undefined, 'a clip to rename', String(clipId));
  const cn = cli('do', 'clip-name', '--track', '0', '--clip', String(clipId), '--name', 'CLIVERB');
  check(cn.ok, 'do clip-name is accepted', cn.out.slice(0, 120));
  const afterCn = await saved('cliv_batch_cn');
  const nameNow = afterCn?.clips?.find((c) => c.id === clipId)?.name;
  check(nameNow === 'CLIVERB',
        'CLIP-NAME REACHES THE CLIP — the name is stored',
        `clip ${clipId} name=${JSON.stringify(nameNow)}`);

  // ── set-bypass, on a device this fixture actually has ────────────────────────────────────
  //
  // Device 3 is the sampler the fixture puts on track 0. Read the flag back per DEVICE, because
  // a bypass written to the wrong insert leaves the same number of devices bypassed as the right
  // one — the same trap remove-track has below.
  const sb = cli('do', 'set-bypass', '--track', '0', '--device', '3', '--bypass', '1');
  check(sb.ok, 'do set-bypass is accepted', sb.out.slice(0, 120));
  const afterSb = await saved('cliv_batch_sb');
  const dev3 = (afterSb?.tracks?.find((t) => t.track_id === 0)?.device_chain ?? [])
    .find((d) => d.device_id === 3);
  const dev7 = (afterSb?.tracks?.find((t) => t.track_id === 0)?.device_chain ?? [])
    .find((d) => d.device_id === 7);
  check(dev3?.bypass === true,
        'SET-BYPASS REACHES DEVICE 3 — bypassed in the document',
        `device 3 bypass=${JSON.stringify(dev3?.bypass)}`);
  /*
   * EXPLICITLY FALSE, not merely "not true". `dev7?.bypass !== true` passes when the device is
   * ABSENT and the field undefined — which is exactly what happened before the reload above, so
   * this check reported the flag going to the right insert while there were no inserts at all.
   */
  check(dev7 !== undefined && dev7.bypass === false,
        'and device 7 beside it is untouched — the flag went to the named insert',
        `device 7 = ${JSON.stringify(dev7)}`);

  // ── note-overlap, a per-track flag ───────────────────────────────────────────────────────
  //
  // This is the flag the tracker's OFF row depends on: with it on, a note is allowed to run past
  // the next one instead of being truncated at edit time. Stored as `allow_note_overlap`
  // (project_file.cpp:677), not under the verb's own name.
  const no = cli('do', 'note-overlap', '--track', '0', '--on', '1');
  check(no.ok, 'do note-overlap is accepted', no.out.slice(0, 120));
  const afterNo = await saved('cliv_batch_no');
  const overlap = afterNo?.tracks?.find((t) => t.track_id === 0)?.allow_note_overlap;
  check(overlap === true || overlap === 1,
        'NOTE-OVERLAP REACHES THE TRACK — allow_note_overlap is stored on',
        `track 0 allow_note_overlap=${JSON.stringify(overlap)}`);

  // ── marker add, which is song-scoped ─────────────────────────────────────────────────────
  //
  // `--nanotick` is REQUIRED by the verb — its own comment says defaulting it would "silently put
  // the marker at tick 0, which looks like a no-op and is not" — so the tick is passed and then
  // asserted, not just the count.
  const markersBefore = (afterNo?.markers ?? []).length;
  const mk = cli('do', 'marker', 'add', '--nanotick', '1920000', '--name', 'CLIVMARK');
  check(mk.ok, 'do marker add is accepted', mk.out.slice(0, 120));
  const afterMk = await saved('cliv_batch_mk');
  const mine = (afterMk?.markers ?? []).find((m) => m.name === 'CLIVMARK');
  check(mine !== undefined,
        'MARKER ADD REACHES THE DOCUMENT — by name, not just by count',
        `${markersBefore} -> ${JSON.stringify((afterMk?.markers ?? []).map((m) => m.name))}`);
  check(mine?.nanotick === 1920000,
        'and at the tick it was given, not at 0',
        `nanotick=${JSON.stringify(mine?.nanotick)}`);

  // ── add-device, appended to a chain we can count before and after ────────────────────────
  //
  // `vst_effect` and not `sampler`: track 0's fixture already has a sampler at device 3, and a
  // track takes ONE head-of-chain instrument, so the engine refuses the second one
  // (chain.rejected, reason add_failed). daw-cli exits 0 on that refusal and says nothing — which
  // is a real gap, but it is the CLI's silent-refusal shape and not add-device's, so it is filed
  // rather than asserted here. Effects stack, so this asserts the add on a kind that can be added.
  const chainBefore = (afterMk?.tracks?.find((t) => t.track_id === 0)?.device_chain ?? [])
    .map((d) => d.device_id);
  const ad = cli('do', 'add-device', '--track', '0', '--kind', 'vst_effect');
  check(ad.ok, 'do add-device is accepted', ad.out.slice(0, 120));
  // "applied" and not "unknown": the engine's ChainSnapshot is the POSITIVE signal, and without it
  // every chain command would sit out the full 750ms refusal window before reporting success —
  // which is not merely slow, it changes the timing of anything driving several edits in sequence.
  check(/"applied"/.test(ad.out) && !/unknown/.test(ad.out),
        'and it reports APPLIED on the engine\'s own acknowledgement, not by timing out',
        `${JSON.stringify(ad.out.trim())} — "unknown" here means the positive signal never arrived`);
  const afterAd = await saved('cliv_batch_ad');
  const chainAfter = (afterAd?.tracks?.find((t) => t.track_id === 0)?.device_chain ?? [])
    .map((d) => d.device_id);
  check(chainAfter.length === chainBefore.length + 1,
        'ADD-DEVICE REACHES THE CHAIN — one more device on track 0',
        `${JSON.stringify(chainBefore)} -> ${JSON.stringify(chainAfter)}`);
  check(chainBefore.every((id) => chainAfter.includes(id)),
        'and the devices already there are still there',
        `${JSON.stringify(chainBefore)} -> ${JSON.stringify(chainAfter)}`);

  // ── AND THE REFUSAL THE LEGAL ADD ABOVE IS THE CONTROL FOR ──────────────────────────────────
  //
  // Track 0 already has a sampler (device 3), and a track takes one head-of-chain instrument, so
  // this add is refused. The engine has always said so — chain.rejected in the log, a rejected:
  // line in history.jsonl, a ChainError on the ring for the UI — and daw-cli alone printed
  // {"sent": "add-device"} and exited 0. That is what this asserts: the ANSWER, not the send.
  //
  // The legal add directly above is the negative control and it is not optional. A tool that
  // reported a refusal for everything would satisfy every assertion here on its own.
  const ad2 = cli('do', 'add-device', '--track', '0', '--kind', 'sampler');
  check(!ad2.ok,
        'A REFUSED ADD-DEVICE EXITS NON-ZERO — it used to exit 0 and say "sent"',
        `exit was 0 and it said: ${JSON.stringify(ad2.out.slice(0, 160))}`);
  check(/head-of-chain instrument/.test(ad2.out),
        'and it SAYS WHY, rather than printing a code',
        `${JSON.stringify(ad2.out.slice(0, 200))} — "chain error 1" is the same fact and tells the `
        + 'reader nothing they can act on');
  const afterAd2 = await saved('cliv_batch_ad2');
  const chainNow = (afterAd2?.tracks?.find((t) => t.track_id === 0)?.device_chain ?? [])
    .map((d) => d.device_id);
  check(chainNow.length === chainAfter.length,
        'and the refusal was real — the chain is unchanged',
        `${JSON.stringify(chainAfter)} -> ${JSON.stringify(chainNow)}`);

  // ── THE SAME ANSWER FOR THE OTHER TWO FAMILIES THAT HAVE ONE ────────────────────────────────
  //
  // routing and mod are the other two families in errorScopeName, they publish the same
  // {errorCode@2, trackId@4} shape, and daw-cli exited 0 on both. One waiter serves all three, so
  // these two checks are what stop that generalisation from being a claim: a family whose codes
  // are mapped but whose diff types are wrong would still print "sent" and pass nothing here.
  const rt = cli('do', 'routing', '--track', '4242', '--audio-out', 'master');
  check(!rt.ok && /no such track/.test(rt.out),
        'A REFUSED ROUTING EDIT SAYS THERE IS NO SUCH TRACK',
        `exit=${rt.ok ? 0 : 'nonzero'} said ${JSON.stringify(rt.out.slice(0, 160))}`);

  const ml = cli('do', 'unmod-link', '--track', '0', '--link', '9999');
  check(!ml.ok && /no such modulation link/.test(ml.out),
        'AND A REFUSED MOD-LINK REMOVAL SAYS THERE IS NO SUCH LINK',
        `exit=${ml.ok ? 0 : 'nonzero'} said ${JSON.stringify(ml.out.slice(0, 160))}`);

  // ── AND THE GLOBAL SCOPE, which the track matcher could not see ─────────────────────────────
  //
  // The journal writes "global" for song-wide commands and "master" for the master track, NOT
  // track:4294967295 and track:4294901760 — so a matcher that only knew the track form missed
  // every refusal on both. 4/5 is a typo, not a time signature, and the engine refuses rather than
  // clamping it to 4/4; the successful time-sig in batch 1 above is this check's control.
  const badSig = cli('do', 'time-sig', '--nanotick', '0', '--sig', '4/5');
  check(!badSig.ok && /time signature/.test(badSig.out),
        'A REFUSED TIME-SIG EXITS NON-ZERO AND NAMES THE TYPO',
        `exit=${badSig.ok ? 0 : 'nonzero'} said ${JSON.stringify(badSig.out.slice(0, 160))}`);
  check(!/4294967295/.test(badSig.out),
        'and it does NOT report the global sentinel as if it were a track id',
        `${JSON.stringify(badSig.out.slice(0, 160))} — 4294967295 in a track field is a number a `
        + 'reader will try to use');

  const zeroBars = cli('do', 'time', 'insert', '--nanotick', '0', '--bars', '0');
  check(!zeroBars.ok,
        'AND A ZERO-BAR RIPPLE IS REPORTED AS THE NON-EDIT IT IS',
        `exit=${zeroBars.ok ? 0 : 'nonzero'} said ${JSON.stringify(zeroBars.out.slice(0, 160))}`);

  // The master track is the third scope word and the one most easily left untested, because
  // --track master is an ordinary thing to do and 4294901760 looks like a track id right up until
  // the journal writes "master" instead. Asserted rather than reasoned about.
  const badMaster = cli('do', 'remove-device', '--track', 'master', '--device', '9999');
  check(!badMaster.ok && /no such device to remove/.test(badMaster.out),
        'A REFUSED EDIT ON THE MASTER TRACK IS REPORTED TOO',
        `exit=${badMaster.ok ? 0 : 'nonzero'} said ${JSON.stringify(badMaster.out.slice(0, 160))}`);
  check(!/4294901760/.test(badMaster.out),
        'and the master id is not printed as a raw number either',
        JSON.stringify(badMaster.out.slice(0, 160)));

  // ── THE PLACEMENT FAMILY, all four verbs against one arrangement ────────────────────────────
  //
  // placement.mjs already covers placements END TO END, but it drives them through the PAGE
  // (say({type:'placement', op:'move'})) and uses daw-cli only to READ them back with
  // `get extents`. So all four CLI verbs were genuinely undriven, which is the distinction the
  // pin list is for: a verb named in a suite is not a verb a suite runs.
  //
  // Read back through `get extents` rather than the saved document, because the document stores
  // placements as a per-track LIST with no id in it — {clip_id, at, length, ...} — and these
  // verbs address a placement BY id. Asserting on list position would pass for a move that hit
  // the wrong placement, which is the whole failure mode.
  const extents = () => {
    const r = cli('get', 'extents');
    const out = [];
    for (const line of String(r.out).split('\n')) {
      // One JSON object per line with a trailing comma the array form does not want, and `id` is
      // spelled `placement`. Same parse as placement.mjs, for the same reasons.
      const t = line.trim().replace(/,$/, '');
      if (!t.startsWith('{')) continue;
      try {
        const o = JSON.parse(t);
        if (o.placement !== undefined) out.push(o);
      } catch { /* not a placement line */ }
    }
    return out;
  };

  // The CLI returns as soon as the command is on the ring; the engine applies it a moment later.
  // Reading extents straight after the call found the arrangement unchanged and reported the add
  // as lost when the engine log said `AddPlacement clip 1 -> placement 11`. So: WAIT for the
  // condition, do not sleep a guessed interval — the same rule the rest of this repo follows.
  const waitExtents = async (pred, ms = 4000) => {
    const deadline = Date.now() + ms;
    let last = extents();
    while (Date.now() < deadline) {
      if (pred(last)) return last;
      await new Promise((r) => setTimeout(r, 100));
      last = extents();
    }
    return last;
  };

  const BEAT = 960000;
  const BAR = BEAT * 4;
  const startExtents = extents();
  console.log(`  the fixture holds ${startExtents.length} placement(s)`);

  if (startExtents.length === 0) {
    check(false, 'the fixture has a placement to work from',
          'get extents returned none — the four placement checks below cannot run, and a batch '
          + 'that silently skips is exactly the shape this file exists to catch');
  } else {
    const seed = startExtents[0];

    // ADD, addressed by --clip (the others take --placement). A count alone would pass for an add
    // that landed on the wrong track or at the wrong tick, so the new one is found by its `at`.
    const addAt = BAR * 12;
    const ap = cli('do', 'add-placement', '--track', String(seed.track),
                   '--clip', String(seed.clip), '--at', String(addAt), '--length', String(BAR));
    check(ap.ok, 'do add-placement is accepted', ap.out.slice(0, 120));
    const afterAdd = await waitExtents((e) => e.length === startExtents.length + 1);
    const mine = afterAdd.find((e) => e.start === addAt && e.track === seed.track);
    check(afterAdd.length === startExtents.length + 1 && mine !== undefined,
          'ADD-PLACEMENT REACHES THE ARRANGEMENT — at the tick it was given, on the named track',
          `${startExtents.length} -> ${afterAdd.length}, `
          + `starts ${JSON.stringify(afterAdd.map((e) => e.start))}`);

    if (mine) {
      // MOVE it, and check the OTHERS did not move. A move that dragged everything would satisfy
      // an assertion about the target alone.
      const others = afterAdd.filter((e) => e.placement !== mine.placement)
        .map((e) => [e.placement, e.start]);
      const moveTo = BAR * 20;
      const mp = cli('do', 'move-placement', '--track', String(seed.track),
                     '--placement', String(mine.placement), '--at', String(moveTo));
      check(mp.ok, 'do move-placement is accepted', mp.out.slice(0, 120));
      const moved = (await waitExtents((es) =>
        es.find((e) => e.placement === mine.placement)?.start === moveTo))
        .find((e) => e.placement === mine.placement);
      check(moved?.start === moveTo,
            'MOVE-PLACEMENT MOVES THAT PLACEMENT — addressed by id, not by list position',
            `placement ${mine.placement} start=${JSON.stringify(moved?.start)}, wanted ${moveTo}`);
      const othersNow = extents().filter((e) => e.placement !== mine.placement)
        .map((e) => [e.placement, e.start]);
      check(JSON.stringify(others) === JSON.stringify(othersNow),
            'and every other placement stayed where it was',
            `${JSON.stringify(others)} -> ${JSON.stringify(othersNow)}`);

      // RESIZE. end-start is the length the engine actually holds; --length is what we asked for.
      const rp = cli('do', 'resize-placement', '--track', String(seed.track),
                     '--placement', String(mine.placement), '--length', String(BAR * 3));
      check(rp.ok, 'do resize-placement is accepted', rp.out.slice(0, 120));
      const resized = (await waitExtents((es) => {
        const m = es.find((e) => e.placement === mine.placement);
        return m !== undefined && m.end - m.start === BAR * 3;
      })).find((e) => e.placement === mine.placement);
      check(resized !== undefined && resized.end - resized.start === BAR * 3,
            'RESIZE-PLACEMENT CHANGES THE LENGTH, and leaves the start alone',
            `start=${JSON.stringify(resized?.start)} end=${JSON.stringify(resized?.end)} `
            + `length=${resized ? resized.end - resized.start : 'gone'}, wanted ${BAR * 3}`);

      // REMOVE, and the count must come back to where the add left it minus one — with the SEED
      // still present, so a remove that took the wrong placement fails here.
      const xp = cli('do', 'remove-placement', '--track', String(seed.track),
                     '--placement', String(mine.placement));
      check(xp.ok, 'do remove-placement is accepted', xp.out.slice(0, 120));
      const afterRemove = await waitExtents((es) =>
        es.find((e) => e.placement === mine.placement) === undefined);
      check(afterRemove.find((e) => e.placement === mine.placement) === undefined,
            'REMOVE-PLACEMENT REMOVES THAT PLACEMENT',
            `${JSON.stringify(afterRemove.map((e) => e.placement))} still has it`);
      check(afterRemove.find((e) => e.placement === seed.placement) !== undefined,
            'and the placement it was NOT asked to remove is still there',
            `seed ${seed.placement} gone from `
            + `${JSON.stringify(afterRemove.map((e) => e.placement))}`);
    }
  }

  // ── sound-addressed, a per-track rule stored under a DIFFERENT name than the verb ───────────
  //
  // Saved as `sound_addressed_only` (project_file.cpp:675), not `sound_addressed`. Reading back
  // the verb's own name would find undefined and read exactly like a flag that never arrived,
  // which is how the note-overlap check in batch 3 nearly went wrong too.
  const sa = cli('do', 'sound-addressed', '--track', '0', '--on', '1');
  check(sa.ok, 'do sound-addressed is accepted', sa.out.slice(0, 120));
  const afterSa = await saved('cliv_batch_sa');
  const soundOnly = afterSa?.tracks?.find((t) => t.track_id === 0)?.sound_addressed_only;
  check(soundOnly === true || soundOnly === 1,
        'SOUND-ADDRESSED REACHES THE TRACK — under its stored name, not the verb name',
        `track 0 sound_addressed_only=${JSON.stringify(soundOnly)}`);

  // ── clip-grid: the CLIP's own subdivision, which outranks the track's ────────────────────────
  //
  // The clip's grid is drawn BEFORE the track's, so this is the authoritative one. Track 0 was set
  // to lines-per-beat 8 by batch 1, so asserting the CLIP moved to 3 also proves the two are
  // separate fields — a writer that set the track's would leave the clip at its fixture value and
  // pass a check that only looked for "something changed".
  const clipBefore = (await saved('cliv_batch_cg0'))?.clips?.find((c) => c.id === 1);
  const cg = cli('do', 'clip-grid', '--track', '0', '--clip', '1', '--lines', '3');
  check(cg.ok, 'do clip-grid is accepted', cg.out.slice(0, 120));
  const afterCg = await saved('cliv_batch_cg');
  const clip1 = afterCg?.clips?.find((c) => c.id === 1);
  check(clip1?.lines_per_beat === 3,
        'CLIP-GRID REACHES THE CLIP — lines_per_beat is 3 on clip 1',
        `was ${JSON.stringify(clipBefore?.lines_per_beat)}, now `
        + `${JSON.stringify(clip1?.lines_per_beat)}`);
  const track0Lines = afterCg?.tracks?.find((t) => t.track_id === 0)?.lines_per_beat;
  check(track0Lines !== 3,
        'and it did NOT write the TRACK grid instead — the two are separate fields',
        `track 0 lines_per_beat=${JSON.stringify(track0Lines)}, which must not be the 3 we sent `
        + 'to the clip');

  // ── delete-note, against a note this check puts there itself ────────────────────────────────
  //
  // The first version of this deleted the fixture's own note (pitch 60 on clip 1, addressed
  // through track 0) and reported the delete as lost. It was not: by this point in the file
  // track 0's placements no longer reference clip 1 at all — an earlier shared-edit gave track 0
  // its own copy (clip 7, empty) and left clip 1 on track 1. The engine received the delete,
  // found nothing at that address, and correctly did nothing.
  //
  // That is the same trap batch 2 fell into: asserting against the fixture as WRITTEN after sixty
  // checks have rewritten it. So this writes its own note first, at a pitch nothing else uses,
  // and reads back the clip TRACK 0 ACTUALLY HOLDS rather than the one the fixture started with.
  const clipOnTrack0 = (doc) =>
    doc?.tracks?.find((t) => t.track_id === 0)?.placements?.[0]?.clip_id;
  const notesOf = (doc, id) => (doc?.clips?.find((c) => c.id === id)?.notes ?? []);

  const DN_TICK = BAR * 2;
  const DN_PITCH = 71;
  const wn = cli('do', 'note', '--track', '0', '--nanotick', String(DN_TICK),
                 '--pitch', String(DN_PITCH));
  check(wn.ok, 'do note is accepted (the setup for delete-note)', wn.out.slice(0, 120));
  const afterWn = await saved('cliv_batch_wn');
  const target = clipOnTrack0(afterWn);
  // BY PITCH, NOT BY TICK. --nanotick is SONG time; the note is stored CLIP-RELATIVE. Track 0
  // holds clip 7 twice, at 0 and at BAR*2, so a note written at song tick BAR*2 lands at
  // clip-relative 0 — and asserting on the tick we sent read [[0,71]] and called a note that was
  // exactly where it belonged missing. DN_PITCH is unused by anything else in this fixture, which
  // is what makes the pitch alone a sufficient address here.
  const hasMine = (doc) => notesOf(doc, target).some((n) => n.pitch === DN_PITCH);
  check(target !== undefined && hasMine(afterWn),
        'the note to delete is really there first — on the clip track 0 HOLDS, not the one the '
        + 'fixture started with',
        `clip ${JSON.stringify(target)} notes=${JSON.stringify(
          notesOf(afterWn, target).map((n) => [n.nanotick, n.pitch]))}`);

  const othersBefore = notesOf(afterWn, target)
    .filter((n) => n.pitch !== DN_PITCH).map((n) => [n.nanotick, n.pitch]);
  const dn = cli('do', 'delete-note', '--track', '0', '--nanotick', String(DN_TICK),
                 '--pitch', String(DN_PITCH));
  check(dn.ok, 'do delete-note is accepted', dn.out.slice(0, 120));
  const afterDn = await saved('cliv_batch_dn');
  check(!hasMine(afterDn),
        'DELETE-NOTE REMOVES THAT NOTE from the clip track 0 holds',
        `clip ${target} still has ${JSON.stringify(
          notesOf(afterDn, target).map((n) => [n.nanotick, n.pitch]))}`);
  check(JSON.stringify(notesOf(afterDn, target)
          .filter((n) => n.pitch !== DN_PITCH).map((n) => [n.nanotick, n.pitch]))
        === JSON.stringify(othersBefore),
        'and every other note in that clip survived — a delete that cleared the clip would pass '
        + 'the check above',
        `${JSON.stringify(othersBefore)} -> ${JSON.stringify(notesOf(afterDn, target)
          .filter((n) => n.pitch !== DN_PITCH).map((n) => [n.nanotick, n.pitch]))}`);

  // ── the sampler's own verbs, against the sampler the fixture already has ─────────────────────
  //
  // Field names are HYPHENATED — voice-cap, default-gate, root — and the tables live in
  // daw-bridge/src/layout.rs. Worth stating because my first attempt to read them used a
  // [a-z_]+ pattern, which cannot see a hyphen and returned a confident, wrong list.
  //
  // The device id is READ from the document rather than assumed to be 3: earlier batches add and
  // remove devices on this track, and a check that hard-codes an id passes or fails for reasons
  // that have nothing to do with the verb under test.
  const samplerDoc = await saved('cliv_batch_s0');
  const samplerDev = samplerDoc?.tracks?.find((t) => t.track_id === 0)
    ?.device_chain?.find((d) => d.sampler !== undefined);
  check(samplerDev !== undefined,
        'track 0 still has a sampler to drive — read from the document, not assumed to be id 3',
        `chain is ${JSON.stringify((samplerDoc?.tracks?.find((t) => t.track_id === 0)
          ?.device_chain ?? []).map((d) => [d.device_id, d.kind]))}`);

  if (samplerDev) {
    const devId = String(samplerDev.device_id);
    const slotId = String(samplerDev.sampler.slots?.[0]?.id ?? 1);
    const readDev = (doc) => doc?.tracks?.find((t) => t.track_id === 0)
      ?.device_chain?.find((d) => d.device_id === samplerDev.device_id);

    // sampler-device: a DEVICE-wide field. voice_cap in the document, --field voice-cap on the
    // wire — the verb's word and the stored word differ again.
    const capBefore = samplerDev.sampler.voice_cap;
    const sd = cli('do', 'sampler-device', '--track', '0', '--device', devId,
                   '--field', 'voice-cap', '--value', '12');
    check(sd.ok, 'do sampler-device is accepted', sd.out.slice(0, 140));
    const afterSd = await saved('cliv_batch_sd');
    check(readDev(afterSd)?.sampler?.voice_cap === 12,
          'SAMPLER-DEVICE REACHES THE DEVICE — voice_cap is 12',
          `was ${JSON.stringify(capBefore)}, now `
          + `${JSON.stringify(readDev(afterSd)?.sampler?.voice_cap)}`);

    // sampler-slot: a SLOT field. --field root writes root_key, so this also pins that the two
    // names refer to one thing.
    const rootBefore = samplerDev.sampler.slots?.[0]?.root_key;
    const ss = cli('do', 'sampler-slot', '--track', '0', '--device', devId, '--slot', slotId,
                   '--field', 'root', '--value', '72');
    check(ss.ok, 'do sampler-slot is accepted', ss.out.slice(0, 140));
    const afterSs = await saved('cliv_batch_ss');
    const slotAfter = readDev(afterSs)?.sampler?.slots
      ?.find((sl) => String(sl.id) === slotId);
    check(slotAfter?.root_key === 72,
          'SAMPLER-SLOT REACHES THAT SLOT — --field root writes root_key',
          `was ${JSON.stringify(rootBefore)}, now ${JSON.stringify(slotAfter?.root_key)}`);
    check(readDev(afterSs)?.sampler?.voice_cap === 12,
          'and the device-wide field set a moment ago is still 12 — a slot write must not '
          + 'rewrite the device',
          `voice_cap=${JSON.stringify(readDev(afterSs)?.sampler?.voice_cap)}`);

    // sampler-slot-name, the one verb here whose stored name matches its own.
    const sn = cli('do', 'sampler-slot-name', '--track', '0', '--device', devId,
                   '--slot', slotId, '--name', 'clivslot');
    check(sn.ok, 'do sampler-slot-name is accepted', sn.out.slice(0, 140));
    const afterSn = await saved('cliv_batch_sn');
    const named = readDev(afterSn)?.sampler?.slots?.find((sl) => String(sl.id) === slotId);
    check(named?.name === 'clivslot',
          'SAMPLER-SLOT-NAME REACHES THE SLOT',
          `name=${JSON.stringify(named?.name)}`);
    check(named?.root_key === 72,
          'and renaming did not undo the root it was given',
          `root_key=${JSON.stringify(named?.root_key)}`);
  }

  // A note to survive the quantize below AND to carry the row op further down. Written first
  // because the clip is EMPTY at this point — delete-note took the only one — and "the stored
  // notes are unchanged" compared [] with [] passes however badly quantize misbehaves. A live
  // check over nothing is still a check about nothing.
  const ro = cli('do', 'note', '--track', '0', '--nanotick', String(BAR), '--pitch', '73');
  check(ro.ok, 'do note is accepted (the setup for quantize and set-row-ops)', ro.out.slice(0, 120));
  const afterRo = await saved('cliv_batch_ro');
  const rowNote = notesOf(afterRo, target).find((n) => n.pitch === 73);
  check(rowNote?.note_id !== undefined,
        'the note that must survive quantize, and carry the row op, is really there',
        `notes=${JSON.stringify(notesOf(afterRo, target).map((n) => [n.pitch, n.note_id]))}`);

  // ── quantize, which changes what SOUNDS and never the stored notes ──────────────────────────
  //
  // So there is nothing to assert in the note list, and a check that looked there would find the
  // notes untouched and call a working verb broken. The lane's setting is the artifact: a
  // `quantize` child object on the track, written UNCONDITIONALLY even when off, so a file always
  // states what the lane does. All three fields are asserted because they travel in one payload
  // and a writer that dropped two of them would still move the first.
  // Snapshotted BEFORE the call. The first version of this compared notesOf(afterQz, target) with
  // the same expression spelled differently and could not fail — the exact shape this repo keeps
  // catching: a green check that passes with the bug present.
  const notesBeforeQz = JSON.stringify(
    notesOf(afterRo, target).map((n) => [n.nanotick, n.pitch, n.duration]));
  const qz = cli('do', 'quantize', '--track', '0', '--grid', String(Q / 2),
                 '--strength', '800', '--swing', '120');
  check(qz.ok, 'do quantize is accepted', qz.out.slice(0, 120));
  const afterQz = await saved('cliv_batch_qz');
  const lane = afterQz?.tracks?.find((t) => t.track_id === 0)?.quantize;
  check(lane?.grid_nanoticks === Q / 2 && lane?.strength_milli === 800
        && lane?.swing_milli === 120,
        'QUANTIZE REACHES THE LANE — grid, strength and swing all three',
        `quantize=${JSON.stringify(lane)}, wanted `
        + `{grid_nanoticks:${Q / 2}, strength_milli:800, swing_milli:120}`);
  const notesAfterQz = JSON.stringify(
    notesOf(afterQz, target).map((n) => [n.nanotick, n.pitch, n.duration]));
  check(notesAfterQz === notesBeforeQz && notesBeforeQz !== '[]',
        'and the stored notes are BYTE-FOR-BYTE what they were, which is what non-destructive '
        + 'means — over a clip that HAS a note, so the comparison is about something',
        `${notesBeforeQz} -> ${notesAfterQz}`);

  // ── set-row-ops, addressed by NOTE ID ───────────────────────────────────────────────────────
  //
  // Row ops live on a note, so this writes its own note and reads the id back rather than
  // guessing one. Only the ops NAMED on the command line are touched — that is the mask — so the
  // check also pins that the note's pitch and position did not move: a handler that rewrote the
  // note wholesale to set one field would pass a check that only looked at probability.
  //
  // `probability` is written to the file ONLY when > 0 (project_file.cpp:332), so absent is the
  // default rather than a missing write. 50 is chosen to be unambiguous either way.
  if (rowNote?.note_id !== undefined) {
    const sro = cli('do', 'set-row-ops', '--track', '0', '--clip', String(target),
                    '--note', String(rowNote.note_id), '--prob', '50');
    check(sro.ok, 'do set-row-ops is accepted', sro.out.slice(0, 140));
    const afterSro = await saved('cliv_batch_sro');
    const withOp = notesOf(afterSro, target).find((n) => n.note_id === rowNote.note_id);
    check(withOp?.probability === 50,
          'SET-ROW-OPS REACHES THAT NOTE — probability is 50',
          `note ${rowNote.note_id} = ${JSON.stringify(withOp)}`);
    check(withOp?.pitch === 73 && withOp?.nanotick === rowNote.nanotick,
          'and setting one op did not move the note it was set on',
          `pitch=${JSON.stringify(withOp?.pitch)} nanotick=${JSON.stringify(withOp?.nanotick)}, `
          + `was pitch=73 nanotick=${rowNote.nanotick}`);
  }

  // ── mod-link, which is refused unless modulation flows FORWARD ──────────────────────────────
  //
  // The source must not sit later in the chain than the target, so source and target are taken
  // from the chain AS IT IS at this point — first device and last device — rather than from
  // hard-coded ids. Earlier batches add and remove devices on this track, so any id written here
  // would be a guess about the state sixty checks from now.
  //
  // unmod-link's REFUSAL is asserted further up; this is the pair that proves the verb does the
  // thing it refuses to do wrongly — create a link, then take it away again by the id it was
  // given.
  const chainDoc = await saved('cliv_batch_ml0');
  const chainNow2 = chainDoc?.tracks?.find((t) => t.track_id === 0)?.device_chain ?? [];
  check(chainNow2.length >= 2,
        'track 0 has at least two devices, so a forward link is possible at all',
        `chain is ${JSON.stringify(chainNow2.map((d) => [d.device_id, d.kind]))}`);

  if (chainNow2.length >= 2) {
    const src = chainNow2[0].device_id;
    const dst = chainNow2[chainNow2.length - 1].device_id;
    const linksBefore = (chainDoc?.tracks?.find((t) => t.track_id === 0)?.mod_links ?? []).length;
    const mkl = cli('do', 'mod-link', '--track', '0',
                    '--source-device', String(src), '--target-device', String(dst),
                    '--source-kind', 'macro', '--target-kind', 'vst',
                    '--source-id', '0', '--target-id', '0', '--depth', '0.5');
    check(mkl.ok, 'do mod-link is accepted', mkl.out.slice(0, 160));
    const afterMkl = await saved('cliv_batch_ml');
    const links = afterMkl?.tracks?.find((t) => t.track_id === 0)?.mod_links ?? [];
    // `src` and `dst`, NOT source/target. I read the writer in project_file.cpp and got the leaf
    // keys — device_id, source_id, kind — without the object names they sit under, so the first
    // version of this looked for L.source.device_id, found undefined on a link that was perfectly
    // correct, and reported the verb broken.
    const mineLink = links.find((L) => L.src?.device_id === src && L.dst?.device_id === dst);
    check(links.length === linksBefore + 1 && mineLink !== undefined,
          'MOD-LINK REACHES THE TRACK — a link from the first device to the last',
          `${linksBefore} -> ${links.length}: ${JSON.stringify(
            links.map((L) => [L.link_id, L.src?.device_id, L.dst?.device_id]))}`);
    check(mineLink?.depth === 0.5 && mineLink?.src?.kind === 'macro'
          && mineLink?.dst?.kind === 'vst_param',
          'and it carries the depth and both kinds it was given, not just the endpoints',
          `depth=${JSON.stringify(mineLink?.depth)} src.kind=${JSON.stringify(mineLink?.src?.kind)}`
          + ` dst.kind=${JSON.stringify(mineLink?.dst?.kind)}`);

    if (mineLink) {
      // And back off again, by the id the engine assigned — which is the whole reason link_id
      // exists and the reason daw-cli refuses to print the AUTO sentinel as if it were one.
      const uml = cli('do', 'unmod-link', '--track', '0', '--link', String(mineLink.link_id));
      check(uml.ok, 'do unmod-link is accepted for a link that DOES exist', uml.out.slice(0, 160));
      const afterUml = await saved('cliv_batch_uml');
      const left = afterUml?.tracks?.find((t) => t.track_id === 0)?.mod_links ?? [];
      check(left.find((L) => L.link_id === mineLink.link_id) === undefined,
            'UNMOD-LINK REMOVES THAT LINK — the round trip closes',
            `link ${mineLink.link_id} still in ${JSON.stringify(left.map((L) => L.link_id))}`);
    }
  }

  // ── automation: written with one verb, READ BACK with another, then deleted ─────────────────
  //
  // `automation-points` and `delete-automation` are the two pinned verbs here; `do automation` is
  // the setup. Note that automation-points is a GET, not a DO — a detail that matters because a
  // reader is exactly the verb whose breakage is invisible from the document: it can return
  // nothing at all and the file on disk still looks right.
  //
  // So this asserts BOTH surfaces: the point comes back from the reader AND it is in the saved
  // document. A reader that returned a hard-coded empty list would pass neither, and one that
  // echoed its own input would pass only the first.
  const AUTO_TICK = BAR * 3;
  const AUTO_PARAM = 1;
  const au = cli('do', 'automation', '--track', '0', '--param', String(AUTO_PARAM),
                 '--nanotick', String(AUTO_TICK), '--value', '0.25');
  check(au.ok, 'do automation is accepted (the setup)', au.out.slice(0, 140));

  // The reader prints ONE object with the points NESTED inside it — {track_id, param, found,
  // discrete, points:[{nanotick,value}]} — not one object per line the way `get extents` does.
  // My first parser treated the wrapper as a point, so the write check failed on a point that was
  // present and correct, and the delete check PASSED VACUOUSLY: it looked for a top-level
  // nanotick that never existed either way, and would have reported success on a delete that did
  // nothing. Two different bugs from one wrong assumption about the shape.
  const readPoints = () => {
    const r = cli('get', 'automation-points', '--track', '0', '--param', String(AUTO_PARAM));
    const raw = String(r.out);
    let wrapper = null;
    try { wrapper = JSON.parse(raw); } catch { /* not JSON at all */ }
    return { ok: r.ok, raw, found: wrapper?.found, points: wrapper?.points ?? [] };
  };
  const got = readPoints();
  check(got.ok, 'get automation-points runs', got.raw.slice(0, 140));
  const mineAuto = got.points.find((pt) => pt.nanotick === AUTO_TICK);
  check(mineAuto !== undefined,
        'AUTOMATION-POINTS READS BACK THE POINT THAT WAS WRITTEN',
        `points=${JSON.stringify(got.points.slice(0, 6))} raw=${got.raw.slice(0, 160)}`);

  const dau = cli('do', 'delete-automation', '--track', '0', '--param', String(AUTO_PARAM),
                  '--nanotick', String(AUTO_TICK));
  check(dau.ok, 'do delete-automation is accepted', dau.out.slice(0, 140));
  const leftAuto = readPoints();
  check(leftAuto.points.find((pt) => pt.nanotick === AUTO_TICK) === undefined,
        'DELETE-AUTOMATION REMOVES THAT POINT — read back through the same reader',
        `still ${JSON.stringify(leftAuto.points.slice(0, 6))}`);

  // ── remove-track, which must remove THAT track and leave the rest ────────────────────────
  //
  // Asserted on WHICH track went, not just the count. A remove that took the wrong one leaves the
  // same number behind, and a count check would pass for it.
  /*
   * NOT THE MASTER. `kMasterTrackId` is 0xFFFF0000 (4294901760) and it appears in the saved
   * tracks list, so "the last track" selected it — and the engine correctly refuses to remove the
   * master. My first version did exactly that and reported "remove-track does not remove track
   * 4294901760", which is the engine being right and the test being wrong.
   */
  const MASTER = 0xFFFF0000;
  const idsBefore = (afterLpb?.tracks ?? []).map((t) => t.track_id).filter((i) => i !== MASTER);
  const victim = idsBefore[idsBefore.length - 1];
  const rm = cli('do', 'remove-track', '--track', String(victim));
  check(rm.ok, 'do remove-track is accepted', rm.out.slice(0, 120));
  const afterRm = await saved('cliv_batch_rm');
  const idsAfter = (afterRm?.tracks ?? []).map((t) => t.track_id).filter((i) => i !== MASTER);
  check(!idsAfter.includes(victim),
        `REMOVE-TRACK REMOVES TRACK ${victim} — from the document, not just the reply`,
        `${JSON.stringify(idsBefore)} -> ${JSON.stringify(idsAfter)}`);
  check(idsAfter.length === idsBefore.length - 1 && idsBefore.filter((i) => i !== victim)
          .every((i) => idsAfter.includes(i)),
        'and leaves every other track alone',
        `${JSON.stringify(idsBefore)} -> ${JSON.stringify(idsAfter)}`);
}

await stack.stop();
console.log(`\n${fail === 0 ? 'ALL PASS' : 'FAILURES'} (${pass} checks${fail ? `, ${fail} failed` : ''})`);
process.exit(fail === 0 ? 0 : 1);
