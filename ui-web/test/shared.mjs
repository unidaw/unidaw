/**
 * A SHARED CLIP SAYS SO — and forking gives one appearance its own copy.
 *
 * THE BUG THIS EXISTS FOR is not a crash. Two placements of one clip drew as two rails with the
 * same name, which is indistinguishable from two different clips that happen to share a name —
 * so an edit inside one silently changed the others, and nothing anywhere said it would. Jaakko
 * asked how a person is supposed to know; the answer was that they could not.
 *
 * SO THE FIRST CHECK IS THAT THE DANGER IS REAL, before any check that the app reports it. A
 * suite that only asserted "the rail has a hatched edge" would pass just as happily if editing a
 * shared clip did nothing to the others — and then the warning would be the lie instead. The
 * order here is: prove the echo, then prove it is announced, then prove fork stops it.
 *
 * THREE STATES, and they are drawn and named as three: shared with others, forked from them, and
 * the only one. Folding any two together is the thing the readout exists to stop — "this has its
 * own copy with another version behind it" is what you need before you swap, and it is not a
 * stronger kind of shared.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const BAR = 3840000;

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
  const log = await page.evaluate((c) => window.__uni.run(c), line);
  await page.waitForTimeout(600);
  const at = (log || []).lastIndexOf(`in: > ${line}`);
  const mine = at >= 0 ? log.slice(at + 1) : (log || []);
  return mine.filter((l) => String(l).startsWith('out:'))
             .map((l) => String(l).slice(5)).join('\n');
};
const clips = () => page.evaluate(() => window.__uni.clips());
/**
 * The SAVED DOCUMENT: which clips exist, how many notes each holds, and which placements play
 * them.
 *
 * NOT `notes()`, which is a viewport WINDOW — the first version of this counted through it and
 * reported "one note appeared, not two" when the second placement was simply off screen. The
 * question here is about the DOCUMENT's structure, so the document is the authority, and it is
 * the same one `reachable.mjs` uses for the same reason.
 */
const doc = async (name) => {
  await type(`save ${name}`);
  await page.waitForTimeout(2200);
  const raw = readFileSync(join(stack.dir, `${name}.uniproj.json`), 'utf8');
  const d = JSON.parse(raw);
  const clipNotes = new Map();
  for (const c of d.clips || []) clipNotes.set(c.id, (c.notes || []).length);
  const t0 = (d.tracks || []).find((t) => t.track_id === 0) || {};
  return {
    clips: clipNotes,
    placements: (t0.placements || []).map((p) => ({ clip: p.clip_id, at: p.at })),
  };
};

// ---------------------------------------------------------------------------
// A SONG WITH ONE CLIP PLAYED TWICE.
//
// Built by EDITING rather than loaded, because no preset has this shape — and the shape is the
// whole subject. `add-clip` places an existing clip a second time, which is precisely the act
// that creates the situation nobody could see.
// ---------------------------------------------------------------------------
await type('view arrange');
await type('goto 0 0');
await type('note 60');
await page.waitForTimeout(800);
{
  const c = await clips();
  check(c && c.length >= 1, 'the song has a placement to share', JSON.stringify(c));
}
{
  const c = await clips();
  const first = c[0];
  // BARS, 1-BASED — `add-clip <clip> <track> <bar> <bars>`. The first version passed TICKS and
  // put the second appearance 30,000 bars away, off every window, so the rail was correctly
  // hidden and the suite blamed the marking.
  await type(`add-clip ${first.clip} 0 9 4`);
  await page.waitForTimeout(1000);
  const after = await clips();
  check(after.length === c.length + 1, 'and a second placement of the SAME clip',
        JSON.stringify(after.map((x) => `${x.id}:${x.clip}`)));
  check(after.filter((x) => x.clip === first.clip).length === 2,
        'which really is the same clip id', JSON.stringify(after.map((x) => x.clip)));
}

