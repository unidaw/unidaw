/**
 * Placement ops against the real engine: does a clip actually move?
 *
 * The unit tests pin the WIRE — which field a start goes in, what a missing edge
 * means, that a negative tick is refused rather than wrapped onto the sentinel.
 * None of that says the engine agrees. This sends each op down the same socket
 * the page uses and reads the result back out of shared memory, so a field the
 * two sides disagree about shows up as a clip that did not move rather than as a
 * test that passes on both sides of a mismatch.
 *
 * Read back with `daw-cli get extents` rather than through the page: the page is
 * the thing being tested at the other end, and a bug in its decode would
 * otherwise hide a bug in the encode.
 */

import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { startStack } from './stack.mjs';

const CLI = fileURLToPath(new URL('../../ui/target/release/daw-cli', import.meta.url));

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack();
const ws = new WebSocket(`ws://127.0.0.1:${stack.base + 2}`);
await new Promise((r, j) => {
  ws.addEventListener('open', r);
  ws.addEventListener('error', () => j(new Error('cmd socket refused')));
});
const say = (m) => ws.send(JSON.stringify(m));
const settle = (ms = 400) => new Promise((r) => setTimeout(r, ms));

/**
 * The placements the engine currently holds, straight from shared memory.
 *
 * The segment comes through the ENVIRONMENT, not a flag: daw-cli reads
 * DAW_UI_SHM_NAME, and an unrecognised flag makes it print its usage and exit —
 * which parses as zero placements and reads exactly like an empty song.
 */
function extents() {
  let out = '';
  try {
    out = execFileSync(CLI, ['get', 'extents'],
                       { encoding: 'utf8', env: { ...process.env, DAW_UI_SHM_NAME: stack.shm } });
  } catch (e) { return []; }
  // One JSON object per line, with a trailing comma the array form does not
  // want. Parsed per line rather than as a document, because the trailing comma
  // makes the whole thing invalid JSON — and `id` is spelled `placement`.
  const out2 = [];
  for (const line of out.split('\n')) {
    const t = line.trim().replace(/,$/, '');
    if (!t.startsWith('{')) continue;
    try {
      const o = JSON.parse(t);
      out2.push({ id: o.placement, clip: o.clip, track: o.track,
                  start: o.start, end: o.end, audio: o.audio });
    } catch (e) { /* not a placement line */ }
  }
  return out2;
}

/** A daw-cli write. `--force` because the sidecar owns the command ring. */
function cliDo(args) {
  try {
    return execFileSync(CLI, args,
                        { encoding: 'utf8', env: { ...process.env, DAW_UI_SHM_NAME: stack.shm } });
  } catch (e) { return 'ERR ' + (e.stdout || e.message); }
}

const NANOTICKS_PER_QUARTER = 960000;
const BAR = NANOTICKS_PER_QUARTER * 4;

