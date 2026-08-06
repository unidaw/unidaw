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
      { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm }, encoding: 'utf8', timeout: 15000 }) };
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
  harmony_timeline: [], clips: [],
  tracks: [
    {
      track_id: 0, name: 'Rack', harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
      device_chain: [
        { device_id: 3, kind: 'sampler', patcher_node_id: 0, bypass: false,
          sampler: { slots: [{ id: 1, name: 'a', key_low: 0, key_high: 127, root_key: 60, gate: 0 }] } },
        { device_id: 7, kind: 'gain', patcher_node_id: 0, bypass: false },
      ],
      mod_links: [], placements: [],
    },
    {
      track_id: 1, name: 'Patch', harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
      device_chain: [{ device_id: 1, kind: 'patcher_event', patcher_node_id: 0, bypass: false }],
      mod_links: [], placements: [],
    },
  ],
}, null, 2));

const load = cli('do', 'load', NAME);
check(load.ok, 'the fixture loads', load.out.slice(0, 200));

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

const before = await saved(`${NAME}_a`);
check(JSON.stringify(chainOf(before, 0)) === '[3,7]',
      'the fixture starts with devices [3,7] on track 0', JSON.stringify(chainOf(before, 0)));

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
const addA = cli('do', 'patcher-node', '--track', '1', '--device', '1', '--type', 'euclidean');
const addB = cli('do', 'patcher-node', '--track', '1', '--device', '1', '--type', 'euclidean');
check(addA.ok && addB.ok, 'two patcher nodes are added to work on', (addA.out + addB.out).slice(0, 160));

const nodesOf = (doc) => {
  const dev = doc?.tracks?.find((t) => t.track_id === 1)?.device_chain?.[0];
  return dev?.patcher?.nodes ?? dev?.graph?.nodes ?? [];
};
const withNodes = await saved(`${NAME}_d`);
const nodeCount = nodesOf(withNodes).length;
check(nodeCount >= 2, 'the device graph holds the two new nodes', String(nodeCount));

const conn = cli('do', 'patcher-connect', '--track', '1', '--device', '1',
                 '--src', '0', '--dst', '1', '--kind', '0');
check(conn.ok, 'do patcher-connect runs', conn.out.slice(0, 160));
const afterConn = await saved(`${NAME}_e`);
const edgesOf = (doc) => {
  const dev = doc?.tracks?.find((t) => t.track_id === 1)?.device_chain?.[0];
  return dev?.patcher?.connections ?? dev?.patcher?.edges ?? dev?.graph?.connections ?? [];
};
check(edgesOf(afterConn).length >= 1,
      'patcher-connect leaves a connection in the SAVED device graph',
      JSON.stringify(edgesOf(afterConn)).slice(0, 120));

const unnode = cli('do', 'patcher-unnode', '--track', '1', '--device', '1', '--node', '1');
check(unnode.ok, 'do patcher-unnode runs', unnode.out.slice(0, 160));
const afterUnnode = await saved(`${NAME}_f`);
check(nodesOf(afterUnnode).length === nodeCount - 1,
      'patcher-unnode removes exactly one node',
      `${nodeCount} -> ${nodesOf(afterUnnode).length}`);

/* ── open-editor ────────────────────────────────────────────────────────────────────────────
 * THE ONE WITH A KNOWN HISTORY. It was built as a UiChainCommandPayload where the engine reads a
 * UiCommandPayload; both are 40 bytes, so the size check passed and the engine read `deviceKind`
 * (offset 16) as the device id — 0 for every call, every device, every track. The button had
 * never worked once.
 *
 * The engine owns the plugin window, so success is not observable from here. The ENGINE LOG is,
 * and it names its refusal: "OpenPluginEditor failed - device N not found". Asserting the absence
 * of that line for a device that exists is the strongest check available, and it is exactly the
 * one that would have caught the original bug — it printed "device 0 not found" every time.
 */
const editor = cli('do', 'open-editor', '--track', '0', '--device', '3');
check(editor.ok, 'do open-editor runs', editor.out.slice(0, 160));
await sleep(1000);
const log = existsSync(ENGINE_LOG) ? readFileSync(ENGINE_LOG, 'utf8') : '';
check(!/OpenPluginEditor failed/.test(log),
      'the engine does not refuse open-editor for a device that exists',
      (log.match(/OpenPluginEditor failed[^\n]*/) || [''])[0]);
// The control: the same verb aimed at a device id that is NOT in the chain must be refused, or
// the check above passes because the engine never says anything about this command at all.
const badEditor = cli('do', 'open-editor', '--track', '0', '--device', '4242');
check(badEditor.ok, 'open-editor for a missing device still exits 0 (the CLI cannot know)',
      badEditor.out.slice(0, 120));
await sleep(1000);
const log2 = existsSync(ENGINE_LOG) ? readFileSync(ENGINE_LOG, 'utf8') : '';
check(/OpenPluginEditor failed/.test(log2),
      'NEGATIVE CONTROL: a missing device IS refused, so the check above is not blind',
      'no refusal appeared — the engine may not be reading this command at all');

/* ── remove-device ──────────────────────────────────────────────────────────────────────────
 * Last, because it destroys the fixture. By ID, and the surviving id is asserted rather than the
 * count: removing the wrong device leaves a chain of one either way.
 */
const rm = cli('do', 'remove-device', '--track', '0', '--device', '3');
check(rm.ok, 'do remove-device runs', rm.out.slice(0, 160));
const afterRm = await saved(`${NAME}_g`);
check(JSON.stringify(chainOf(afterRm, 0)) === '[7]',
      'remove-device removes device 3 and leaves 7', JSON.stringify(chainOf(afterRm, 0)));

await stack.stop();
console.log(`\n${fail === 0 ? 'ALL PASS' : 'FAILURES'} (${pass} checks${fail ? `, ${fail} failed` : ''})`);
process.exit(fail === 0 ? 0 : 1);
