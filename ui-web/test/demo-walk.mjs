#!/usr/bin/env node
/**
 * THE DEMO RUNBOOK, WALKED IN ORDER.
 *
 * docs/DEMO.md describes seven things being shown on Friday. Each of them has a suite that
 * asserts it properly — same-data, harmony-quantize, inspect, sampler-default, patcher-device,
 * device-rail, ai-demo. This file asserts the thing NONE of them can: that the steps work IN
 * SEQUENCE, in one session, on one song, in the order a person will actually perform them.
 *
 * That is a different question and it has bitten this session twice. Every individual AI prompt
 * worked; asked one after another they interfered, because the conversation carried. Every
 * sampler suite passed; the load-and-play gesture was silent, because each suite remapped the
 * slot first and none of them just played a note. A sequence is not the sum of its steps.
 *
 * DELIBERATELY SHALLOW PER STEP. The depth lives in the per-area suites and repeating it here
 * would mean two places to update when a surface changes — this asks only "did the step do its
 * job", so a failure points at the runbook rather than at the feature.
 *
 * NO AI HERE. The prompts cost money and need a key, so they stay in ai-demo.mjs, which the
 * sweep excludes for that reason. Run that one too before the demo.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};
const step = (s) => console.log(`\n${s}`);

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
const engine = () => page.evaluate(() => window.__uni.engineState() || {});

/** The rail gesture the runbook tells you to use. Ensures open rather than toggling. */
const pick = async (cat, want) => {
  const open = await page.evaluate(() => {
    const r = document.querySelector('.br');
    return !!r && r.offsetParent !== null && getComputedStyle(r).display !== 'none';
  });
  if (!open) await page.keyboard.press('Meta+b');
  await settle(500);
  return page.evaluate(async ([c, w]) => {
    const chip = document.querySelector(`.br-chip[data-cat="${c}"]`);
    if (!chip) return `no ${c} chip`;
    if (chip.disabled) return `${c} is unavailable`;
    chip.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    await new Promise((r) => setTimeout(r, 400));
    const rows = [...document.querySelectorAll('.br-item')].filter((el) => el.offsetParent !== null);
    const row = rows.find((el) => (el.textContent || '').toLowerCase().includes(w.toLowerCase()));
    if (!row) return `no "${w}" in ${c}`;
    row.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    return true;
  }, [cat, want]);
};

console.log('\nwalking docs/DEMO.md end to end\n');

await run('new demowalk');
await settle(1400);

// ===========================================================================
step('1. the tracker — click the grid, edit mode, type');
// ===========================================================================
await run('view tracker');
await settle(400);
/*
 * CLICKED, not `goto`. The runbook says this and it is the single most common way to make the
 * demo look broken: `goto` moves the cursor and does NOT hand the grid the keyboard, so the
 * next note key goes nowhere with nothing said.
 */
const grid = await page.$('#tracker') || await page.$('.tk');
const box = grid ? await grid.boundingBox() : null;
check(!!box, 'the tracker grid is on screen to click');
if (box) await page.mouse.click(box.x + 60, box.y + Math.min(60, box.height / 2));
await settle(300);

const st1 = await page.evaluate(() => window.__uni.state());
check(st1.focus === 'centre', 'clicking it gives the grid the keyboard', st1.focus);
if (!st1.editMode) { await page.keyboard.press('Meta+e'); await settle(300); }
check((await page.evaluate(() => window.__uni.state().editMode)) === true,
      'edit mode is on, so the note keys write');

await run('goto 4 0');
await settle(200);
await page.keyboard.press('z');
await settle(600);
check(((await engine()).noteCount || 0) > 0, 'typing on the piano row writes a note',
      `${(await engine()).noteCount} note(s)`);

// ===========================================================================
step('2. one song, three views');
// ===========================================================================
const seen = {};
for (const v of ['arrange', 'piano', 'tracker']) {
  await run(`view ${v}`);
  await settle(500);
  seen[v] = await page.evaluate(() => (window.__uni.notes() || []).map((n) => n.id).join(','));
}
check(seen.arrange === seen.piano && seen.piano === seen.tracker && seen.tracker !== '',
      'the arrangement, the roll and the tracker show the same note ids',
      JSON.stringify(seen));

