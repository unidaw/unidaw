/**
 * EVERY ENGINE-BACKED SUITE, IN ONE RUN.
 *
 * `npm run e2e` runs e2e.mjs and nothing else. There are twenty-eight suites that
 * stand up a real engine, and twenty-seven of them ran only when somebody remembered
 * them by name — which is how several went weeks without being run at all, and how a
 * suite can rot into permanent red without anyone noticing it went red.
 *
 * SEQUENTIALLY, not in parallel. Each suite starts an engine, a plugin host and a
 * browser; running several at once starves the audio producer and turns real checks
 * into load-average failures that say nothing about the code.
 *
 * AND IT NAMES WHAT IT DID NOT RUN, every time. A runner that quietly covers less than
 * it appears to is worse than no runner: "all suites passed" has to mean the set it
 * printed, not the set that happened to be cheap today. The excluded ones are listed
 * with their reason on every run, so the exclusions are read as often as the results.
 *
 *   node test/all.mjs                  the deterministic set
 *   node test/all.mjs --with-audio     also the three that need a working output device
 *   node test/all.mjs --only kit,ops   just these
 */

import { spawn } from 'node:child_process';
import { readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));

/*
 * WHY EACH EXCLUSION, in the file rather than in someone's head. A reason nobody
 * re-reads is indistinguishable from a real limitation, so these are printed.
 */
const EXCLUDED = {
  'ask-live.mjs': 'needs an API key and the network, and costs money per run',
  'audible.mjs': 'needs a working audio OUTPUT device; this machine\'s default opens and never calls back',
  'chop-audible.mjs': 'needs a working audio output device (see audible.mjs)',
  'sound-op-audible.mjs': 'needs a working audio output device (see audible.mjs)',
  'repro-hang.mjs': 'a reproduction script for one bug, not a suite — it is meant to hang',
  'soak.mjs': 'minutes of heap soak; run it deliberately, not on every sweep',
};

/** Not suites: helpers, fixtures, and the ones with their own npm script. */
const NOT_A_SUITE = new Set([
  'stack.mjs', 'serve.mjs', 'wav.mjs', 'all.mjs',
  'unit.mjs',            // node --test, `npm run unit`
  'shot.mjs', 'alloc.mjs', 'alloc-where.mjs', 'frametime.mjs', 'scale.mjs', 'layout.mjs',
]);

const argv = process.argv.slice(2);
const withAudio = argv.includes('--with-audio');
const onlyArg = argv.find((a) => a.startsWith('--only'));
const only = onlyArg
  ? new Set((onlyArg.includes('=') ? onlyArg.split('=')[1] : argv[argv.indexOf(onlyArg) + 1] || '')
      .split(',').map((s) => s.trim()).filter(Boolean).map((s) => (s.endsWith('.mjs') ? s : s + '.mjs')))
  : null;

const files = readdirSync(HERE).filter((f) => f.endsWith('.mjs')).sort();
const audioOnes = ['audible.mjs', 'chop-audible.mjs', 'sound-op-audible.mjs'];
const skipped = [];
const suites = files.filter((f) => {
  if (NOT_A_SUITE.has(f)) return false;
  if (only) return only.has(f);
  if (EXCLUDED[f]) {
    if (withAudio && audioOnes.includes(f)) return true;
    skipped.push([f, EXCLUDED[f]]);
    return false;
  }
  return true;
});

if (only) {
  const missing = [...only].filter((f) => !suites.includes(f));
  if (missing.length) {
    console.error(`no such suite: ${missing.join(', ')}`);
    process.exit(2);
  }
}

/** The last line a suite prints that states its own result. */
const SUMMARY = /(ALL PASS[^\n]*|\d+ of \d+ FAILED|\d+ FAILURES?|# fail \d+)/g;

const run = (file) => new Promise((resolve) => {
  const started = process.hrtime.bigint();
  const child = spawn(process.execPath, [join(HERE, file)], { cwd: join(HERE, '..', '..') });
  let out = '';
  child.stdout.on('data', (d) => { out += d; });
  child.stderr.on('data', (d) => { out += d; });
  child.on('close', (code) => {
    const secs = Number(process.hrtime.bigint() - started) / 1e9;
    const found = out.match(SUMMARY);
    resolve({ file, code, secs, summary: found ? found[found.length - 1] : '(no summary line)', out });
  });
});

console.log(`running ${suites.length} engine-backed suites, one at a time\n`);
const results = [];
for (const f of suites) {
  process.stdout.write(`  ${f.padEnd(24)}`);
  const r = await run(f);
  results.push(r);
  console.log(`${r.code === 0 ? 'ok  ' : 'FAIL'}  ${r.secs.toFixed(0)}s  ${r.summary}`);
}

const failed = results.filter((r) => r.code !== 0);

/*
 * A FAILING SUITE PRINTS ITS OWN FAILING LINES HERE. Otherwise the sweep tells you
 * that something broke and makes you re-run it to find out what, which is how a
 * sweep becomes a thing people skip.
 */
for (const r of failed) {
  console.log(`\n─── ${r.file} ───`);
  const lines = r.out.split('\n').filter((l) => /FAIL|not ok|Error|error:/i.test(l));
  console.log(lines.slice(0, 20).map((l) => '  ' + l.trim()).join('\n') || '  (no FAIL lines — check the suite\'s own output)');
}

if (skipped.length) {
  console.log(`\n${skipped.length} NOT RUN, and why:`);
  for (const [f, why] of skipped) console.log(`  ${f.padEnd(24)} ${why}`);
  console.log('  (--with-audio adds the three that need an output device)');
}

console.log(`\n${failed.length === 0
  ? `all ${results.length} suites passed`
  : `${failed.length} of ${results.length} suites FAILED: ${failed.map((r) => r.file).join(', ')}`}`);
process.exit(failed.length === 0 ? 0 : 1);
