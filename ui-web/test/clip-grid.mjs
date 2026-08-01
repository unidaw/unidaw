/**
 * A CLIP'S OWN GRID (opcode 94), from the UI.
 *
 * `linesPerBeat`, `timeSigNumerator` and `timeSigDenominator` have been persisted on
 * ProjectClip and published in `UiClipExtent`'s flag bits since kShmVersion 19, and the
 * renderer has honoured them FIRST — a lane's rows resolve clip -> track -> zoom. No
 * command could write any of them, so the case per-lane grids exist for, a verse in 4
 * and a bridge in 3 on ONE track, could be loaded from hand-edited JSON and never
 * authored. Backend landed the opcode; this is the UI half.
 *
 * WHAT THIS ASSERTS, in the order that makes each next claim mean anything:
 *   1. the command ARRIVES — a published read-back moves, not just a hopeful send
 *   2. it lands on the clip that was NAMED, checked both ways round, because a
 *      handler that edits the first match passes when the clip under test is first
 *      (backend's own negative control found exactly that in the engine)
 *   3. each field can be set ALONE — per-field flags mean the test has to omit each
 *      field in turn, not confirm that some field survived
 *   4. out-of-range values are REFUSED, not clamped
 *   5. the tracker's grid actually changes, which is the whole point
 *   6. the header badge names the level in force, and its click edits THAT level
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8 });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
page.on('pageerror', (e) => check(false, 'no page error', e.message));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1200);
await page.evaluate(() => window.__uni.run('view tracker'));
await page.evaluate(() => window.__uni.loadProject('meter'));
await page.waitForTimeout(2500);

/** Every clip the engine publishes, with its grid — the read-back, not our own hope. */
const clips = async () => (await page.evaluate(() => window.__uni.meters())).clips;
const clipOf = async (id) => (await clips()).find((c) => c.clipId === id);

console.log('\n[the project]');
const all = await clips();
check(all.length >= 2, 'the project has at least two clips to tell apart',
      JSON.stringify(all.map((c) => c.clipId)));
const withGrid = all.filter((c) => c.grid);
check(withGrid.length >= 2, 'and at least two publish a grid of their own',
      JSON.stringify(withGrid.map((c) => `${c.clipId}:${c.grid.linesPerBeat}`)));

const A = withGrid[0], B = withGrid[1];
console.log(`  clip A = ${A.clipId} on track ${A.track}, clip B = ${B.clipId} on track ${B.track}`);

/** Set one field and wait for the published extent to show it. */
const setGrid = async (track, clip, opts) => {
  const ok = await page.evaluate(({ t, c, o }) => window.__uni.clipGrid(t, c, o),
                                 { t: track, c: clip, o: opts });
  await page.waitForTimeout(900);
  return ok;
};

console.log('\n[it arrives]');
const aWas = (await clipOf(A.clipId)).grid;
await setGrid(A.track, A.clipId, { lines: aWas.linesPerBeat === 6 ? 3 : 6 });
const aNow = (await clipOf(A.clipId)).grid;
check(aNow.linesPerBeat !== aWas.linesPerBeat,
      'a clip-grid command moves the published grid',
      `${aWas.linesPerBeat} -> ${aNow.linesPerBeat}`);

/**
 * ADDRESSED BOTH WAYS ROUND. Editing A and checking B is only half: a handler that
 * edits the first clip it finds passes that when A happens to be first. So the same
 * assertion is made from each end.
 */
console.log('\n[it lands on the clip that was named]');
const bBefore = (await clipOf(B.clipId)).grid;
check(bBefore.linesPerBeat === B.grid.linesPerBeat,
      'editing A did not touch B',
      `B ${B.grid.linesPerBeat} -> ${bBefore.linesPerBeat}`);
await setGrid(B.track, B.clipId, { lines: bBefore.linesPerBeat === 8 ? 12 : 8 });
const aAfterB = (await clipOf(A.clipId)).grid;
check(aAfterB.linesPerBeat === aNow.linesPerBeat,
      'and editing B did not touch A',
      `A ${aNow.linesPerBeat} -> ${aAfterB.linesPerBeat}`);
