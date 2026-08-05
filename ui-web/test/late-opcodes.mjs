#!/usr/bin/env node
/**
 * THE THREE COMMANDS THE ENGINE HAD AND NO SURFACE COULD SEND.
 *
 * `npm test` carries a check called "every engine command has a caller, or a recorded reason it
 * has none". It read the engine's own `UiCommandType` enum and found three: SetMarkerColor,
 * SetClipText and RequestSamplerEnvelope. All three were capability that existed, was tested
 * engine-side, and could not be reached from anywhere a person could touch.
 *
 * They are three DIFFERENT shapes of the same defect, which is why they are worth one file:
 *
 *   SetMarkerColor          a field that was WRITE-ONCE. AddMarker set the colour and nothing
 *                           could change it afterwards, on a value the project persists, the
 *                           engine publishes and the arrangement draws.
 *   SetClipText             a field with NO WRITER AT ALL — the last two in the project,
 *                           persisted, published, rendered, and unreachable. A clip's name
 *                           could be drawn forever and never changed.
 *   RequestSamplerEnvelope  a WRITER WITH NO READER. Opcode 84 could write a multi-point curve,
 *                           both loop ranges, the release fade and the rate; nothing could read
 *                           any of it back, so an editor built on it could only overwrite.
 *
 * WHY A LIVE TEST AND NOT JUST THE RATCHET. Satisfying "something calls this opcode" is a
 * search for a string in a source file. It cannot tell whether the command was built correctly,
 * whether the engine accepted it, or whether the field changed — and the plugin-editor button in
 * this same session was called by a caller, sent on every click, and had never once worked
 * because the id was written to the wrong offset of a same-sized struct. A caller is not a
 * feature. This drives each one and reads the result back.
 */

import { chromium } from 'playwright';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await page.waitForTimeout(1500);

const run = (c) => page.evaluate((x) => window.__uni.run(x), c);
const settle = (ms) => page.waitForTimeout(ms);

console.log('\nthe three commands nothing could send\n');

await run('new lateops');
await settle(1200);

// ===========================================================================
// SetMarkerColor — a field that was write-once.
// ===========================================================================
await run('marker 0 intro');
await settle(900);

/*
 * `.list`, not the return value. `markers()` answers {version, count, truncated, songEnd, list} —
 * indexing it directly yields undefined, which reads as "no marker exists" and had this file
 * reporting a working feature as broken.
 */
const before = await page.evaluate(() => ((window.__uni.markers() || {}).list || [])[0] || null);
check(!!before, 'a marker exists to recolour', JSON.stringify(before));

if (before) {
  const said = await run(`colormarker ${before.id} 0xff8800`);
  await settle(1200);
  const after = await page.evaluate((id) =>
    (((window.__uni.markers() || {}).list) || []).find((m) => m.id === id) || null, before.id);

  check(!/refus|no marker|unknown|expects/i.test(String(said)),
        'the console accepts a recolour', JSON.stringify(said));
  /*
   * THE READ-BACK IS THE CLAIM. "the command was sent" is what the plugin-editor button could
   * also have said, every time, for as long as it never worked.
   */
  check(after && after.color === 0xff8800,
        'and the marker actually changes colour — read back from the engine',
        `was ${before && before.color}, now ${after && after.color}`);
  check(after && after.color !== (before && before.color),
        'the colour is different from what it was, so this is not a no-op passing');

  // Absent is not "unchanged" on this wire, so a recolour with no colour must be refused
  // rather than painting the marker black.
  const noColour = await page.evaluate((id) => window.__uni.colorMarker(id), before.id);
  check(noColour === false, 'a recolour with no colour is refused, not applied as black',
        String(noColour));
}

// ===========================================================================
// SetClipText — fields with no writer at all.
// ===========================================================================
await run('goto 0 0');
await run('note 48');
await settle(1000);

const clip = await page.evaluate(() => {
  const cs = window.__uni.clips ? window.__uni.clips() : null;
  return Array.isArray(cs) && cs.length ? cs[0] : null;
});
check(!!clip, 'a clip exists to rename', JSON.stringify(clip));

