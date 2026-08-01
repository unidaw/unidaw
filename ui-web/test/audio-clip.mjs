/**
 * AN AUDIO CLIP WAS READ-ONLY FROM EVERY SURFACE.
 *
 * `gain_db`, `fade_in`, `fade_out` and `source_start_frame` persist on ProjectClip, are
 * published in the audio-source region, and are honoured by the renderer — and until
 * opcode 95 nothing could write any of them. A clip two dB too loud, or a splice that
 * clicked, was fixable only by editing the project JSON by hand.
 *
 * The fades are the ones that matter most: every cut in recorded audio clicks without
 * one, and a click is not something you mix out later.
 *
 * They were also drawn by NOTHING, which is the other half of this. A two-second fade
 * looked exactly like a hard start, so the only way to know what a clip did was to listen
 * to it — and this machine currently produces no audio at all, which makes "listen to it"
 * not a fallback.
 *
 * WHAT THIS ASSERTS:
 *   1. the fixture starts with NO fade and NO gain, so nothing below can pass vacuously
 *   2. a fade set from the console reaches the engine and comes back published
 *   3. it is DRAWN, in pixels, on the clip that was named and not on its neighbour
 *   4. the drawn width follows the ZOOM, because a fade is a length in time
 *   5. it is clamped to the clip, so a fade longer than its region cannot bleed
 *   6. gain shows only when it is not unity
 *   7. the counts refuse a negative rather than clamping it
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
await page.evaluate(() => window.__uni.run('view arrange'));
await page.evaluate(() => window.__uni.loadProject('waveform'));
await page.waitForTimeout(3000);

const audio = async () => (await page.evaluate(() => window.__uni.arrangeProbe())).audio || [];

console.log('\n[the fixture]');
let a = await audio();
check(a.length >= 2, 'the project draws at least two audio clips',
      JSON.stringify(a.map((c) => c.clipId)));
/*
 * NOTHING IS SET TO BEGIN WITH. Without this the drawing checks below could pass on a
 * fixture that already had fades — the hand-written fixture decides what is findable, and
 * this one is authored at zero for exactly this reason.
 */
check(a.every((c) => c.fadeInTicks === 0 && c.fadeOutTicks === 0 && c.gainMb === 0),
      'and every one starts with no fade and no gain — so nothing below passes vacuously',
      JSON.stringify(a.map((c) => [c.fadeInTicks, c.fadeOutTicks, c.gainMb])));
check(a.every((c) => c.drawnIn === 0 && c.drawnOut === 0),
      'and nothing is drawn for them yet',
      JSON.stringify(a.map((c) => [c.drawnIn, c.drawnOut])));

const target = a[0], other = a[1];
const Q = 960000;                                   // one quarter note in nanoticks

console.log('\n[a fade arrives and is drawn]');
const said = await page.evaluate(({ t, c }) =>
  window.__uni.run(`audio-clip ${t} ${c} fade-in ${960000 * 2}`),
  { t: target.track, c: target.clipId });
await page.waitForTimeout(1200);
a = await audio();
const now = a.find((c) => c.clipId === target.clipId);
check(now && now.fadeInTicks === Q * 2, 'the engine publishes the fade back',
      `${now && now.fadeInTicks} — said "${said}"`);
check(now && now.drawnIn > 0, 'and the clip DRAWS it, in pixels',
      now ? `${now.drawnIn}px on a ${now.w}px clip` : 'no clip');

/*
 * ON THE CLIP THAT WAS NAMED. A field written to the first match, or to whichever clip the
 * renderer happened to be holding, passes every check above — this is the one that sees it.
 */
const neighbour = a.find((c) => c.clipId === other.clipId);
check(neighbour && neighbour.fadeInTicks === 0 && neighbour.drawnIn === 0,
      'and the other clip is untouched, in the model and on screen',
      neighbour ? `${neighbour.fadeInTicks} ticks, ${neighbour.drawnIn}px` : 'no neighbour');

/*
 * A FADE IS A LENGTH IN TIME, so its drawn width has to follow the zoom. A fade stored in
 * PIXELS would look right at the zoom it was set at and wrong everywhere else — and that is
 * the shape of bug that survives a screenshot golden.
 */
console.log('\n[it is a length in time, not in pixels]');
const widthNow = now.drawnIn;
await page.keyboard.press('=');                     // zoom in one step
await page.waitForTimeout(600);
const zoomed = (await audio()).find((c) => c.clipId === target.clipId);
check(zoomed && zoomed.drawnIn > widthNow,
      'zooming in makes the same fade wider on screen',
      `${widthNow}px -> ${zoomed && zoomed.drawnIn}px`);
check(zoomed && zoomed.fadeInTicks === Q * 2,
      'and the fade itself did not change', String(zoomed && zoomed.fadeInTicks));