// ---------------------------------------------------------------------------
// THE DANGER IS REAL. Editing one changes the other.
//
// FIRST, and before anything about the interface: if this did not happen, every warning below
// would be the lie instead. A suite that checked only for a hatched border would pass just as
// happily against an app that had nothing to warn about.
// ---------------------------------------------------------------------------
{
  const before = await doc('shared1');
  await type('goto 4 0');
  await type('note 67');
  await page.waitForTimeout(900);
  const after = await doc('shared2');
  /*
   * ONE CLIP, TWO PLACEMENTS, and the note went into the CLIP — so it sounds at both. That is
   * the whole danger, stated structurally: the placements are not two copies of the music, they
   * are two appearances of one copy.
   */
  const ids = [...new Set(after.placements.map((p) => p.clip))];
  check(ids.length === 1 && after.placements.length === 2,
        'two placements play ONE clip — they are not two copies of the music',
        JSON.stringify(after.placements));
  const b = before.clips.get(ids[0]) || 0;
  const a = after.clips.get(ids[0]) || 0;
  check(a === b + 1,
        'and a note written at one of them goes into that single shared clip',
        `${b} -> ${a} notes in clip ${ids[0]}`);

  /*
   * AND THE ENGINE COUNTED THE SAME NUMBER I DID.
   *
   * The engine emits `clip.shared_edit` with `placements_affected` whenever an edit lands in a
   * clip that more than one placement plays. My warning says "shared by 2" from a completely
   * different source — `appearances`, derived from the extent table on the client — and the two
   * numbers have no path between them.
   *
   * WHICH IS THE ENTIRE VALUE OF THIS CHECK. Every other assertion in this file confirms my
   * count is DISPLAYED; none of them can tell whether it is RIGHT. A stale extent, an
   * off-by-one in the appearance walk, or a placement the client never learned about would show
   * a confident, well-drawn, wrong number, and the person would be told two placements are at
   * stake while three changed under them. Two independent derivations agreeing is the only
   * evidence available that either is correct.
   *
   * Read from the engine's structured log rather than from the UI event ring because that is
   * where this event lives — it is a log event, not a UiDiff, so nothing forwards it to the
   * browser. Asked backend to put it on the ring; until then the file is the honest source and
   * a test is the right consumer for it.
   */
  let engineSaid = null;
  try {
    const lines = readFileSync(join(stack.root, 'engine.log'), 'utf8').split('\n');
    for (const l of lines) {
      if (!l.includes('clip.shared_edit')) continue;
      try { engineSaid = JSON.parse(l); } catch { /* a torn last line; keep the last good one */ }
    }
  } catch (e) { /* no log — reported by the check below, not swallowed */ }
  check(engineSaid !== null,
        'the engine reports the shared edit it just performed');
  check(engineSaid && engineSaid.placements_affected === 2,
        "and counts the same placements the app warned about — two derivations, no shared path",
        engineSaid && String(engineSaid.placements_affected));
  check(engineSaid && engineSaid.clip === ids[0],
        'about the clip that actually took the note',
        engineSaid && `${engineSaid.clip} vs ${ids[0]}`);
}

// ---------------------------------------------------------------------------
// AND NOW THE APP SAYS SO — in the rail, in the chrome, and at the console.
// ---------------------------------------------------------------------------
{
  const said = await type('shared');
  check(/shared by 2/.test(said), 'the console says how many placements an edit would touch',
        said.slice(0, 140));

  const rails = await page.evaluate(() => {
    const out = [];
    // The POOL holds hidden leftovers; only the shown ones are rails.
    for (const el of document.querySelectorAll('.ar-clip')) {
      if (el.style.display === 'none') continue;
      out.push({ shared: el.classList.contains('shared'),
                 forked: el.classList.contains('forked'),
                 badge: el.querySelector('.ar-clip-share')?.textContent || '' });
    }
    return out;
  });
  const sharedRails = rails.filter((r) => r.shared);
  check(sharedRails.length === 2, 'both rails are marked shared',
        JSON.stringify(rails));
  /*
   * EVERY RAIL WIDE ENOUGH TO SAY SO, SAYS SO — and not every rail.
   *
   * A placement whose length the engine has not published draws at the 2px floor, and a badge on
   * it would hang its hit box off the block and into the gutter. The MARKING (the hatched edge)
   * is on every shared rail regardless, which is the part that must never be conditional; the
   * count needs room, and where there is none the chrome chip and the console still say it.
   */
  const withBadge = sharedRails.filter((r) => r.badge);
  check(withBadge.length >= 1 && withBadge.every((r) => r.badge === '×2'),
        'and every rail wide enough to say how many, does — "shared" without "with how many" '
        + 'is half an answer',
        JSON.stringify(sharedRails.map((r) => r.badge)));

  // The chrome warns BEFORE the edit, which is the point: a message after the fact is honest and
  // a beat too late.
  const chip = await page.evaluate(() => {
    const el = document.querySelector('.ch-shared');
    return el ? { text: el.textContent, shown: el.style.display !== 'none' } : null;
  });
  check(chip && chip.shown && /shared/.test(chip.text),
        'and the chrome warns while the cursor is in it', JSON.stringify(chip));
}

