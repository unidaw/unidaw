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
  /*
   * ONE GLYPH, EVEN WHEN THERE IS ROOM FOR THE VALUE.
   *
   * This asserted `p40` for a while — the cell drew the fullest form that fit. Two things ended
   * that: it ALLOCATED a canonical string per cell per frame while scrolling (alloc.mjs measured
   * +200 B/draw over a 1200 budget, and no per-cell cache helps when every cell holds a
   * different note), and it made a column a MIXTURE — one op showed a value, three showed
   * glyphs, so scanning meant reading two notations at once.
   *
   * The value is one keypress away in the readout, which is a better answer than squeezing it
   * in where it happens to fit.
   */
  check(one === 'p', 'one op draws one glyph, like every other count of ops',
        JSON.stringify(one));
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

// ---------------------------------------------------------------------------
// A NOTE LOADED FROM A PROJECT can have its ops set.
//
// THIS CHECK WAS THE INVERSE UNTIL BACKEND FIXED IT. `applySetRowOps` searched `ownedClips`
// only, so a loaded project's notes — which live in SOURCE clips — came back `no_such_note`.
// Editing a project you opened is the ordinary case and that was backwards from it.
//
// It was asserted AS A GAP rather than left untested, precisely so it would fail the day it
// started working. It failed on the next merge. That is the whole argument for writing a
// limitation down as a check instead of a comment: a comment goes stale silently and a check
// tells you the moment it is wrong.
// ---------------------------------------------------------------------------
{
  await page.evaluate(() => window.__uni.run('goto 0 0'));
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(400);
  check(await page.evaluate(() => window.__uni.opsTextAtCursor()) === 'ret3 p60 d1/6',
        'the loaded note reads its authored ops');
  await page.evaluate(() => window.__uni.run('ops ret9'));
  await page.waitForTimeout(2000);
  const after = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(after === 'ret9',
        'and its ops can be SET — a note loaded from a project is reachable now', after);
  // ...and the full mask cleared the other two, on a loaded note as on a fresh one.
  check(!/p\d|d\d/.test(after), 'with the ops it did not name cleared, as everywhere else',
        after);
}

// ---------------------------------------------------------------------------
// AND NOW WRITING THEM, on a note this session created.
//
// SetRowOps (81) exists as of tonight; before it, row ops could be DRAWN and authored by
// nothing — not the tracker, not the console, not daw-cli — a display onto data only a text
// editor could produce.
//
// A NOTE TYPED HERE, not the fixture's. `applySetRowOps` searches `ownedClips` only, and a
// project's notes live in SOURCE clips until something forks them, so a loaded note is
// rejected with `no_such_note`. That is an engine gap, it is reported, and it is asserted as a
// gap further down rather than papered over by only ever testing the path that works.
// ---------------------------------------------------------------------------
{
  /*
   * A FRESH SONG for the write half.
   *
   * Typing into the loaded project's placement is not enough: that placement is a SOURCE clip
   * and a note typed into it stays there, so the write is rejected exactly as a loaded note is.
   * An empty song has no placements, so the first note creates an OWNED clip — which is the
   * only kind `applySetRowOps` can currently reach.
   *
   * That the write half needs a brand-new song to work at all IS the gap, stated as setup.
   */
  await page.evaluate(() => window.__uni.run('new opswrite'));
  await page.waitForTimeout(2500);
  await page.evaluate(() => window.__uni.run('zoom 1'));
  await page.evaluate(() => window.__uni.run('goto 32 0'));
  await page.waitForTimeout(300);
  await page.evaluate(() => window.__uni.run('note 67'));
  await page.waitForTimeout(1800);
  await page.evaluate(() => window.__uni.run('goto 32 0'));
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(400);

  const id = await page.evaluate(() => window.__uni.noteIdAtCursor());
  check(id > 0, 'the note this session wrote has an id to address', String(id));
  check(await page.evaluate(() => window.__uni.opsTextAtCursor()) === '',
        'and starts with no ops');

  await page.evaluate(() => window.__uni.run('ops ret5 p25'));
  await page.waitForTimeout(2000);
  check(await page.evaluate(() => window.__uni.opsTextAtCursor()) === 'ret5 p25',
        'the ops the console wrote come back from the ENGINE, not from what was typed');

  /*
   * AN OP LEFT OUT IS CLEARED. The mask is always full: a bit CLEAR leaves an op alone and a bit
   * SET with zero clears it, so a partial mask could not express a deletion at all. This is what
   * makes the cell's contract the obvious one — what it shows is what the note has.
   */
  await page.evaluate(() => window.__uni.run('ops p40'));
  await page.waitForTimeout(2000);
  const only = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(only === 'p40', 'an op left out of the string is CLEARED, not left behind', only);

  await page.evaluate(() => window.__uni.run('ops'));
  await page.waitForTimeout(2000);
  check(await page.evaluate(() => window.__uni.opsTextAtCursor()) === '',
        'an empty string clears every op on the note');

  // A MALFORMED TOKEN IS REFUSED BY NAME, in the engine parser's own words, and changes nothing.
  await page.evaluate(() => window.__uni.run('ops ret3'));
  await page.waitForTimeout(1800);
  const bad = await page.evaluate(() => window.__uni.run('ops p0'));
  await page.waitForTimeout(600);
  check(/probability must be 1/.test((bad || []).join(' ')),
        'a bad token is refused in the same words the engine parser uses',
        (bad || []).join(' ').slice(-120));
  check(await page.evaluate(() => window.__uni.opsTextAtCursor()) === 'ret3',
        'and the refused edit changed nothing');
}

