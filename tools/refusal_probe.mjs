/*
 * WHICH VERBS CAN ACTUALLY BE REFUSED — asked of a live engine, not of grep.
 *
 * A client that waits for a refusal pays the whole window whenever none arrives, because silence is
 * the only success signal these families publish. So a wait is worth adding exactly where the engine
 * can write a refusal, and nowhere else.
 *
 * ANSWERING THIS STATICALLY FAILED, and the failure is worth recording. The rule "does a file
 * handling this command type also contain the string rejected:" reports NO for every sampler op —
 * including sampler_set_slot, watched writing rejected:no_such_slot into history.jsonl on a live
 * engine. reportSamplerReject lives in a different file from the handler, and no amount of grep sees
 * through a shared reporter. So the answer has to come from the engine itself.
 *
 * EVERY INVOCATION BELOW IS COPIED FROM cli-verbs.mjs, which drives these verbs successfully. The
 * first draft of this probe invented flags (--tick for automation, --row for set-row-ops, --id for
 * marker) and every one of them would have been refused by daw-cli's own argument parsing, never
 * reaching the engine, and reported as "the engine never refuses this" — the exact false negative
 * the probe exists to avoid.
 *
 * Run with no sweep in flight. Prints a table; changes nothing.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, existsSync } from 'node:fs';
import { resolve, join } from 'node:path';

import { startStack } from '../ui-web/test/stack.mjs';

const ROOT = resolve(new URL('..', import.meta.url).pathname);
const stack = await startStack({ numBlocks: 8, keepDir: true });

const cli = (...args) => {
  try {
    return { ok: true, out: execFileSync(join(ROOT, 'ui/target/release/daw-cli'), args, {
      env: { ...process.env, DAW_UI_SHM_NAME: stack.shm, DAW_PROJECT_DIR: stack.dir },
      encoding: 'utf8', timeout: 20000 }) };
  } catch (e) {
    return { ok: false, out: String(e.stdout || '') + String(e.stderr || e.message || '') };
  }
};

/*
 * A track id far outside anything the fixture holds is the most portable way to be wrong: it does
 * not depend on what the project contains, and no_track is the documented refusal for the clip
 * family. Flags otherwise exactly as cli-verbs drives them.
 */
const BAD = '4242';
const PROBES = [
  ['audio-clip',        ['do', 'audio-clip', '--track', BAD, '--clip', '1',
                         '--field', 'fade-in', '--value', '960000']],
  ['clip-grid',         ['do', 'clip-grid', '--track', BAD, '--clip', '1', '--lines', '3']],
  ['set-row-ops',       ['do', 'set-row-ops', '--track', BAD, '--clip', '1',
                         '--note', '1', '--prob', '50']],
  ['automation',        ['do', 'automation', '--track', BAD, '--param', '0',
                         '--nanotick', '0', '--value', '0.25']],
  ['delete-automation', ['do', 'delete-automation', '--track', BAD, '--param', '0',
                         '--nanotick', '0']],
  // CONTROLS: two families PROVEN to journal refusals. If these come back silent the probe itself
  // is broken — fixture, ids, or journal path — and every NONE above is worthless, not evidence.
  ['CONTROL sampler',   ['do', 'sampler-slot', '--track', '0', '--device', '1', '--slot', BAD,
                         '--field', 'root', '--value', '60']],
  ['CONTROL chain',     ['do', 'remove-device', '--track', '0', '--device', BAD]],
];

const journal = join(stack.dir, 'history.jsonl');
const lines = () => (existsSync(journal) ? readFileSync(journal, 'utf8').split('\n').filter(Boolean) : []);

const results = [];
for (const [name, args] of PROBES) {
  const before = lines().length;
  const r = cli(...args);
  await new Promise((res) => setTimeout(res, 500));   // past the CLI's own 250ms window
  const fresh = lines().slice(before);
  const rejected = fresh.filter((l) => l.includes('"outcome":"rejected:'));
  results.push({
    name,
    exit: r.ok ? 0 : 1,
    ops: [...new Set(rejected.map((l) => (l.match(/"op":"([a-z_]+)"/) || [])[1]))],
    reasons: [...new Set(rejected.map((l) => (l.match(/"outcome":"rejected:([a-z_]+)"/) || [])[1]))],
    // Any journal line at all proves the command REACHED the engine. Without this, a locally
    // refused argument and an accepted-but-never-refused command look identical.
    reached: fresh.length > 0,
    said: r.out.trim().split('\n')[0] || '',
  });
}

console.log('\n  verb                exit  reached  journalled refusal');
console.log('  ' + '-'.repeat(78));
for (const r of results) {
  const verdict = r.ops.length ? `${r.ops.join(',')} -> ${r.reasons.join(',')}` : 'NONE';
  console.log(`  ${r.name.padEnd(20)}${String(r.exit).padEnd(6)}${String(r.reached).padEnd(9)}${verdict}`);
  if (r.said) console.log(`  ${' '.repeat(35)}said: ${r.said.slice(0, 80)}`);
}
const fired = results.filter((r) => r.name.startsWith('CONTROL') && r.ops.length).length;
console.log(`\n  controls that fired: ${fired}/2 `
  + (fired === 2 ? '(probe is sound — a NONE above is the engine really writing nothing)'
                 : '!! PROBE IS BROKEN — every NONE above is meaningless'));
console.log('  a NONE with reached=false means daw-cli refused the ARGUMENTS; the engine never saw it.');
await stack.stop();
