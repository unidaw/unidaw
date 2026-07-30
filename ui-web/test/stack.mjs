/**
 * A disposable engine + sidecar + page server, for tests that edit documents.
 *
 * The e2e suite used to run against whatever stack happened to be up — which in
 * practice was Jaakko's, the one he was using. Every run added devices, wrote
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
 * suite while Jaakko has the app open, or the several `tools/webstack.sh`
 * instances other agents keep alive in this repo. See findFreeBase.
 */

import { spawn } from 'node:child_process';
import { mkdtempSync, cpSync, rmSync, existsSync, readFileSync } from 'node:fs';
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
   * CLEAR STALE PLUGIN-HOST SOCKETS FIRST.
   *
   * The engine names them by TRACK INDEX in /tmp — `/tmp/daw_host_track_0.sock` — so they are
   * shared by every engine on this machine and NOT scoped to a run the way the SHM segment and
   * the ports are. A run that is killed rather than stopped leaves them behind, and the next
   * engine's host cannot bind a path that already exists: the host fails to start, the track
   * has no instrument, and every meter reads the silence sentinel.
   *
   * WHICH LOOKS LIKE A BROKEN APP. It presented as two e2e failures — "a real dBFS level while
   * playing: -32768" — after a UI-only change, with ten sockets in /tmp and no engine or host
   * process alive to own them. audible.mjs documents the same trap and tells you to go and look;
   * this removes the condition instead, because a suite that needs a manual `rm` before it is
   * trusted is a suite people learn to ignore.
   *
   * ONLY WHEN NOTHING IS LISTENING. A socket with a live host behind it belongs to a session
   * somebody is using — deleting that would break the app they are looking at to make a test
   * pass. Connect first; only an unreachable path is stale.
   */
  {
    const { readdirSync, unlinkSync } = await import('node:fs');
    const net = await import('node:net');
    let names = [];
    try {
      names = readdirSync('/tmp').filter((n) => /^daw_host_track_\d+\.sock$/.test(n));
    } catch { /* no /tmp to read is not this function's problem */ }
    for (const n of names) {
      const path = `/tmp/${n}`;
      const alive = await new Promise((res) => {
        const c = net.connect(path);
        const done = (v) => { try { c.destroy(); } catch {} res(v); };
        c.once('connect', () => done(true));
        c.once('error', () => done(false));
        setTimeout(() => done(false), 250);
      });
      if (!alive) { try { unlinkSync(path); } catch { /* raced with another run */ } }
    }
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
  let audioStartedAt = 0;
  if (capture) {
    const engineLog = join(root, 'engine.log');
    await until(() => {
      try { return readFileSync(engineLog, 'utf8').includes('Audio output started'); }
      catch { return false; }
    }, 'the audio device to open', 30000, 50);
    audioStartedAt = Date.now();
  }
  /** Seconds into the capture WAV for a wall-clock instant, or -1 if not capturing. */
  const captureOffset = (atMs) => {
    if (!audioStartedAt) return -1;
    const endsAt = audioStartedAt + runSeconds * 1000;
    return (atMs - (endsAt - captureSeconds * 1000)) / 1000;
  };

  const stop = () => {
    for (const p of procs) { try { p.kill('SIGTERM'); } catch {} }
    // The engine spawns plugin hosts of its own; they exit with it, but give
    // them a moment before the directory goes.
    setTimeout(() => {
      if (!keepDir) { try { rmSync(root, { recursive: true, force: true }); } catch {} }
    }, 500);
  };
  process.on('exit', stop);

  return { url: `http://127.0.0.1:${base}/index.html`, dir, root, shm, base,
           capture, captureOffset, audioStartedAt, runSeconds, stop };
}
