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
