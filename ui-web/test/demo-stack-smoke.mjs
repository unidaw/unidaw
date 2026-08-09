#!/usr/bin/env node
/**
 * IS THE DEMO'S OWN STACK ALIVE? — run this against a RUNNING `tools/webstack.sh`.
 *
 * Every other suite here boots its own stack through `stack.mjs`: a temp copy of presets/, its own
 * ports, its own SHM name. That is right for tests and it means NOTHING in the sweep ever touches
 * `tools/webstack.sh`, which is the first line of the demo runbook. The two share the engine and
 * the sidecar and differ in every wire around them — ports, project directory, the key file, the
 * decision to keep the engine alive after the last tab closes.
 *
 * So a green sweep says nothing about whether the demo starts. This is the missing check, and it
 * is deliberately NOT in the sweep: it needs a stack somebody else started, and starting a second
 * one while `all.mjs` runs collides on the engine.
 *
 *     DAW_WEBSTACK_ALLOW_CREDENTIALS=1 DAW_ENV_FILE=$PWD/.env KEEP_ENGINE=1 tools/webstack.sh
 *     node ui-web/test/demo-stack-smoke.mjs
 *
 * WHAT IT ASKS, in the order the demo depends on them:
 *   - the page loads and CONNECTS (canSend) — the sidecar found the engine's segment
 *   - a project loads and brings tracks and notes with it
 *   - the SCALE REGISTRY arrived, which the chord numerals' casing needs and which is published
 *     once per client rather than polled; a stack that never sends it draws every chord upper
 *     case and nothing else looks wrong
 *   - nothing threw
 */