await page.keyboard.press('-');
await page.waitForTimeout(600);

/*
 * CLAMPED TO THE CLIP. The model permits a fade longer than the region it sits on; the
 * DRAWING must not spill past the clip's own end, or the picture says the neighbour is
 * fading.
 */
console.log('\n[clamped to the clip]');
await page.evaluate(({ t, c }) =>
  window.__uni.run(`audio-clip ${t} ${c} fade-in ${960000 * 400}`),
  { t: target.track, c: target.clipId });
await page.waitForTimeout(1200);
const huge = (await audio()).find((c) => c.clipId === target.clipId);
check(huge && huge.fadeInTicks > huge.w * 100, 'a fade longer than the clip is accepted',
      huge ? `${huge.fadeInTicks} ticks over a ${huge.w}px clip` : 'no clip');
check(huge && huge.drawnIn <= huge.w,
      'and is drawn no wider than the clip it is on',
      huge ? `${huge.drawnIn}px inside ${huge.w}px` : 'no clip');

// Back to something sane, and the fade OUT as well — the two are separate fields and a
// setter that wrote one for the other passes every check that only looks at the in.
await page.evaluate(({ t, c }) =>
  window.__uni.run(`audio-clip ${t} ${c} fade-in ${960000}`), { t: target.track, c: target.clipId });
await page.waitForTimeout(900);
await page.evaluate(({ t, c }) =>
  window.__uni.run(`audio-clip ${t} ${c} fade-out ${960000 * 3}`), { t: target.track, c: target.clipId });
await page.waitForTimeout(1200);
const both = (await audio()).find((c) => c.clipId === target.clipId);
check(both && both.fadeInTicks === Q && both.fadeOutTicks === Q * 3,
      'the two fades are separate fields',
      both ? `in ${both.fadeInTicks}, out ${both.fadeOutTicks}` : 'no clip');
check(both && both.drawnOut > both.drawnIn,
      'and the longer one is drawn wider',
      both ? `in ${both.drawnIn}px, out ${both.drawnOut}px` : 'no clip');

console.log('\n[gain]');
check(both && both.gainText === '', 'a clip at unity shows no gain badge',
      JSON.stringify(both && both.gainText));
await page.evaluate(({ t, c }) => window.__uni.run(`audio-clip ${t} ${c} gain -3.5`),
                    { t: target.track, c: target.clipId });
await page.waitForTimeout(1200);
const quiet = (await audio()).find((c) => c.clipId === target.clipId);
check(quiet && quiet.gainMb === -350, 'gain is typed in dB and lands in millibels',
      String(quiet && quiet.gainMb));
check(quiet && quiet.gainText === '-3.5', 'and the badge appears, in dB',
      JSON.stringify(quiet && quiet.gainText));

/*
 * REFUSED, NOT CLAMPED. A fade length and an in-point are counts: a negative one is not a
 * quiet value, it is a caller who meant something else. Refused in the PAGE as well as the
 * engine, because the engine's refusal is a log line a browser cannot read.
 */
console.log('\n[refusals]');
const beforeBad = (await audio()).find((c) => c.clipId === target.clipId);
for (const [field, value] of [['fade-in', -1], ['fade-out', -960000], ['start', -5]]) {
  const r = await page.evaluate(({ t, c, f, v }) => {
    const ok = window.__uni.audioClip(t, c, f, v);
    return { ok, reject: window.__uni.state().reject };
  }, { t: target.track, c: target.clipId, f: field, v: value });
  check(r.ok === false && !!r.reject, `refused: ${field} ${value}`,
        `returned ${r.ok}, reject ${JSON.stringify(r.reject)}`);
}
const bad = await page.evaluate(({ t, c }) => {
  const ok = window.__uni.audioClip(t, c, 'nonsense', 1);
  return { ok, reject: window.__uni.state().reject };
}, { t: target.track, c: target.clipId });
check(bad.ok === false && /field/.test(String(bad.reject)),
      'and an unknown field is named, not silently ignored', JSON.stringify(bad.reject));

await page.waitForTimeout(800);
const afterBad = (await audio()).find((c) => c.clipId === target.clipId);
check(afterBad && afterBad.fadeInTicks === beforeBad.fadeInTicks
      && afterBad.fadeOutTicks === beforeBad.fadeOutTicks,
      'and not one of them changed the clip',
      `${beforeBad.fadeInTicks}/${beforeBad.fadeOutTicks} -> ` +
      `${afterBad && afterBad.fadeInTicks}/${afterBad && afterBad.fadeOutTicks}`);

/*
 * THE POINTER GESTURE.
 *
 * A fade you can only type is a fade nobody adjusts by ear, and the corner handle is where
 * every DAW puts it. The zone is the TOP of the corner, so trim keeps the rest of the edge:
 * trimming is the more common and the more destructive of the two, and a gesture that
 * silently became the other one would be worse than not having it.
 */
