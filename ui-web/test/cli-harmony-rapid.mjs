#!/usr/bin/env node
/**
 * FOUR KEY CHANGES FROM daw-cli, ONE PROCESS EACH — do they all land?
 *
 * The web UI's version of this was a real defect: four rapid harmony writes all quoted the same
 * base, the engine took one and refused three, and the console said "key set" every time. Fixed by
 * making the verb WAIT for its own apply (5d04227).
 *
 * daw-cli gets the COUNTER right where the page did not — `handle.harmony_version()`, with a
 * comment saying harmony has its own counter. What it does not do is wait: it sends, prints
 * `{"sent": "harmony", "base_version": N}` and exits 0. So the question this file answers is
 * whether the process boundary is enough of a delay on its own, or whether a shell loop writing a
 * progression hits the same silent refusal one surface over.
 *
 * ── WHY IT IS WORTH ASKING RATHER THAN ASSUMING EITHER WAY ──────────────────────────────────
 *
 * Each invocation is a fresh process: spawn, attach to shared memory, read the counter, send, exit.
 * That is milliseconds of real work between the read and the previous command's apply, and it may
 * well be enough — which is exactly why it must be MEASURED. "Probably fine because processes are
 * slow" is a timing assumption about another thread's schedule, and this project has now been
 * bitten three times by precisely that reasoning.
 *
 * `cli-verbs.mjs` already warns in its header that daw-cli "exits 0 for a command the engine then
 * drops", so the exit code cannot answer this. The saved document can.
 *
 * ── WHAT A FAILURE HERE WOULD MEAN ──────────────────────────────────────────────────────────
 *
 * That `daw-cli do harmony` needs the same treatment as the sidecar's `send_harmony_and_await`:
 * wait for the counter to move past the base it quoted, and report the refusal instead of printing
 * "sent". A pass means the process boundary already provides the gap, and this file becomes the
 * guard that says so — so that nobody later "optimises" the CLI into a persistent connection and
 * reintroduces it without noticing.
 */

import { execFileSync } from 'node:child_process';
import { readFileSync, existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { startStack } from './stack.mjs';

const ROOT = resolve(new URL('../..', import.meta.url).pathname);
const Q = 960000;
const BAR = Q * 4;

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const stack = await startStack({ keepDir: true });

const cli = (...args) => {
  try {
    return { ok: true, out: execFileSync(join(ROOT, 'ui/target/release/daw-cli'), args,
      { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm, DAW_PROJECT_DIR: stack.dir },
        encoding: 'utf8', timeout: 15000 }) };
  } catch (e) {
    return { ok: false, out: String(e.stdout || '') + String(e.stderr || e.message || '') };
  }
};

/** The harmony events in the saved document — the oracle, since exit 0 proves nothing here. */
const savedHarmony = async (tag) => {
  cli('do', 'save', tag);
  for (let i = 0; i < 40; i++) {
    const p = join(stack.dir, `${tag}.uniproj.json`);
    if (existsSync(p)) {
      try {
        const doc = JSON.parse(readFileSync(p, 'utf8'));
        return (doc.harmony_timeline || []).map((h) => [h.nanotick, h.root]).sort((a, b) => a[0] - b[0]);
      } catch { /* mid-write */ }
    }
    await sleep(150);
  }
  return null;
};

console.log('\nfour key changes written by four daw-cli processes back to back\n');

const made = cli('do', 'new', 'cliharm');
check(made.ok, 'a project to write into', made.out.trim().slice(0, 120));
await sleep(1500);

/*
 * NOTHING AWAITED BETWEEN THEM beyond the spawn itself. Adding a settle here would measure a
 * different thing entirely — the question IS whether the process boundary suffices.
 */
const roots = [0, 3, 6, 10];
const sent = roots.map((r, i) =>
  cli('do', 'harmony', '--root', String(r), '--scale', '1', '--nanotick', String(i * BAR)));

check(sent.every((s) => s.ok), 'all four commands were accepted by the CLI',
      sent.map((s) => s.out.trim()).join(' | ').slice(0, 200));

// The bases each process quoted. If they are all the SAME, the engine saw four edits composed
// against one state and refused three — the exact shape the page had.
const bases = sent.map((s) => (s.out.match(/"base_version":\s*(\d+)/) || [])[1]).join(',');
console.log(`  the four processes quoted base_version: ${bases}`);

await sleep(2500);
const got = await savedHarmony('cliharm_out');

check(got !== null, 'the project saved', String(got));
check(got && got.length === 4,
      'ALL FOUR KEY CHANGES ARE IN THE DOCUMENT',
      `${got && got.length} landed: ${JSON.stringify(got)} — if the bases printed above are all `
      + 'equal, each process read the counter before the previous edit was applied and the engine '
      + 'refused the losers in silence. daw-cli would then need the same wait-for-apply the '
      + 'sidecar got in 5d04227');
check(got && JSON.stringify(got.map((h) => h[1])) === JSON.stringify(roots),
      'with the roots that were asked for, in bar order', JSON.stringify(got));

await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
