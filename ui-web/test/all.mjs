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

import { spawn, execSync } from 'node:child_process';
import { readdirSync, statSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
// This worktree's root. Used to tell OUR engine processes from the other agent's, which run
// from a different checkout and must never be touched.
const ROOT = join(HERE, '..', '..');

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
  /*
   * `sampler-device-id.mjs` USED TO BE HERE and is not any more. It was held out for the
   * capture problem below, and it no longer takes a capture — its audio verdict comes from an
   * offline render, which has no device, no origin and no starvation. The structural half still
   * drives a live stack, but nothing in it depends on the tap.
   *
   * Worth stating because the exclusion outlived its reason by exactly one commit: the suite was
   * converted in the morning and stayed out of the sweep, so seventeen checks including the one
   * that catches a sampler landing on the no-device sentinel were running nowhere.
   */
  /*
   * Needs a stack SOMEBODY ELSE started — `tools/webstack.sh`, the demo's own — so it cannot boot
   * one for itself, and starting a second engine mid-sweep collides with the one all.mjs is
   * driving. Run it by hand before a demo; it is the only thing here that touches webstack.sh at
   * all, and a green sweep says nothing about whether the demo starts.
   */
  'demo-stack-smoke.mjs': 'needs a running tools/webstack.sh — run it by hand before a demo',
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
  // `notes.mjs` is a LIBRARY — YIN pitch and onset detection — like wav.mjs beside it. Left in
  // the discovery set it would be spawned as a suite, export its functions, exit 0 and be
  // reported as a pass, which is a green line for a file that asserts nothing. Its own coverage
  // is in unit.mjs.
  'stack.mjs', 'serve.mjs', 'wav.mjs', 'notes.mjs', 'all.mjs',
  'unit.mjs',            // node --test, `npm run unit`
  'shot.mjs', 'alloc.mjs', 'alloc-where.mjs', 'frametime.mjs', 'scale.mjs', 'layout.mjs',
]);

/**
 * How long one suite may take before the sweep stops waiting for it.
 *
 * Not a performance budget — e2e really does take five minutes and chrome.mjs has been
 * measured at 320s under load. This is only the point past which "slow" is better treated
 * as "stuck", so the remaining suites still get to run and the report says which one hung.
 */
const HANG_MS = 12 * 60 * 1000;

/*
 * OLD STACK DIRECTORIES ARE PRUNED BEFORE THE SWEEP STARTS.
 *
 * Most suites call `startStack({ keepDir: true })` so their engine log and saved projects survive
 * for debugging — which is right, and is how several bugs were found today. Nothing ever removed
 * them. Measured 2026-08-08: 1257 `uni-e2e-*` directories holding 19 GB, on a disk at 92%.
 *
 * Whether that pressure contributes to the flakes this harness now reports is NOT established,
 * and this is not offered as a fix for them. It is a leak either way: a day of sweeps should not
 * cost twenty gigabytes, and a directory with twelve hundred siblings is a worse place to create
 * the next one.
 *
 * SIX HOURS, so anything from the current session is untouched — a suite is killed at twelve
 * minutes, so nothing this old can still be running — and today's failures stay inspectable for
 * the rest of a working day. Failures are exactly what keepDir is for.
 *
 * Best effort: a directory that will not delete is not worth failing a sweep over.
 */
function pruneOldStacks() {
  const dir = tmpdir();
  const cutoff = Date.now() - 6 * 60 * 60 * 1000;
  let gone = 0;
  try {
    for (const name of readdirSync(dir)) {
      if (!name.startsWith('uni-e2e-')) continue;
      const p = join(dir, name);
      try {
        if (statSync(p).mtimeMs < cutoff) { rmSync(p, { recursive: true, force: true }); gone++; }
      } catch { /* vanished, or not ours to remove */ }
    }
  } catch { /* no temp dir to read: nothing to prune */ }
  if (gone) console.log(`pruned ${gone} stack director${gone === 1 ? 'y' : 'ies'} older than 6h\n`);
}
pruneOldStacks();

