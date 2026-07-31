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
  }, {
    /*
     * A SECOND TRACK WHOSE NOTES CARRY NO OPS AT ALL.
     *
     * This is what makes the per-track ops column testable. `uiTrackOpsWidth[t]` is 0 for a
     * track like this and non-zero for track 0, and a fixture with only ONE track cannot tell
     * "the column is hidden correctly" from "the column is always hidden" — the same blind spot
     * that let a one-device project publish patcher owner 0 and look right.
     */
    track_id: 1, name: 'NoOps', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false }, device_chain: [], mod_links: [],
    placements: [{
      clip_id: 2, at: 0, length: Q * 16, chords: [], mutes: [],
      notes: [
        { note_id: 11, pitch: 48, velocity: 100, nanotick: 0, duration: Q },
        { note_id: 12, pitch: 50, velocity: 100, nanotick: Q * 4, duration: Q },
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
 * STEPPING THROUGH THE OPS INSIDE ONE CELL.
 *
 * The collapsed cell is one glyph per op, so it is a row of things to point at — and pointing is
 * the only way to reach a dense row, because the edit buffer holds 48 characters and the user's
 * own case (forty-odd ops on one row) spells to several hundred. Left/Right walks them before
 * leaving the cell, the selected one draws in FULL, and `@` then opens that op alone.
 *
 * The arrival state is deliberately "none selected": stepping into the ops field must still mean
 * the field, and `@` there must still open the whole row, or every existing habit changes.
 */
{
  // The cursor is on the note the block above left at `ret3 p20 o80` — three ops, three glyphs.
  const cellText = () => page.evaluate(() => {
    const c = document.querySelector('.tk-cell.cursor');
    return c ? c.textContent : '';
  });
  check(await cellText() === 'rpo', 'the cell collapses to one glyph per op', await cellText());

  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(300);
  check(await cellText() === 'ret3',
        'stepping right inside the cell selects the first op and shows it in full',
        await cellText());

  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(300);
  check(await cellText() === 'p20', 'and again selects the next one', await cellText());

  // `@` on a selected op opens THAT op, not the row.
  await page.keyboard.press('@');
  await page.waitForTimeout(300);
  // `data-kind="editing"` is the cell being typed into, same as the whole-row case above.
  const seeded = await page.evaluate(() => {
    const c = document.querySelector('.tk-cell[data-kind="editing"]');
    return c ? c.textContent : null;
  });
  check(seeded === 'p20', 'the buffer opens seeded with just that op', JSON.stringify(seeded));

  // Replace it, and the other two must survive — the half that fails on a full mask.
  // TWO backspaces, not three: `p20` back to `p`, then `99`. Three leaves `99` with no prefix,
  // which names no op and is refused — correctly, and it looked like the edit not landing.
  for (const ch of ['Backspace', 'Backspace', '9', '9']) await page.keyboard.press(ch);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(1800);
  const after = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(after === 'ret3 p99 o80',
        'the single-op edit lands and the other two ops are untouched', after);

  // Leaving the cell drops the selection, so the cell collapses again and `@` means the row.
  await page.keyboard.press('ArrowLeft');
  await page.keyboard.press('ArrowLeft');
  await page.keyboard.press('ArrowLeft');
  await page.keyboard.press('ArrowLeft');
  await page.waitForTimeout(400);
  await page.evaluate(() => window.__uni.run('goto 0 0'));
  await page.waitForTimeout(400);
}

/*
 * THE v33 OPS, END TO END: a retrigger ramp and a conditional trig.
 *
 * `rv` only means anything beside `ret` — it is the difference between a roll and a stutter —
 * and `c` is the first op whose EFFECT changes from pass to pass while the row does not. Both
 * are written through the app's own verbs and read back off the note, because a mirror that
 * parses and formats correctly can still be sending the wrong bytes.
 */
{
  /*
   * ON THE OPS FIELD to read them back. `goto` lands on the track's first column and
   * `opsTextAtCursor` answers '' anywhere but field 2 — correctly, it is that cell's contents —
   * so a read taken straight after a goto reports an empty row that is carrying its ops fine.
   * Writing does not care which column the cursor is in; reading does.
   */
  // Row 32 is where this suite wrote its note; row 0 has none, and `ops` on an empty row is a
  // refusal rather than a write — which reads back as '' and looks like the ops being dropped.
  await page.evaluate(() => window.__uni.run('goto 32 0'));
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('ArrowRight');
  await page.waitForTimeout(300);
  const wrote = await page.evaluate(() => window.__uni.run('ops ret4 rv-60 c1:2'));
  void wrote;
  await page.waitForTimeout(1800);
  const spelt = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(spelt === 'ret4 rv-60 c1:2', 'a ramp and a conditional round-trip through the engine',
        JSON.stringify(spelt));

  // ONE OP AT A TIME still works on them: the mask has two more bits and `op` sets exactly one.
  await page.evaluate(() => window.__uni.run('op c2:4'));
  await page.waitForTimeout(1500);
  const after = await page.evaluate(() => window.__uni.opsTextAtCursor());
  check(after === 'ret4 rv-60 c2:4',
        'and `op` changes the conditional alone, leaving the ramp and the retrigger',
        JSON.stringify(after));

  /*
   * PRE AND NOT PRE, which are conditionals that are NOT an A:B form at all — `cpre` fires when
   * the previous conditional trig on the same track fired, `cnpre` when it did not. They are
   * round-tripped here rather than only unit-tested, because the parser agreeing with its Rust
   * twin says nothing about whether 130 survives the wire: the code lives in the same byte as
   * every packed A:B, and a client that clamped it to the 1..64 range would spell `c` back with
   * an empty value and lose the op silently.
   */
  /*
   * AN INVERTED CHECK, because this does not work yet and the reason is not on this side.
   *
   * `applyRowOpEdit` (clip_edit.cpp) refuses any trig condition above kTrigConditionMaxAB — 64 —
   * and PRE is 130. So `cpre` parses on both sides, formats on both sides, resolves correctly in
   * `trigConditionFires`, round-trips through a project file, and CANNOT BE SET BY A COMMAND:
   * the engine answers ValueOutOfRange and the row keeps whatever it had.
   *
   * Written to assert the CURRENT, BROKEN behaviour so it FAILS the day backend widens the
   * range. A comment would rot; a passing check that says "this is refused" turns into a red
   * line the moment it stops being true, and then it gets flipped. Reported to backend.
   *
   * The value half is asserted too: after the refusal the row must be UNCHANGED. An edit that
   * refused the conditional and applied the retrigger anyway would be the worse outcome, and
   * "unchanged" is the only part of this that will still be true after the fix.
   */
  const preResults = [];
  for (const form of ['cpre', 'cnpre']) {
    await page.evaluate((f) => window.__uni.run(`op ${f}`), form);
    await page.waitForTimeout(1500);
    preResults.push(await page.evaluate(() => window.__uni.opsTextAtCursor()));
  }
  check(preResults.every((x) => x === 'ret4 rv-60 c2:4'),
        'cpre/cnpre are still REFUSED by the engine (clip_edit.cpp caps a trig condition at 64 '
        + 'and PRE is 130) — flip this check when that range widens',
        JSON.stringify(preResults));
  // ...and an A:B still works, so the refusal above is about the VALUE and not about the op
  // having stopped working — the control that makes the inverted check a finding.
  await page.evaluate(() => window.__uni.run('op c3:4'));
  await page.waitForTimeout(1500);
  check(await page.evaluate(() => window.__uni.opsTextAtCursor()) === 'ret4 rv-60 c3:4',
        'while an A:B conditional in range still lands — the refusal is the value, not the op');

  /*
   * A CONDITIONAL THAT COULD NEVER FIRE IS REFUSED, not normalised. A > B names a pass that
   * does not exist in the cycle, so a note carrying it would simply never sound — which is not
   * something anyone types on purpose, and silently turning it into c1:2 would be inventing an
   * intention.
   */
  const bad = await page.evaluate(() => window.__uni.run('op c3:2'));
  await page.waitForTimeout(800);
  const kept = await page.evaluate(() => window.__uni.opsTextAtCursor());
  // c3:4 now, not c2:4 — the pre block above leaves the row on the last A:B it set. Named
  // rather than recomputed, so a check that depends on a previous block's end state says so.
  check(/never fire/.test(String(bad)) && kept === 'ret4 rv-60 c3:4',
        'a conditional whose A exceeds B is refused, and the row is untouched',
        `${String(bad).slice(-60)} / ${kept}`);
}

/*
 * NOTHING IS EVER HIDDEN — the design's own words, and now checkable.
 *
 * The collapsed cell draws one glyph per op, and `.tk-cell` is a FIXED WIDTH with
 * `overflow: hidden`. So there is a number of ops past which the run is silently clipped, and
 * the ops that fall off the end are exactly the ones nobody can see are missing — the same
 * failure the priority chain had, arrived at from the other direction.
 *
 * v33 took the count from five to seven, which makes this worth measuring rather than assuming.
 * The check compares the cell's scrollWidth against its clientWidth, which is the browser's own
 * answer to "is any of this off the end".
 */
{
  await page.evaluate(() => window.__uni.run('ops ret4 p60 d1/6 s7 o80 rv-60 c1:2'));
  await page.waitForTimeout(1800);
  // ...and park the cursor on ANOTHER ROW, so the cell under test is drawing its collapsed run
  // rather than the selected op expanded. The expansion is the sub-cell cursor working; it is
  // simply not what this measures.
  await page.evaluate(() => window.__uni.run('goto 33 0'));
  await page.waitForTimeout(600);

  /*
   * ADDRESSED BY ROW AND COLUMN, NOT BY THE CURSOR.
   *
   * `.tk-cell.cursor` is the wrong handle for this: the sub-cell cursor EXPANDS the selected op
   * to its full token, so the cell under the cursor may be showing `d1/6` rather than the run —
   * which is the feature working, and it read as the run being wrong. `goto` also preserves the
   * column, so arrowing to reach field 2 can step INTO the ops instead of onto them.
   *
   * The question here is what the collapsed run looks like, so ask a cell that is not the
   * cursor's.
   */
  const drawn = await page.evaluate(() => {
    const row = document.querySelector('.tk-row[data-row="32"]');
    const c = row && row.querySelector('.tk-cell[data-track="0"][data-col="2"]');
    if (!c) return null;
    return { text: c.textContent, scroll: c.scrollWidth, client: c.clientWidth };
  });
  check(drawn && drawn.text.length === 7,
        'a note carrying all seven ops draws seven glyphs', JSON.stringify(drawn));
  /*
   * AND HOW MUCH ROOM IS LEFT, because the design contemplates far more than seven.
   *
   * "Say I used 43 ops in a row, they all show in a single cell" is the case this was drawn
   * around, and a cell that holds ten is not that. Printed rather than asserted: the count that
   * matters is not a constant this file should own, and the check that FAILS is the clipping one
   * above — this line is so the number is visible before it does.
   */
  if (drawn) {
    const perGlyph = drawn.scroll > 0 ? (drawn.scroll - 12) / drawn.text.length : 0;
    const fits = perGlyph > 0 ? Math.floor((drawn.client - 12) / perGlyph) : 0;
    console.log(`  the ops cell holds about ${fits} glyphs (${drawn.client}px, `
                + `~${perGlyph.toFixed(2)}px each) — the design's own example is 43`);
  }
  check(drawn && drawn.scroll <= drawn.client,
        'and the cell is wide enough to show them — nothing is clipped',
        drawn && `${drawn.text.length} glyphs need ${drawn.scroll}px in a ${drawn.client}px cell`);
}

// ---------------------------------------------------------------------------
// THE OPS COLUMN IS PER TRACK (kShmVersion 34 / uiTrackOpsWidth).
//
// The cell measurement above is what motivated this: the ops cell holds about seven glyphs and
// seven ops now exist, so the column is exactly full — and it was being drawn, empty, on every
// track that will never use one. The engine publishes the widest run per track; 0 means the
// column has never held anything and it goes away.
// ---------------------------------------------------------------------------
{
  /*
   * BACK TO `opsfix`, WHICH IS THE TWO-TRACK FIXTURE.
   *
   * The write half above runs `new opswrite` — a fresh, one-track song — and everything after it
   * is in that song. Asked there, this section read `opsWidths()` as a one-element array and
   * reported the differential broken; a fixture with one track cannot tell "hidden correctly"
   * from "always hidden", which is the same blind spot the second track was added to close.
   */
  await page.evaluate(() => window.__uni.loadProject('opsfix'));
  await page.waitForTimeout(2500);
  await page.evaluate(() => window.__uni.run('zoom 1'));
  // ...AND BACK TO THE TOP. The write half left the cursor on row 32 and the viewport with it,
  // so every DOM probe below was reading rows the fixture's notes are not on — which looks
  // exactly like the notes not being drawn.
  await page.evaluate(() => window.__uni.run('goto 0 0'));
  await page.waitForTimeout(700);

  const widths = await page.evaluate(() => window.__uni.opsWidths());
  check(Array.isArray(widths) && widths.length >= 2,
        'the engine publishes a per-track op width', JSON.stringify(widths));
  /*
   * THE DIFFERENTIAL, which is the only form of this question worth asking.
   *
   * Track 0's notes carry ops and track 1's do not, so a correct reading is (>0, 0). An
   * always-0 read and an always-non-zero read each fail exactly one half of this, and neither
   * could be caught by looking at one track.
   */
  check(widths && widths[0] > 0 && widths[1] === 0,
        'and it distinguishes the track that uses ops from the one that does not',
        `t0=${widths && widths[0]} t1=${widths && widths[1]}`);
  /*
   * COUNTED IN GLYPHS, which is backend's stated contract: "one per op present — retrigger,
   * probability, delay, sound, offset, ramp, condition". Note 1 carries three of them, so the
   * widest run in track 0 is 3 — and the cell above proves the page draws exactly that many
   * characters for it, so the two sides are counting the same thing.
   */
  /*
   * COUNTED IN GLYPHS, which is backend's stated contract: "one per op present — retrigger,
   * probability, delay, sound, offset, ramp, condition". Asserted against what the PAGE DRAWS
   * rather than against a literal 3: the two sides counting the same thing is the claim, and a
   * hardcoded number would only ever check that this fixture is still this fixture.
   */
  const drawnRuns = (await opsCells()).map((c) => c.text.length);
  const widest = drawnRuns.length ? Math.max(...drawnRuns) : -1;
  check(widths && widths[0] === widest,
        'and it counts GLYPHS — the same number the page draws for the widest note',
        `engine says ${widths && widths[0]}, the cells draw ${JSON.stringify(drawnRuns)}`);

  const shown = await page.evaluate(() => window.__uni.opsShown());
  check(shown && shown[0] === 1 && shown[1] === 0,
        'so track 0 draws the column and track 1 does not', JSON.stringify(shown));

  // ...and the DOM agrees, which is the half a state accessor cannot answer. A zero-width box
  // is the thing that actually recovers the space.
  /*
   * SCOPED TO `.tk-band`, because the RULER is not in it and looks exactly like a row.
   *
   * `buildRuler` renders one exemplar of each width class as a two-track row so the widths can
   * be measured without drawing every track — `visibility: hidden`, which still has layout. An
   * unscoped `[data-track="1"]` found the ruler's second exemplar, measured 230px of a track
   * that is not the song's, and reported the column present when it was gone.
   */
  const boxes = () => page.evaluate(() => {
    const w = (t) => {
      const c = document.querySelector(`.tk-band .tk-cell.ops[data-track="${t}"][data-col="2"]`);
      return c ? Math.round(c.getBoundingClientRect().width) : -1;
    };
    const track = (t) => {
      const e = document.querySelector(`.tk-band .tk-track[data-track="${t}"]`);
      return e ? Math.round(e.getBoundingClientRect().width) : -1;
    };
    return { ops0: w(0), ops1: w(1), t0: track(0), t1: track(1) };
  });
  const b = await boxes();
  check(b.ops0 > 0 && b.ops1 === 0,
        'the ops cell has width on track 0 and none on track 1', JSON.stringify(b));
  check(b.t1 > 0 && b.t1 < b.t0,
        'so track 1 is genuinely narrower — the space is recovered, not just blanked',
        JSON.stringify(b));

  /*
   * AND IT COMES BACK. A column you cannot get back is a column you can never type the first op
   * into, because the cell to type it into would not be on screen — which would make this a
   * feature that quietly removes an ability rather than one that saves width.
   *
   * Driven through the CONSOLE verb, since that is the path an agent has; the header control
   * calls the same function, and a unit check holds the console and the api together.
   */
  const said = await page.evaluate(() => window.__uni.run('ops-column 1 on'));
  await page.waitForTimeout(400);
  const back = await boxes();
  check(back.ops1 > 0, '`ops-column 1 on` brings the column back', `${said} -> ${JSON.stringify(back)}`);
  check(back.t1 === b.t0, 'and track 1 is then exactly as wide as track 0',
        JSON.stringify(back));

  /*
   * THE HIT TEST'S INVERSE, with the column hidden again.
   *
   * On a track that hides its ops columns the drawn boxes are fields 0,1,3,4,... — so the third
   * box along is field 3, not field 2. `hitTest` divides by a uniform cell width to get a
   * VISIBLE index and has to map it back; get that wrong and every click past the first note
   * group lands one field to the left, which is a cursor in the wrong column and an edit to the
   * wrong thing, with nothing on screen to say so.
   *
   * Two note columns, so there IS a group past the first — with one, both mappings agree and the
   * check proves nothing.
   */
  await page.evaluate(() => { window.__uni.run('ops-column 1 off'); window.__uni.run('columns 2'); });
  await page.waitForTimeout(500);
  const mapped = await page.evaluate(() => {
    const cells = [...document.querySelectorAll('.tk-band .tk-row[data-row="0"] .tk-cell[data-track="1"]')]
      .filter((c) => c.getBoundingClientRect().width > 0);
    return cells.map((c) => {
      const r = c.getBoundingClientRect();
      const h = window.__uni.clickAt(r.left + r.width / 2, r.top + r.height / 2);
      return { want: Number(c.dataset.col), got: h ? h.col : null };
    });
  });
  const wrong = mapped.filter((x) => x.want !== x.got);
  check(mapped.length > 2 && wrong.length === 0,
        'with the column hidden, every drawn cell still hit-tests to its own field',
        `${mapped.length} cells, wrong: ${JSON.stringify(wrong.slice(0, 4))}`);
  // THE NEGATIVE CONTROL. If the drawn cells do not actually skip a field, the mapping above was
  // never exercised and the check passed on a track whose columns are all present.
  const skipped = mapped.some((x, i) => i > 0 && x.want - mapped[i - 1].want > 1);
  check(skipped, 'and the drawn cells really do skip the ops fields — the check had something to get wrong',
        JSON.stringify(mapped.map((x) => x.want)));
  await page.evaluate(() => window.__uni.run('columns 1'));
  await page.waitForTimeout(300);
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