if (clip) {
  /*
   * `clip.clip`, NOT `clip.id`. `clips()` reports the PLACEMENT id as `id` and the clip id as
   * `clip`, and SetClipText addresses the CLIP. The two are equal in a fresh song, which is the
   * worst possible arrangement: the wrong one works until the day a song has more than one
   * placement of anything.
   */
  const said = await run(`cliptext 0 ${clip.clip} name VERSE A`);
  await settle(1500);

  check(!/refus|unknown|expects|error/i.test(String(said)),
        'the console accepts a clip rename', JSON.stringify(said));

  /*
   * THE SAVED PROJECT IS THE ORACLE, not the read-back. The engine logs `clip_text.set` and
   * writes the name into the project — and the PUBLISHED extent keeps the old one until the
   * song is reloaded, so `clips()` answers "Clip" for a clip that is called "VERSE A" on disk.
   *
   * Asserted against the file because that is where the truth is, and "VERSE A" rather than
   * "VERSE" because a `rest` argument arrives as separate words: taking only the first is the
   * trap namemarker documents, and a clip called "VERSE" looks like a working rename.
   */
  await run('save lateops');
  await settle(2000);
  let saved = null;
  try {
    saved = JSON.parse(readFileSync(join(stack.dir, 'lateops.uniproj.json'), 'utf8'));
  } catch (e) { /* the check below reports it */ }
  const savedName = saved && (saved.clips.find((c) => c.id === clip.clip) || {}).name;
  check(savedName === 'VERSE A',
        'the clip is renamed in the project, keeping both words',
        `the saved clip is called ${JSON.stringify(savedName)}`);

  /*
   * AND THE PUBLISHED NAME FOLLOWS, with no reload.
   *
   * THIS CHECK WAS INVERTED, and it was inverted around a wrong diagnosis. It asserted that the
   * published name STAYED STALE and recorded that as an engine gap, reported as such. Backend
   * measured the engine — the new name is in UiClipExtent within one publish cycle, and
   * tools/clip_text_check.sh already pins it — and pointed at wire.js, where the change detector
   * omitted the one field this opcode writes. A pure rename touches only the 32 name bytes, so
   * `changed` stayed false and the decode never ran.
   *
   * Worth keeping as a note rather than quietly rewriting: an inverted check is a claim about
   * where a defect lives, and this one pointed at the wrong half of the system. It passed for
   * the whole time it was wrong, because it was asserting the symptom.
   */
  const shown = await page.evaluate((id) => {
    const cs = window.__uni.clips ? window.__uni.clips() : [];
    return (cs.find((c) => c.clip === id) || {}).name;
  }, clip.clip);
  check(shown === 'VERSE A',
        'and the published name follows immediately, with no reload',
        `the arrangement shows ${JSON.stringify(shown)}`);
}

// ===========================================================================
// RequestSamplerEnvelope — a writer that had no reader.
// ===========================================================================
await run('sampler');
await settle(1500);

/*
 * ASKED THROUGH THE CONSOLE, ANSWERED ON THE ACK CHANNEL. The reply is not a return value, so
 * the assertion is that an ANSWER ARRIVES — and specifically that it is the envelope shape and
 * not the timeout, which the sidecar reports as an error rather than as "found: false". Those
 * two must stay distinguishable: "the engine looked and there is nothing" and "the engine never
 * answered" are different facts and only the second means something is broken.
 */
const reply = await page.evaluate(() => new Promise((res) => {
  const seen = [];
  const ws = new WebSocket(`ws://127.0.0.1:${(Number(location.port) || 8173) + 2}`);
  ws.onopen = () => ws.send(JSON.stringify({ type: 'samplerenvelope', track: 0, device: 0 }));
  ws.onmessage = (e) => { seen.push(String(e.data)); if (seen.length >= 1) res(seen[0]); };
  setTimeout(() => res(seen[0] || ''), 4000);
}));

check(reply !== '', 'the engine answers an envelope request at all',
      'nothing came back within 4s');
check(/samplerenvelope/.test(reply), 'the answer is an envelope shape, not a timeout',
      String(reply).slice(0, 200));
if (/samplerenvelope/.test(reply)) {
  let parsed = null;
  try { parsed = JSON.parse(reply).samplerenvelope; } catch (e) { /* checked below */ }
  check(!!parsed, 'and it parses', String(reply).slice(0, 160));
  if (parsed) {
    // The FIELDS that make the answer usable for drawing. A reply that carried only `found`
    // would satisfy "something answered" and still leave an editor unable to draw anything.
    check(Array.isArray(parsed.points), 'it carries the points', JSON.stringify(parsed).slice(0, 160));
    check(parsed.timeBase !== undefined,
          'and the TIME BASE, without which the point times mean nothing',
          JSON.stringify(parsed).slice(0, 160));
    check(Array.isArray(parsed.sustainLoop) && Array.isArray(parsed.releaseLoop),
          'and both loop ranges', JSON.stringify(parsed).slice(0, 160));
  }
}

check(errors.length === 0, 'no page errors', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