try {
  say({ type: 'load', name: 'webtest' });
  await settle(1500);

  const before = extents();
  check(before.length > 0, 'the fixture has placements to move',
        `daw-cli returned ${before.length}`);
  if (!before.length) throw new Error('nothing to test against');

  const target = before[0];
  const startId = target.id;

  console.log('\nmove');
  {
    const to = target.start + BAR * 2;
    say({ type: 'placement', op: 'move', track: target.track, id: startId, at: to });
    await settle();
    const now = extents().find((e) => e.id === startId);
    check(now && now.start === to, 'the clip is where it was sent',
          now ? `start=${now.start}, wanted ${to}` : 'the placement vanished');
    // The point of a stable id: it survived an edit. If ids were still list
    // indices this lookup would silently find a different clip.
    check(now !== undefined, 'and it still answers to the same id');
    // A move must not resize.
    check(now && now.end - now.start === target.end - target.start,
          'a move does not change the length',
          now && `${now.end - now.start} vs ${target.end - target.start}`);
  }

  console.log('\ntrim');
  {
    const cur = extents().find((e) => e.id === startId);
    // RIGHT EDGE: length only, start omitted. This is the case the all-ones
    // sentinel exists for — if `at` were being sent as a real value, or the
    // sentinel decoded as a position, the clip would jump to tick 0 or to
    // 2^64-1 here rather than staying put.
    const newLen = BAR * 3;
    say({ type: 'placement', op: 'resize', track: cur.track, id: startId, len: newLen });
    await settle();
    let now = extents().find((e) => e.id === startId);
    check(now && now.start === cur.start, 'a right-edge trim leaves the start alone',
          now && `start=${now.start}, was ${cur.start}`);
    check(now && now.end - now.start === newLen, 'and sets the length',
          now && `len=${now.end - now.start}, wanted ${newLen}`);

    // LEFT EDGE: both fields in one command. Two commands would show the clip at
    // an intermediate position; one is why ResizePlacement carries both.
    const at = now.start + BAR;
    const len = now.end - at;
    say({ type: 'placement', op: 'resize', track: cur.track, id: startId, at, len });
    await settle();
    now = extents().find((e) => e.id === startId);
    check(now && now.start === at && now.end - now.start === len,
          'a left-edge trim moves the start and the length together',
          now && `start=${now.start} len=${now.end - now.start}, wanted ${at}/${len}`);
  }

  console.log('\nadd and remove');
  {
    const cur = extents().find((e) => e.id === startId);
    const n0 = extents().length;
    say({ type: 'placement', op: 'add', track: cur.track, clip: cur.clip,
          at: BAR * 20, len: BAR * 2 });
    await settle();
    const after = extents();
    check(after.length === n0 + 1, 'add places one more clip', `${n0} -> ${after.length}`);
    const added = after.find((e) => e.start === BAR * 20);
    check(added !== undefined, 'at the position it was given');
    // A NEW id, not a reused one. A recycled id is a valid id that refers to a
    // different clip, which is the failure a stable id exists to prevent.
    check(added && !before.some((e) => e.id === added.id),
          'with an id no earlier placement had', added && `id=${added.id}`);

    say({ type: 'placement', op: 'remove', track: added.track, id: added.id });
    await settle();
    const gone = extents();
    check(gone.length === n0, 'remove takes it away again', `${gone.length} vs ${n0}`);
    check(!gone.some((e) => e.id === added.id), 'and the id is not there');
    // The clip it referred to is untouched: a placement is a reference, and
    // removing one must not destroy the material.
    check(gone.some((e) => e.clip === cur.clip),
          'while the clip it pointed at survives elsewhere');
  }

  console.log('\nrefusals');
  {
    const cur = extents().find((e) => e.id === startId);
    const snapshot = JSON.stringify(extents());
    // An id nothing has. The engine says "not found" and nothing moves — the
    // point being that it does not move SOMETHING ELSE.
    say({ type: 'placement', op: 'move', track: cur.track, id: 99999, at: BAR * 5 });
    await settle();
    check(JSON.stringify(extents()) === snapshot,
          'a move addressed to nothing changes nothing');
  }

  console.log('\ncross-track');
  {
    // The lane drag. This check was written as its own opposite — it asserted the
    // engine REFUSED a cross-track move, because it did, and failing was the
    // signal to turn the gesture on. It duly failed the day the engine gained
    // one, and `CROSS_TRACK_DRAG` in arrange.js went true.
    const cur = extents().find((e) => e.id === startId);
    const dest = cur.track + 1;
    say({ type: 'placement', op: 'move', track: cur.track, id: startId,
          at: cur.start + BAR, toTrack: dest });
    await settle(600);
    const now = extents().find((e) => e.id === startId);
    check(now && now.track === dest, 'a clip moves to another lane',
          now ? `track=${now.track}, wanted ${dest}` : 'the placement vanished');
    check(now && now.start === cur.start + BAR, 'and moves in time in the same command',
          now && `start=${now.start}, wanted ${cur.start + BAR}`);
    // The id is the whole reason a drag can be keyed on it: crossing a lane must
    // not mint a new one, or the thing under the mouse changes identity mid-drag.
    check(now !== undefined, 'keeping its stable id across the lane change');
  }

  console.log('\nundo');
  {
    const cur = extents().find((e) => e.id === startId);
    say({ type: 'placement', op: 'move', track: cur.track, id: startId, at: BAR * 30 });
    await settle();
    check(extents().find((e) => e.id === startId).start === BAR * 30, 'moved, ready to undo');

    /*
     * A BASE-LESS UNDO IS FILLED IN, AND THAT CHANGED — read this before
     * believing an older comment about it.
     *
     * The engine validates Undo against the clip version, so a command carrying
     * base 0 is stale by definition and never applies. For a long time this file
     * sent a bare `{type:"undo"}`, watched nothing happen, and PINNED that as
     * correct. It cost an hour the first time — it reads exactly like "placement
     * ops are not undoable after all", and I was one message from reporting it as
     * an engine defect.
     *
     * What made it true was that the PAGE always stamped a base. M2.17 ended
     * that: acceptance went per-track, the page cannot know a per-track counter,
     * and it stopped stamping anything. So the sidecar resolves the base now —
     * per-track for a track-scoped edit, global for Undo, Redo, Load and Save —
     * and an explicit base is still honoured, which is what leaves optimistic
     * concurrency available to anyone who wants it.
     *
     * Which means a base-less undo APPLIES now, and that is the better contract:
     * "undo" is a complete sentence, and silently dropping it is not arbitration,
     * it is a command that does nothing for a reason no caller can see.
     */
    say({ type: 'undo' });
    await settle(900);
    const back = extents().find((e) => e.id === startId);
    check(back && back.start === cur.start,
          'an undo with no base is resolved by the sidecar and applies',
          back && `start=${back.start}, was ${cur.start}`);
    // The id has to survive the store swap, or every held reference breaks on
    // the commonest operation in a DAW.
    check(back !== undefined, 'and the placement id survives the undo');
  }
} catch (e) {
  fail++;
  console.log('  FAIL  threw:', e.message);
} finally {
  try { ws.close(); } catch {}
  stack.stop();
}

console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