const argv = process.argv.slice(2);
const withAudio = argv.includes('--with-audio');
const onlyArg = argv.find((a) => a.startsWith('--only'));
const only = onlyArg
  ? new Set((onlyArg.includes('=') ? onlyArg.split('=')[1] : argv[argv.indexOf(onlyArg) + 1] || '')
      .split(',').map((s) => s.trim()).filter(Boolean).map((s) => (s.endsWith('.mjs') ? s : s + '.mjs')))
  : null;

const files = readdirSync(HERE).filter((f) => f.endsWith('.mjs')).sort();
// `--with-audio` opts the capture suites back in. sampler-device-id is NOT here any more: it
// renders offline and is swept unconditionally.
const audioOnes = ['audible.mjs', 'chop-audible.mjs', 'sound-op-audible.mjs',
                   'note-off-cuts.mjs'];
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
  /*
   * EVERY SUITE KEEPS ITS STACK DIRECTORY DURING A SWEEP.
   *
   * The suites that flake are exactly the ones that do not ask for keepDir, so the engine log
   * naming the cause was deleted before this runner learned the suite had failed — kit.mjs and
   * params.mjs both flaked on 2026-08-08 and both take their evidence with them. The runner
   * cannot pass keepDir (the suite constructs its own stack), so it says so through the
   * environment and stack.mjs honours it.
   *
   * Bounded by pruneOldStacks() above: six hours, which is long enough to read today's failure
   * and short enough that a day of sweeps does not cost twenty gigabytes again.
   */
  const child = spawn(process.execPath, [join(HERE, file)], {
    cwd: join(HERE, '..', '..'),
    env: { ...process.env, DAW_KEEP_STACK: '1' },
  });
  let out = '';
  child.stdout.on('data', (d) => { out += d; });
  child.stderr.on('data', (d) => { out += d; });

  /*
   * A SUITE THAT HANGS MUST NOT HANG THE SWEEP.
   *
   * There was no bound here at all, so one stuck suite blocked every one after it, for as
   * long as anyone left it. `panic.mjs` did exactly that: its browser died mid-run and an
   * `await page.*` never settled, so the node process sat in uv__io_poll with nothing to
   * wake it — fifteen minutes, two days before a demo, and the sweep looked like it was
   * simply slow.
   *
   * The ceiling is generous on purpose. e2e legitimately takes five minutes and chrome has
   * been seen at 320s under load, so this is not a performance assertion — it is the
   * difference between a sweep that finishes and one that has to be noticed.
   *
   * SIGTERM first, then SIGKILL, because a suite killed outright leaves its engine and
   * sidecar behind: its own `stop()` is what reaps them, and it needs a moment to run.
   */
  const bomb = setTimeout(() => {
    out += `\n\n*** killed after ${(HANG_MS / 1000) | 0}s — this suite hung ***\n`;
    try { child.kill('SIGTERM'); } catch { /* already gone */ }
    setTimeout(() => { try { child.kill('SIGKILL'); } catch { /* already gone */ } }, 5000);
  }, HANG_MS);

  child.on('close', (code) => {
    clearTimeout(bomb);
    const secs = Number(process.hrtime.bigint() - started) / 1e9;
    const found = out.match(SUMMARY);
    resolve({ file, code, secs, summary: found ? found[found.length - 1] : '(no summary line)', out });
  });
});