console.log('\n[dragging the handle]');
await page.evaluate(({ t, c }) => window.__uni.run(`audio-clip ${t} ${c} fade-in 0`),
                    { t: target.track, c: target.clipId });
await page.waitForTimeout(1000);
const box = await page.evaluate((clipId) => {
  const els = [...document.querySelectorAll('.ar-clip')];
  const el = els.find((x) => x._clipId === clipId);
  if (!el) return null;
  const r = el.getBoundingClientRect();
  return { x: r.x, y: r.y, w: r.width, h: r.height };
}, target.clipId);
check(box !== null && box.w > 40, 'the clip is on screen and wide enough to grab',
      JSON.stringify(box));

if (box && box.w > 40) {
  const zeroed = (await audio()).find((c) => c.clipId === target.clipId);
  check(zeroed.fadeInTicks === 0, 'and its fade is back to zero before the drag',
        String(zeroed.fadeInTicks));

  // The TOP-LEFT corner, 4px in and 3px down — inside the fade band, and inside the trim
  // zone too, which is the point: the band is what tells them apart.
  await page.mouse.move(box.x + 4, box.y + 3);
  await page.mouse.down();
  await page.mouse.move(box.x + 40, box.y + 3, { steps: 6 });
  const preview = await page.evaluate((clipId) => {
    const el = [...document.querySelectorAll('.ar-clip')].find((x) => x._clipId === clipId);
    return el ? { drawn: el._fiW, fading: el.classList.contains('fading') } : null;
  }, target.clipId);
  /*
   * PREVIEWED BEFORE THE BUTTON COMES UP. A fade redrawn only on the engine's answer lags
   * the pointer by the round trip, which is the one thing a direct-manipulation gesture
   * must not do — so this is asserted MID-DRAG, where that failure is visible.
   */
  check(preview && preview.drawn > 20, 'the ramp follows the pointer before the button is up',
        JSON.stringify(preview));
  check(preview && preview.fading === true, 'and the clip says it is being faded');

  await page.mouse.up();
  await page.waitForTimeout(1200);
  const after = (await audio()).find((c) => c.clipId === target.clipId);
  check(after && after.fadeInTicks > 0, 'releasing commits it to the engine',
        `${after && after.fadeInTicks} ticks`);
  check(after && after.drawnIn > 20, 'and the engine\'s value is what stays drawn',
        `${after && after.drawnIn}px`);

  /*
   * AND THE NEIGHBOUR IS UNTOUCHED. A gesture that read the wrong element is the failure
   * this codebase produces most, and it looks correct from the dragged clip alone.
   */
  const n2 = (await audio()).find((c) => c.clipId === other.clipId);
  check(n2 && n2.fadeInTicks === 0, 'and the other clip still has no fade',
        String(n2 && n2.fadeInTicks));

  /*
   * ESCAPE ABANDONS IT. A preview with no way back is a gesture you cannot try.
   */
  const beforeEsc = (await audio()).find((c) => c.clipId === target.clipId).fadeInTicks;
  await page.mouse.move(box.x + 4, box.y + 3);
  await page.mouse.down();
  await page.mouse.move(box.x + 120, box.y + 3, { steps: 6 });
  await page.keyboard.press('Escape');
  await page.mouse.up();
  await page.waitForTimeout(1000);
  const esc = (await audio()).find((c) => c.clipId === target.clipId);
  check(esc && esc.fadeInTicks === beforeEsc, 'Escape mid-drag leaves the fade alone',
        `${beforeEsc} -> ${esc && esc.fadeInTicks}`);

  /*
   * AND THE EDGE STILL TRIMS. The fade band took the top of the corner; the rest of the
   * edge has to still be the trim handle, or this gesture was bought by breaking one that
   * was already there.
   */
  const clipBefore = await page.evaluate(() => window.__uni.arrangeProbe().clips);
  await page.mouse.move(box.x + 2, box.y + box.h - 4);
  await page.mouse.down();
  await page.mouse.move(box.x + 60, box.y + box.h - 4, { steps: 6 });
  const dragging = await page.evaluate(() => {
    const g = document.querySelector('.ar-ghost');
    return { ghost: !!g, w: g ? g.getBoundingClientRect().width : 0 };
  });
  await page.keyboard.press('Escape');
  await page.mouse.up();
  await page.waitForTimeout(600);
  check(dragging.ghost === true,
        'the lower part of the same edge still starts a TRIM, not a fade',
        JSON.stringify(dragging));
  check((await page.evaluate(() => window.__uni.arrangeProbe().clips)) === clipBefore,
        'and abandoning it changed nothing');
}

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
