/**
 * VELOCITY WAS READ-ONLY IN THE PIANO ROLL.
 *
 * The roll has drawn velocity as OPACITY since it existed and offered no way to change it,
 * so the only way to make one note softer than its neighbour was the tracker's volume
 * column — a different surface, in hex, on a grid you may not have the note lined up with.
 * For a roll, that is the edit people reach for most after moving a note.
 *
 * IT IS A MODE, NOT A MODIFIER, and this is the place that argument is strongest: velocity
 * and pitch are both VERTICAL. A modifier would put two different musical meanings on one
 * gesture with nothing on screen telling them apart, and the wrong one transposes.
 *
 * WHAT THIS ASSERTS:
 *   1. the mode refuses to turn on when there is nothing to drag
 *   2. it is visible — the roll is tinted, not silently armed
 *   3. while OFF, a vertical drag still moves the note's PITCH (the gesture is not stolen)
 *   4. while ON, the same drag changes velocity and leaves pitch and tick alone
 *   5. the drawn opacity follows the drag BEFORE the button comes up
 *   6. and a number is shown, because an opacity cannot be aimed
 *   7. it clamps to 1, not 0 — a velocity of zero is a note-off, so the floor would delete
 *   8. Escape abandons it
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

console.log('\n[the mode refuses when it would do nothing]');
await page.evaluate(() => window.__uni.run('new veltest'));
await page.waitForTimeout(1500);
await page.evaluate(() => window.__uni.run('view piano'));
await page.waitForTimeout(600);
const empty = await page.evaluate(() => ({
  said: window.__uni.run('vel-edit on'),
  on: window.__uni.state().velocityEdit,
  reject: window.__uni.state().reject,
}));
check(empty.on !== true, 'an empty song refuses velocity edit mode', JSON.stringify(empty));
check(/no notes/.test(String(empty.reject || empty.said)),
      'and says why — a mode you can enter and do nothing in looks broken',
      JSON.stringify(empty.reject || empty.said));

// Now give it something to edit, from the tracker, through the console.
await page.evaluate(() => window.__uni.run('view tracker'));
await page.evaluate(() => window.__uni.run('goto 0 0'));
await page.evaluate(() => window.__uni.run('note 60 960000 90'));
await page.waitForTimeout(1200);
await page.evaluate(() => window.__uni.run('view piano'));
await page.waitForTimeout(900);

const probe = () => page.evaluate(() => window.__uni.pianoProbe());
let p = await probe();
console.log('\n[a note to work on]');
check(p && p.notes >= 1, 'the roll draws the note', p ? `${p.notes} notes` : 'no probe');
const noteOf = (q) => (q.velocities && q.velocities[0]) || null;
check(noteOf(p) && noteOf(p).vel === 90, 'at the velocity it was written with',
      JSON.stringify(noteOf(p)));

/*
 * WHILE THE MODE IS OFF, A VERTICAL DRAG STILL MOVES PITCH.
 *
 * The control that proves the mode is doing something rather than the drag having always
 * meant velocity — and it is also the regression check for the gesture this one shares an
 * axis with. Without it, a mode that did nothing and a mode that stole the drag look the
 * same from the velocity side.
 */
console.log('\n[with the mode off]');
const noteBox = () => page.evaluate(() => {
  const el = document.querySelector('.pr-note');
  if (!el) return null;
  const r = el.getBoundingClientRect();
  return { x: r.x + r.width / 2, y: r.y + r.height / 2, h: r.height };
});
const engNote = () => page.evaluate(() => {
  const e = window.__uni.engineNotes ? window.__uni.engineNotes() : null;
  if (e && e.length) return e[0];
  const st = window.__uni.engine();
  return st ? { count: st.notes } : null;
});
let box = await noteBox();
check(box !== null, 'the note has a box to grab', JSON.stringify(box));

const pitchOf = async () => (await probe()).pitchRange;
const beforePitch = await pitchOf();
if (box) {
  await page.mouse.move(box.x, box.y);
  await page.mouse.down();
  await page.mouse.move(box.x, box.y - box.h * 2, { steps: 6 });
  await page.mouse.up();
  await page.waitForTimeout(1200);
  const afterPitch = await pitchOf();
  check(JSON.stringify(afterPitch) !== JSON.stringify(beforePitch),
        'a vertical drag moves the note in PITCH',
        `${JSON.stringify(beforePitch)} -> ${JSON.stringify(afterPitch)}`);
  const q = await probe();
  check(noteOf(q) && noteOf(q).vel === 90, 'and its velocity is untouched',
        JSON.stringify(noteOf(q)));
}

