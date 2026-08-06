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
  clips: [{ id: 1, name: 'BEFORE', length: Q * 4, lines_per_beat: 4, kind: 'symbolic',
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

writeFileSync(join(stack.dir, 'renrace.uniproj.json'), JSON.stringify(fixture('renrace'), null, 2));
writeFileSync(join(stack.dir, 'renrace2.uniproj.json'), JSON.stringify(fixture('renrace2'), null, 2));

/* ── IN THE WINDOW ──────────────────────────────────────────────────────────────────────────
 * Wait only until the clip is visible — that is the earliest a person could click it — and rename
 * at once. Waiting longer would step outside the very window this is about.
 */
const t0 = Date.now();
await run('load renrace');
await page.waitForFunction(
  () => { const e = window.__uni.engineState(); return e && e.noteCount > 0; },
  null, { timeout: 10000 }).catch(() => {});
/*
 * `.clip`, NOT `.id`. `__uni.clips()` returns { id: placementId, clip: clipId, ... } and
 * `cliptext` wants the CLIP. This fixture has one of each and both are 1, so passing the wrong
 * field would have worked here and tested nothing — the kind of accident that only shows up on a
 * project where the numbers have drifted apart.
 */
const clipId = await page.evaluate(() => (window.__uni.clips() || [])[0]?.clip);
check(clipId !== undefined, 'the loaded project has a clip to rename', String(clipId));

// FOUR arguments: <track> <clip> name|source <text>. Two would be refused by the
// console's own gate, and this suite would then report 'the rename did not land' for a
// reason that has nothing to do with the race it is about.
/*
 * HOW TIGHT THE WINDOW ACTUALLY WAS. A race test that waits too long is a test that did not try,
 * and it reports "does not reproduce" in exactly the same words as one that did. So the gap
 * between the load and the rename is measured and asserted: if it grew past a second, the run
 * says nothing about the race and must say so rather than passing.
 */
const gap = Date.now() - t0;
check(gap < 1000,
      `the rename went out ${gap}ms after the load — inside the window`,
      `${gap}ms is too long to say anything about a race that resolves in the publish interval`);
const r1 = await run(`cliptext 0 ${clipId} name IMMEDIATE`);
check(!/not|error|refus/i.test(String(r1)), 'the rename command was ACCEPTED', String(r1));
await settle(1200);
const immediate = await savedName('renrace_out');
const immediateLanded = immediate === 'IMMEDIATE';

/* ── OUTSIDE IT, the control ────────────────────────────────────────────────────────────── */
await run('load renrace2');
await settle(3000);
const clipId2 = await page.evaluate(() => (window.__uni.clips() || [])[0]?.clip);
const r2 = await run(`cliptext 0 ${clipId2} name SETTLED`);
check(!/not|error|refus/i.test(String(r2)), 'the control rename was accepted', String(r2));
await settle(1200);
const settled = await savedName('renrace2_out');
const settledLanded = settled === 'SETTLED';

check(settledLanded,
      'CONTROL: the same rename lands after a generous settle',
      `clip name is ${JSON.stringify(settled)} — if this fails the race is not what is wrong, `
      + 'SetClipText is broken outright');

if (settledLanded && !immediateLanded) {
  // REPRODUCED. Assert the CAUSE from the engine's own mouth rather than inferring it.
  const log = join(stack.dir, '..', 'engine.log');
  const text = existsSync(log) ? readFileSync(log, 'utf8') : '';
  const mismatch = /clip\.version_mismatch/.test(text);
  check(false,
        'a clip renamed right after a load LANDS',
        `got ${JSON.stringify(immediate)}, expected "IMMEDIATE" — backend's task #120 reproduces `
        + `in this tree. engine log says clip.version_mismatch: ${mismatch}`);
  note(mismatch ? 'confirmed by clip.version_mismatch in the engine log'
                : 'NOT accompanied by clip.version_mismatch — so it may be a different cause; '
                  + 'do not assume backend\'s diagnosis without this line');
} else {
  check(immediateLanded,
        'A CLIP RENAMED RIGHT AFTER A LOAD LANDS — backend task #120 does not reach this tree',
        `got ${JSON.stringify(immediate)}, expected "IMMEDIATE"`);
}

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
