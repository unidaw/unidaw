/**
 * PER-NOTE ROW OPS: drawn, collapsed when they must be, and readable when they are.
 *
 * The cell used to resolve ops by PRIORITY — `n.retrigger ? 'R'+n : n.probability ? ... : 'D'`
 * — so a note carrying `ret3 p60 d1/6` drew `R3` and the other two were invisible while the
 * engine played all three. `parse_row_ops` has always taken a whitespace-separated LIST, so the
 * notation was never single-op; only the display was.
 *
 * DRIVEN THROUGH A REAL ENGINE, with the ops authored in a project file — which is the only way
 * to author them, because no command carries row ops. That absence is why this suite matters:
 * the ops it renders cannot be produced from any surface, so a rendering bug here would only
 * ever be seen by someone who hand-edited JSON.
 *
 * AND IT WATCHES FOR PAGE ERRORS. The readout shipped throwing `ticksPerBeat is not defined` on
 * every frame — a missing import, invisible to the unit tests because they never run the draw
 * path, and invisible to journey because it never parks the cursor on the ops field of a note
 * that has any. That is the SECOND time this exact function has been used in index.html without
 * being imported.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { writeFileSync } from 'node:fs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ numBlocks: 8 });
const Q = 960000;

/*
 * THREE OPS ON ONE NOTE, and one note with none.
 *
 * Three because that is the case the old display could not show: whichever two lost the
 * priority contest vanished.
 *
 * A BAR APART, not a beat, and keyed `nanotick` rather than `at`.
 *
 * Both were wrong in earlier versions of this file and both failed the same way: every note
 * landed on tick 0, the note column drew "3 evts", one ops cell served all three, and the suite
 * reported the op display broken when what it had built was a collision. `at` is silently
 * ignored — `project_file.cpp` reads `nanotick` — so a mistyped key does not error, it just
 * puts every note at the start.
 *
 * The repo's own lesson, which I re-learned instead of reading: a hand-written fixture decides
 * which bugs are findable, and the honest way to build one is to EDIT a project through the
 * real commands and save it (tools/edited_roundtrip_check.sh). This one is hand-written because
 * no command can author a row op at all — which is itself the gap this suite documents. `delay` is authored in NANOTICKS here because that is what the
 * project file stores — 160000 of a 960000 beat is a sixth, and the readout has to reduce it
 * back to `d1/6` rather than printing the tick count it happens to be stored as.
 */
