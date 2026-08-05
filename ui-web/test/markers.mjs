/**
 * THE SPINE — named ticks, the spans between them, and the one command that moves music.
 *
 * A MARKER IS A NAMED TICK AND STORES NO LENGTH. Two adjacent markers are a span, so a section's
 * "length" is the next marker's tick minus this one's — derived, never stored. Every check here
 * is a consequence of that, and it is what makes the four marker ops TOTAL: they move no music
 * and can fail only on a bad id.
 *
 * WHICH MAKES THE SEPARATION THE THING WORTH TESTING. `time` is a different command with a
 * different name, and it is the one that ripples: everything at or after a tick moves, in one
 * transaction the engine refuses whole and undoes whole. Editing a label used to do that as a
 * SIDE EFFECT, with no undo entry big enough to hold it. So this asserts that a marker op moves
 * nothing, that a time edit moves everything, and that undo puts it back.
 *
 * BARS ARE THE ENGINE'S. `bar` and `beat` are prefix-summed through the meter map there, and
 * this proves that matters by setting 7/8 and requiring the numbers to CHANGE on markers whose
 * ticks did not move. Under `tick / barLength` they would not change at all, and the labels
 * would sit between ruler numbers that disagree with them. This app computed bars that way until
 * v29 made mid-song meter authorable — so this is the check that the lie is over.
 *
 * Every edit is checked against the ENGINE's published spine, never against the reply: an ack is
 * a receipt for a message, not for an outcome. And every state is checked in the STRIP as well,
 * from the DOM, because the decode being right and the picture being right are different claims.
 *
 * BOTH SURFACES for each operation, per the standing rule that the console and the UI have all the
 * functionality: a pointer-only capability and a console-only one are the same defect.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

const BAR = 3840000;                     // one 4/4 bar in nanoticks

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8 });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1000);

/** The engine's spine, as published. The authority for every assertion here. */
const spine = () => page.evaluate(() => window.__uni.markers());
/** The STRIP's spine, read back from the DOM. */
const strip = () => page.evaluate(() => window.__uni.arrangeProbe().spine);
/** Type a console line and hand back everything it printed. */
const type = async (line) => {
  const log = await page.evaluate((c) => window.__uni.run(c), line);
  await page.waitForTimeout(600);
  const at = (log || []).lastIndexOf(`in: > ${line}`);
  const mine = at >= 0 ? log.slice(at + 1) : (log || []);
  return mine.filter((l) => String(l).startsWith('out:'))
             .map((l) => String(l).slice(5)).join('\n');
};
/** `name@bar+bars` per marker, in spine order. One string, easy to diff. */
const shape = async () => (await spine()).list.map((m) => `${m.name}@${m.bar}+${m.bars}`);

await page.evaluate(() => window.__uni.run('view arrange'));
await page.waitForTimeout(400);

// ---------------------------------------------------------------------------
// AN EMPTY SPINE, and the app SAYS SO.
//
// First, because "nothing is missing" is trivially true of a surface that draws nothing: this
// has to be established before any later claim about the strip means anything.
// ---------------------------------------------------------------------------
check((await spine()).count === 0, 'a new project has no markers');
check(/no markers/.test(await type('markers')),
      'and the console says so rather than printing an empty list');
check((await strip()).count === 0, 'nothing is drawn on the strip yet');

// ---------------------------------------------------------------------------
// ADD, from the console. A marker takes a TICK and no length — the whole simplification.
// ---------------------------------------------------------------------------
await type('marker 0 intro');
await type(`marker ${4 * BAR} verse`);
await type(`marker ${12 * BAR} chorus`);
{
  const m = await spine();
  check(m.count === 3, 'three markers', String(m.count));
  /*
   * IN TICK ORDER, whatever order they arrived in. The engine keeps the spine sorted, and a strip
   * deriving a span from an unsorted list would draw negative widths — which is why the decoder
   * clamps, and why this is asserted rather than assumed.
   */
  check(JSON.stringify(m.list.map((x) => x.tick)) === JSON.stringify([0, 4 * BAR, 12 * BAR]),
        'in tick order', JSON.stringify(m.list.map((x) => x.tick)));
  /*
   * THE SPAN EACH ONE BEGINS IS DERIVED, from the next marker. Nothing stores it — the v29
   * contract in one assertion.
   */
  check(m.list[0].endTick === 4 * BAR && m.list[0].bars === 4,
        'and the span each one begins is derived from the NEXT marker',
        JSON.stringify({ end: m.list[0].endTick, bars: m.list[0].bars }));
  check(m.list[1].bars === 8, 'including the middle one', String(m.list[1].bars));
  // The bars come from the ENGINE, prefix-summed. 4/4 here, so 1, 5, 13.
  check(JSON.stringify(m.list.map((x) => x.bar)) === JSON.stringify([1, 5, 13]),
        'with the bar numbers the engine resolved', JSON.stringify(m.list.map((x) => x.bar)));
}
{
  const drawn = (await strip()).visible;
  check(drawn.length === 3 && drawn[0].name === 'intro', 'and the strip draws all three',
        JSON.stringify(drawn.map((d) => d.name)));
  /*
   * THE LAST ONE IS STILL VISIBLE. Its span runs to `songEnd`, which in a song with no material
   * is its own tick — so the derived width is zero, and a zero-width block draws as nothing and
   * reads as a MISSING marker. Floored, so it stays clickable, nameable and removable.
   */
  check(drawn[2] && drawn[2].w > 0, 'including the last, which has no span after it',
        JSON.stringify(drawn[2]));
}

