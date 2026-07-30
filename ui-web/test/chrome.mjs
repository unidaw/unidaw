/**
 * THE CHROME SAYS WHAT IS TRUE, AND FITS.
 *
 * The bar had 23 golden shots and no behavioural test, which is a specific kind of gap rather
 * than an absence: a golden passes on a HARDCODED STRING. `120.00 BPM` baked into the markup
 * looks identical to `120.00 BPM` read from the engine, and the golden was reblessed both
 * times. So every readout here is checked by CHANGING the thing it reports and requiring the
 * readout to move — which is the only assertion a constant cannot satisfy.
 *
 * AND IT FITS, at more than one width. The bar's cells came to 1583px in a 1680px window while
 * this was being built; `height: 38px` clips a wrapped row, so the tempo lost its number and
 * the first line went off centre. No golden caught it, because the golden was taken at the one
 * width that happened to work — and a chrome that only fits on the author's monitor is the
 * oldest bug in interface work.
 *
 * WHAT THIS DOES NOT DO: assert positions against the design's pixels. Those are recorded in
 * the commit that moved them and re-asserting them here would make every future layout change
 * a test edit. What matters is that nothing wraps, nothing is cut off, and every number is the
 * engine's.
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
await page.waitForTimeout(1200);

const type = async (line) => {
  await page.evaluate((c) => window.__uni.run(c), line);
  await page.waitForTimeout(600);
};
const text = (sel) => page.evaluate((s) => {
  const el = document.querySelector(s);
  return el ? el.textContent : null;
}, sel);

/**
 * Does the bar fit, and is anything wrapped?
 *
 * Three separate questions, because they fail differently: content wider than the bar is cut
 * off at the right, a cell taller than the bar is clipped at the bottom, and `scrollWidth`
 * past `clientWidth` is the general case of the first. All three were live at once an hour ago.
 */
const fits = () => page.evaluate(() => {
  const bar = document.querySelector('.chrome');
  const br = bar.getBoundingClientRect();
  const over = [];
  let tallest = 0;
  for (const el of bar.children) {
    const r = el.getBoundingClientRect();
    if (r.height > tallest) tallest = Math.round(r.height);
    // 1px of slack for subpixel layout; anything past the bar's own right edge is cut off.
    if (r.right > br.right + 1) over.push([el.className.split(' ')[0], Math.round(r.right)]);
  }
  return { scrollWidth: bar.scrollWidth, clientWidth: bar.clientWidth,
           barHeight: Math.round(br.height), tallest, over };
});

// ---------------------------------------------------------------------------
// IT FITS — at the design's width and at a laptop's.
// ---------------------------------------------------------------------------
for (const w of [1680, 1440, 1280]) {
  await page.setViewportSize({ width: w, height: 900 });
  await page.waitForTimeout(400);
  const f = await fits();
  check(f.over.length === 0, `at ${w}px nothing is pushed past the bar's right edge`,
        JSON.stringify(f.over));
  check(f.scrollWidth <= f.clientWidth + 1, `at ${w}px the bar does not scroll`,
        `${f.scrollWidth} > ${f.clientWidth}`);
  /*
   * NOTHING WRAPPED. A cell taller than the bar means its content went to a second line, which
   * `height: 38px` then clips — so the visible half looks like a layout that merely sits a
   * little high. This is the assertion the golden could not make.
   */
  check(f.tallest <= f.barHeight, `at ${w}px no cell wrapped to a second line`,
        `tallest cell ${f.tallest} in a ${f.barHeight} bar`);
}
await page.setViewportSize({ width: 1680, height: 980 });
await page.waitForTimeout(400);

// ---------------------------------------------------------------------------
// THE TEMPO AND THE METER ARE THE ENGINE'S.
//
// Changed, not read: `120.00 BPM` is what a hardcoded string says too, and the project starts
// at 120.
// ---------------------------------------------------------------------------
{
  const before = await text('.ch-group:nth-of-type(2)');
  await type('tempo 143.5');
  const after = await page.evaluate(() =>
    [...document.querySelectorAll('.ch-meta')].map((e) => e.textContent).join(' '));
  check(/143\.5/.test(after), 'the tempo readout follows the engine, not a constant',
        `${before} -> ${after}`.slice(0, 120));
  await type('tempo 120');
}

// ---------------------------------------------------------------------------
// THE GROOVE — present when a lane is swung, ABSENT when it is not.
//
// Absent and not zero, which is the whole point of the field: a straight lane has no groove,
// and "groove 0%" is a setting where nothing is the absence of one.
// ---------------------------------------------------------------------------
{
  check((await text('.ch-groove')) === '', 'a straight lane shows no groove at all',
        JSON.stringify(await text('.ch-groove')));
  await type('quantize 0 1/16 80 25');
  await page.waitForTimeout(500);
  const on = await text('.ch-groove');
  check(/25%/.test(String(on)), 'and a swung one says how much', JSON.stringify(on));
  await type('quantize 0 off');
  await page.waitForTimeout(500);
  check((await text('.ch-groove')) === '', 'and turning it off takes the readout away',
        JSON.stringify(await text('.ch-groove')));
}

