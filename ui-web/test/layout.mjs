/**
 * THE APP FITS THE WINDOW IT IS GIVEN — every surface, at every size.
 *
 * WHY THIS EXISTS. The chrome's cells came to 1583px in a 1680px window while the design's
 * readouts were being added, `height: 38px` clipped the row that wrapped, and the tempo lost
 * its number. Twenty-three golden shots passed through it, because a golden is taken at the ONE
 * width that happened to work — and "it looks right on the author's monitor" is the oldest bug
 * in interface work.
 *
 * So this is the complement to the goldens rather than more of them: no pixel is compared, and
 * no measurement is asserted against a number from the design. What is asserted is that at
 * every size, on every surface, NOTHING IS CLIPPED, NOTHING OVERFLOWS, AND NOTHING COLLAPSES TO
 * ZERO — three failures a screenshot at a fixed size cannot see.
 *
 * THE SIZES ARE CHOSEN, not swept. The design's own 1680, a 1440 laptop, a 1280 laptop, and
 * one deliberately cruel 1024x640 — because the interesting question is not "does it work at
 * 1679" but "what does it do when there is genuinely not enough room". A shell that clips a
 * surface at 1024 is a shell that will clip one at 1440 the next time a readout is added.
 *
 * WHAT A FAILURE HERE MEANS. Not that a size must be supported: that the app is LYING at it.
 * A clipped row shows half its content and looks like a row that sits a little high; a
 * collapsed surface shows nothing and looks like an empty song. Both are worse than a
 * scrollbar or a hidden panel, and either of those is a legitimate way to pass this.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

const SIZES = [
  { w: 1680, h: 980, why: 'the design\'s own' },
  { w: 1440, h: 900, why: 'a 15-inch laptop' },
  { w: 1280, h: 800, why: 'a 13-inch laptop' },
  { w: 1024, h: 640, why: 'deliberately not enough room' },
];
const VIEWS = ['tracker', 'arrange', 'patcher', 'mixer', 'piano'];

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
await page.waitForTimeout(1200);
// Real material, so a surface with nothing in it cannot pass by being empty.
await page.evaluate(() => window.__uni.loadProject('meter'));
await page.waitForTimeout(2000);

/**
 * Measure the shell: what is clipped, what overflows, what has collapsed.
 *
 * ALL OF IT IN ONE EVALUATE. Reading the DOM across several round trips lets a frame land in
 * between, so two measurements can describe two different layouts — which is how a comparison
 * of "the bar's width" against "its content's width" can disagree with itself. One pass, one
 * layout.
 */
const measure = () => page.evaluate(() => {
  const out = { clipped: [], overflow: [], collapsed: [], scrollers: [] };
  const vw = window.innerWidth, vh = window.innerHeight;

  /*
   * A row is CLIPPED when its content is taller than the box, and the box does not scroll.
   * That is the chrome's failure exactly: `height: 38px` with two lines inside it and
   * `overflow` unset, so the second line is drawn and then cut off with nothing to say so.
   *
   * `overflow` visible OR hidden both count — hidden is what silently truncates. A box that
   * scrolls is fine: the content is reachable and the person can see that there is more.
   */
  /*
   * The shell's own boxes, by the ids it actually uses. Read off the markup rather than guessed:
   * the first version asked for `#rail` and `#status`, which do not exist — and a selector that
   * matches nothing is SKIPPED by the loop below, so every assertion about it passed. A test
   * that measures nothing is the failure this whole suite exists to complain about.
   */
  const rows = ['#chrome', '#crumb', '#main', '#browser', '#centre', '#stage',
                '#rdock', '#chain'];
  for (const sel of rows) {
    const el = document.querySelector(sel);
    if (!el) { out.missing = (out.missing || []).concat(sel); continue; }
    const cs = getComputedStyle(el);
    const scrolls = /auto|scroll/.test(cs.overflowY) || /auto|scroll/.test(cs.overflow);
    const r = el.getBoundingClientRect();
    if (scrolls) { out.scrollers.push(sel); continue; }
    if (el.scrollHeight > el.clientHeight + 1) {
      out.clipped.push([sel, el.scrollHeight, el.clientHeight]);
    }
    const scrollsX = /auto|scroll/.test(cs.overflowX) || /auto|scroll/.test(cs.overflow);
    if (!scrollsX && el.scrollWidth > el.clientWidth + 1) {
      out.overflow.push([sel, el.scrollWidth, el.clientWidth]);
    }
    /*
     * COLLAPSED: on screen with no area. A panel the shell still lays out but has squeezed to
     * nothing shows an empty song, which reads as the project being empty rather than as the
     * window being small. HIDDEN is fine and is the honest answer at a small size — this only
     * catches "displayed and zero".
     */
    if (cs.display !== 'none' && cs.visibility !== 'hidden' && (r.width < 2 || r.height < 2)) {
      out.collapsed.push([sel, Math.round(r.width), Math.round(r.height)]);
    }
  }

  // Nothing may stick out of the window itself. `#app` is the frame; a child past its edge is
  // content nobody can reach, since the shell does not scroll as a page.
  const app = document.querySelector('#app');
  if (app) {
    out.appScrollW = app.scrollWidth; out.appClientW = app.clientWidth;
    out.appScrollH = app.scrollHeight; out.appClientH = app.clientHeight;
  }
  out.docScrollW = document.documentElement.scrollWidth;
  out.vw = vw; out.vh = vh;
  return out;
});