// ---------------------------------------------------------------------------
// TYPED INTO THE CELL, with real keystrokes. The console verb is one surface; the standing rule
// here is that both reach everything, and "edit the cell to edit an individual op" is the
// gesture this whole design was drawn around.
//
// The buffer opens SEEDED and the seed is EDITABLE, not selected — the point is to change one
// op out of several, and a selected seed would make the first keystroke wipe the rest.
// ---------------------------------------------------------------------------
{
  // Back to the note this session wrote, which currently reads `ret3`.
  await page.evaluate(() => window.__uni.run('goto 32 0'));
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(400);
  check(await page.evaluate(() => window.__uni.opsTextAtCursor()) === 'ret3',
        'standing on a note whose ops are known');

  await page.keyboard.press('@');
  await page.waitForTimeout(300);
  // `data-kind="editing"` is what the view-model marks the cell being typed into — see the
  // entryOverlay branch in viewmodel.js. It holds the buffer's raw text, with no caret glyph
  // unless the buffer is empty, which is why looking for one found nothing.
  const seeded = await page.evaluate(() => {
    const c = document.querySelector('.tk-cell[data-kind="editing"]');
    return c ? c.textContent : null;
  });
  check(seeded && seeded.startsWith('ret3'),
        '@ opens the buffer SEEDED with what the note has', JSON.stringify(seeded));

  /*
   * AND THE GRAMMAR IS IN FRONT OF YOU while you type it.
   *
   * `OP_SCHEMA`'s own comment says one definition should feed "entry, autocomplete, docs and the
   * linter"; this side used none of it, so the ops cell was a text field you had to already know
   * the language of. The readout narrows from the whole list to ONE op's meaning as soon as the
   * token identifies one — the list while you are choosing, the meaning once you have.
   */
  const hint = await page.evaluate(() =>
    document.querySelector('.ch-mode') ? document.querySelector('.ch-mode').textContent : '');
  check(/retrigger N even strikes/.test(hint),
        'and the readout explains the op being typed', JSON.stringify(hint));

  /*
   * TYPE ONE MORE OP ONTO THE END, which is the whole gesture: change one op without retyping
   * the rest. If the seed were selected the space would wipe `ret3` and this would commit `p55`
   * alone. `begin` in entry.js guarantees it is not, for its own reason — this check is what
   * proves that guarantee reaches here rather than assuming it.
   */
  for (const ch of ' p55') await page.keyboard.press(ch === ' ' ? 'Space' : ch);
  await page.waitForTimeout(200);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(2000);
  const after = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(after === 'ret3 p55',
        'the typed edit lands, with the seeded op kept and the new one added', after);

  // ESCAPE ABANDONS. A buffer with no way out is a trap, and the ops cell is the one place a
  // person is most likely to open one by accident — Enter is not a rare key.
  await page.keyboard.press('@');
  await page.waitForTimeout(200);
  for (const ch of ' d1') await page.keyboard.press(ch === ' ' ? 'Space' : ch);
  await page.keyboard.press('Escape');
  await page.waitForTimeout(800);
  check(await page.evaluate(() => window.__uni.opsTextAtCursor()) === 'ret3 p55',
        'Escape abandons the edit and the note is untouched');
}

/*
 * ONE OP AT A TIME — the promise the collapsed cell makes.
 *
 * A cell holding forty ops is only editable if ONE of them can be changed without restating the
 * other thirty-nine, and on this wire that is a real distinction rather than a convenience: the
 * mask says which fields the command is speaking about, a bit left clear means "leave it alone",
 * and a bit set with a zero value means "clear it". `ops` sets every bit — right for typing a row
 * out, wrong for changing one thing in it, because it carries this client's copy of all five
 * fields and would overwrite whatever else had changed.
 *
 * The check is therefore not "the op changed" but "the OTHERS did not", which is the half that
 * fails when the mask is wrong. A full-mask implementation passes every assertion about the op
 * being set and drops the rest of the row on the floor.
 */
{
  // The cursor is still on the note the block above edited, which is the point: these verbs act
  // on the note under it, and moving away first would only test the refusal.
  // A full row first, through the verb that means "replace the row".
  await page.evaluate(() => window.__uni.run('ops ret3 p55 s5 o80'));
  await page.waitForTimeout(1500);
  const before = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(before === 'ret3 p55 s5 o80', 'a full row of ops is set to start from', before);

  await page.evaluate(() => window.__uni.run('op p20'));
  await page.waitForTimeout(1500);
  const after = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(after === 'ret3 p20 s5 o80',
        'one op changes and the other three are left exactly as they were', after);

  // A BARE PREFIX CLEARS THAT ONE. Distinct from `ops` with the token missing, which clears the
  // row: the mask bit is set (so the field is written) with a zero value (so it is cleared).
  await page.evaluate(() => window.__uni.run('op s'));
  await page.waitForTimeout(1500);
  const cleared = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(cleared === 'ret3 p20 o80',
        'a bare prefix clears just that op and leaves the rest', cleared);

  // AND A NON-OP IS REFUSED, rather than sending a command with an empty mask — which the engine
  // would accept and do nothing about, reading exactly like an edit that did not land.
  const refused = await page.evaluate(() => window.__uni.run('op zz9'));
  const still = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(/not an op/.test(String(refused)) && still === 'ret3 p20 o80',
        'a token that names no op is refused, and nothing changes', `${refused} / ${still}`);
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
