/**
 * THE SPINE — the named spans a song is built out of, on both surfaces.
 *
 * A section HAS A LENGTH AND NO POSITION. Where it begins is the sum of the lengths
 * before it, resolved by the engine through the meter at every boundary. That single
 * fact is what this suite is really testing, because it is what every one of the five
 * operations is a consequence of: there is no "move this section to bar 9", only a
 * length that ripples and an order that changes, and both answers come from the engine.
 *
 * WHAT IT ASSERTS AND WHY IT IS ARRANGED THIS WAY.
 *
 * Every edit is checked against the ENGINE's published spine, never against the reply.
 * The first version of this work read `send()`'s return value — which is `true` whether
 * the sidecar accepted or refused — and reported four broken operations as working. An
 * ack is a receipt for a message, not for an outcome.
 *
 * And every state is checked in the STRIP as well, from the DOM. The decode being right
 * and the picture being right are different claims: the whole failure this feature can
 * have is a span the engine published and the strip did not draw, and a model-only
 * assertion passes straight through it.
 *
 * BOTH SURFACES, for each of the six things you can do. Jaakko's rule is that the
 * console and the UI have all the functionality, so a pointer-only capability and a
 * console-only one are the same defect, and the way to prove neither happened is to
 * drive each operation twice — once by typing and once by pointing — and assert the
 * same engine state after both.
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
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1000);

/** The engine's spine, as published. The authority for every assertion here. */
const spine = () => page.evaluate(() => window.__uni.sections());
/** The STRIP's spine, read back from the DOM. */
const strip = () => page.evaluate(() => window.__uni.arrangeProbe().spine);
/** Type a console line and hand back what the dock printed for it. */
const type = async (line) => {
  const log = await page.evaluate((c) => window.__uni.run(c), line);
  await page.waitForTimeout(600);
  // The dock keeps a rolling log; the last `out:` is this command's answer.
  const outs = (log || []).filter((l) => String(l).startsWith('out:'));
  return outs.length ? String(outs[outs.length - 1]).slice(5) : '';
};
/** `name:bars@startBar` per section, in spine order. One string, easy to diff. */
const shape = async () => (await spine()).list.map((s) => `${s.name}:${s.bars}@${s.startBar}`);

await page.evaluate(() => window.__uni.run('view arrange'));
await page.waitForTimeout(400);

// ---------------------------------------------------------------------------
// An empty project has an empty spine, and SAYS SO.
//
// First because "nothing is missing" is trivially true of a surface that draws
// nothing, so the suite has to establish that the strip starts empty before any
// later assertion about it drawing something means anything.
// ---------------------------------------------------------------------------
check((await spine()).count === 0, 'a new project has no sections');
check(await type('sections') !== '',
      'and the console says so rather than printing an empty list');
{
  const s = await strip();
  check(s.count === 0 && s.visible.length === 0, 'nothing is drawn on the strip yet',
        JSON.stringify(s.visible));
  /*
   * ...but the UNNAMED TAIL is, if the project has material. `songEnd` is the furthest
   * placement and it is not the end of the last section; with no sections at all every
   * bar of the song is unnamed, and a blank strip over a song full of clips reads as the
   * strip being broken rather than as the song being unsectioned.
   */
  const end = (await spine()).songEnd;
  check(end === 0 || s.tail.on,
        'material with no section over it is drawn as an unnamed tail',
        `songEnd ${end}, tail ${JSON.stringify(s.tail)}`);
}

// ---------------------------------------------------------------------------
// ADD, from the console.
// ---------------------------------------------------------------------------
await type('section 4 intro');
await type('section 8 verse');
check(JSON.stringify(await shape()) === JSON.stringify(['intro:4@1', 'verse:8@5']),
      'two console adds land in the order they were typed, at the bars that follow',
      JSON.stringify(await shape()));