// A multi-word name. `rest: true` hands the words over separately, and taking only the first
// renamed one to "VERSE" when the person typed "VERSE A".
await type(`marker ${20 * BAR} drop it`);
check((await spine()).list[3].name === 'drop it', 'a multi-word name arrives whole',
      (await spine()).list[3].name);
await type(`delmarker ${(await spine()).list[3].id}`);

// ---------------------------------------------------------------------------
// ADD, by pointer — the `+` in the gutter. It takes no argument: a marker goes at the playhead,
// which is where "here" is.
// ---------------------------------------------------------------------------
{
  const before = (await spine()).count;
  await page.locator('.ar-spine-btn').nth(0).click();
  await page.waitForTimeout(800);
  const after = await spine();
  check(after.count === before + 1, 'the + button adds a marker',
        `${before} -> ${after.count}`);
  const ids = (await strip()).visible.map((v) => v.id);
  check(ids.length === after.count, 'and the strip draws it', JSON.stringify(ids));
  // Removed again, so the ticks below stay the ones this suite chose.
  const added = after.list.find((m) => !['intro', 'verse', 'chorus'].includes(m.name));
  if (added) { await type(`delmarker ${added.id}`); await page.waitForTimeout(500); }
}

// ---------------------------------------------------------------------------
// SELECT, by pointer — and the console can see what was selected.
// ---------------------------------------------------------------------------
{
  const box = await page.locator('.ar-span').nth(1).boundingBox();
  await page.mouse.click(box.x + 20, box.y + box.height / 2);
  await page.waitForTimeout(300);
  const want = (await spine()).list[1].id;
  check((await strip()).selected === want, 'clicking a marker selects it',
        `${(await strip()).selected} vs ${want}`);
  // The SAME id the console names — a surface with its own idea of "this marker" is one the
  // console cannot agree with.
  const seen = await page.evaluate(() => window.__uni.state().selectedMarker);
  check(seen === want, 'and the page holds the id the console would name', String(seen));
  await page.mouse.click(box.x + 20, box.y + box.height / 2);
  await page.waitForTimeout(250);
  check((await strip()).selected === 0, 'clicking it again clears the selection');
}

// ---------------------------------------------------------------------------
// RENAME, both ways.
// ---------------------------------------------------------------------------
{
  const id = (await spine()).list[0].id;
  await type(`namemarker ${id} the top`);
  check((await spine()).list[0].name === 'the top', 'namemarker renames, spaces and all',
        (await spine()).list[0].name);
  const drawn = (await strip()).visible.find((v) => v.id === id);
  check(drawn && drawn.name === 'the top', 'and the strip draws the new name',
        JSON.stringify(drawn));

  page.once('dialog', (d) => d.accept('DOUBLED'));
  const box = await page.locator(`.ar-span[data-marker="${id}"]`).boundingBox();
  await page.mouse.dblclick(box.x + 20, box.y + box.height / 2);
  await page.waitForTimeout(800);
  check((await spine()).list[0].name === 'DOUBLED',
        'double-clicking a marker renames it', (await spine()).list[0].name);

  page.once('dialog', (d) => d.dismiss());
  await page.mouse.dblclick(box.x + 20, box.y + box.height / 2);
  await page.waitForTimeout(600);
  check((await spine()).list[0].name === 'DOUBLED', 'and cancelling it changes nothing');
}