console.log('\n[turning it on]');
const on = await page.evaluate(() => ({
  said: window.__uni.run('vel-edit on'),
  on: window.__uni.state().velocityEdit,
}));
check(on.on === true, 'the mode turns on with notes present', JSON.stringify(on));
/*
 * DRAW BEFORE ASKING WHAT IS DRAWN.
 *
 * `setVelocityEdit` sets state and calls `schedule()`, which coalesces to one draw per
 * animation frame — so reading the class and the view model immediately after it returns
 * reads the frame BEFORE the mode existed. It failed intermittently exactly that way, and
 * the two checks that failed were the two that ask about rendered state while the one that
 * asks about `state()` passed, which is the tell.
 *
 * `redraw()` is the synchronous draw the app exposes for this; a sleep would make the same
 * race quieter rather than absent.
 */
await page.evaluate(() => window.__uni.redraw());
const tinted = await page.evaluate(() =>
  document.querySelector('.pr').classList.contains('vel-edit'));
check(tinted === true, 'and the roll SAYS it is on — a mode you cannot see is a modifier');
check((await probe()).velocityEdit === true, 'the view model carries it');

console.log('\n[dragging velocity]');
box = await noteBox();
const pitchBefore = await pitchOf();
const velBefore = noteOf(await probe()).vel;
if (box) {
  await page.mouse.move(box.x, box.y);
  await page.mouse.down();
  await page.mouse.move(box.x, box.y - 40, { steps: 8 });     // up is louder, 2px per unit
  const mid = await probe();
  /*
   * ASSERTED MID-DRAG. A preview that only appears after the engine answers lags the
   * pointer by the round trip, and the failure is invisible from any state read after the
   * button comes up — which is how the arrangement's fade preview shipped broken.
   */
  check(mid.dragVel > velBefore, 'the roll proposes a louder value while the button is down',
        `${velBefore} -> ${mid.dragVel}`);
  check(noteOf(mid) && noteOf(mid).vel === mid.dragVel,
        'and the DRAWN note wears it, which is the only thing a person sees',
        JSON.stringify(noteOf(mid)));
  check(/^vel \d+$/.test(String(mid.readout)),
        'with the number beside it — an opacity cannot be aimed', JSON.stringify(mid.readout));

  await page.mouse.up();
  await page.waitForTimeout(1200);
  const after = await probe();
  check(noteOf(after) && noteOf(after).vel === mid.dragVel,
        'releasing commits it to the engine',
        `${JSON.stringify(noteOf(after))} vs proposed ${mid.dragVel}`);
  check(JSON.stringify(await pitchOf()) === JSON.stringify(pitchBefore),
        'and the note did NOT move in pitch',
        `${JSON.stringify(pitchBefore)} -> ${JSON.stringify(await pitchOf())}`);
  check(after.dragVel === -1 && after.readout !== null,
        'and the readout is put away', JSON.stringify({ dragVel: after.dragVel }));
}

/*
 * THE FLOOR IS 1, NOT 0.
 *
 * A MIDI velocity of zero is a NOTE-OFF, so a note stored at zero draws, saves and reloads
 * and can never sound. The store keeps it — verified, not assumed, by the negative control
 * that let zero through — which makes it worse than a deletion rather than better: there is
 * nothing missing to notice, just a note that is silent for a reason invisible in the roll,
 * where velocity is drawn as opacity and zero opacity is the same picture as very quiet.
 */
console.log('\n[the floor]');
box = await noteBox();
if (box) {
  const countBefore = (await page.evaluate(() => window.__uni.engine())).notes;
  await page.mouse.move(box.x, box.y);
  await page.mouse.down();
  await page.mouse.move(box.x, box.y + 600, { steps: 10 });
  const floor = await probe();
  check(floor.dragVel === 1, 'a drag to the floor proposes 1, not 0', String(floor.dragVel));
  await page.mouse.up();
  await page.waitForTimeout(1200);
  const after = await probe();
  check(noteOf(after) && noteOf(after).vel === 1, 'and lands on 1',
        JSON.stringify(noteOf(after)));
  // The note survives either way — the store does not treat 0 specially, which is exactly
  // why the clamp has to. Kept as a control on the drag itself, not as the discriminator.
  const countAfter = (await page.evaluate(() => window.__uni.engine())).notes;
  check(countAfter === countBefore, 'and the drag did not destroy the note it was softening',
        `${countBefore} -> ${countAfter}`);
}

console.log('\n[escape]');
box = await noteBox();
if (box) {
  const velNow = noteOf(await probe()).vel;
  await page.mouse.move(box.x, box.y);
  await page.mouse.down();
  await page.mouse.move(box.x, box.y - 100, { steps: 6 });
  await page.keyboard.press('Escape');
  await page.mouse.up();
  await page.waitForTimeout(1000);
  check(noteOf(await probe()).vel === velNow, 'Escape mid-drag leaves the velocity alone',
        `${velNow} -> ${noteOf(await probe()).vel}`);
}

console.log('\n[turning it off]');
await page.evaluate(() => window.__uni.run('vel-edit off'));
await page.waitForTimeout(400);
check((await page.evaluate(() => window.__uni.state().velocityEdit)) === false,
      'the mode turns off again');
check((await page.evaluate(() =>
        document.querySelector('.pr').classList.contains('vel-edit'))) === false,
      'and the roll stops saying it is on');

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