for (const { w, h, why } of SIZES) {
  await page.setViewportSize({ width: w, height: h });
  // Two frames' worth: the shell measures itself on resize and the surfaces rebind from those
  // measurements, so a single frame can catch the old boxes with the new window.
  await page.waitForTimeout(700);
  console.log(`\n[${w}x${h} — ${why}]`);

  for (const view of VIEWS) {
    await page.evaluate((v) => window.__uni.setView(v), view);
    await page.waitForTimeout(450);
    const m = await measure();
    // NAMED, not skipped. A selector that matches nothing would otherwise make every assertion
    // about it pass — see `rows` above, where exactly that happened.
    check(!m.missing || m.missing.length === 0, `${view}: every box this suite measures exists`,
          JSON.stringify(m.missing));
    check(m.clipped.length === 0, `${view}: no row is taller than its box`,
          JSON.stringify(m.clipped));
    check(m.overflow.length === 0, `${view}: no row is wider than its box`,
          JSON.stringify(m.overflow));
    check(m.collapsed.length === 0, `${view}: nothing is displayed with no area`,
          JSON.stringify(m.collapsed));
    /*
     * AND THE PAGE ITSELF DOES NOT SCROLL. `#app` is `position: absolute; inset: 0` — the shell
     * is the window — so a document wider than the viewport means something inside broke out of
     * it, and the person gets a horizontal scrollbar over a DAW.
     */
    check(m.docScrollW <= m.vw + 1, `${view}: the document does not scroll sideways`,
          `${m.docScrollW} > ${m.vw}`);
  }

  /*
   * THE SURFACE STILL HAS SOMETHING IN IT.
   *
   * The strongest check at a cruel size and the one the others cannot make: a shell can pass
   * every geometric assertion above while the tracker draws no cells, because "no rows fit" and
   * "the layout is fine" are compatible. Asked per surface, of the surface's own probe.
   */
  await page.evaluate(() => window.__uni.setView('tracker'));
  await page.waitForTimeout(500);
  const tk = await page.evaluate(() => window.__uni.probe());
  // `rowCount`, not `rows` — looked up. The first version read a field that does not exist, so
  // `tk.rows > 0` was `undefined > 0` and the check failed at EVERY size while reporting a
  // tracker with three tracks in it. A guessed field name reports something confidently.
  check(tk && tk.rowCount > 0 && tk.tracks > 0,
        `at ${w}x${h} the tracker still draws rows and tracks`,
        JSON.stringify(tk && { rowCount: tk.rowCount, tracks: tk.tracks }));

  await page.evaluate(() => window.__uni.setView('arrange'));
  await page.waitForTimeout(500);
  const ar = await page.evaluate(() => window.__uni.arrangeProbe());
  check(ar && ar.lanes > 0, `at ${w}x${h} the arrangement still draws lanes`,
        JSON.stringify(ar && { lanes: ar.lanes, clips: ar.clips }));
  /*
   * ...INCLUDING THE SPINE STRIP, which is the newest row in that surface and the one most
   * likely to be squeezed out. It has a fixed height and sits between the ruler and the lanes,
   * so if the band is short it is the thing that gets clipped.
   */
  check(ar && ar.spine && typeof ar.spine.count === 'number',
        `at ${w}x${h} the markers strip is still there`,
        JSON.stringify(ar && ar.spine && { count: ar.spine.count, pool: ar.spine.pool }));

  await page.evaluate(() => window.__uni.setView('mixer'));
  await page.waitForTimeout(500);
  const mx = await page.evaluate(() => window.__uni.mixerProbe());
  check(mx && mx.strips > 0, `at ${w}x${h} the mixer still draws strips`,
        JSON.stringify(mx && { strips: mx.strips }));
}

await page.setViewportSize({ width: 1680, height: 980 });
await page.waitForTimeout(500);
check(errors.length === 0, 'and nothing threw at any size', errors.slice(0, 3).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
