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
 *   node test/all.mjs --with-audio     also the five that measure sound through a capture
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
  // Same bargain as ask-live, different question: ask-live covers the agent's plumbing,
  // ai-demo types the sentences a person types and asserts the SONG changed. Both cost
  // money per run, so both are deliberate rather than swept.
  'ai-demo.mjs': 'asks a real model real questions — needs a key and costs money per run',
  'audible.mjs': 'live capture — see THE CAPTURE PROBLEM below',
  'chop-audible.mjs': 'live capture — see THE CAPTURE PROBLEM below',
  'sound-op-audible.mjs': 'live capture — see THE CAPTURE PROBLEM below',
  'note-off-cuts.mjs': 'live capture — see THE CAPTURE PROBLEM below',
  'sampler-device-id.mjs': 'live capture — see THE CAPTURE PROBLEM below',
  'repro-hang.mjs': 'a reproduction script for one bug, not a suite — it is meant to hang',
  'soak.mjs': 'minutes of heap soak; run it deliberately, not on every sweep',
};

/*
 * THE CAPTURE PROBLEM — why five suites are held out, stated once rather than five times.
 *
 * This machine produces no audio at all, and as of 2026-08-01 the root cause is OPEN. The engine
 * opens the default output, the device reports its name, rate and block size, JUCE's isPlaying()
 * answers TRUE — and CoreAudio never invokes the IO proc. Not device-specific: the built-in
 * speakers behave identically on a quiet machine with no other engine running.
 *
 * Every suite that measures SOUND does it through DAW_CAPTURE_WAV, which records what the device
 * is handed — so with no callbacks there is nothing to record, and the checks fail for a reason
 * that has nothing to do with the code they test.
 *
 * THE SIGNAL TO READ IS THE ONE AT THE DEVICE BOUNDARY:
 *
 *   Audio device callbacks: N from the DEVICE, M reaching the engine
 *
 * N of zero means the device never asked for audio, so nothing under test can cause it or fix
 * it. That line exists because the OLD one did not mean what both of us read it as: "Audio
 * underrun summary: 0 of 0 playback callbacks" counts callbacks THAT HAD A TRACK TO PLAY, so a
 * healthy device with the transport stopped prints 0 of 0 as well. I cited it here as the
 * independent second signal that made these exclusions safe, and it was not one — the exclusions
 * are right, and the reason I gave for them was wrong. Backend's counter at the boundary is the
 * measurement that actually distinguishes the two.
 *
 * The rule stands even though I misapplied it: an excuse is safe only when the second signal is
 * INDEPENDENT of the failure mode. "The producer reported no underruns" is not independent, which
 * is how note-off-cuts came to report a dead device as a hard failure of the sampler. Neither was
 * the counter I reached for instead. Check what a number counts before leaning on it.
 *
 * `--with-audio` runs all five, which is what to do on a machine whose device works.
 */

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
const audioOnes = ['audible.mjs', 'chop-audible.mjs', 'sound-op-audible.mjs',
                   'note-off-cuts.mjs', 'sampler-device-id.mjs'];
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
  console.log('  (--with-audio adds the five that measure sound through a live capture)');
}

console.log(`\n${failed.length === 0
  ? `all ${results.length} suites passed`
  : `${failed.length} of ${results.length} suites FAILED: ${failed.map((r) => r.file).join(', ')}`}`);
process.exit(failed.length === 0 ? 0 : 1);