// ---------------------------------------------------------------------------
// MOVING A MARKER MOVES THE MARKER, AND NOTHING ELSE.
//
// The difference from the sections it replaces, where changing a "length" rippled the song.
// ---------------------------------------------------------------------------
{
  const before = await spine();
  const id = before.list[1].id;
  await type(`movemarker ${id} ${6 * BAR}`);
  const after = await spine();
  check(after.list[1].tick === 6 * BAR, 'movemarker moves it', String(after.list[1].tick));
  check(after.list[2].tick === before.list[2].tick,
        'and NOTHING else moves — a marker op touches no music',
        `${before.list[2].tick} -> ${after.list[2].tick}`);
  // ...and the derived spans follow, because they are derived.
  check(after.list[0].bars === 6, 'the span before it grew, because a span is a subtraction',
        String(after.list[0].bars));
}

// ---------------------------------------------------------------------------
// INSERTING TIME MOVES EVERYTHING AT OR AFTER IT — the capability the drag needs, and the reason
// sections were worth replacing rather than deleting.
// ---------------------------------------------------------------------------
{
  const before = await spine();
  await type(`time ${6 * BAR} 2`);
  await page.waitForTimeout(900);
  const after = await spine();
  check(after.list[0].tick === before.list[0].tick,
        'a marker BEFORE the point stays where it is');
  check(after.list[1].tick === before.list[1].tick + 2 * BAR,
        'the marker AT the point moves with the music',
        `${before.list[1].tick} -> ${after.list[1].tick}`);
  check(after.list[2].tick === before.list[2].tick + 2 * BAR,
        'and so does every later one',
        `${before.list[2].tick} -> ${after.list[2].tick}`);

  /*
   * AND IT IS UNDOABLE, which the section version never was: it moved every placement on every
   * track plus three song timelines and pushed no undo entry, because the entry held at most two
   * tracks. There is a song-scoped one now.
   */
  await type('undo');
  await page.waitForTimeout(900);
  check(JSON.stringify((await spine()).list.map((m) => m.tick))
        === JSON.stringify(before.list.map((m) => m.tick)),
        'and undo puts the whole ripple back',
        JSON.stringify((await spine()).list.map((m) => m.tick)));
}

// ---------------------------------------------------------------------------
// THE BOUNDARY DRAG, which sends that same time edit.
//
// The gesture is unchanged from the section version and its meaning is now literal: the handle
// you grab is the next marker, and the next marker is what moves.
// ---------------------------------------------------------------------------
{
  const before = await spine();
  const first = before.list[0];
  const el = page.locator(`.ar-span[data-marker="${first.id}"]`);
  const box = await el.boundingBox();
  const barPx = box.width / first.bars;
  const y = box.y + box.height / 2;

  await page.mouse.move(box.x + box.width - 3, y);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width - 3 + 2 * barPx, y, { steps: 8 });
  /*
   * MID-DRAG: the preview follows the pointer AND SURVIVES A FRAME. It did not once — the render
   * loop rebound the width from the model on the next frame, and there is always a next frame,
   * so the boundary snapped back under the pointer while the drag was live. The gesture read as
   * broken and the command it sent was correct, which is the worse way round.
   */
  const midW = await el.evaluate((n) => parseFloat(n.style.width));
  check(Math.abs(midW - (box.width + 2 * barPx)) < 3,
        'the boundary follows the pointer during the drag, and stays there',
        `${midW} vs ${box.width + 2 * barPx}`);
  await page.mouse.up();
  await page.waitForTimeout(1000);

  const after = await spine();
  check(after.list[1].tick === before.list[1].tick + 2 * BAR,
        'and releasing it inserts the time — the NEXT marker moved',
        `${before.list[1].tick} -> ${after.list[1].tick}`);
  check(after.list[2].tick === before.list[2].tick + 2 * BAR,
        'along with everything after it');
  await type('undo');
  await page.waitForTimeout(900);
}

