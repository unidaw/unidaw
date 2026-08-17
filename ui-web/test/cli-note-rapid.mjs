#!/usr/bin/env node
/**
 * FOUR NOTE WRITES, FOUR daw-cli PROCESSES, BACK TO BACK — does exact outcome tracking survive it?
 *
 * This is the AE-RING-02 regression. The former id-correlated event-ring peek raced the
 * sidecar's true drain and could report success after losing a version refusal. SHM v41 replaces
 * that inference with an append-only broadcast outcome region keyed by command id, opcode, scope,
 * and sent base version. Each CLI process now waits for its exact terminal outcome and performs
 * the one permitted fresh-ticket retry after an automatically based stale refusal.
 *
 * The suite remains a causal A/B: this arm runs the sidecar's drain thread, while
 * cli-note-rapid-control.mjs omits only the sidecar. Both are discovered by all.mjs.
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

const loadStatus = () => {
  const result = cli('get', 'transport');
  if (!result.ok) return null;
  try {
    const value = JSON.parse(result.stdout);
    return Number.isInteger(value.load_seq) && Number.isInteger(value.load_ok)
      ? { seq: value.load_seq, ok: value.load_ok } : null;
  } catch { return null; }
};

/** The notes track 0 actually holds in the saved document — the mutation oracle. Exact terminal
 *  outcomes prove that the guard accepted and the handler returned, not that the requested note
 *  is present in the persisted document. Follows placements rather than assuming clip id 1. */
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
  const loadBefore = loadStatus();
  check(loadBefore !== null, 'the pre-load acknowledgement is readable', String(loadBefore));
  if (loadBefore === null) throw new Error('cannot read the pre-load acknowledgement');
  const made = cli('do', 'new', 'clinoterapid');
  check(made.ok, 'a project to write into', made.out.trim().slice(0, 120));
  await until(() => {
    const status = loadStatus();
    return status !== null && status.seq !== loadBefore.seq && status.ok === 1;
  }, 'the engine post-load acknowledgement');
  const journalStart = historyOffset();

/*
 * Each spawn is synchronous, exactly like cli-harmony-rapid.mjs: process N+1 starts immediately
 * after process N exits, with no extra settle. The sidecar's drain remains active, but SHM v41's
 * outcome broadcast has no consumer cursor for it to steal; the control suite omits that bystander.
 * Four distinct
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
        `${JSON.stringify(got)} — exact command terminals completed, but the persisted mutation `
        + 'does not match the request (AE-RING-02 regression)');
} finally {
  await stack.stop();
}

console.log(`  retained evidence: ${stack.root}`);
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