// ===========================================================================
step('3. the sampler — DEVICES then SAMPLES, then play');
// ===========================================================================
check(await pick('devs', 'sampler') === true, 'DEVICES puts a sampler on the track');
await settle(1500);
check(await pick('smpl', 'waveform_probe') === true, 'SAMPLES puts a file in it');
await settle(2500);

const kit = await page.waitForFunction(() => {
  const t = window.__uni.state().cursor.track;
  for (let d = 0; d < 6; d++) {
    const k = window.__uni.samplerKitCached(t, d);
    if (k && k.slots && k.slots.length) return k;
  }
  return null;
}, null, { timeout: 25000 }).then((h) => h.jsonValue()).catch(() => null);
check(!!kit, 'the kit comes back with a slot in it');
/*
 * THE RUNBOOK'S CLAIM, which was false until today: a sample loaded this way is playable
 * without remapping. Asserted on the KEY RANGE rather than on audio — the render belongs to
 * sampler-default.mjs, and repeating it here would double the cost of this walk for a fact
 * that is already pinned.
 */
if (kit) {
  const s = kit.slots[0];
  check(s.keyLow <= 48 && s.keyHigh >= 48,
        'and it answers the keys the tracker types — no remapping needed',
        `keyLow ${s.keyLow}, keyHigh ${s.keyHigh}`);
}

// ===========================================================================
step('4. chords and strums, read back in the CELL panel');
// ===========================================================================
await run('view tracker');
await run('goto 8 0');
await settle(300);
await run('chord 4 seventh 1 4 240000 20 15');
await settle(1200);
check(((await engine()).chordCount || 0) > 0, 'a strummed seventh is written',
      `${(await engine()).chordCount} chord(s)`);

await run('goto 8 0');
await settle(600);
const insp = await page.evaluate(() => window.__uni.inspect());
const strumLine = (insp.rows || []).find((r) => r.label === 'strum');
check(!!strumLine && /^yes/.test(String(strumLine.value)),
      'and the CELL panel says it IS a strum, in words',
      JSON.stringify(strumLine));

// ===========================================================================
step('5. the harmony lane');
// ===========================================================================
await run('harmony 0 major 0');
await settle(1000);
check(((await page.evaluate(() => window.__uni.harmony() || [])).length) > 0,
      'a key change lands on the harmony timeline');
await run('harmony-quantize 0 on');
await settle(800);
const hq = await page.evaluate(() => window.__uni.harmonyQuantized());
check(Array.isArray(hq) && hq[0] === 1, 'and the track quantizes to it',
      JSON.stringify(hq));

// ===========================================================================
step('6. the patcher — add it, then OPEN it before editing');
// ===========================================================================
await run('add-track');
await settle(700);
await run('goto 0 1');
await settle(300);
check(await pick('devs', 'patcher event') === true, 'DEVICES puts a patcher on a new track');
await settle(1400);

/*
 * DOUBLE-CLICK THE CARD FIRST. This is the step the runbook calls out, and skipping it is not
 * cosmetic: without a device open, every patcher edit is pool-scoped and the device saves with
 * no nodes at all, while the published graph looks perfectly correct.
 */
const opened = await page.evaluate(() => {
  const cards = [...document.querySelectorAll('.dv-card')].filter((el) => el.style.display !== 'none');
  const card = cards.find((el) => /patcher/i.test(el.textContent || ''));
  if (!card) return null;
  card.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
  return card._devId;
});
await settle(800);
const target = await page.evaluate(() => window.__uni.patchTarget());
check(opened !== null && target.device === opened && target.track === 1,
      'double-clicking it addresses THAT device — edits will be saved',
      `${JSON.stringify(target)} vs device ${opened}`);

await run('addnode euclidean');
await settle(600);
await run('addnode out');
await settle(600);
const ids = await page.evaluate(() => (window.__uni.nodes() || []).map((n) => n.id));
check(ids.length >= 2, 'two nodes go into the graph', JSON.stringify(ids));

// ===========================================================================
step('7. it survives a save and a reload — the demo is not a one-way trip');
// ===========================================================================
await run('save demowalk');
await settle(1800);
const before = await engine();
await run('load demowalk');
await settle(2500);
const after = await engine();
check(after.noteCount === before.noteCount && after.chordCount === before.chordCount,
      'the notes and chords come back',
      `notes ${before.noteCount}->${after.noteCount}, chords ${before.chordCount}->${after.chordCount}`);

check(errors.length === 0, 'nothing threw in the browser during the whole walk',
      errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