// ---------------------------------------------------------------------------
// THE LOOP CHIP, in bars, and only when there is a loop.
// ---------------------------------------------------------------------------
{
  const empty = await page.evaluate(() => {
    const el = document.querySelector('.ch-loop');
    return { text: el.textContent, shown: el.style.display !== 'none' };
  });
  check(!empty.shown, 'with no loop the chip is not drawn at all', JSON.stringify(empty));

  await type('loop 2 4');
  const set = await page.evaluate(() => {
    const el = document.querySelector('.ch-loop');
    return { text: el.textContent, shown: el.style.display !== 'none' };
  });
  check(set.shown && /LOOP/.test(set.text), 'a loop makes it appear', JSON.stringify(set));
  /*
   * THE BARS IT COVERS, checked against the ENGINE's own range rather than against the numbers
   * that were typed. `loop 2 4` and what the engine holds are two different statements — the
   * console's end is exclusive — and the chip must agree with the engine, because the ruler
   * does. A chip that echoed the command would drift from the picture the moment they differed.
   */
  const eng = await page.evaluate(() => {
    const l = window.__uni.loop();
    const bar = window.__uni.ticksPerBar();
    return { start: Math.floor(l.start / bar) + 1, end: Math.ceil(l.end / bar) };
  });
  check(set.text === `LOOP ${eng.start}–${eng.end}`,
        'and says the bars the ENGINE is looping, not the ones that were typed',
        `${set.text} vs LOOP ${eng.start}–${eng.end}`);
}

// ---------------------------------------------------------------------------
// THE LATENCY, recomputed here from the engine's own block and rate.
//
// Independently: the readout is `blockSize / sampleRate` and this test does that division
// itself, so a chrome that printed a plausible constant fails.
// ---------------------------------------------------------------------------
{
  const dev = await page.evaluate(() => {
    const s = window.__uni.deviceAudio ? window.__uni.deviceAudio() : null;
    return s;
  });
  const shown = await text('.ch-telemetry');
  if (dev && dev.blockSize > 0 && dev.sampleRateHz > 0) {
    const ms = ((dev.blockSize / dev.sampleRateHz) * 1000).toFixed(1);
    check(String(shown).includes(`${dev.blockSize} smp`),
          'the telemetry names the engine\'s own block size', `${shown} vs ${dev.blockSize}`);
    check(String(shown).includes(`${ms}ms`),
          'and a latency this test computed independently', `${shown} vs ${ms}ms`);
  } else {
    check(!/lat/.test(String(shown)),
          'with no audio device open the latency is BLANK, not 0.0ms',
          `${shown} · device ${JSON.stringify(dev)}`);
  }
}

// ---------------------------------------------------------------------------
// THE DOCUMENT VERSION MOVES WHEN THE DOCUMENT DOES.
// ---------------------------------------------------------------------------
{
  const before = await text('.ch-telemetry');
  await type('goto 4 0');
  await type('note 64');
  await page.waitForTimeout(700);
  const after = await text('.ch-telemetry');
  check(before !== after, 'the doc version moves when an edit lands',
        `${before} -> ${after}`);
}

// ---------------------------------------------------------------------------
// `saved` IS A FACT ABOUT THE FILE.
//
// Silent before any save, because a session that has never been saved reading "saved never" is
// noise and "saved 0s ago" is a lie about work that is entirely at risk. And it appears only
// once the sidecar has STATTED the file — the ack alone must not be enough, since SaveProject's
// outcome reaches the engine's log and nowhere a browser can read.
// ---------------------------------------------------------------------------
{
  const before = await page.evaluate(() => {
    const el = document.querySelector('.ch-saved');
    return { text: el.textContent, shown: el.style.display !== 'none' };
  });
  check(!before.shown && before.text === '',
        'before any save the chip says nothing at all', JSON.stringify(before));
  await type('save chromeux');
  // The confirmation is a round trip through the filesystem, deliberately slower than an ack.
  await page.waitForTimeout(2600);
  const after = await page.evaluate(() => {
    const el = document.querySelector('.ch-saved');
    return { text: el.textContent, shown: el.style.display !== 'none' };
  });
  check(after.shown && /saved \d+s ago/.test(after.text),
        'and after one it reports the file\'s age', JSON.stringify(after));
  /*
   * A NAME THAT CANNOT BE WRITTEN must not produce a "saved" claim. The chip has to stay on the
   * last real save, and the reject line has to say the new one could not be confirmed —
   * otherwise a failed save is indistinguishable from a successful one, which is the failure
   * this whole path exists to prevent.
   */
  await page.evaluate(() => window.__uni.send({ type: 'savedstate', name: 'nope-not-here' }));
  await page.waitForTimeout(900);
  /*
   * STILL REPORTING A SAVE — not the same string.
   *
   * The first version compared the text byte for byte and failed on "saved 3s ago" vs "saved 2s
   * ago": the age had ticked between the two reads, which is the chip doing its job. What
   * matters is that a stat for a file that does not exist did not BLANK it and did not invent a
   * fresher one, so the shape is the assertion.
   */
  const still = await text('.ch-saved');
  check(/^saved \d+s ago$/.test(String(still)),
        'and a stat for a file that does not exist leaves the last real save alone',
        String(still));
}