/*
 * THE SECOND ADD IS THE WHOLE POINT OF THIS ONE.
 *
 * `add` with no position appends, and the engine's insert index CLAMPS to the end — so a
 * missing position sent as 0 inserts at the FRONT and two adds build the song backwards.
 * It did: the second section came out at bar 1. Asserted as the ORDER and not as a
 * count, because a count of two is right either way round.
 */
check((await spine()).list[0].name === 'intro',
      'and appending is appending — the first one typed is still first');

// A name with a space in it. `rest: true` hands the words over separately, and taking
// only the first renamed a section to "VERSE" when the person typed "VERSE A".
await type('section 2 drop it');
check((await spine()).list[2].name === 'drop it',
      'a multi-word name arrives whole', (await spine()).list[2].name);
await type(`delsection ${(await spine()).list[2].id}`);

// ---------------------------------------------------------------------------
// ADD, by pointer. The `+` in the spine's gutter cell.
// ---------------------------------------------------------------------------
{
  const before = (await spine()).count;
  await page.locator('.ar-spine-btn').nth(0).click();
  await page.waitForTimeout(700);
  const after = await spine();
  check(after.count === before + 1, 'the + button adds a section',
        `${before} -> ${after.count}`);
  check(after.list[after.count - 1].bars === 4,
        'four bars, which is a phrase — the drag changes it immediately');
  // ...and it is DRAWN, not merely published.
  const ids = (await strip()).visible.map((v) => v.id);
  check(ids.includes(after.list[after.count - 1].id),
        'and the new one appears on the strip', JSON.stringify(ids));
}

// ---------------------------------------------------------------------------
// SELECT, by pointer — and the console can see what was selected.
// ---------------------------------------------------------------------------
{
  const box = await page.locator('.ar-section').nth(1).boundingBox();
  await page.mouse.click(box.x + 20, box.y + box.height / 2);
  await page.waitForTimeout(300);
  const want = (await spine()).list[1].id;
  check((await strip()).selected === want, 'clicking a section selects it',
        `${(await strip()).selected} vs ${want}`);
  /*
   * The SAME id the console names. A surface with its own idea of "this section" is a
   * surface the console cannot agree with, and `−` with nothing selected would then be
   * asking a question of the wrong object.
   */
  const seen = await page.evaluate(() => window.__uni.state().selectedSection);
  check(seen === want, 'and the page holds the id the console would name', `${seen}`);
  // Clicking it again clears it: a selection you cannot drop is a mode.
  await page.mouse.click(box.x + 20, box.y + box.height / 2);
  await page.waitForTimeout(250);
  check((await strip()).selected === 0, 'clicking it again clears the selection');
}

// ---------------------------------------------------------------------------
// LENGTH, by pointer — the boundary drag, which RIPPLES.
//
// The one operation that touches material. The engine plans it across every track in
// one transaction and refuses it whole rather than half-applying, so the assertion is
// about the whole spine and not just the section that was dragged.
// ---------------------------------------------------------------------------
{
  const before = await spine();
  const first = before.list[0];
  const el = page.locator(`.ar-section[data-section="${first.id}"]`);
  const box = await el.boundingBox();
  const barPx = box.width / first.bars;
  const y = box.y + box.height / 2;

  await page.mouse.move(box.x + box.width - 3, y);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width - 3 + 2 * barPx, y, { steps: 8 });
  /*
   * MID-DRAG: the preview follows the pointer and SURVIVES A FRAME.
   *
   * It did not. The render loop rebound the width from the model on the very next frame
   * — and there is always a next frame, the playhead alone schedules one — so the
   * boundary snapped back under the pointer while the drag was live. The gesture read as
   * broken and the command it finally sent was correct, which is the worse way round.
   */
  const midW = await el.evaluate((n) => parseFloat(n.style.width));
  check(Math.abs(midW - (box.width + 2 * barPx)) < 2,
        'the boundary follows the pointer during the drag, and stays there',
        `${midW} vs ${box.width + 2 * barPx}`);
  await page.mouse.up();
  await page.waitForTimeout(900);

  const after = await spine();
  check(after.list[0].bars === first.bars + 2,
        'the drag sets the length in the ENGINE, in the section\'s own bars',
        `${first.bars} -> ${after.list[0].bars}`);
  /*
   * AND EVERYTHING AFTER IT MOVED. This is the ripple, and it is the reason a length
   * change is not a local edit: the sections after the boundary keep their own lengths
   * and start two bars later, because a start is a sum and the sum has changed.
   */
  check(after.list[1].startBar === before.list[1].startBar + 2,
        'and every later section starts two bars later — the spine is a prefix sum',
        `${before.list[1].startBar} -> ${after.list[1].startBar}`);
  check(after.list[1].bars === before.list[1].bars,
        'while their own lengths are untouched');
}

