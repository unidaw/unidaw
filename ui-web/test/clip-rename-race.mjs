#!/usr/bin/env node
/**
 * RENAMING A CLIP THE MOMENT A PROJECT OPENS.
 *
 * Backend, landing the undo switchover: "the PUBLISHED per-track clip version and the harmony
 * version LAG the engine's after a load. Any client that stamps its base from published state gets
 * refused on its first edit. daw-cli hides it for notes with an auto-retry; clip-name has none and
 * just fails. If ui-web renames a clip right after opening a project, expect
 * clip.version_mismatch."
 *
 * ── WHY THIS FILE EXISTS RATHER THAN A SHRUG ────────────────────────────────────────────────
 *
 * Their undo work is not in this branch, so the three behaviour changes they describe do not apply
 * here. The LAG might be theirs or might be the engine's all along — and if it is the engine's, it
 * applies to this tree too, on the morning of a demo.
 *
 * There is already good evidence it does not bite for NOTES: fifty-four suites load a project and
 * immediately edit, over and over, and pass. The uncovered case is the exact one named —
 * SetClipText, which `late-opcodes.mjs` does test but from a fixture it built rather than from a
 * project it just opened.
 *
 * ── THE CONTROL IS THE WHOLE DESIGN ─────────────────────────────────────────────────────────
 *
 * A race reproduces or it does not, and "it passed" proves nothing unless the same edit is also
 * made OUTSIDE the window and seen to work. So this renames twice: once in the window that is
 * claimed to fail, once after a generous settle. Three outcomes and they mean different things:
 *
 *   both land            -> the lag does not reach this tree. Recorded, and the suite still
 *                           guards the sequence for whoever merges main later.
 *   only the settled one  -> reproduced. The engine log should then say clip.version_mismatch,
 *                           and this file asserts THAT rather than inferring the cause — backend
 *                           misdiagnosed it twice before finding it.
 *   neither               -> not this race at all; SetClipText is broken outright, which is a
 *                           different and larger bug.
 *
 * ── THE MECHANISM IS ALREADY VISIBLE IN THE SOURCE ──────────────────────────────────────────
 *
 * `dockApi.clipText` (index.html) stamps its base like this:
 *
 *     base: engine ? engine.clipVersion : 0
 *
 * That is PUBLISHED state, which is precisely what backend says lags after a load — and it is the
 * GLOBAL counter rather than the track's, which is the same confusion that has bitten this project
 * three times already (set_harmony, the page's socket layer, delete_harmony). There is no retry on
 * this path, which matches "clip-name has none and just fails".
 *
 * So this suite has a named suspect before it runs. It still measures rather than assumes: a
 * plausible mechanism that reproduces nothing is a theory, and this project has paid for enough of
 * those.
 */

import { chromium } from 'playwright';
import { readFileSync, existsSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};
const note = (what) => console.log('  NOTE ', what);

const Q = 960000;
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

