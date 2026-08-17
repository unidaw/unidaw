#!/usr/bin/env node
/**
 * FOUR NOTE WRITES, FOUR daw-cli PROCESSES, BACK TO BACK — does await_clip_outcome survive it?
 *
 * `cli-harmony-rapid.mjs` asked this question for harmony and found the answer changes with the
 * mechanism: a bare `harmony_version()` counter poll survives rapid-fire fine (5/5), but the
 * id-correlated ring peek (`peek_ui_diffs_correlated`, the same one `await_clip_outcome` already
 * uses for `do note` / `do delete-note` / `do chord`) does not — it broke 3/5 when tried for
 * harmony, root-caused to AE-RING-02 (docs/architecture/decisions/AE-RING-02-bystander-drain.md):
 * daw-sidecar's `drain_engine_events` thread ticks every 50ms and TRUE-DRAINS the same
 * single-consumer ring a bystander CLI process only PEEKS, so a diff a bystander has not yet
 * observed can vanish out from under it — reported as `ClipOutcome::Unknown`, which `await_clip_outcome`
 * silently buckets with `Applied` (prints "sent", exits 0, does not retry).
 *
 * `await_clip_outcome` is ALREADY SHIPPED — this is not a proposed change, it is the mechanism
 * `do note`/`do delete-note`/`do chord` have used since P2-CMD-00. AE-RING-02 rated its exposure
 * "PLAUSIBLE, not CONFIRMED": the identical ring, the identical peek pattern, the identical live
 * sidecar drain thread, but no test drives it under the rapid-fire shape that caught harmony.
 * `cli-verbs.mjs` exercises `do note`/`do delete-note` against the same live drain thread and
 * passes, but its commands are distributed through a much longer serial suite rather than sent
 * four back to back.
 *
 * This file is that missing probe. It is deliberately NOT in the default all.mjs sweep: the
 * shipped defect is timing-dependent, so a green run proves nothing and a red run is expected
 * until AE-RING-02 has an owner-approved fix. Run it repeatedly and read the saved document plus
 * the journal evidence it prints. A pass here does not clear AE-RING-02 in general — it answers
 * one question: whether the SAME race that broke harmony's attempted fix also breaks the shipped
 * `do note` member of the clip/chord outcome mechanism.
 *
 * Run the causal pair with the same trial count:
 *   node ui-web/test/cli-note-rapid.mjs
 *   node ui-web/test/cli-note-rapid.mjs --without-sidecar
 * The first arm starts the sidecar only after SHM exists and waits for its event-drain attachment;
 * the second changes only the presence of that sidecar process.
 */

import { spawnSync } from 'node:child_process';
import { readFileSync, existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { startStack } from './stack.mjs';

const ROOT = resolve(new URL('../..', import.meta.url).pathname);
const Q = 960000;
const args = process.argv.slice(2);
if (args.some((arg) => arg !== '--without-sidecar')) {
  console.error('usage: node ui-web/test/cli-note-rapid.mjs [--without-sidecar]');
  process.exit(2);
}
const withSidecar = !args.includes('--without-sidecar');

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (probe, what, timeoutMs = 5000) => {
  const start = Date.now();
  while (Date.now() - start <= timeoutMs) {
    if (probe()) return;
    await sleep(20);
  }
  throw new Error(`timed out waiting for ${what}`);
};

const stack = await startStack({ keepDir: true, withSidecar, sidecarAfterEngine: withSidecar });

const cli = (...args) => {
  const result = spawnSync(join(ROOT, 'ui/target/release/daw-cli'), args,
    { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm, DAW_PROJECT_DIR: stack.dir },
      encoding: 'utf8', timeout: 15000 });
  const stdout = String(result.stdout || '');
  const stderr = String(result.stderr || '');
  return {
    ok: !result.error && result.status === 0,
    out: stdout + stderr + String(result.error?.message || ''),
    stdout,
    stderr,
  };
};

const historyPath = join(stack.dir, 'history.jsonl');
const historyOffset = () => existsSync(historyPath) ? readFileSync(historyPath).length : 0;
const historySince = (offset) => {
  if (!existsSync(historyPath)) return { entries: [], errors: [] };
  const tail = readFileSync(historyPath).subarray(offset).toString('utf8');
  const complete = tail.endsWith('\n') ? tail : tail.slice(0, tail.lastIndexOf('\n') + 1);
  const entries = [];
  const errors = [];
  for (const line of complete.split('\n').filter(Boolean)) {
    try { entries.push(JSON.parse(line)); }
    catch (error) { errors.push(String(error)); }
  }
  return { entries, errors };
};

/** The notes track 0 actually holds in the saved document — the oracle, since exit 0 proves
 *  nothing here (`await_clip_outcome`'s Unknown branch prints "sent" too). Follows placements to
 *  clips rather than assuming clip id 1, since `locateEditTarget(..., create=true)` mints
 *  whatever clip id the engine chooses. */