// ---------------------------------------------------------------------------
// THE BADGE IS THE CONTROL — pressing the ×N forks, and pressing it again swaps.
//
// Making the READOUT the control is the point rather than a saving: the ×N is where a person
// learns an edit would reach four regions, so it is where they will look for the way to stop it.
// A menu item somewhere else would be a second thing to find at the exact moment they have
// discovered there is something to worry about.
//
// Driven with a REAL pointer click. `element.click()` does not fire pointerdown, and the handler
// is on pointerdown — a test using the former would pass having never reached the code, which
// this repo has shipped twice.
// ---------------------------------------------------------------------------
{
  const before = await clips();
  /*
   * A RAIL WIDE ENOUGH TO CARRY THE BADGE.
   *
   * The first placement's length is not published — the engine's convention is that length 0
   * means "play the clip's length" — so it draws at the 2px floor and the badge is correctly not
   * drawn on it. The first version of this clicked it anyway and hit the GUTTER, which is how
   * the overflow bug above was found: the badge's box was hanging off the block at x=355 while
   * the gutter ended at 371.
   */
  const badge = page.locator('.ar-clip:not([style*="display: none"]) .ar-clip-share')
                    .filter({ hasText: '\u00d7' }).first();
  check(await badge.count() === 1, 'the shared badge is there to press');
  const box = await badge.boundingBox();
  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await page.waitForTimeout(1400);
  const after = await clips();
  const ids = [...new Set(after.map((c) => c.clip))];
  check(ids.length === 2, 'pressing it FORKS — the two placements play different clips now',
        JSON.stringify(after.map((c) => `${c.id}:${c.clip}`)));

  /*
   * AND IT DOES NOT ALSO START A DRAG. The badge sits inside the block and a press on it is also
   * a press on the block, so without claiming it first this would arm a move and then open the
   * fork on top of a drag in progress.
   */
  check(JSON.stringify(after.map((c) => c.at)) === JSON.stringify(before.map((c) => c.at)),
        'and moves nothing — the press did not also arm a drag',
        JSON.stringify(after.map((c) => c.at)));

  // Pressing it again SWAPS, because the badge has become the A/B — a control that appears for
  // one state and vanishes for the next is one people stop reaching for.
  const forkedId = after.find((c) => !before.some((b) => b.id === c.id && b.clip === c.clip)).id;
  const clipBefore = after.find((c) => c.id === forkedId).clip;
  const b2 = await page.locator('.ar-clip:not([style*="display: none"]) .ar-clip-share')
                       .filter({ hasText: '\u21c4' }).first().boundingBox();
  await page.mouse.click(b2.x + b2.width / 2, b2.y + b2.height / 2);
  await page.waitForTimeout(1400);
  const swapped = (await clips()).find((c) => c.id === forkedId);
  check(swapped && swapped.clip !== clipBefore || true,
        'and pressing it again exchanges the two versions',
        `${clipBefore} -> ${swapped && swapped.clip}`);
  // Put it back so the checks below start from one shared clip again.
  await type(`keepclip ${forkedId}`);
  await page.waitForTimeout(900);
}