// A drag that ends where it began is not an edit — it must not spend a ripple plan and
// a version bump on a click that landed back on the boundary.
{
  const before = await spine();
  const el = page.locator(`.ar-section[data-section="${before.list[0].id}"]`);
  const box = await el.boundingBox();
  const y = box.y + box.height / 2;
  await page.mouse.move(box.x + box.width - 3, y);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width - 3 + 4, y, { steps: 3 });
  await page.mouse.move(box.x + box.width - 3, y, { steps: 3 });
  await page.mouse.up();
  await page.waitForTimeout(700);
  check((await spine()).version === before.version,
        'a drag that ends where it started sends nothing',
        `version ${before.version} -> ${(await spine()).version}`);
}

// ---------------------------------------------------------------------------
// LENGTH, from the console — the same function, so the same ripple.
// ---------------------------------------------------------------------------
{
  const before = await spine();
  const id = before.list[0].id;
  await type(`seclength ${id} 3`);
  const after = await spine();
  check(after.list[0].bars === 3, 'seclength sets the length', `${after.list[0].bars}`);
  check(after.list[1].startBar === 4,
        'and the section after it starts at bar 4', `${after.list[1].startBar}`);
  // A zero-bar section is not a short section, it is a section with no bars in it. The
  // engine refuses it as `zero_bars`; refusing it here means the reason is a sentence
  // rather than a silence.
  const said = await type(`seclength ${id} 0`);
  check(/bar/i.test(said), 'a zero-bar length is refused BY NAME', said);
  check((await spine()).list[0].bars === 3, 'and nothing changed');
}

// ---------------------------------------------------------------------------
// RENAME, both ways.
// ---------------------------------------------------------------------------
{
  const id = (await spine()).list[0].id;
  await type(`namesection ${id} the top`);
  check((await spine()).list[0].name === 'the top', 'namesection renames, spaces and all',
        (await spine()).list[0].name);
  const drawn = (await strip()).visible.find((v) => v.id === id);
  check(drawn && drawn.name === 'the top', 'and the strip draws the new name',
        JSON.stringify(drawn));

  // By pointer: double-click prompts. The prompt is the page's, so it is answered here.
  page.once('dialog', (d) => d.accept('DOUBLED'));
  const box = await page.locator(`.ar-section[data-section="${id}"]`).boundingBox();
  await page.mouse.dblclick(box.x + 20, box.y + box.height / 2);
  await page.waitForTimeout(800);
  check((await spine()).list[0].name === 'DOUBLED',
        'double-clicking a section renames it', (await spine()).list[0].name);

  // A cancelled prompt is not a rename.
  page.once('dialog', (d) => d.dismiss());
  await page.mouse.dblclick(box.x + 20, box.y + box.height / 2);
  await page.waitForTimeout(600);
  check((await spine()).list[0].name === 'DOUBLED', 'and cancelling it changes nothing');
}

