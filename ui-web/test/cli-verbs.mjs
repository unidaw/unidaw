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
 * A RANGE, not a selection — the console's transpose acts on what is selected, and a selection is
 * view state this surface does not have. Same arithmetic as the agent's tool: both call
 * plan_transpose in daw-bridge, so the two cannot answer differently.
 *
 * THIS FIXTURE IS THE REFUSAL CASE, and by accident rather than design — it places one clip three
 * times so that `get shared` has something to count. That is exactly the shape a range transpose
 * cannot describe: `read_track_clip` flattens the track, so a clip placed three times contributes
 * its notes three times, and writing them back edits ONE clip repeatedly. Measured before the
 * guard existed: two notes became three.
 *
 * So this asserts the refusal and the reason. The positive case is covered against a live engine
 * on a track with no shared clips, in daw-agent's engine_e2e
 * (transpose_moves_notes_in_place_and_keeps_their_column).
 */
{
  const before = await saved(`${NAME}_t0`);
  const pitchesOf = (doc) => (doc ? (doc.clips || []) : [])
    .flatMap((c) => (c.notes || []).map((n) => n.pitch)).sort((a, b) => a - b);
  const p0 = pitchesOf(before);
  check(p0.length > 0, 'the fixture has notes', JSON.stringify(p0));

  const up = cli('do', 'transpose', '--track', '0', '--semitones', '12');
  check(!up.ok,
        'TRANSPOSE OVER A SHARED CLIP IS REFUSED, not silently done three times',
        up.out.slice(0, 220));
  check(/shared/i.test(up.out) && /get shared/.test(up.out),
        'and the refusal names the read that explains it',
        `${up.out.slice(0, 220)} — a refusal that does not say what to do next is one the caller `
        + 'works around');

  await sleep(800);
  const after = await saved(`${NAME}_t1`);
  check(JSON.stringify(pitchesOf(after)) === JSON.stringify(p0),
        'AND NOTHING MOVED — a refusal that half-applies is worse than no refusal',
        `${JSON.stringify(p0)} -> ${JSON.stringify(pitchesOf(after))}`);

  // The two argument refusals, which do not depend on the fixture's shape.
  const zero = cli('do', 'transpose', '--track', '0', '--semitones', '0');
  check(!zero.ok, '0 semitones is REFUSED as a non-edit', zero.out.slice(0, 160));
  const backwards = cli('do', 'transpose', '--track', '0', '--semitones', '1',
                        '--from', '9600000', '--to', '9600000');
  check(!backwards.ok, 'an empty range is REFUSED, not reported as 0 transposed',
        backwards.out.slice(0, 160));
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

await stack.stop();
console.log(`\n${fail === 0 ? 'ALL PASS' : 'FAILURES'} (${pass} checks${fail ? `, ${fail} failed` : ''})`);
process.exit(fail === 0 ? 0 : 1);