const savedNotes = async (tag) => {
  const save = cli('do', 'save', tag);
  if (!save.ok) return { save, notes: null };
  for (let i = 0; i < 40; i++) {
    const p = join(stack.dir, `${tag}.uniproj.json`);
    if (existsSync(p)) {
      try {
        const doc = JSON.parse(readFileSync(p, 'utf8'));
        const clips = new Map((doc.clips || []).map((c) => [c.id, c]));
        const track0 = (doc.tracks || []).find((t) => t.track_id === 0);
        const notes = (track0?.placements || [])
          .flatMap((pl) => ((clips.get(pl.clip_id) || {}).notes || [])
            .map((n) => [pl.at + n.nanotick, n.pitch]));
        return { save, notes: notes.sort((a, b) => a[0] - b[0]) };
      } catch { /* mid-write */ }
    }
    await sleep(150);
  }
  return { save, notes: null };
};

console.log(`\nfour note writes, four daw-cli processes, sidecar ${withSidecar ? 'ON' : 'OFF'}\n`);

try {
  if (withSidecar) {
    const sidecarLog = join(stack.root, 'sidecar.log');
    await until(() => existsSync(sidecarLog)
      && readFileSync(sidecarLog, 'utf8').includes(`event drain attached to ${stack.shm}`),
    'the sidecar event drain to attach');
  }
  const beforeNew = historyOffset();
  const made = cli('do', 'new', 'clinoterapid');
  check(made.ok, 'a project to write into', made.out.trim().slice(0, 120));
  await until(() => historySince(beforeNew).entries.some((entry) =>
    entry.op === 'load_project' && entry.outcome === 'received'), 'the new project load');
  const journalStart = historyOffset();

/*
 * Each spawn is synchronous, exactly like cli-harmony-rapid.mjs: process N+1 starts immediately
 * after process N exits, with no extra settle. With the sidecar ON, each process's peek loop can
 * race the sidecar's unconditional drain thread; OFF is the negative control. Four distinct
 * quarter-note ticks in bar 0 all target the ONE clip `locateEditTarget` auto-creates for that
 * bar, and four distinct pitches make a partial write visible as a missing pair, not just a count.
 */
  const pitches = [60, 64, 67, 71];
  const expected = pitches.map((pitch, i) => [i * Q, pitch]);
  const sent = pitches.map((p, i) =>
    cli('do', 'note', '--track', '0', '--nanotick', String(i * Q), '--pitch', String(p)));

  check(sent.every((s) => s.ok), 'all four CLI processes exited 0',
        sent.map((s) => s.out.trim()).join(' | ').slice(0, 200));
  check(sent.every((s) => /"sent"\s*:\s*"note"/.test(s.stdout)),
        'all four CLI processes reported the note as sent',
        sent.map((s) => s.stdout.trim()).join(' | ').slice(0, 200));
  const retries = sent.flatMap((s) => s.stderr.split('\n'))
    .filter((line) => line.includes('was stale; retried at'));
  console.log(`  retries the CLI reported: ${retries.length ? retries.join(' | ') : 'none'}`);

  const saved = await savedNotes('clinoterapid_out');
  const got = saved.notes;
  const history = historySince(journalStart);
  const journal = history.entries;
  const noteAttempts = journal.flatMap((entry, i) => {
    if (entry.op !== 'write_note' || entry.outcome !== 'received'
        || !Number.isInteger(entry.params?.pitch) || !Number.isInteger(entry.params?.nanotick)) {
      return [];
    }
    const next = journal[i + 1];
    const rejected = next?.op === 'write_note' && next?.outcome === 'rejected:version'
      && next?.seq === entry.seq + 1 && next?.base_version === entry.base_version
      && next?.scope === entry.scope;
    return [{ pitch: entry.params.pitch, nanotick: entry.params.nanotick,
              base: entry.base_version, rejected }];
  });
  const landed = new Set((got || []).map(([nanotick, pitch]) => `${nanotick}:${pitch}`));
  const missing = pitches.map((pitch, i) => ({ pitch, nanotick: i * Q }))
    .filter(({ pitch, nanotick }) => !landed.has(`${nanotick}:${pitch}`));
  const refusalProven = missing.every(({ pitch, nanotick }) => {
    const attempts = noteAttempts.filter((a) => a.pitch === pitch && a.nanotick === nanotick);
    return attempts.some((a) => a.rejected) && !attempts.some((a) => !a.rejected);
  });

  check(saved.save.ok, 'the save CLI process exited 0', saved.save.out.trim().slice(0, 160));
  check(got !== null, 'the project saved', String(got));
  check(history.errors.length === 0, 'the journal evidence is parseable', history.errors.join(' | '));
  if (missing.length > 0) {
    check(refusalProven,
          'every missing note is a journal-proven version refusal with no successful retry',
          `missing=${JSON.stringify(missing)} attempts=${JSON.stringify(noteAttempts)}`);
  }
  check(got && JSON.stringify(got) === JSON.stringify(expected),
        'THE DOCUMENT CONTAINS EXACTLY THE FOUR REQUESTED NOTES',
        `${JSON.stringify(got)} — when the journal check above passes, await_clip_outcome failed `
        + 'to observe the engine\'s version-refusal diff and daw-cli reported success without '
        + 'retrying (AE-RING-02)');
} finally {
  await stack.stop();
}

console.log(`  retained evidence: ${stack.root}`);
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