/*
 * BETWEEN SUITES THERE SHOULD BE NO ENGINE AT ALL, and when there is, NAME WHO LEFT IT.
 *
 * Sweep 29 ran with an engine and sidecar alive from three minutes in — still running thirty-two
 * minutes later, while every suite before the one then executing had reported ok. Nothing noticed.
 * A survivor is not just untidy: stack.mjs's own comment says "a survivor holds the audio device
 * and the next run blocks on it, which reads as SILENCE rather than as a leak", and the suite
 * running alongside that orphan (chrome.mjs) hung past the twelve-minute kill and then passed on
 * its retry once the orphan was gone. That is suggestive, not proof — but an invisible leak cannot
 * be investigated at all, and a named one can.
 *
 * MATCHED BY THIS REPO'S OWN BINARY PATHS, never by process name. `pkill -f daw_engine` is
 * recorded in this project's notes as a mistake that has cost real time: the other agent runs its
 * engine from a different checkout, and a pattern kill takes theirs too. ROOT is this worktree, so
 * a process outside it is somebody else's and is left strictly alone.
 *
 * Killed rather than merely reported, because the next suite inherits the problem otherwise; the
 * warning is what makes the leak findable.
 */
const reapStrays = (afterFile) => {
  let out = '';
  try {
    out = execSync('ps -eo pid=,args=', { encoding: 'utf8' });
  } catch { return; }
  const mine = [];
  for (const line of out.split('\n')) {
    const m = line.trim().match(/^(\d+)\s+(.*)$/);
    if (!m) continue;
    const [, pid, args] = m;
    if (!args.includes(ROOT)) continue;                 // not from this worktree: not ours
    if (!/build\/daw_engine|target\/release\/daw-sidecar/.test(args)) continue;
    mine.push(Number(pid));
  }
  if (!mine.length) return;
  console.log(`\n  !! ${afterFile} left ${mine.length} engine/sidecar process(es) running — `
            + 'killing them. A survivor holds the audio device and the next suite blocks on it, '
            + 'which reads as silence rather than as a leak.');
  for (const pid of mine) { try { process.kill(pid, 'SIGKILL'); } catch { /* already gone */ } }
};

/*
 * CLEAR THE DECK FIRST, so the attribution below is honest.
 *
 * reapStrays names the suite that just ran, which is right during the sweep and wrong for
 * anything already running when it started — a hand-run suite that leaked, or a stack somebody
 * left up. Without this the first suite gets blamed for it, and a wrong name in a leak report is
 * worse than no report: it sends the next reader to a suite that is innocent.
 */
reapStrays('(before the sweep started)');

/*
 * WHAT ELSE IS ON THIS MACHINE, recorded at both ends of the sweep.
 *
 * Sweep 32 reported two failures and four flaky suites. Every one of them was a TIMING shape — an
 * offline render that hit spawnSync ETIMEDOUT, a chain that had not been published yet, a device
 * not yet visible — and none was a wrong answer. sampler-device-id took 1050s and timed out inside
 * the sweep, then passed STANDALONE in 50s on the same binaries. The cause was mds_stores at 84%
 * CPU and two mediaanalysisd processes at 48% and 18%, all running for two days: load average 6.
 *
 * The report had no way to say that, so it said "FAILED" instead — and a gate that cannot tell a
 * slow machine from a broken build is one a reader has to second-guess every time. This does not
 * excuse anything and must not: a failure is still a failure and the exit code is unchanged. It
 * just puts the one measurement that distinguishes the two cases in the report, so nobody has to
 * think to run `ps -r` an hour later and hope the evidence is still there. It usually is not.
 *
 * `%cpu` and not just elapsed time, because the daemons that do this have been up for days — a
 * check by age alone sees nothing unusual about any of them.
 */
function machineLoad() {
  try {
    const load = execSync('uptime', { encoding: 'utf8' }).trim().replace(/^.*load averages?:/, '').trim();
    const top = execSync("ps -r -eo %cpu=,comm= | head -3", { encoding: 'utf8' })
      .trim().split('\n')
      .map((l) => {
        const [pct, ...rest] = l.trim().split(/\s+/);
        return `${rest.join(' ').split('/').pop()} ${pct}%`;
      }).join(', ');
    return { load, top };
  } catch { return { load: '?', top: '?' }; }
}