// ---------------------------------------------------------------------------
// A MID-SONG METER, and why the bars must be the engine's.
// ---------------------------------------------------------------------------
{
  const before = (await spine()).list.map((m) => ({ tick: m.tick, bar: m.bar }));
  await type('timesig 7/8 0');
  await page.waitForTimeout(900);
  const after = await spine();
  check(JSON.stringify(after.list.map((m) => m.tick))
        === JSON.stringify(before.map((m) => m.tick)),
        'setting a meter moves no marker — their ticks are unchanged',
        JSON.stringify(after.list.map((m) => m.tick)));
  /*
   * BUT THEIR BAR NUMBERS CHANGE. This is the assertion the whole meter argument rests on: bar
   * numbering is a prefix sum through the map, so the same tick is a different bar in 7/8. A
   * client dividing ticks by one bar length would report the SAME numbers here and be wrong.
   */
  const moved = after.list.filter((m, i) => m.bar !== before[i].bar).length;
  check(moved > 0, 'but their BAR NUMBERS change, because bars are a prefix sum through the map',
        `${JSON.stringify(before.map((m) => m.bar))} -> `
        + `${JSON.stringify(after.list.map((m) => m.bar))}`);
  await type('timesig 4/4 0');
  await page.waitForTimeout(700);
}

// ---------------------------------------------------------------------------
// REMOVE — which is NOT destructive — and the − button.
// ---------------------------------------------------------------------------
{
  const before = await spine();
  const id = before.list[before.count - 1].id;
  await type(`delmarker ${id}`);
  const after = await spine();
  check(after.count === before.count - 1, 'delmarker removes one',
        `${before.count} -> ${after.count}`);
  /*
   * AND THE STRIP FORGOT IT. `store.markers` is a POOL — it grows and is never shrunk — so after
   * a removal the tail still holds the marker just deleted, with its name and position. A reader
   * that ignores the count draws it, which my own accessor once did.
   */
  const ids = (await strip()).visible.map((v) => v.id);
  check(!ids.includes(id), 'and the strip is not still drawing it', JSON.stringify(ids));
}
{
  await page.evaluate(() => window.__uni.selectMarker(0));
  await page.waitForTimeout(200);
  const before = (await spine()).count;
  await page.locator('.ar-spine-btn').nth(1).click();
  await page.waitForTimeout(600);
  check((await spine()).count === before, 'the − button with nothing selected removes nothing');
  const said = await page.evaluate(() => window.__uni.state().reject);
  check(/select/i.test(String(said)), 'and says which action is missing', String(said));

  const id = (await spine()).list[0].id;
  await page.evaluate((i) => window.__uni.selectMarker(i), id);
  await page.waitForTimeout(250);
  await page.locator('.ar-spine-btn').nth(1).click();
  await page.waitForTimeout(700);
  check((await spine()).count === before - 1, 'and with one selected it removes that one',
        `${before} -> ${(await spine()).count}`);
  check((await strip()).selected === 0,
        'clearing the selection with it — an id nothing holds is not a selection');
}

// ---------------------------------------------------------------------------
// AN ID THAT DOES NOT EXIST is refused BY NAME, on every op that addresses one.
// ---------------------------------------------------------------------------
{
  const gone = 9999;
  for (const line of [`delmarker ${gone}`, `namemarker ${gone} x`, `movemarker ${gone} 0`]) {
    const said = await type(line);
    check(/no marker/i.test(said), `\`${line}\` names the missing marker`, said);
  }
  check(/beats\/note-value/.test(await type('timesig 7/0')),
        'and a nonsense time signature is refused by shape');
}

// ---------------------------------------------------------------------------
// THE SPINE SURVIVES A SAVE AND A LOAD. The last thing a person has is the file.
// ---------------------------------------------------------------------------
{
  await type(`marker ${28 * BAR} finale`);
  const before = await shape();
  await type('save spinecheck');
  await page.waitForTimeout(2000);
  const seq = await page.evaluate(() => (window.__uni.loadStatus() || {}).seq || 0);
  await page.evaluate(() => window.__uni.loadProject('spinecheck'));
  await page.waitForFunction((s) => {
    const l = window.__uni.loadStatus(); return l && l.seq > s && l.ok;
  }, seq, { timeout: 15000 }).catch(() => {});
  await page.waitForTimeout(900);
  check(JSON.stringify(await shape()) === JSON.stringify(before),
        'the spine comes back from the file exactly as it went in',
        `${JSON.stringify(before)} vs ${JSON.stringify(await shape())}`);
  const drawn = (await strip()).visible.map((v) => v.name);
  const want = (await spine()).list.slice(0, drawn.length).map((m) => m.name);
  check(JSON.stringify(drawn) === JSON.stringify(want),
        'and the strip is drawing the loaded spine, not the one before it',
        `${JSON.stringify(drawn)} vs ${JSON.stringify(want)}`);
}

check(errors.length === 0, 'and nothing threw while asking', errors.slice(0, 2).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