import assert from 'node:assert/strict';
import {
  closeSync,
  constants as fsConstants,
  fstatSync,
  lstatSync,
  openSync,
  readFileSync,
  readdirSync,
  realpathSync,
} from 'node:fs';
import { basename, dirname, isAbsolute, join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const LOCATOR_FAILURE = 'demo stack ready locator is missing or invalid';
const LOG_DIRECTORY_NAME = /^daw-webstack-log\.[A-Za-z0-9]{8}$/;

function locatorFailure() {
  // A locator can contain a caller-supplied path. Keep every externally visible
  // failure generic so rejecting one never prints that path as a side effect.
  return new Error(LOCATOR_FAILURE);
}

function isWithin(root, candidate) {
  const rel = relative(root, candidate);
  return rel === '' || (!isAbsolute(rel) && rel !== '..' && !rel.startsWith(`..${sep}`));
}

/** Mirror webstack.sh's deliberately narrow SHM contract and segment encoding. */
export function webstackSegment(shm) {
  if (typeof shm !== 'string' || !/^\/[A-Za-z0-9_]{1,120}$/.test(shm)) {
    throw locatorFailure();
  }
  return shm.replace(/[^A-Za-z0-9]/g, '_');
}

/** Parse the exact, immutable READY record published by tools/webstack.sh. */
export function parseWebstackReadyState(text, expectedSegment) {
  if (typeof text !== 'string' || !/^_[A-Za-z0-9_]{1,120}$/.test(expectedSegment)) {
    throw locatorFailure();
  }
  const lines = text.split('\n');
  if (lines.length !== 7 || lines[6] !== ''
      || lines[0] !== 'DAW_WEBSTACK_STATE=1'
      || lines[1] !== 'READY=1'
      || lines[2] !== `SEG=${expectedSegment}`
      || !lines[3].startsWith('LOG_DIR=')
      || !lines[4].startsWith('ENGINE_LOG=')
      || !lines[5].startsWith('ENGINE_PID=')) {
    throw locatorFailure();
  }
  const logDir = lines[3].slice('LOG_DIR='.length);
  const engineLog = lines[4].slice('ENGINE_LOG='.length);
  const enginePid = lines[5].slice('ENGINE_PID='.length);
  if (!isAbsolute(logDir) || !isAbsolute(engineLog) || !/^[1-9][0-9]*$/.test(enginePid)
      || !Number.isSafeInteger(Number(enginePid))) {
    throw locatorFailure();
  }
  return Object.freeze({ segment: expectedSegment, logDir, engineLog, enginePid });
}

/** Pure path-policy half of locator validation, exported for a no-device test. */
export function validateWebstackReadyPaths(record, {
  tempRoot, logDir, statePath, expectedSegment,
}) {
  if (!record || record.segment !== expectedSegment
      || !isAbsolute(tempRoot) || !isAbsolute(logDir) || !isAbsolute(statePath)
      || dirname(logDir) !== tempRoot || !LOG_DIRECTORY_NAME.test(basename(logDir))
      || !isWithin(tempRoot, logDir)
      || record.logDir !== logDir
      || record.engineLog !== join(logDir, 'engine.log')
      || statePath !== join(logDir, `uni-web-stack${expectedSegment}.state`)
      || !isWithin(logDir, statePath) || !isWithin(logDir, record.engineLog)) {
    throw locatorFailure();
  }
  return true;
}

/** Select by segment + current pid, never by a log's name, age, or mtime. */
export function selectCurrentWebstackState(candidates, expectedSegment, enginePid) {
  if (!Array.isArray(candidates) || !/^_[A-Za-z0-9_]{1,120}$/.test(expectedSegment)
      || !/^[1-9][0-9]*$/.test(enginePid)) {
    throw locatorFailure();
  }
  const matches = candidates.filter(({ record }) => record.segment === expectedSegment
    && record.enginePid === enginePid);
  if (matches.length !== 1) throw locatorFailure();
  return matches[0];
}

function trustedOsTempRoot() {
  // webstack.sh's daw_os_temp_root canonicalizes literal /tmp and ignores the
  // caller's TMPDIR/TMP/TEMP. Use the same authority here.
  const root = realpathSync('/tmp');
  const stat = lstatSync(root);
  if (!stat.isDirectory() || stat.isSymbolicLink()) throw locatorFailure();
  return root;
}

function modeOf(stat) {
  return stat.mode & 0o777;
}

function validatedLogDirectory(candidate, tempRoot) {
  const stat = lstatSync(candidate);
  if (!stat.isDirectory() || stat.isSymbolicLink() || modeOf(stat) !== 0o700
      || dirname(candidate) !== tempRoot || !LOG_DIRECTORY_NAME.test(basename(candidate))) {
    throw locatorFailure();
  }
  const canonical = realpathSync(candidate);
  if (canonical !== candidate || !isWithin(tempRoot, canonical)) throw locatorFailure();
  return canonical;
}

function readValidatedRegularFile(candidate, parent, expectedMode) {
  const before = lstatSync(candidate);
  if (!before.isFile() || before.isSymbolicLink() || modeOf(before) !== expectedMode) {
    throw locatorFailure();
  }
  const canonical = realpathSync(candidate);
  if (canonical !== candidate || dirname(canonical) !== parent || !isWithin(parent, canonical)) {
    throw locatorFailure();
  }
  const noFollow = fsConstants.O_NOFOLLOW || 0;
  const fd = openSync(canonical, fsConstants.O_RDONLY | noFollow);
  try {
    const opened = fstatSync(fd);
    if (!opened.isFile() || modeOf(opened) !== expectedMode
        || opened.dev !== before.dev || opened.ino !== before.ino) {
      throw locatorFailure();
    }
    return readFileSync(fd, 'utf8');
  } finally {
    closeSync(fd);
  }
}

function firstEnginePid(text) {
  const lines = text.split('\n');
  if (lines.at(-1) === '') lines.pop();
  if (lines.length === 0 || lines.some((line) => !/^[1-9][0-9]*$/.test(line)
      || !Number.isSafeInteger(Number(line)))) {
    throw locatorFailure();
  }
  return lines[0];
}

/** Resolve and read only the READY log belonging to the requested live segment. */
export function readSegmentEngineLog(shm) {
  try {
    const expectedSegment = webstackSegment(shm);
    const tempRoot = trustedOsTempRoot();
    const pidPath = join(tempRoot, `uni-web-stack${expectedSegment}.pids`);
    const enginePid = firstEnginePid(readValidatedRegularFile(pidPath, tempRoot, 0o600));
    const candidates = [];

    for (const name of readdirSync(tempRoot)) {
      if (!LOG_DIRECTORY_NAME.test(name)) continue;
      try {
        const logDir = validatedLogDirectory(join(tempRoot, name), tempRoot);
        const statePath = join(logDir, `uni-web-stack${expectedSegment}.state`);
        const record = parseWebstackReadyState(
          readValidatedRegularFile(statePath, logDir, 0o600), expectedSegment,
        );
        validateWebstackReadyPaths(record, { tempRoot, logDir, statePath, expectedSegment });
        if (record.enginePid !== enginePid) continue;
        const log = readValidatedRegularFile(record.engineLog, logDir, 0o600);
        candidates.push({ record, log });
      } catch {
        // Old runs, partial runs, and hostile lookalikes are not candidates.
      }
    }
    const selected = selectCurrentWebstackState(candidates, expectedSegment, enginePid);
    // A same-segment restart replaces the pidfile. Re-read it after discovery so
    // a run changing underneath the smoke cannot turn an old locator into the
    // apparent current one merely because it was current at the first read.
    const confirmedPid = firstEnginePid(
      readValidatedRegularFile(pidPath, tempRoot, 0o600),
    );
    if (confirmedPid !== enginePid) throw locatorFailure();
    return selected.log;
  } catch {
    throw locatorFailure();
  }
}

function locatorSelfTest() {
  const tempRoot = '/private/tmp';
  const expectedSegment = '_daw_web_ui';
  const logDir = `${tempRoot}/daw-webstack-log.Ab12Cd34`;
  const statePath = `${logDir}/uni-web-stack${expectedSegment}.state`;
  const state = 'DAW_WEBSTACK_STATE=1\nREADY=1\nSEG=_daw_web_ui\n'
    + `LOG_DIR=${logDir}\nENGINE_LOG=${logDir}/engine.log\nENGINE_PID=4321\n`;
  assert.equal(webstackSegment('/daw_web_ui'), expectedSegment);
  const record = parseWebstackReadyState(state, expectedSegment);
  assert.equal(validateWebstackReadyPaths(
    record, { tempRoot, logDir, statePath, expectedSegment },
  ), true);
  assert.equal(selectCurrentWebstackState([
    { record: { ...record, enginePid: '1111' }, log: 'stale' },
    { record, log: 'current' },
    { record: { ...record, segment: '_another_segment' }, log: 'foreign' },
  ], expectedSegment, '4321').log, 'current');
  assert.throws(() => parseWebstackReadyState(
    state.replace('SEG=_daw_web_ui', 'SEG=_another_segment'), expectedSegment,
  ), { message: LOCATOR_FAILURE });
  assert.throws(() => parseWebstackReadyState(`${state}EXTRA=1\n`, expectedSegment),
                { message: LOCATOR_FAILURE });
  const foreignLogDir = '/private/tmp-foreign/daw-webstack-log.Ab12Cd34';
  const foreignRecord = {
    ...record,
    logDir: foreignLogDir,
    engineLog: `${foreignLogDir}/engine.log`,
  };
  assert.throws(() => validateWebstackReadyPaths(foreignRecord, {
    tempRoot,
    logDir: foreignLogDir,
    statePath: `${foreignLogDir}/uni-web-stack${expectedSegment}.state`,
    expectedSegment,
  }), { message: LOCATOR_FAILURE });
  const secretPath = '/private/tmp/credentials-must-not-print.env';
  let diagnostic = '';
  try {
    validateWebstackReadyPaths({ ...record, engineLog: secretPath }, {
      tempRoot, logDir, statePath, expectedSegment,
    });
  } catch (error) {
    diagnostic = error.message;
  }
  assert.equal(diagnostic, LOCATOR_FAILURE);
  assert.equal(diagnostic.includes(secretPath), false);
  assert.throws(() => selectCurrentWebstackState([
    { record, log: 'one' }, { record, log: 'ambiguous' },
  ], expectedSegment, '4321'), { message: LOCATOR_FAILURE });
  console.log('demo stack locator policy: PASS (exact state, bounded paths, segment+pid selection)');
}

async function smokeDemoStack() {
const { chromium } = await import('playwright');

const URL = process.env.DEMO_URL || 'http://127.0.0.1:8173/index.html';
let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

console.log(`\nsmoking the demo stack at ${URL}\n`);
let reached = true;
try {
  await page.goto(URL, { waitUntil: 'load', timeout: 15000 });
} catch (e) {
  reached = false;
  check(false, 'the page is being served', `${String(e).slice(0, 120)} — is tools/webstack.sh running?`);
}

if (reached) {
  const up = await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                                        { timeout: 25000 }).then(() => true).catch(() => false);
  check(up, 'the page connects to the engine',
        'canSend() never became true — the sidecar is up but has not attached to the segment');

  if (up) {
    await page.waitForTimeout(1500);
    await page.evaluate(() => window.__uni.run('load demo'));
    await page.waitForTimeout(2500);
    const st = await page.evaluate(() => {
      const e = window.__uni.engineState() || {};
      return { tracks: e.trackCount, notes: e.noteCount };
    });
    check(st.tracks > 0 && st.notes > 0, 'a project loads, with tracks and notes',
          JSON.stringify(st));

    /*
     * SENT ONCE PER CLIENT, not polled — so this is the reading most likely to be silently absent
     * on a stack that is otherwise working. Without it every chord numeral draws upper case
     * (see nameChord: no scale, no claim about quality) and nothing else looks wrong.
     */
    const scales = await page.evaluate(() => (window.__uni.scaleNames() || []).length);
    check(scales > 0, 'the scale registry arrived — chord numerals can be cased',
          `${scales} scales`);
  }
}

/*
 * DOES SOUND ACTUALLY LEAVE THE MACHINE?
 *
 * Nothing else in this repo asks. Every audio assertion we have goes through the OFFLINE RENDER,
 * which is the right oracle precisely because it does not touch a device — byte-exact, no
 * hardware, no CoreAudio. Which means the entire suite can be green on a machine where the demo
 * would be silent, and that is the one failure a runbook cannot absorb: it happens in front of
 * people, in the first ten seconds, with no error on screen.
 *
 * The engine already answers it, and answers it the strong way. It does not trust `start()`
 * returning true, and it does not trust the device's own `isPlaying()` — both report success on a
 * machine where CoreAudio opens the device, reports its name and rate, and never runs the IO
 * proc. It COUNTS REAL CALLBACKS and prints one of two lines. Both agents have lost time to "the
 * app makes no sound" against the weaker version of this check.
 *
 * So this reads the line rather than re-deriving it. `webstack.sh` publishes a
 * READY record inside its unique run-owned log directory. The record is useful
 * only when its segment and engine pid match the segment's numeric pidfile; an
 * old or different segment's log is never a fallback.
 */
{
  const shm = process.env.DAW_UI_SHM_NAME || '/daw_web_ui';
  let log = '';
  let located = true;
  try { log = readSegmentEngineLog(shm); } catch { located = false; }
  const started = /Audio output started/.test(log);
  const opened = /OPENED BUT NEVER STARTED/.test(log);
  check(located && started && !opened,
        'AUDIO IS ACTUALLY RUNNING — the device ran a real callback, not just opened',
        !located
          ? 'no validated READY engine log for the requested segment; is tools/webstack.sh running?'
          : opened
          ? `the engine says the device OPENED BUT NEVER STARTED: ${
              (log.match(/OPENED BUT NEVER STARTED[^\n]*/) || [''])[0].slice(0, 160)}`
          : 'the validated engine log has no "Audio output started" — every render will still be perfect');
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exitCode = fail ? 1 : 0;
}

const moduleFile = realpathSync(fileURLToPath(import.meta.url));
let isMain = false;
try { isMain = Boolean(process.argv[1]) && realpathSync(process.argv[1]) === moduleFile; } catch {}
if (isMain) {
  if (process.argv[2] === '--self-test-locator' && process.argv.length === 3) {
    locatorSelfTest();
  } else if (process.argv.length === 2) {
    await smokeDemoStack();
  } else {
    console.error('usage: node ui-web/test/demo-stack-smoke.mjs [--self-test-locator]');
    process.exitCode = 2;
  }
}