check((await clipOf(B.clipId)).grid.linesPerBeat !== bBefore.linesPerBeat,
      'while B itself moved');

/**
 * EACH FIELD ALONE. The flags exist so that setting the meter does not reset the
 * subdivision — which can only be tested by OMITTING a field and watching it survive.
 * Backend's check sent the numerator every time and passed with the numerator flag
 * ignored entirely.
 */
console.log('\n[one field at a time]');
const base = (await clipOf(A.clipId)).grid;
await setGrid(A.track, A.clipId, { num: base.numerator === 5 ? 7 : 5 });
let g = (await clipOf(A.clipId)).grid;
check(g.numerator !== base.numerator, 'the numerator alone can be set',
      `${base.numerator} -> ${g.numerator}`);
check(g.linesPerBeat === base.linesPerBeat, 'and the subdivision was left alone',
      `${base.linesPerBeat} -> ${g.linesPerBeat}`);
check(g.denominator === base.denominator, 'and so was the denominator',
      `${base.denominator} -> ${g.denominator}`);

const mid = g;
await setGrid(A.track, A.clipId, { den: mid.denominator === 8 ? 4 : 8 });
g = (await clipOf(A.clipId)).grid;
check(g.denominator !== mid.denominator, 'the denominator alone can be set',
      `${mid.denominator} -> ${g.denominator}`);
check(g.numerator === mid.numerator, 'and the numerator was left alone',
      `${mid.numerator} -> ${g.numerator}`);
check(g.linesPerBeat === mid.linesPerBeat, 'and the subdivision again',
      `${mid.linesPerBeat} -> ${g.linesPerBeat}`);

const mid2 = g;
await setGrid(A.track, A.clipId, { lines: mid2.linesPerBeat === 4 ? 3 : 4 });
g = (await clipOf(A.clipId)).grid;
check(g.linesPerBeat !== mid2.linesPerBeat, 'the subdivision alone can be set',
      `${mid2.linesPerBeat} -> ${g.linesPerBeat}`);
check(g.numerator === mid2.numerator && g.denominator === mid2.denominator,
      'and the meter was left alone', `${g.numerator}/${g.denominator}`);

/**
 * REFUSED, NOT CLAMPED. Every bound is a packing limit, so a clamp would store a grid
 * nobody asked for and report success. The refusal has to be visible in the UI too —
 * the engine's own refusal is a log line a browser cannot read.
 */
console.log('\n[refusals]');
const before = (await clipOf(A.clipId)).grid;
for (const [opts, why] of [
  [{ lines: 0 }, 'lines 0 is the packer\'s no-grid sentinel'],
  [{ lines: 32 }, 'lines 32 packs as 0 in five bits'],
  [{ num: 0 }, 'numerator 0'],
  [{ num: 32 }, 'numerator 32'],
  [{ den: 6 }, 'denominator 6 is not a power of two'],
  [{ den: 256 }, 'denominator 256 is past the 3-bit exponent'],
  [{}, 'a command naming no field at all'],
]) {
  const ok = await page.evaluate(({ t, c, o }) => window.__uni.clipGrid(t, c, o),
                                 { t: A.track, c: A.clipId, o: opts });
  const reject = await page.evaluate(() => window.__uni.state().reject);
  check(ok === false && !!reject, `refused: ${why}`, `returned ${ok}, reject ${JSON.stringify(reject)}`);
}
await page.waitForTimeout(600);
const after = (await clipOf(A.clipId)).grid;
check(JSON.stringify(after) === JSON.stringify(before),
      'and not one of them changed the clip',
      `${JSON.stringify(before)} -> ${JSON.stringify(after)}`);

/**
 * THE ROWS ACTUALLY CHANGE. Everything above is the model; this is the point. Put the
 * cursor in clip A, set a subdivision the axis cannot divide evenly, and confirm the
 * tracker draws off-grid rows that it did not draw before.
 */