const loadBefore = machineLoad();
console.log(`running ${suites.length} engine-backed suites, one at a time`);
console.log(`  machine at start: load ${loadBefore.load} · busiest: ${loadBefore.top}\n`);
const results = [];
for (const f of suites) {
  process.stdout.write(`  ${f.padEnd(24)}`);
  let r = await run(f);
  // Before anything else, including the retry: a survivor from THIS suite must not be inherited
  // by the next one, and the retry is a "next one" too.
  reapStrays(f);
  /*
   * ONE RETRY, AND THE RESULT SAYS WHICH KIND OF FAILURE IT WAS.
   *
   * This is NOT here to make red sweeps green. It is here because the sweep had stopped
   * carrying information. Four consecutive sweeps each reported one to three failures, a
   * DIFFERENT suite every time, and every one of them passed when run on its own:
   *
   *   19  demo-walk, sampler-from-ui, sampler-render      all passed alone
   *   20  e2e (hung at 1569s vs 312s)                     passed alone, 429 checks
   *   21  e2e, harmony-quantize, params                   params was REAL and is fixed
   *   22  full-song, sampler-device-id                    passed alone, twice
   *
   * With that noise floor a green sweep and a red sweep say the same thing, and the honest
   * reading of "3 of 64 FAILED" became "run them again and see" — which is what a person then
   * did by hand, every time, for hours.
   *
   * So the sweep does it, and REPORTS THE DIFFERENCE, which is the part that matters:
   *
   *   ok            passed first time
   *   ok (FLAKY)    failed, then passed — counted as a PASS for the exit code, and listed
   *                 separately at the end so it cannot be mistaken for a clean run
   *   FAIL          failed twice — a real failure, and the exit code says so
   *
   * WHAT "FAIL" DOES AND DOES NOT MEAN. It was written here that two failures in a row put a
   * suite far outside the observed flake rate and so made it a real bug. That is WRONG, and
   * sweep 25 showed it: open-patcher failed BOTH attempts and then passed three times out of
   * three when run alone. The retry runs seconds after the first, against whatever state the
   * preceding suites left behind — so any cause that persists across both attempts survives the
   * retry untouched, and the second run is not an independent sample.
   *
   * So FAIL means "failed twice", nothing more. It is worth more attention than FLAKY and it is
   * still not proof. Run it alone before believing it.
   *
   * WHAT THIS DELIBERATELY DOES NOT DO is hide the flake. A flaky suite is a defect in the
   * gate and stays visible under its own heading, because the failure mode this project keeps
   * writing down is a check that passes for the wrong reason — and "retry until green" with no
   * record would be exactly that, built into the harness.
   */
  let flaky = false;
  if (r.code !== 0) {
    const first = r;
    const second = await run(f);
    reapStrays(f);
    if (second.code === 0) {
      flaky = true;
      r = { ...second, firstOut: first.out, firstSummary: first.summary, firstSecs: first.secs };
    } else {
      r = { ...second, firstSummary: first.summary };
    }
  }
  r.flaky = flaky;
  results.push(r);
  const label = r.code !== 0 ? 'FAIL' : (flaky ? 'ok (FLAKY)' : 'ok  ');
  console.log(`${label}  ${r.secs.toFixed(0)}s  ${r.summary}`);
}

const flakes = results.filter((r) => r.flaky);
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

/*
 * THE FLAKES GET THEIR OWN HEADING, and their FIRST run's failing lines.
 *
 * A suite that passes on retry is not a pass — it is a suite that cannot be trusted to say
 * anything, and the failing lines from the run that failed are the only evidence of why. They
 * are printed here rather than discarded, because the next person to look at this is trying to
 * work out whether one root cause explains several of them (so far: kit answers and chain
 * snapshots that intermittently do not arrive in the time the suite allows).
 */
