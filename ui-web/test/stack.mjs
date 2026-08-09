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
import {
  accessSync,
  constants as fsConstants,
  cpSync,
  existsSync,
  lstatSync,
  mkdtempSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  realpathSync,
  rmSync,
  statSync,
  unlinkSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, isAbsolute, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const MODULE_FILE = realpathSync(fileURLToPath(import.meta.url));
const ROOT = realpathSync(join(dirname(MODULE_FILE), '..', '..'));
const UIWEB = realpathSync(join(ROOT, 'ui-web'));
const PRESETS = realpathSync(join(ROOT, 'presets'));
const PATCHER_PRESET_DIR = realpathSync(join(PRESETS, 'patcher'));

export const STACK_LOCAL_PATHS = Object.freeze({
  repositoryRoot: ROOT,
  webRoot: UIWEB,
  presetRoot: PRESETS,
  patcherPresetDir: PATCHER_PRESET_DIR,
});

const bin = (p) => join(ROOT, p);

function isWithin(root, path) {
  const rel = relative(root, path);
  return rel === '' || (!isAbsolute(rel) && rel !== '..' && !rel.startsWith(`..${sep}`));
}

for (const [path, label] of [
  [UIWEB, 'web source'],
  [PRESETS, 'preset fixtures'],
  [PATCHER_PRESET_DIR, 'patcher presets'],
]) {
  if (!isWithin(ROOT, path) || !lstatSync(path).isDirectory()) {
    throw new Error(`stack: ${label} is not a checkout-local directory`);
  }
}

function trustedTempRoot() {
  const saved = new Map();
  for (const name of ['TMPDIR', 'TMP', 'TEMP']) {
    saved.set(name, process.env[name]);
    delete process.env[name];
  }
  let candidate;
  try {
    candidate = realpathSync(tmpdir());
  } finally {
    for (const [name, value] of saved) {
      if (value === undefined) delete process.env[name];
      else process.env[name] = value;
    }
  }
  const target = lstatSync(candidate);
  if (!target.isDirectory() || target.isSymbolicLink()) {
    throw new Error('stack: canonical OS temporary root is invalid');
  }
  for (let current = candidate;; current = dirname(current)) {
    if (existsSync(join(current, '.git'))) {
      throw new Error('stack: refusing a temporary root inside a Git checkout');
    }
    const parent = dirname(current);
    if (parent === current) break;
  }
  return candidate;
}

function removeStackTempRoot(path, tempRoot) {
  const parent = realpathSync(dirname(path));
  const target = lstatSync(path);
  if (parent !== tempRoot || !basename(path).startsWith('uni-e2e-')
      || !target.isDirectory() || target.isSymbolicLink()) {
    throw new Error('stack: refusing to remove an invalid temporary root');
  }
  rmSync(path, { recursive: true, force: true });
}

const CHILD_STEERING = [
  'ANTHROPIC_API_KEY', 'DAW_ENV_FILE',
  'NODE_OPTIONS', 'NODE_PATH', 'PLAYWRIGHT_BROWSERS_PATH', 'CARGO_TARGET_DIR',
  'DAW_PLUGIN_CACHE', 'DAW_PATCHER_PRESET_DIR',
  'DAW_CAPTURE_WAV', 'DAW_CAPTURE_SECONDS',
  'DAW_EVENT_LOG', 'DAW_EVENT_LOG_OFF', 'DAW_HOST_SOCKET_PREFIX',
  'DAW_UI_SHM_NAME', 'DAW_PROJECT_DIR', 'DAW_HOST_BINARY', 'DAW_ENGINE_NUM_BLOCKS',
  'TMPDIR', 'TMP', 'TEMP',
];
const CHILD_STEERING_KEYS = new Set(CHILD_STEERING.map((name) => name.toUpperCase()));

function credentialFreeBase(source, tempDir) {
  const environment = { ...source };
  // Environment keys are case-insensitive on Windows. Delete by normalized key
  // so a differently-cased credential or dependency variable cannot survive the
  // policy and win nondeterministically when Node builds the child's env block.
  for (const name of Object.keys(environment)) {
    if (name.toUpperCase().startsWith('DAW_') || CHILD_STEERING_KEYS.has(name.toUpperCase())) {
      delete environment[name];
    }
  }
  if (tempDir) {
    environment.TMPDIR = tempDir;
    environment.TMP = tempDir;
    environment.TEMP = tempDir;
  }
  return environment;
}

/**
 * Pure process-boundary policy, exported so credential isolation can be checked
 * without starting an engine, opening an audio device, or making a paid request.
 * Credentials are absent from every role by default. In the explicit paid mode,
 * only the sidecar receives them.
 */
export function stackChildEnvironments(source, {
  allowCredentials = false,
  credentials = {},
  shm = '',
  projectDir = '',
  hostBin = '',
  pluginCache = '',
  patcherPresetDir = '',
  capture = '',
  captureSeconds = 0,
  numBlocks = 0,
  tempDir = '',
} = {}) {
  if (typeof allowCredentials !== 'boolean') {
    throw new Error('stack: allowCredentials must be boolean');
  }
  const base = credentialFreeBase(source, tempDir);
  const engine = { ...base };
  const sidecar = { ...base };
  const page = { ...base };
  const cli = { ...base };

  if (shm) {
    engine.DAW_UI_SHM_NAME = shm;
    sidecar.DAW_UI_SHM_NAME = shm;
    cli.DAW_UI_SHM_NAME = shm;
  }
  if (projectDir) {
    engine.DAW_PROJECT_DIR = projectDir;
    sidecar.DAW_PROJECT_DIR = projectDir;
  }
  if (hostBin) engine.DAW_HOST_BINARY = hostBin;
  if (pluginCache) engine.DAW_PLUGIN_CACHE = pluginCache;
  if (patcherPresetDir) {
    engine.DAW_PATCHER_PRESET_DIR = patcherPresetDir;
    sidecar.DAW_PATCHER_PRESET_DIR = patcherPresetDir;
  }
  if (capture) {
    engine.DAW_CAPTURE_WAV = capture;
    engine.DAW_CAPTURE_SECONDS = String(captureSeconds);
  }
  if (numBlocks) engine.DAW_ENGINE_NUM_BLOCKS = String(numBlocks);

  if (allowCredentials) {
    if (credentials.anthropicApiKey) {
      sidecar.ANTHROPIC_API_KEY = credentials.anthropicApiKey;
    }
    if (credentials.envFile) sidecar.DAW_ENV_FILE = credentials.envFile;
  }
  return { engine, sidecar, page, cli };
}

// Kept as the narrow common-policy surface used by the repository guard. It is
// intentionally credential-free even when a caller is constructing paid-mode
// sidecar credentials; role separation happens in stackChildEnvironments().
export function stackChildEnvironment(source, {
  pluginCache = '', patcherPresetDir = '', tempRoot = '',
} = {}) {
  const environment = credentialFreeBase(source, tempRoot);
  if (pluginCache) environment.DAW_PLUGIN_CACHE = pluginCache;
  if (patcherPresetDir) environment.DAW_PATCHER_PRESET_DIR = patcherPresetDir;
  return environment;
}

/** The exact relative credential search performed by daw-sidecar/src/ask.rs. */
export function sidecarCredentialSearchPaths(cwd) {
  return [resolve(cwd, '.env'), resolve(cwd, '../.env'), resolve(cwd, '../../.env')];
}

export function explicitStackCredentials(source, allowCredentials = false) {
  if (typeof allowCredentials !== 'boolean') {
    throw new Error('stack: allowCredentials must be boolean');
  }
  if (!allowCredentials) return {};
  const credentials = {};
  if (typeof source.ANTHROPIC_API_KEY === 'string'
      && source.ANTHROPIC_API_KEY.trim() !== '') {
    credentials.anthropicApiKey = source.ANTHROPIC_API_KEY;
  }
  if (typeof source.DAW_ENV_FILE === 'string' && source.DAW_ENV_FILE !== '') {
    try {
      if (!isAbsolute(source.DAW_ENV_FILE)) throw new Error('relative');
      const canonical = realpathSync(source.DAW_ENV_FILE);
      if (!statSync(canonical).isFile()) throw new Error('not-file');
      accessSync(canonical, fsConstants.R_OK);
      credentials.envFile = canonical;
    } catch {
      // Never include the candidate: a credential path is sensitive even when
      // invalid, and callers only need to know which explicit contract failed.
      throw new Error('stack: explicit DAW_ENV_FILE must name an absolute readable file');
    }
  }
  return credentials;
}

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
 * @param {boolean} [opts.allowCredentials] explicit paid-test opt-in. False by
 *   default; true passes validated ambient ask credentials to the sidecar only.
 */
export async function startStack({ base = 0, shm = '', keepDir = false,
                                   capture = '', captureSeconds = 30,
                                   runSeconds = 0, numBlocks = 0,
                                   allowCredentials = false } = {}) {
  const procs = [];
  if (typeof allowCredentials !== 'boolean') {
    throw new Error('stack: allowCredentials must be boolean');
  }
  if (!base) base = await findFreeBase();
  // The segment name has to be unique too, or two runs share one engine's memory
  // — the same collision one layer down, and a much harder one to see.
  if (!shm) shm = `/daw_e2e_${base}`;

  const artifact = (candidate, what) => {
    if (!existsSync(candidate)) throw new Error(`stack: ${what} is not built in this checkout`);
    const source = lstatSync(candidate);
    const canonical = realpathSync(candidate);
    if (!source.isFile() || source.isSymbolicLink() || !isWithin(ROOT, canonical)) {
      throw new Error(`stack: ${what} is not a checkout-local regular artifact`);
    }
    return canonical;
  };
  const buildCandidate = bin('build');
  const buildCache = join(buildCandidate, 'CMakeCache.txt');
  if (!existsSync(buildCache) || lstatSync(buildCache).isSymbolicLink()) {
    throw new Error('stack: checkout build cache is missing or is a symlink');
  }
  const configuredSource = readFileSync(buildCache, 'utf8').split('\n')
    .filter((line) => line.startsWith('CMAKE_HOME_DIRECTORY:INTERNAL='))
    .at(-1)?.slice('CMAKE_HOME_DIRECTORY:INTERNAL='.length);
  if (!configuredSource || realpathSync(configuredSource) !== ROOT) {
    throw new Error('stack: checkout build was configured from a different source root');
  }
  const engineBin = artifact(bin('build/daw_engine'), 'daw_engine');
  const hostBin = artifact(bin('build/juce_host_process'), 'juce_host_process');
  const sidecarBin = artifact(bin('ui/target/release/daw-sidecar'), 'daw-sidecar');
  const cliBin = artifact(bin('ui/target/release/daw-cli'), 'daw-cli');
  const buildCwd = realpathSync(buildCandidate);
  if (!isWithin(ROOT, buildCwd)) {
    throw new Error('stack: build directory resolves outside this checkout');
  }
  let pluginCache = bin('build/plugin_cache.json');
  if (existsSync(pluginCache)) pluginCache = artifact(pluginCache, 'plugin_cache.json');

  const credentials = explicitStackCredentials(process.env, allowCredentials);
  const tempRoot = trustedTempRoot();
  const root = mkdtempSync(join(tempRoot, 'uni-e2e-'));
  // The WHOLE presets tree, not just projects/. An audio clip stores its file as
  // `../audio/waveform_probe.wav` — relative to the project directory — so a copy
  // of projects/ alone leaves every audio source dangling one level up. Copying
  // presets/ and pointing DAW_PROJECT_DIR at its projects/ keeps the relative
  // paths meaning what they meant in the repo.
  //
  // A copy, not the real thing, so a test that saves cannot rewrite the fixtures
  // everything else reads.
  cpSync(PRESETS, root, { recursive: true });
  const dir = join(root, 'projects');
  const runtimeRoot = mkdtempSync(join(root, '.runtime-'));
  const sidecarCwd = join(runtimeRoot, 'sidecar', 'work');
  const childTemp = join(runtimeRoot, 'tmp');
  mkdirSync(sidecarCwd, { recursive: true });
  mkdirSync(childTemp, { recursive: true });
  const credentialSearch = sidecarCredentialSearchPaths(sidecarCwd);
  if (!credentialSearch.every((candidate) => isWithin(runtimeRoot, candidate))) {
    throw new Error('stack: sidecar credential search escapes its run-owned resource root');
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
      // NOT tests/, benches/ or examples/. They are .rs under a watched crate and they are not
      // compiled into the release binary, so `cargo build --release` cannot clear a warning they
      // cause — the third form of the unclearable-warning defect this file documents, and the
      // second one I have written into it today. Editing an engine_e2e test warned that the
      // sidecar was stale, forever.
      if (name === 'tests' || name === 'benches' || name === 'examples') continue;
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

  const childEnvironments = stackChildEnvironments(process.env, {
    allowCredentials,
    credentials,
    shm,
    projectDir: dir,
    hostBin,
    pluginCache,
    patcherPresetDir: PATCHER_PRESET_DIR,
    capture,
    captureSeconds,
    numBlocks,
    tempDir: childTemp,
  });
  if (capture) {
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
    env: childEnvironments.engine,
    cwd: buildCwd,
    stdio: ['ignore', log('engine'), log('engine')],
  });
  procs.push(engine);

  const sidecar = spawn(sidecarBin, [
    '--shm', shm, '--port', String(base + 1), '--cmd-port', String(base + 2),
    '--keep-engine', '--plugin-cache', pluginCache,
  ], {
    env: childEnvironments.sidecar,
    cwd: sidecarCwd,
    stdio: ['ignore', log('sidecar'), log('sidecar')],
  });
  procs.push(sidecar);

  const server = spawn(process.execPath, [join(UIWEB, 'test/serve.mjs'), String(base)],
                       { cwd: UIWEB, env: childEnvironments.page,
                         stdio: ['ignore', log('serve'), log('serve')] });
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
      execFileSync(cliBin, ['get', 'transport'],
                   { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'],
                     env: childEnvironments.cli });
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
  /*
   * RETURNS A PROMISE, so `await stack.stop()` actually waits for the part that matters.
   *
   * It used to return undefined. `await undefined` resolves on the next tick, so every suite that
   * carefully wrote `await stack.stop()` was in exactly the same position as the thirty-three that
   * do not await it: the SIGTERM had gone out, and the `setTimeout` holding the SIGKILL escalation
   * and the directory removal had not run yet. Then `process.exit(...)` cancelled it.
   *
   * Found because full-song.mjs — which DOES await — was named by the sweep's new stray reaper as
   * leaving an engine and a sidecar running. It awaits correctly; the await was a no-op.
   *
   * The exit-handler path (reapSync, below) still exists and still matters: it covers the suites
   * that never call stop() at all, and the case where a suite throws before reaching it.
   */
  const stop = () => new Promise((resolveStop) => {
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
      /*
       * `DAW_KEEP_STACK=1` OVERRIDES, so a sweep can keep what a suite would have thrown away.
       *
       * The suites that flake are the ones that do NOT pass keepDir — kit.mjs and params.mjs both
       * flaked on 2026-08-08 and both delete their directory on the way out, so the engine log
       * that would say WHY was gone before the runner even learned the suite had failed. The
       * runner cannot pass keepDir either: it does not construct the stack, the suite does.
       *
       * An environment variable is the one channel that reaches every suite without editing all
       * sixty-four of them. all.mjs sets it; a hand-run suite is unaffected and still cleans up
       * after itself.
       *
       * The disk cost is bounded by the prune at the top of all.mjs — six hours, which was added
       * after a day of sweeps left 1257 directories and 19 GB behind.
       */
      const keep = keepDir || process.env.DAW_KEEP_STACK === '1';
      if (!keep) { try { removeStackTempRoot(root, tempRoot); } catch {} }
      resolveStop();
    }, 500);
  });

  /*
   * THE SAME WORK, SYNCHRONOUSLY, FOR THE EXIT PATH — because the version above never finished.
   *
   * `stop()` sends SIGTERM immediately and then does the two things that matter inside a
   * `setTimeout`: escalate to SIGKILL for anything that ignored it, and remove the directory.
   * THIRTY-THREE SUITES call `stack.stop()` without awaiting it and then `process.exit(...)`,
   * which cancels pending timers — so for those, neither ever ran. The exit handler registered
   * here had the identical problem: an 'exit' listener cannot await, so its timer is dead on
   * arrival.
   *
   * The consequences are the two leaks that have cost the most time on this project, and the
   * comment above already predicted one of them: "a survivor holds the audio device and the next
   * run blocks on it, which reads as SILENCE rather than as a leak". That is the shape of every
   * sweep-only flake seen on 2026-08-07/08 — silent renders, kit answers that never arrive,
   * chains that never publish, none of it reproducible when the suite is run alone. The other is
   * 1257 stack directories and 19 GB.
   *
   * `Atomics.wait` is the only way to pause inside an exit handler. The event loop is stopped, so
   * `p.exitCode` cannot update while we wait and a process that died politely still looks alive —
   * hence SIGKILL is sent unconditionally afterwards. Signalling an already-dead child is an
   * ESRCH we catch, which is a far better trade than leaving one running.
   */
  let reaped = false;
  const reapSync = () => {
    if (reaped) return;
    reaped = true;
    for (const p of procs) { try { p.kill('SIGTERM'); } catch {} }
    try {
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 500);
    } catch { /* no SharedArrayBuffer: fall through and kill immediately */ }
    for (const p of procs) { try { p.kill('SIGKILL'); } catch {} }
    const keep = keepDir || process.env.DAW_KEEP_STACK === '1';
    if (!keep) { try { removeStackTempRoot(root, tempRoot); } catch {} }
  };
  process.on('exit', reapSync);

  return { url: `http://127.0.0.1:${base}/index.html`, dir, root, shm, base, sidecarCwd,
           capture, captureOffset, audioStartedAt, runSeconds, stop,
           /*
            * Did the device actually PULL a block? The only honest basis for believing or
            * disbelieving a silence — an empty capture from a device that never ran says
            * nothing about the code that produced it. False when `capture` was not asked for.
            */
           audioRunning };
} catch (e) {
  killSpawned();
  const keep = keepDir || process.env.DAW_KEEP_STACK === '1';
  if (!keep) { try { removeStackTempRoot(root, tempRoot); } catch {} }
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