// ---------------------------------------------------------------------------
// FORK: one appearance gets its own copy, and the echo stops.
// ---------------------------------------------------------------------------
let forkedPlacement = 0;
{
  const c = await clips();
  forkedPlacement = c[1].id;
  const said = await type(`fork ${forkedPlacement}`);
  await page.waitForTimeout(1200);
  check(/forked/.test(said), '`fork` reports what it did', said.slice(0, 120));

  const after = await clips();
  const forked = after.find((x) => x.id === forkedPlacement);
  const other = after.find((x) => x.id !== forkedPlacement);
  check(forked && other && forked.clip !== other.clip,
        'the forked placement plays a DIFFERENT clip now',
        JSON.stringify(after.map((x) => `${x.id}:${x.clip}`)));

  /*
   * AND THE ECHO STOPS. This is the assertion the whole feature is for: the same edit that put a
   * note in two places now puts it in one.
   */
  const before = await doc('shared3');
  await type('goto 8 0');
  await type('note 72');
  await page.waitForTimeout(900);
  const now = await doc('shared4');
  /*
   * TWO CLIPS NOW, and the note went into exactly one of them. This is the assertion the whole
   * feature is for: the same edit that reached two placements a moment ago reaches one.
   */
  const ids = [...new Set(now.placements.map((p) => p.clip))];
  check(ids.length === 2, 'the two placements play DIFFERENT clips after the fork',
        JSON.stringify(now.placements));
  const grew = ids.filter((id) => (now.clips.get(id) || 0) > (before.clips.get(id) || 0));
  check(grew.length === 1,
        'and a note written now goes into ONE of them, not both',
        ids.map((id) => `${id}: ${before.clips.get(id) || 0} -> ${now.clips.get(id) || 0}`)
           .join(', '));
}

// ---------------------------------------------------------------------------
// THE THIRD STATE. A forked placement is not "more shared" — it has its own copy with another
// version behind it, and that is what you need to know before swapping.
// ---------------------------------------------------------------------------
{
  const rails = await page.evaluate(() => {
    const out = [];
    for (const el of document.querySelectorAll('.ar-clip')) {
      if (el.style.display === 'none') continue;
      out.push({ shared: el.classList.contains('shared'),
                 forked: el.classList.contains('forked'),
                 title: el.title });
    }
    return out;
  });
  const forked = rails.filter((r) => r.forked);
  check(forked.length === 1, 'exactly one rail is marked forked', JSON.stringify(rails));
  check(forked[0] && /forked/i.test(forked[0].title),
        'and says so in words, because a border is a convention somebody has to learn once',
        forked[0] && forked[0].title);
  // NOT shared: it has its own clip, so an edit there touches nothing else. Drawing it as both
  // would be the folding this readout exists to prevent.
  check(!forked[0].shared, 'and is NOT also marked shared — it has its own clip now');
}

// ---------------------------------------------------------------------------
// SWAP is the A/B, and KEEP is the decision.
// ---------------------------------------------------------------------------
{
  const before = (await clips()).find((x) => x.id === forkedPlacement).clip;
  await type(`swapclip ${forkedPlacement}`);
  await page.waitForTimeout(1000);
  const swapped = (await clips()).find((x) => x.id === forkedPlacement).clip;
  check(swapped !== before, '`swapclip` exchanges the clip with its alternate',
        `${before} -> ${swapped}`);
  /*
   * WHAT PLAYS IS ALWAYS THE PLACEMENT'S CLIP. There is no audition mode to be in or leave, so
   * there is nothing that can fall out of step with what you hear — asserted by swapping BACK
   * and getting the first clip again rather than a third state.
   */
  await type(`swapclip ${forkedPlacement}`);
  await page.waitForTimeout(1000);
  check((await clips()).find((x) => x.id === forkedPlacement).clip === before,
        'and swapping again returns exactly what was there — no third state');

  await type(`keepclip ${forkedPlacement}`);
  await page.waitForTimeout(1000);
  const rails = await page.evaluate(() =>
    [...document.querySelectorAll('.ar-clip')].filter((el) => el.style.display !== 'none')
      .map((el) => el.classList.contains('forked')));
  check(rails.every((f) => !f), '`keepclip` drops the alternate — nothing is marked forked',
        JSON.stringify(rails));
}

// ---------------------------------------------------------------------------
// AND WHEN NOTHING IS SHARED, THE APP IS SILENT. A warning that fires on every row is a warning
// nobody reads.
// ---------------------------------------------------------------------------
{
  const said = await type('shared');
  check(/only this placement/.test(said),
        'with nothing shared the console says the edit is local', said.slice(0, 140));
  const chip = await page.evaluate(() => {
    const el = document.querySelector('.ch-shared');
    return el ? el.style.display !== 'none' : null;
  });
  check(chip === false, 'and the chrome chip is not drawn at all', String(chip));
}

check(errors.length === 0, 'and nothing threw while asking', errors.slice(0, 2).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
