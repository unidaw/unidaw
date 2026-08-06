/**
 * A disposable engine + sidecar + page server, for tests that edit documents.
 *
 * The e2e suite used to run against whatever stack happened to be up — which in
 * practice was the owner's, the one in use at the time. Every run added devices, wrote
 * notes and loaded projects into a long-lived engine, so runs were not
 * independent: the first pass was green, the third failed twelve checks, and the
 * fourth crashed. I started diagnosing a patcher bug that did not exist. The
 * suite had simply been editing the same document all afternoon.
 *
 * A test that edits state must own the state it edits. This starts a private
 * engine on its own shared-memory segment, with its own COPY of the projects
 * directory, and its own ports; nothing it does can reach the stack a person is
 * using, and every run begins from the same bytes.
 *
 * Ports are FOUND, not fixed, so two runs can coexist — CI, or me running the
 * suite while the app is open in front of someone, or the several `tools/webstack.sh`
 * instances other agents keep alive in this repo. See findFreeBase.
 */

import { spawn } from 'node:child_process';
import { statSync, readdirSync } from 'node:fs';
import { mkdtempSync, cpSync, rmSync, existsSync, readFileSync, unlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));   // repo root
const UIWEB = fileURLToPath(new URL('..', import.meta.url));

const bin = (p) => join(ROOT, p);

