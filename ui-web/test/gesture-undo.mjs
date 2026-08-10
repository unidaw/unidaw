/*
 * A KNOB DRAG IS ONE UNDO STEP.
 *
 * A drag sends one SetDeviceParam per pointer sample, so undo replayed it sample by sample:
 * twenty moves, twenty Ctrl-Z presses to get back where you started. The engine half landed on
 * main (gestureFlagsApplyTo, bits 14/15, SetDeviceParam only) and was inert until a UI said so.
 * This drives the UI half through the actual mouse path.
 *
 * WHAT IT MEASURES, and why not the parameter value. The obvious check is "press undo once and see
 * the value come back", but reading a plugin parameter's value back out of the page needs uid and
 * index plumbing the chain probe does not expose. The engine already publishes the thing itself:
 *
 *     DAW_EVENT("undo.version_recorded").field("versions", documentHistory->size())
 *
 * with a comment saying a check can read it and assert the count rises once per mutating command.
 * So the question "is this drag one undo step" is answered by how far that count moves.
 *
 * OBSERVED ON THE WIRE, not by patching a function. The first version of this wrapped
 * window.__uni.send to watch the flags go by; onChainParam calls conn.send DIRECTLY, so it
 * intercepted nothing, reported "0 sends", and three checks passed vacuously on an empty array —
 * [].every() is true. Playwright's websocket frames are what actually left the page, and no
 * refactor of the send path can quietly blind them.
 *
 * THE CONTROL sends the same parameter the same number of times WITHOUT the flags, through
 * __uni.send with the uid and index read off the rack's own row. If the control ever stops needing
 * several versions, this suite has stopped measuring anything.
 */
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { startStack } from './stack.mjs';
import { chromium } from 'playwright';

const stack = await startStack({ numBlocks: 8, keepDir: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const ENGINE_LOG = join(stack.root, 'engine.log');

let pass = 0;
const fails = [];
const check = (ok, what, detail = '') => {
  if (ok) { pass += 1; console.log(`  PASS  ${what}`); }
  else { fails.push(what); console.log(`  FAIL  ${what}${detail ? ` — ${detail}` : ''}`); }
};

/** The engine's own history size, from the last version it recorded. */
const versions = () => {
  const log = existsSync(ENGINE_LOG) ? readFileSync(ENGINE_LOG, 'utf8') : '';
  const all = [...log.matchAll(/"event":"undo\.version_recorded"[^\n]*?"versions":(\d+)/g)];
  return all.length ? Number(all[all.length - 1][1]) : 0;
};

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1500, height: 900 } });
/** Every setparam that actually left the page, with whatever gesture it carried. */
const wire = [];
page.on('websocket', (ws) => ws.on('framesent', (f) => {
  const t = typeof f.payload === 'string' ? f.payload : '';
  if (t.includes('"setparam"')) {
    const g = t.match(/"gesture"\s*:\s*"([a-z]+)"/);
    wire.push(g ? g[1] : '-');
  }
}));
const errors = [];
page.on('pageerror', (e) => errors.push(String(e).slice(0, 160)));
await page.goto(stack.url);
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await sleep(1500);

// The rack has to be showing a device with parameters before there is a bar to drag.
const probe = await page.evaluate(() => window.__uni.chainProbe());
check(!!probe && probe.cards > 0, 'the rack shows a device', JSON.stringify(probe).slice(0, 140));
await page.waitForFunction(() => {
  const c = window.__uni.chainProbe();
  return !!c && (c.params || []).some((n) => n > 0);
}, null, { timeout: 20000 }).catch(() => {});
const withParams = await page.evaluate(() => (window.__uni.chainProbe().params || []).some((n) => n > 0));
check(withParams, 'and one of its devices published parameters, so there is a bar to drag');

/** The rack's own row knows which parameter the bar drags — index and uid both live on it. */
const target = await page.evaluate(() => {
  const bar = document.querySelector('.dv-p-bar');
  if (!bar) return null;
  let row = bar.parentElement, card = bar;
  while (row && !(row._pi >= 0)) row = row.parentElement;
  while (card && !card._devId) card = card.parentElement;
  return row && card ? { index: row._pi, uid: row._uid, device: card._devId } : null;
});
check(!!target && !!target.uid, 'the rack row names the parameter its bar drags',
      JSON.stringify(target));

const drag = async () => {
  await page.evaluate(async () => {
    const bar = document.querySelector('.dv-p-bar');
    const r = bar.getBoundingClientRect();
    const y = r.top + r.height / 2;
    const at = (f) => r.left + r.width * f;
    const ev = (type, x) => bar.dispatchEvent(new PointerEvent(type,
      { clientX: x, clientY: y, bubbles: true, pointerId: 1, isPrimary: true }));
    ev('pointerdown', at(0.15));
    for (const f of [0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85]) {
      ev('pointermove', at(f));
      await new Promise((r2) => setTimeout(r2, 45));
    }
    ev('pointerup', at(0.85));
  });
  await sleep(1400);
};

if (target && target.uid) {
  console.log('\n[the control: the same parameter, the same number of writes, NO gesture]');
  const v0 = versions();
  await page.evaluate(async (t) => {
    for (const v of [150, 250, 350, 450, 550, 650, 750, 850]) {
      window.__uni.send({ type: 'setparam', track: 0, device: t.device,
                          index: t.index, uid: t.uid, valueMilli: v });
      await new Promise((r) => setTimeout(r, 45));
    }
  }, target);
  await sleep(1400);
  const plainDelta = versions() - v0;
  check(plainDelta > 1,
        'WITHOUT the flags the engine records MORE THAN ONE version — so the check below can fail',
        `${plainDelta} versions for 8 writes`);

  console.log('\n[the same parameter, dragged with the pointer]');
  wire.length = 0;
  const v1 = versions();
  await drag();
  const gestureDelta = versions() - v1;
  check(wire.length >= 4, 'the drag put several setparam frames on the wire',
        `${wire.length}: ${wire.join(',')}`);
  check(wire[0] === 'begin' && wire[wire.length - 1] === 'end',
        'it opens with BEGIN and closes with END', wire.join(','));
  check(wire.length > 2 && wire.slice(1, -1).every((g) => g === '-'),
        'and the moves between carry neither bit — one gesture, not one per sample',
        wire.join(','));
  check(gestureDelta < plainDelta,
        'THE DRAG RECORDS FEWER UNDO STEPS THAN THE SAME WRITES WITHOUT THE FLAGS',
        `${gestureDelta} versions vs ${plainDelta}`);
  check(gestureDelta <= 1,
        'and it is ONE step — the whole drag collapses to a single undo',
        `${gestureDelta} versions`);
}

check(errors.length === 0, 'no page errors', errors.join(' | '));
console.log(`\n${fails.length ? `FAILURES (${pass + fails.length} checks, ${fails.length} failed)`
                               : `ALL PASS (${pass} checks)`}`);
await browser.close();
await stack.stop();
process.exit(fails.length ? 1 : 0);