console.log('\n[the tracker follows]');
await page.evaluate((t) => window.__uni.goto(0, t), A.track);
await page.evaluate(() => window.__uni.setZoom(1));
await page.waitForTimeout(300);
const offRows = () => page.evaluate((t) => {
  const out = [];
  for (const rowEl of document.querySelectorAll('[data-row]')) {
    const r = Number(rowEl.dataset.row);
    if (!Number.isFinite(r) || r < 0 || r >= 12) continue;
    const cell = rowEl.querySelector(`.tk-cell[data-track="${t}"]`);
    if (cell && cell.dataset.kind === 'offgrid') out.push(r);
  }
  return out.sort((a, b) => a - b);
}, A.track);

await setGrid(A.track, A.clipId, { lines: 16 });
await page.waitForTimeout(400);
const dense = await offRows();
await setGrid(A.track, A.clipId, { lines: 1 });
await page.waitForTimeout(400);
const sparse = await offRows();
check(sparse.length > dense.length,
      'a coarser clip grid leaves more rows off-grid',
      `at 16/beat ${JSON.stringify(dense)}, at 1/beat ${JSON.stringify(sparse)}`);

/**
 * THE BADGE NAMES THE LEVEL IN FORCE, AND ITS CLICK EDITS THAT LEVEL.
 *
 * The badge used to print the TRACK's subdivision whatever the rows were drawn at, and
 * its click wrote the track's — so on a clip-gridded lane it changed a number nothing
 * was using and the screen did not move.
 */
console.log('\n[the header badge]');
const badge = async () => page.evaluate((t) => {
  const h = document.querySelector(`.htrack[data-track="${t}"] .hlpb`);
  return h ? { text: h.textContent.trim(), fromClip: h.classList.contains('from-clip') } : null;
}, A.track);

await page.evaluate((t) => window.__uni.goto(0, t), A.track);
await page.waitForTimeout(300);
const b1 = await badge();
const live = (await clipOf(A.clipId)).grid;
check(b1 !== null && b1.text.startsWith(String(live.linesPerBeat)),
      'the badge shows the subdivision actually in force at the cursor',
      `badge ${JSON.stringify(b1)} vs clip ${live.linesPerBeat}/beat`);
check(b1 !== null && b1.fromClip,
      'and marks it as coming from the clip, not the track', JSON.stringify(b1));

const trackLpbBefore = (await page.evaluate(() => window.__uni.engine())).lpb[A.track];
await page.click(`.htrack[data-track="${A.track}"] .hlpb`);
await page.waitForTimeout(900);
const afterClick = (await clipOf(A.clipId)).grid;
const trackLpbAfter = (await page.evaluate(() => window.__uni.engine())).lpb[A.track];
check(afterClick.linesPerBeat !== live.linesPerBeat,
      'clicking it changes the CLIP\'s subdivision',
      `${live.linesPerBeat} -> ${afterClick.linesPerBeat}`);
check(trackLpbAfter === trackLpbBefore,
      'and leaves the track\'s own value alone',
      `track ${trackLpbBefore} -> ${trackLpbAfter}`);

/**
 * AND THE CONSOLE SAYS THE SAME THING. Every verb has to be reachable both ways or
 * "one grammar that drives every surface" is only true of the parts I remembered.
 */
console.log('\n[the console verb]');
const said = await page.evaluate(({ t, c }) =>
  window.__uni.run(`clip-grid ${t} ${c} lines 2`), { t: A.track, c: A.clipId });
await page.waitForTimeout(900);
check((await clipOf(A.clipId)).grid.linesPerBeat === 2,
      'clip-grid <track> <clip> lines <n> sets it', String(said));
const bad = await page.evaluate(({ t, c }) =>
  window.__uni.run(`clip-grid ${t} ${c} den 6`), { t: A.track, c: A.clipId });
check(/refus|power of two/i.test(String(bad)),
      'and says why when the value cannot be represented', String(bad));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