/** Wait until `probe()` is true, or throw with a diagnosis. */
async function until(probe, what, ms = 25000, every = 250) {
  const t0 = Date.now();
  for (;;) {
    let ok = false;
    try { ok = await probe(); } catch { ok = false; }
    if (ok) return;
    if (Date.now() - t0 > ms) throw new Error(`stack: timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, every));
  }
}

async function listening(port) {
  // A connect attempt, not a fetch: the sidecar's ports speak websocket and a
  // GET would be answered oddly or not at all. All we need to know is that
  // something has bound the port.
  const net = await import('node:net');
  return new Promise((resolve) => {
    const s = net.connect({ port, host: '127.0.0.1' }, () => { s.destroy(); resolve(true); });
    s.on('error', () => resolve(false));
    setTimeout(() => { s.destroy(); resolve(false); }, 400);
  });
}

/**
 * Refuse a port somebody else is already on.
 *
 * Without this the harness "came up in 11ms" — it had found a stack from a
 * previous run still listening, attached the tests to it, and reported success.
 * Every guarantee this file exists to make (own engine, own documents, same
 * bytes every run) was void, silently, in the one direction that looks like
 * everything working.
 */
async function demandFree(ports) {
  for (const p of ports) {
    if (await listening(p)) {
      throw new Error(`stack: port ${p} is already in use — a previous run is still `
                    + `up, or something else has it. Refusing to attach to it.`);
    }
  }
}

/**
 * @param {object} opts
 * @param {number} [opts.base] page port; sidecar takes base+1 and base+2, which
 *   is the relationship index.html hardcodes.
 * @param {string} [opts.shm] shared-memory name.
 * @param {boolean} [opts.keepDir] leave the project copy on disk for inspection.
 */
/**
 * Three CONSECUTIVE free ports.
 *
 * Consecutive because index.html derives its sockets from the page port —
 * `ws://127.0.0.1:{port+1}` for state and `+2` for commands — so the triple is
 * the unit, not the single port.
 *
 * Found rather than fixed. A fixed base collided with `tools/webstack.sh`, which
 * several agents run in this repo at once; the harness then attached to a stack
 * it did not start and did not control, and reported "up in 11ms". Asking the OS
 * for a port it is willing to give is the only version of this that cannot be
 * wrong about who owns it.
 */
async function findFreeBase(tries = 40) {
  const net = await import('node:net');
  const grab = () => new Promise((resolve, reject) => {
    const s = net.createServer();
    s.on('error', reject);
    s.listen(0, '127.0.0.1', () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });
  for (let i = 0; i < tries; i++) {
    const p = await grab();
    // The OS gave us p; p+1 and p+2 are only ours if nobody else holds them.
    if (!(await listening(p + 1)) && !(await listening(p + 2))) return p;
  }
  throw new Error('stack: could not find three consecutive free ports');
}

/**
 * @param {string} [opts.capture] write the engine's master output here as WAV.
 *   The engine takes this at startup only, so a test that wants to hear what it
 *   built has to ask for it before the first note exists.
 * @param {number} [opts.captureSeconds] how much to keep.
 */
/**
 * @param {number} [opts.numBlocks] pipeline depth, DAW_ENGINE_NUM_BLOCKS.
 *   A test machine also running several browsers and engines starves the audio
 *   producer: 1237 of 2759 playback callbacks dropped a track in one run, and the
 *   capture came out as perfect silence. That is not the application failing, it
 *   is the box being busy, and a deeper pipeline is the documented lever for it.
 */
export async function startStack({ base = 0, shm = '', keepDir = false,
                                   capture = '', captureSeconds = 30,
                                   runSeconds = 0, numBlocks = 0 } = {}) {
  const procs = [];
  if (!base) base = await findFreeBase();
  // The segment name has to be unique too, or two runs share one engine's memory
  // — the same collision one layer down, and a much harder one to see.
  if (!shm) shm = `/daw_e2e_${base}`;
  const root = mkdtempSync(join(tmpdir(), 'uni-e2e-'));
  // The WHOLE presets tree, not just projects/. An audio clip stores its file as
  // `../audio/waveform_probe.wav` — relative to the project directory — so a copy
  // of projects/ alone leaves every audio source dangling one level up. Copying
  // presets/ and pointing DAW_PROJECT_DIR at its projects/ keeps the relative
  // paths meaning what they meant in the repo.
  //
  // A copy, not the real thing, so a test that saves cannot rewrite the fixtures
  // everything else reads.
  cpSync(join(ROOT, 'presets'), root, { recursive: true });
  const dir = join(root, 'projects');

  const engineBin = bin('build/daw_engine');
  const hostBin = bin('build/juce_host_process');
  const sidecarBin = bin('ui/target/release/daw-sidecar');
  for (const [p, what] of [[engineBin, 'daw_engine'], [sidecarBin, 'daw-sidecar']]) {
    if (!existsSync(p)) throw new Error(`stack: ${what} not built at ${p}`);
  }

  /*
   * A BINARY OLDER THAN THE CONTRACT IT SPEAKS.
   *
   * The C++ engine and the Rust sidecar are separate builds over a SHARED contract — the headers
   * in apps/ and the wire in ui/. Merging touches both, and rebuilding one is a mistake with no
   * symptom of its own: the stale side simply does not know about a field, so the feature that
   * needs it looks broken and every test around it passes. That happened three times in one
   * night — a missing SetRowOps, a kit version reading 0, a command the engine ignored — and
   * each cost a debugging round that started by suspecting the new code.
   *
   * So the stack says so. A WARNING and not a throw: a stale binary is usually a mistake and
   * occasionally deliberate (bisecting an old engine against new tests is a real thing to do),
   * and a suite that refuses to run is worse than one that tells you what it is running.
   */
  /*
   * PER-LANGUAGE PARTITIONS, each binary against the sources it is actually built from.
   *
   * The first version compared every binary against the newest of ALL the contract files, and
   * backend caught the defect in their copy of it: that marks a binary stale for an edit its own
   * build cannot clear. Mine had the mirror of the same bug — a Rust-only edit marked
   * `daw_engine` stale, and cmake will never rebuild the engine for a change to main.rs, so it
   * sat permanently red. A guard that cannot be made green gets ignored exactly like one that
   * fires constantly, which is the failure mode both versions were reaching for.
   *
   * (Their specific case — cargo unable to clear a C++ edit — does not apply here: bindgen's
   * build script watches the headers, so `cargo build` genuinely relinks. Verified rather than
   * assumed, by touching a header and watching the binary's mtime move.)
   *
   * The cross-language case still lands, because a real contract change touches both mirrors:
   * shared_memory.h and layout.rs move together or the wire is already broken.
   */
  /*
   * THE RUST SIDE IS A RULE PER BINARY, AND BOTH HALVES OF THAT MATTER.
   *
   * It was a hand-maintained list naming `daw-bridge/src/layout.rs` and `daw-sidecar/src/main.rs`.
   * The sidecar also links daw-agent — every agent tool is compiled into it — so eight new tools
   * could be newer than the binary running them and this guard said nothing. daw-cli was not
   * watched at all, while several suites shell out to it, including the one whose whole purpose
   * is proving its verbs work. A list of files is wrong the moment a crate is added, and nobody
   * adding a crate comes here.
   *
   * PER BINARY, because the first fix — one Rust partition over the whole workspace — recreated
   * the defect this file already documents two paragraphs up. Editing daw-agent marked `daw-cli`
   * stale, and cargo will NEVER relink daw-cli for that edit: it does not depend on daw-agent. A
   * warning that cannot be cleared by doing what it asks is ignored exactly like one that never
   * fires, and I reintroduced it within an hour of reading the comment explaining it.
   *
   * So each binary is measured against the crates it actually links, from Cargo.toml:
   *   daw-sidecar -> daw-sidecar, daw-agent, daw-bridge
   *   daw-cli     -> daw-cli, daw-bridge
   */
  const rustSources = (d, out = []) => {
    if (!existsSync(d)) return out;
    for (const name of readdirSync(d)) {
      if (name === 'target' || name === 'node_modules' || name === '.git') continue;
      const p2 = join(d, name);
      const st = statSync(p2);
      if (st.isDirectory()) rustSources(p2, out);
      else if (name.endsWith('.rs') || name === 'Cargo.toml') out.push(p2);
    }
    return out;
  };
  const crates = (...names) => names.flatMap((n) => rustSources(bin(join('ui', n))));
  const CARGO_FIX = 'cargo build --release --manifest-path ui/Cargo.toml';
  const PARTITIONS = [
    { srcs: ['apps/shared_memory.h', 'apps/event_payloads.h'].map((f) => bin(f)),
      bins: [[engineBin, 'daw_engine'], [hostBin, 'juce_host_process']],
      fix: 'cmake --build build --target daw_engine juce_host_process -j 8' },
    { srcs: crates('daw-sidecar', 'daw-agent', 'daw-bridge'),
      bins: [[sidecarBin, 'daw-sidecar']], fix: CARGO_FIX },
    { srcs: crates('daw-cli', 'daw-bridge'),
      bins: [[bin('ui/target/release/daw-cli'), 'daw-cli']], fix: CARGO_FIX },
  ];
  for (const part of PARTITIONS) {
    let newest = 0, newestName = '';
    for (const p2 of part.srcs) {
      if (!existsSync(p2)) continue;
      const t = statSync(p2).mtimeMs;
      if (t > newest) { newest = t; newestName = p2.replace(ROOT, ''); }
    }
    for (const [p2, what] of part.bins) {
      if (!existsSync(p2) || !newest) continue;
      const age = statSync(p2).mtimeMs;
      if (newest <= age) continue;
      const mins = Math.round((newest - age) / 60000);
      /*
       * A BROKEN BUILD LOOKS EXACTLY LIKE A SKIPPED ONE, which backend hit and which is the
       * stronger case for this check than the one it was written for: a target that fails to
       * compile leaves its previous binary in place, and that binary keeps passing every test.
       * Four hours of green on a host that had not built since.
       */
      console.warn(`stack: ${what} is ${mins} min older than ${newestName} — it may not know `
                   + 'about a field the other side is sending, and a target that FAILED to build '
                   + `leaves its old binary in place and passing. Rebuild: ${part.fix}`);
    }
  }

  await demandFree([base, base + 1, base + 2]);

  const env = {
    ...process.env,
    DAW_UI_SHM_NAME: shm,
    DAW_PROJECT_DIR: dir,
    DAW_HOST_BINARY: hostBin,
  };
  if (capture) {
    env.DAW_CAPTURE_WAV = capture;
    env.DAW_CAPTURE_SECONDS = String(captureSeconds);
    /*
     * DELETE LAST RUN'S CAPTURE. HERE, because this is where the path becomes this run's.
     *
     * A suite that asks for a capture and then reads it back has no way to tell "the engine
     * wrote nothing" from "the engine wrote nothing and I am reading the file from last
     * time" — the second is a PASS, on evidence from a different run of different code.
     *
     * Found 2026-08-01 with the device dead: `/tmp/bypass_check.wav` was 38 hours old and
     * bypass.mjs had been reporting ALL PASS off it, including through two full sweeps that
     * I then quoted as green. mods, panic and patchcfg read their captures the same way and
     * had the same hole. note-off-cuts and three others delete theirs and carry a comment
     * about why; putting it in each suite meant six of ten did not.
     *
     * A guard that is satisfied by its own leftovers is not a guard, and one that every
     * caller has to remember is one half the callers will not.
     */
    try { unlinkSync(capture); } catch { /* absent is the normal case */ }
  }
  if (numBlocks) env.DAW_ENGINE_NUM_BLOCKS = String(numBlocks);
  // Where the API key lives, if the caller has said. The agent loop reads this
  // at ask time; a stack started without it simply cannot ask, which is the
  // right failure and the one it reports.
  if (process.env.DAW_ENV_FILE) env.DAW_ENV_FILE = process.env.DAW_ENV_FILE;
  // Logs to disk, not /dev/null. The first version discarded them, so when the
  // engine fell back to a stand-in plugin the only evidence was a wrong device
  // name three sections into the suite.
  const { openSync } = await import('node:fs');
  const log = (name) => openSync(join(root, name + '.log'), 'a');
  // `--run-seconds` matters for capture: the engine writes the WAV when its
  // capture window CLOSES, and a SIGTERM on the way out skips that entirely — the
  // first version of the audio test killed the engine and then looked for a file
  // that was never going to exist.
  /*
   * SWEEP OLD PLUGIN-HOST SOCKETS.
   *
   * The engine names them `/tmp/daw_host_<pid>_<track>.sock` and does not remove them, so every
   * run of every suite leaves a handful behind for ever. MEASURED: 978 of them after a day of
   * this, which is not a correctness problem now that the name carries a pid — two runs cannot
   * collide — but it is a thousand files in /tmp and it was one before the rename, when they
   * WERE shared and a leftover stopped the next engine's host from binding.
   *
   * BY AGE, NOT BY PROBING. The first version connected to each path to see whether anything
   * answered, which is exact and costs a syscall and a timeout EACH — at 978 files that is
   * minutes of setup before the engine starts, and it made a suite look like it was hanging.
   * A socket older than the cutoff cannot belong to a run that is starting now, and one younger
   * than it is left alone whether or not anybody is listening: deleting a live session's socket
   * to tidy up would break the app somebody is looking at.
   */
  {
    const { readdirSync, statSync, unlinkSync } = await import('node:fs');
    const CUTOFF_MS = 10 * 60 * 1000;
    const now = Date.now();
    let swept = 0;
    try {
      for (const n of readdirSync('/tmp')) {
        if (!/^daw_host_[0-9a-f_]+\.sock$/.test(n)) continue;
        const path = `/tmp/${n}`;
        try {
          if (now - statSync(path).mtimeMs < CUTOFF_MS) continue;
          unlinkSync(path);
          swept++;
        } catch { /* raced with another run, or not ours to remove */ }
      }
    } catch { /* no /tmp to read is not this function's problem */ }
    if (swept > 20) console.log(`  (swept ${swept} stale plugin-host sockets from /tmp)`);
  }

  const engineArgs = runSeconds ? ['--run-seconds', String(runSeconds)] : [];
  const engine = spawn(engineBin, engineArgs, {
    env, cwd: bin('build'), stdio: ['ignore', log('engine'), log('engine')],
  });
  procs.push(engine);

  const sidecar = spawn(sidecarBin, [
    '--shm', shm, '--port', String(base + 1), '--cmd-port', String(base + 2),
    '--keep-engine', '--plugin-cache', bin('build/plugin_cache.json'),
  ], { env, cwd: ROOT, stdio: ['ignore', log('sidecar'), log('sidecar')] });
  procs.push(sidecar);

  const server = spawn(process.execPath, [join(UIWEB, 'test/serve.mjs'), String(base)],
                       { cwd: UIWEB, stdio: ['ignore', log('serve'), log('serve')] });
  procs.push(server);

/*
 * FROM HERE ON, A FAILURE MUST TAKE ITS PROCESSES WITH IT.
 *
 * Everything below can time out, and until 2026-08-02 a timeout threw straight out of
 * startStack — before `stop` was even defined — leaving the engine, the sidecar and the page
 * server running. The engine is spawned WITHOUT `--run-seconds` unless a caller asks for one,
 * so a leaked one runs for ever.
 *
 * That cascades: a leaked engine holds a plugin host and the machine, the next run is slower,
 * IT times out, and leaks another. Three accumulated inside five minutes here, and the second
 * and third failures were caused by the first.
 *
 * So the waits run inside a guard that kills what this function started and re-throws with the
 * cleanup noted, because "timed out" plus a silently leaked engine is a far more confusing
 * report than "timed out".
 */
const killSpawned = () => {
  for (const p of procs) { try { p.kill('SIGKILL'); } catch { /* already gone */ } }
};
try {
  await until(() => listening(base), `page server on ${base}`);
  await until(() => listening(base + 1), `sidecar on ${base + 1}`);
  /*
   * AND THE ENGINE'S SEGMENT, which is the thing anything actually needs.
   *
   * A listening port is not a running engine. The sidecar binds its sockets
   * immediately and attaches to shared memory per CONNECTION, so a test that
   * connects before the engine has created the segment gets
   * `attach failed: cannot open /daw_e2e_NNNN` on its first command — and then
   * every assertion after it fails for its own apparent reason: no clips, no
   * chain, a tempo that will not set, audio sources that did not decode.
   *
   * How long the engine takes to get there depends on the AUDIO DEVICE. Opening
   * a Bluetooth speaker takes seconds where a built-in output takes
   * milliseconds, so this raced on one machine and not another, and produced a
   * run of thirty unrelated failures that read as a broken build.
   */
  await until(async () => {
    const { execFileSync } = await import('node:child_process');
    try {
      execFileSync(bin('ui/target/release/daw-cli'), ['get', 'transport'],
                   { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'],
                     env: { ...process.env, DAW_UI_SHM_NAME: shm } });
      return true;
    } catch { return false; }
  }, `the engine to publish ${shm}`, 30000);

  /*
   * WHERE IN THE WAV A MOMENT LANDS.
   *
   * The capture is a RING of the last `captureSeconds`, and `--run-seconds` is a
   * sleep that begins when the audio device opens — so the file's last sample is
   * `runSeconds` after "Audio output started", and its first is `captureSeconds`
   * before that. Both ends are therefore known, and any wall-clock instant can be
   * turned into an offset into the file.
   *
   * Without this an audio test has to GUESS its windows from how long it thinks
   * setup took, and setup time is dominated by plugin scanning and by how long
   * the audio device takes to open — seconds on a Bluetooth speaker, milliseconds
   * on the built-in output. A guess that is two seconds out reads a window from
   * the wrong phase and reports the feature broken.
   *
   * The segment wait above is NOT the anchor: the segment exists well before the
   * device is open, which is exactly the gap that varies.
   */
  /*
   * WAIT FOR THE DEVICE TO ANSWER EITHER WAY.
   *
   * "Audio output started" used to be printed by the function that CALLED start(), whether or
   * not the device ever pulled a block. The engine made it honest — it is printed only once a
   * callback has actually landed, and a device that opens and never runs prints "Audio output
   * OPENED BUT NEVER STARTED" instead.
   *
   * That is the right change and it broke this gate: on a machine whose device never calls
   * back the line never appears, so every one of the nine suites that sets `capture` sat here
   * for thirty seconds and then threw "timed out waiting for the audio device to open" —
   * which is false twice over. The device opened immediately. It is not going to start.
   *
   * So: wait for EITHER, and carry the answer. A suite that only wants an engine gets on with
   * it; a suite that wants SOUND reads `audioRunning` and says the run cannot answer rather
   * than reporting its subject broken.
   */
  let audioStartedAt = 0;
  let audioRunning = false;
  if (capture) {
    const engineLog = join(root, 'engine.log');
    const readLog = () => { try { return readFileSync(engineLog, 'utf8'); } catch { return ''; } };
    await until(() => {
      const log = readLog();
      return log.includes('Audio output started')
          || log.includes('Audio output OPENED BUT NEVER STARTED');
    }, 'the audio device to answer, either way', 30000, 50);
    audioRunning = readLog().includes('Audio output started');
    audioStartedAt = Date.now();
    if (!audioRunning) {
      console.log('  stack: the audio device opened and never started — this run cannot '
                + 'answer any question about SOUND (see daw_audio_probe)');
    }
  }
  /** Seconds into the capture WAV for a wall-clock instant, or -1 if not capturing. */
  const captureOffset = (atMs) => {
    if (!audioStartedAt) return -1;
    const endsAt = audioStartedAt + runSeconds * 1000;
    return (atMs - (endsAt - captureSeconds * 1000)) / 1000;
  };

  /*
   * STOP, AND MAKE SURE IT STOPPED.
   *
   * This sent SIGTERM and returned. That is usually enough — and "usually" is the problem: a
   * leaked engine holds the audio device, and the NEXT engine blocks waiting for it and never
   * starts its own `--run-seconds` clock, so it lingers indefinitely too. The failures that
   * produces do not look like leaks. They look like a suite that captured silence, or one that
   * took 935 seconds where it usually takes 10. This project has already spent a night reading a
   * machine carrying 79 stray engines as a bug in the sampler.
   *
   * So: SIGTERM, a grace period, then SIGKILL anything still alive — and SAY SO, loudly, because
   * a process that needed the KILL is a defect somewhere even though this recovered from it.
   *
   * `spawn(bin, ...)` directly rather than through a shell is what makes the handle trustworthy
   * here. Backend found the other shape in tools/: `( cd … && ./daw_engine … ) &` makes `$!` the
   * SUBSHELL, so the kill reaps the subshell and leaves the engine. Worth knowing when reading
   * any of those scripts.
   */
  const stop = () => {
    for (const p of procs) { try { p.kill('SIGTERM'); } catch {} }
    setTimeout(() => {
      const stubborn = procs.filter((p) => p.exitCode === null && p.signalCode === null);
      for (const p of stubborn) {
        console.log(`  stack: pid ${p.pid} ignored SIGTERM — sending KILL. A survivor holds the `
                  + 'audio device and the next run blocks on it, which reads as silence rather '
                  + 'than as a leak.');
        try { p.kill('SIGKILL'); } catch {}
      }
      // The engine spawns plugin hosts of its own; they exit with it, but give
      // them a moment before the directory goes.
      if (!keepDir) { try { rmSync(root, { recursive: true, force: true }); } catch {} }
    }, 500);
  };
  process.on('exit', stop);

  return { url: `http://127.0.0.1:${base}/index.html`, dir, root, shm, base,
           capture, captureOffset, audioStartedAt, runSeconds, stop,
           /*
            * Did the device actually PULL a block? The only honest basis for believing or
            * disbelieving a silence — an empty capture from a device that never ran says
            * nothing about the code that produced it. False when `capture` was not asked for.
            */
           audioRunning };
} catch (e) {
  killSpawned();
  throw new Error(`${e && e.message ? e.message : e} — the stack's own engine, sidecar and `
                + 'page server were killed rather than left running. An engine spawned without '
                + '--run-seconds never exits on its own, and a leaked one makes the NEXT run '
                + 'slow enough to time out too.');
}
}

/**
 * A GATE FOR CHECKS THAT CAN ONLY BE ANSWERED IF THE DEVICE RAN.
 *
 * `stack.audioRunning` is the engine's verdict at the device boundary — true only once a
 * callback has actually landed. When it is false there is no capture, and every question
 * about SOUND is unanswerable rather than answered "no".
 *
 * That signal is INDEPENDENT of what the suites test: it says nothing about whether bypass
 * works or a modulator moves, only whether this run could observe it. That independence is
 * the whole condition for an excuse being safe rather than a way of not looking — and it is
 * the condition I got wrong once already, by reaching for "0 of 0 playback callbacks", which
 * counts callbacks that had a track to play and reads 0 of 0 on a healthy idle engine.
 *
 * Shared rather than copied into each suite, because the version that lived in each suite
 * was present in four of ten and keyed on the underrun count in the one place it mattered
 * most.
 *
 *     const { soundCheck, banner } = soundGate(stack, check);
 *     soundCheck(peak > 0.01, 'it makes a sound');
 *     ...
 *     console.log(banner(fail, pass));
 */
export function soundGate(stack, check) {
  let blocked = 0;
  const soundCheck = (ok, what, detail) => {
    if (stack.audioRunning) return check(ok, what, detail);
    blocked++;
    console.log('  BLOCK', what, '— the audio device never started, so this run cannot answer it');
    return undefined;
  };
  /* A BLOCKED run is not a pass, and the banner has to say so — a note in a line above it
     scrolls away, and "ALL PASS" on a run that could not hear anything is how a suite stops
     being read. */
  const banner = (fail, pass) => {
    const note = blocked
      ? ` \u00b7 ${blocked} BLOCKED (the audio device never started — see daw_audio_probe)` : '';
    return `\n${fail === 0 ? `ALL PASS (${pass} checks)${note}`
                          : `${fail} of ${pass + fail} FAILED${note}`}`;
  };
  return { soundCheck, banner, blockedCount: () => blocked };
}