if (flakes.length) {
  console.log(`\n${flakes.length} FLAKY — failed, then passed on a retry. Not a clean sweep:`);
  for (const r of flakes) {
    console.log(`  ${r.file.padEnd(24)} first run: ${r.firstSummary} (${(r.firstSecs || 0).toFixed(0)}s)`);
    /*
     * HARNESS WARNINGS ARE NOT FAILURES, and one of them says so in a way this filter used to
     * believe: stack.mjs warns that "a target that FAILED to build leaves its old binary in
     * place", which matches /FAIL/ and outranked nothing — it simply filled all six lines and
     * pushed the real cause out. A whole sweep was diagnosed from those two warnings before
     * anyone noticed they were about mtimes. Suite output only.
     */
    /*
     * RANKED, NOT FILTERED — and the second version of this was as wrong as the first.
     *
     * Originally this matched /FAIL/ and let stack.mjs's staleness warning ("a target that FAILED
     * to build leaves its old binary in place") fill all six slots, pushing the suite's own
     * failures out. I fixed that by DROPPING stack: lines. Then a merge left juce_host_process 55
     * hours older than the event_payloads.h it speaks, stack.mjs said so precisely, and under the
     * new filter that line would have been thrown away — the one time it was the whole diagnosis.
     *
     * So: a suite's own failures first, harness warnings after, and the warnings only take the
     * space nobody else wants. Neither crowds the other out.
     */
    const all = (r.firstOut || '').split('\n').filter((l) => /FAIL|not ok|Error|error:/i.test(l));
    const harness = all.filter((l) => /^\s*stack: /.test(l));
    const own = all.filter((l) => !/^\s*stack: /.test(l));
    const lines = [...own, ...harness];
    console.log(lines.slice(0, 6).map((l) => '      ' + l.trim()).join('\n')
      || '      (no failure line at all — see the duration above: a run many times its usual '
         + 'length was killed as a hang, and a hang prints nothing)');
  }
}

if (skipped.length) {
  console.log(`\n${skipped.length} NOT RUN, and why:`);
  for (const [f, why] of skipped) console.log(`  ${f.padEnd(24)} ${why}`);
  console.log('  (--with-audio adds the five that measure sound through a live capture)');
}

const flakeNote = flakes.length ? ` — ${flakes.length} FLAKY (${flakes.map((r) => r.file).join(', ')})` : '';
/*
 * ALWAYS, not only when something failed. I recorded the START load unconditionally and the END
 * load only on failure, then spent several sweeps explaining flakes with the start number — and a
 * table across sweeps 33-42 refutes it: sweep 42 started at the LOWEST load of the ten (3,77) and
 * produced the MOST flakes (7), while sweep 34 started highest (6,71) and produced 4.
 *
 * What actually moved in 42 was load DURING the run, 3,77 at the start to 6,07 at the end, with
 * the flakes clustered on the alphabetically-late suites where it was highest. A one-sample-at-
 * the-door measurement cannot see that, and a measurement only taken when the news is bad cannot
 * be compared against the runs where it was good. Both ends, every run, or the numbers cannot be
 * used for anything.
 */
{
  const after = machineLoad();
  console.log(`\nMACHINE: load ${loadBefore.load} at the start, ${after.load} now`
              + ` · busiest now: ${after.top}`);
}
if (failed.length || flakes.length) {
  console.log('  A load average near or above the core count means every timing-sensitive suite '
              + 'here was competing for CPU.\n  It does not make a failure less real — but a suite '
              + 'that takes many times its usual seconds and then\n  passes on a retry is reporting '
              + 'the machine, not the build. Re-run it alone before believing it.');
}
console.log(`\n${failed.length === 0
  ? `all ${results.length} suites passed${flakeNote}`
  : `${failed.length} of ${results.length} suites FAILED: ${failed.map((r) => r.file).join(', ')}${flakeNote}`}`);
process.exit(failed.length === 0 ? 0 : 1);
