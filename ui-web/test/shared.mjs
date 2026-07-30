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
  check(sharedRails.every((r) => r.badge === '×2'),
        'and each says how many — "shared" without "with how many" is half an answer',
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