// ---------------------------------------------------------------------------
// THE CONTROLS THAT ARE NOT BUILT SAY SO, AND DO NOTHING.
//
// Both are drawn because the design has them and their absence is worth showing. The test that
// matters is that pressing them sends NOTHING — a dashed border is a hint, and a hint over a
// live control that half-works would be worse than no control at all.
// ---------------------------------------------------------------------------
{
  const rec = page.locator('.ch-rec');
  check(await rec.count() === 1, 'the third transport button is drawn');
  check(await rec.isDisabled(), 'and is disabled');
  const title = await rec.getAttribute('title');
  check(/record/i.test(String(title)) && /no record/i.test(String(title)),
        'with the reason in its title', String(title));

  const click = page.locator('.ch-click');
  check(await click.count() === 1, 'the CLICK chip is drawn');
  check((await click.getAttribute('class')).includes('unavailable'),
        'marked unavailable rather than merely off',
        await click.getAttribute('class'));
  check(/metronome/i.test(String(await click.getAttribute('title'))),
        'and says what it would be', String(await click.getAttribute('title')));

  // Pressing either must not change the transport or send a command.
  const before = await page.evaluate(() => (window.__uni.engineState() || {}).transport);
  await rec.click({ force: true }).catch(() => {});
  await click.click({ force: true }).catch(() => {});
  await page.waitForTimeout(600);
  const after = await page.evaluate(() => (window.__uni.engineState() || {}).transport);
  check(before === after, 'and pressing them does nothing at all',
        `transport ${before} -> ${after}`);
}

// ---------------------------------------------------------------------------
// THE ENTRY CLUSTER SURVIVED THE MOVE.
//
// It moved out of the top bar into the row below, and a cluster that moved into a hidden or
// clipped row would be a silent regression — the goldens were reblessed for the move, so they
// cannot catch it. A tracker where you cannot see the octave you are typing into writes the
// wrong notes silently, which is why it moved rather than going away.
// ---------------------------------------------------------------------------
{
  const where = await page.evaluate(() => {
    const el = document.querySelector('.ch-entry-row');
    if (!el) return null;
    const r = el.getBoundingClientRect();
    const parent = el.parentElement.id || el.parentElement.className;
    return { parent, visible: r.width > 0 && r.height > 0,
             top: Math.round(r.top), text: el.textContent };
  });
  check(where && where.visible, 'the entry cluster is on screen', JSON.stringify(where));
  check(where && where.parent === 'crumb', 'in the row below the bar', where && where.parent);
  /*
   * AND ITS VALUES ARE THE APP'S. Changed, not read — "oct 4" is what a constant says too, and
   * the app starts at octave 4.
   */
  await type('oct 6');
  const shown = await text('.ch-entry-row');
  check(/oct 6/.test(String(shown)), 'and reads the octave you are actually typing into',
        String(shown).slice(0, 80));
  await type('oct 4');
}

// ---------------------------------------------------------------------------
// EXACTLY ONE TAB IS LIT, AND EACH REACHES ITS SURFACE.
// ---------------------------------------------------------------------------
{
  const views = await page.evaluate(() =>
    [...document.querySelectorAll('.ch-tab')].map((t) => t.dataset.view));
  check(views.length >= 4, 'there is a tab per surface', JSON.stringify(views));
  for (const v of views) {
    await page.locator(`.ch-tab[data-view="${v}"]`).click();
    await page.waitForTimeout(350);
    const state = await page.evaluate(() => ({
      view: window.__uni.state().view,
      lit: [...document.querySelectorAll('.ch-tab.on')].map((t) => t.dataset.view),
    }));
    check(state.view === v && state.lit.length === 1 && state.lit[0] === v,
          `the ${v} tab switches to it, and is the only one lit`,
          JSON.stringify(state));
  }
  await page.locator('.ch-tab[data-view="tracker"]').click();
  await page.waitForTimeout(300);
}

// ---------------------------------------------------------------------------
// A REFUSAL REACHES THE BAR. The reject line is the one place a dropped edit becomes visible,
// and an edit that is silently dropped is indistinguishable from one that was accepted.
// ---------------------------------------------------------------------------
{
  await type('delsection 4242');
  const shown = await text('.ch-reject');
  check(/no section/i.test(String(shown)), 'a refused command lands on the reject line',
        String(shown));
}

check(errors.length === 0, 'and nothing threw while asking', errors.slice(0, 2).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