/** A project with one named clip, placed once. */
const fixture = (name) => ({
  schema_version: 4, meta: { name, created_utc: 0, modified_utc: 0 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [],
  clips: [{ id: 1, name: `BEFORE_${name}`, length: Q * 4, lines_per_beat: 4, kind: 'symbolic',
            time_sig_numerator: 4, time_sig_denominator: 4, chords: [],
            notes: [{ nanotick: 0, duration: Q, pitch: 60, velocity: 100, column: 0, note_id: 1 }] }],
  tracks: [{
    track_id: 0, name: 'T', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
    device_chain: [], mod_links: [],
    placements: [{ clip_id: 1, at: 0, length: Q * 4, notes: [], chords: [], mutes: [] }],
  }],
});

/** The clip's name in the saved project. */
const savedName = async (tag) => {
  await run(`save ${tag}`);
  for (let i = 0; i < 40; i++) {
    const p = join(stack.dir, `${tag}.uniproj.json`);
    if (existsSync(p)) {
      try {
        const doc = JSON.parse(readFileSync(p, 'utf8'));
        const c = (doc.clips || [])[0];
        if (c) return c.name;
      } catch { /* mid-write */ }
    }
    await settle(150);
  }
  return null;
};

console.log('\nrenaming a clip the moment a project opens\n');

/*
 * ── FIVE ATTEMPTS, NOT ONE ──────────────────────────────────────────────────────────────────
 *
 * THE LESSON THIS FILE WAS BOUGHT WITH. I ran the race once, saw it pass, and reported that
 * backend's fault "does not reach this tree". It is INTERMITTENT — the window is a single consumer
 * pass — so one green run cannot establish absence, and the mechanism IS present here:
 * `bumpAllTrackClipVersions()` is called from engine_load_project.cpp on the load path, exactly as
 * it is on their branch inside applyDocument.
 *
 * So the suite tries repeatedly. Five is not a proof either — nothing here can prove the absence of
 * a rare fault — but it is five times the chance of catching one, for about fifteen seconds.
 */
const ATTEMPTS = 5;
for (let i = 0; i < ATTEMPTS; i++) {
  writeFileSync(join(stack.dir, `renrace${i}.uniproj.json`),
                JSON.stringify(fixture(`renrace${i}`), null, 2));
}
writeFileSync(join(stack.dir, 'renrace_ctl.uniproj.json'),
              JSON.stringify(fixture('renrace_ctl'), null, 2));

let landed = 0;
let worstGap = 0;
const refusals = [];
for (let i = 0; i < ATTEMPTS; i++) {
  /*
   * WAIT FOR *THIS* PROJECT, not for "some notes exist".
   *
   * The first version waited on `noteCount > 0`, which after the first iteration was ALREADY TRUE
   * from the previous project — so attempts 1..4 fired the rename 5ms in, before the new document
   * had loaded at all, and reported 1/5 landed. That reads exactly like backend's race and is not:
   * it is renaming a clip in a project that is not open yet. The giveaway was the gap collapsing
   * from 37ms to 5ms, because a load does not finish in five milliseconds.
   *
   * Each fixture now starts with its OWN clip name, so "this project is open" is observable
   * instead of inferred.
   */
  const t0 = Date.now();
  await run(`load renrace${i}`);
  await page.waitForFunction(
    (want) => ((window.__uni.clips() || [])[0] || {}).name === want,
    `BEFORE_renrace${i}`, { timeout: 10000 }).catch(() => {});
  const id = await page.evaluate(() => (window.__uni.clips() || [])[0]?.clip);
  // `.clip`, NOT `.id` — clips() returns { id: placementId, clip: clipId } and cliptext wants the
  // CLIP. Both are 1 in this fixture, so the wrong field would pass while testing nothing.
  // FOUR arguments: <track> <clip> name|source <text>. Two are refused by the console's gate,
  // which would read as the race reproducing when nothing had been sent.
  await run(`cliptext 0 ${id} name TOOK${i}`);
  const gap = Date.now() - t0;
  worstGap = Math.max(worstGap, gap);
  await settle(1000);
  const got = await savedName(`renrace${i}_out`);
  if (got === `TOOK${i}`) landed += 1;
  else refusals.push(`attempt ${i}: name is ${JSON.stringify(got)} after ${gap}ms`);
}

check(worstGap < 1500,
      `every rename went out inside the window — worst gap ${worstGap}ms`,
      `${worstGap}ms is too long to say anything about a race that resolves in one consumer pass`);
check(landed === ATTEMPTS,
      `A CLIP RENAMED RIGHT AFTER A LOAD LANDS, ${ATTEMPTS} times out of ${ATTEMPTS}`,
      `${landed}/${ATTEMPTS} landed. ${refusals.join('; ')} — backend task #120 reproduces here. `
      + 'The engine log should say clip.version_mismatch; if it does not, the cause is something '
      + 'else and their diagnosis must not be assumed');

/* ── OUTSIDE THE WINDOW, the control ───────────────────────────────────────────────────────
 * A race that never fires proves nothing unless the same edit is seen to work when it cannot be
 * racing. This separates "no race here" from "SetClipText is broken outright".
 */
await run('load renrace_ctl');
await settle(3000);
const clipId2 = await page.evaluate(() => (window.__uni.clips() || [])[0]?.clip);
const r2 = await run(`cliptext 0 ${clipId2} name SETTLED`);
check(!/not|error|refus/i.test(String(r2)), 'the control rename was accepted', String(r2));
await settle(1200);
const settled = await savedName('renrace_ctl_out');
const settledLanded = settled === 'SETTLED';

check(settledLanded,
      'CONTROL: the same rename lands after a generous settle',
      `clip name is ${JSON.stringify(settled)} — if this fails the race is not what is wrong, `
      + 'SetClipText is broken outright');

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