writeFileSync(`${stack.dir}/opsfix.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'opsfix', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [], clips: [],
  tracks: [{
    track_id: 0, name: 'T', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false }, device_chain: [], mod_links: [],
    placements: [{
      clip_id: 1, at: 0, length: Q * 16, chords: [], mutes: [],
      notes: [
        { note_id: 1, pitch: 60, velocity: 100, nanotick: 0, duration: Q,
          retrigger: 3, probability: 60, delay: 160000 },
        // A note with ONE op: it fits, so its VALUE must show rather than its glyph.
        { note_id: 2, pitch: 62, velocity: 100, nanotick: Q * 4, duration: Q, probability: 40 },
        // ...and one with none, which must draw nothing at all.
        { note_id: 3, pitch: 64, velocity: 100, nanotick: Q * 8, duration: Q },
      ],
    }],
  }],
}));

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => { if (!errors.includes(e.message)) errors.push(e.message); });

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1200);
await page.evaluate(() => window.__uni.loadProject('opsfix'));
await page.waitForTimeout(2500);
/*
 * ZOOM TO MATCH THE LANE'S GRID. The default zoom aggregates a bar into one row, so three
 * notes a bar apart share a single cell — the note column draws "3 evts" and there is one ops
 * cell for all of them, which two earlier versions of this suite read as the op display being
 * broken. The finest zoom has the opposite problem: 80000 ticks a row puts the third note past
 * the end of the pool.
 *
 * Zoom 1 is 240000 a row with four lines to the beat, which is exactly the fixture's
 * `lines_per_beat`, so the notes land on rows 0, 16 and 32 and all three are drawn.
 */
await page.evaluate(() => window.__uni.run('zoom 1'));
await page.waitForTimeout(700);

/*
 * Every ops cell that has anything in it, by row.
 *
 * Found rather than assumed: the display row of a note is its tick through the ZOOM's grid, not
 * `tick / beat`, so a fixture authored at ticks 0, Q and 2Q does not land on rows 0, 4 and 8 at
 * every zoom. An earlier version of this suite indexed rows directly, read the wrong note, and
 * reported the op display broken when it was the test that was.
 */
const opsCells = () => page.evaluate(() => {
  const out = [];
  for (const c of document.querySelectorAll('.tk-cell[data-track="0"][data-col="2"]')) {
    const t = c.textContent;
    if (t) out.push({ row: Number(c.closest('.tk-row').dataset.row), text: t });
  }
  return out.sort((a, b) => a.row - b.row);
});

// ---------------------------------------------------------------------------
// EVERY OP IS DRAWN. This is the bug the whole design exists to remove.
// ---------------------------------------------------------------------------
const cells = await opsCells();
{
  check(cells.length === 2,
        'exactly the two notes that carry ops draw anything — the third draws nothing',
        JSON.stringify(cells));
  const three = cells.length ? cells[0].text : null;
  check(three !== null, 'the ops cell is there to read', String(three));
  check(three && three.length === 3,
        'a note with THREE ops draws three marks — none outranks another',
        JSON.stringify(three));
  for (const g of ['r', 'p', 'd']) {
    check(three && three.includes(g),
          `the ${g} op is visible; the old priority chain dropped it silently`,
          JSON.stringify(three));
  }
}

// ---------------------------------------------------------------------------
// ...AND THE VALUE, WHENEVER IT FITS. Collapsed-to-a-glyph is right for three ops in a narrow
// cell and wrong for one op with the cell to itself.
// ---------------------------------------------------------------------------
{
  const one = cells.length > 1 ? cells[1].text : null;
  check(one === 'p40', 'one op with room shows its VALUE, not just its mark', JSON.stringify(one));
  // The third note carries nothing, and `cells` only collects non-empty ones — so its absence
  // from the list IS the assertion that a note with no ops draws no ink.
  check(!cells.some((c) => c.text === '' || c.text === '···'),
        'a note with no ops draws nothing — no dots, no ink', JSON.stringify(cells));
}

// ---------------------------------------------------------------------------
// THE COLLAPSED FORM IS READABLE. `rpd` says WHICH ops without saying what they are set to, so
// standing on the cell has to answer the rest — otherwise the compact form is a dead end.
// ---------------------------------------------------------------------------
{
  await page.evaluate(() => window.__uni.run('goto 0 0'));
  await page.waitForTimeout(300);
  const readout = async () => page.evaluate(() => ({
    col: window.__uni.state().cursor.col,
    mode: document.querySelector('.ch-mode') ? document.querySelector('.ch-mode').textContent : null,
  }));
  const atNote = await readout();
  check(atNote.mode === '#=note', 'the note field says what a digit does there', atNote.mode);
  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(250);
  check((await readout()).mode === '#=vel', 'and so does the velocity field');
  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(400);
  const ops = await readout();
  check(ops.col === 2, 'the cursor reaches the ops field', JSON.stringify(ops));
  /*
   * THE CANONICAL STRING, which is exactly what `parse_row_ops` accepts and exactly what an
   * agent writes. Reading it is how the grammar for typing it gets learned.
   *
   * `d1/6` and not `d160000t`: the project stores the delay in nanoticks, and printing that
   * back would be showing the storage rather than the notation.
   */
  check(ops.mode === 'ret3 p60 d1/6',
        'standing on the ops cell prints the canonical string the engine parsed',
        JSON.stringify(ops.mode));
}

/*
 * NOTHING THREW. Load-bearing: the readout shipped throwing a missing-import ReferenceError on
 * every frame, and every other suite passed through it — the unit tests never run the draw
 * path, and journey never parks on the ops field of a note that has ops.
 */
check(errors.length === 0, 'and nothing threw while drawing or reading it',
      errors.slice(0, 3).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