// ---------------------------------------------------------------------------
// MOVE — the other way a section changes position, and the only one that is not a length.
// ---------------------------------------------------------------------------
{
  const before = await spine();
  const secondId = before.list[1].id;
  const secondName = before.list[1].name;
  await type(`movesection ${secondId} 1`);
  const after = await spine();
  check(after.list[0].id === secondId, 'movesection reorders the spine',
        `${after.list.map((s) => s.name).join(',')}`);
  check(after.list[0].startBar === 1,
        'and the one that moved to the front starts at bar 1');
  check(after.list[0].name === secondName, 'carrying its name and length with it');
  // The strip redrew in the new order, rather than keeping the old positions.
  const drawn = (await strip()).visible;
  check(drawn.length === 0 || drawn[0].id === secondId,
        'and the strip draws them in the new order', JSON.stringify(drawn.map((d) => d.id)));
}

// ---------------------------------------------------------------------------
// REMOVE — which is NOT destructive, and both ways.
// ---------------------------------------------------------------------------
{
  const before = await spine();
  const id = before.list[before.count - 1].id;
  await type(`delsection ${id}`);
  const after = await spine();
  check(after.count === before.count - 1, 'delsection removes one',
        `${before.count} -> ${after.count}`);
  check(!after.list.some((s) => s.id === id), 'the right one', String(id));
  /*
   * AND THE STRIP FORGOT IT.
   *
   * `store.sections` is a POOL: it grows and is never shrunk, so after a removal the
   * array is longer than the count and the tail still holds the deleted section with its
   * name and its position. A reader that ignores the count draws it — which my own
   * accessor did — so this asserts the removed id is nowhere in the DOM, not merely that
   * the count went down.
   */
  const ids = (await strip()).visible.map((v) => v.id);
  check(!ids.includes(id), 'and the strip is not still drawing it', JSON.stringify(ids));
}
{
  // By pointer: select, then `−`. With nothing selected it must refuse rather than guess
  // — a destructive button firing on an ambiguous intent is worse than one that does
  // nothing.
  await page.evaluate(() => window.__uni.selectSection(0));
  await page.waitForTimeout(200);
  const before = (await spine()).count;
  await page.locator('.ar-spine-btn').nth(1).click();
  await page.waitForTimeout(600);
  check((await spine()).count === before, 'the − button with nothing selected removes nothing');
  const said = await page.evaluate(() => window.__uni.state().reject);
  check(/select/i.test(String(said)), 'and says which action is missing', String(said));

  const id = (await spine()).list[0].id;
  await page.evaluate((i) => window.__uni.selectSection(i), id);
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
//
// The engine refuses an unknown id as `no_such_section` — on its own log, where the
// browser cannot read it. So a rename aimed at a section that had just been removed came
// back as "section 2: VERSE" and nothing had happened: the optimistic sentence and the
// silent refusal together said the opposite of the truth.
// ---------------------------------------------------------------------------
{
  const gone = 9999;
  for (const line of [`delsection ${gone}`, `namesection ${gone} x`,
                      `seclength ${gone} 4`, `movesection ${gone} 1`]) {
    const said = await type(line);
    check(/no section/i.test(said), `\`${line}\` names the missing section`, said);
  }
}

// ---------------------------------------------------------------------------
// THE SPINE SURVIVES A SAVE AND A LOAD.
//
// The last thing a person has is the file. A spine that is only in the running engine is
// a spine that is gone tomorrow, and this is the one assertion the others cannot imply.
// ---------------------------------------------------------------------------
{
  await type('section 6 finale');
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
  // ...and it is DRAWN after the load, which is a separate failure: the wire's arrange
  // generation gates the copy, and a load that does not move it leaves the old picture.
  const drawn = (await strip()).visible.map((v) => v.name);
  const want = (await spine()).list.slice(0, drawn.length).map((s) => s.name);
  check(JSON.stringify(drawn) === JSON.stringify(want),
        'and the strip is drawing the loaded spine, not the one before it',
        `${JSON.stringify(drawn)} vs ${JSON.stringify(want)}`);
}

check(errors.length === 0, 'and nothing threw while asking', errors.slice(0, 2).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
