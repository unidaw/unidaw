/**
 * WHAT A PARAMETER IS, not just where it is.
 *
 * Before v30 the rack drew every parameter as an anonymous 0..1 bar. Setting a filter to 2 kHz
 * meant dragging and reading the display string until it said 2 kHz — a binary search rather
 * than an interface — and a three-position switch was drawn as a smooth fader, which says the
 * values between the positions are reachable when they are not.
 *
 * The engine now publishes unit, default, endpoint TEXTS, step count, discreteness and
 * automatability. The endpoint texts are the load-bearing ones: a VST3 through JUCE normalises
 * every parameter to 0..1, so the numeric min and max say nothing at all and the only place the
 * real range exists is as the plugin's own rendering of its ends.
 *
 * THE NEGATIVE CONTROL IS THE POINT OF THIS FILE. A metadata field that is carried, decoded and
 * displayed but never ACTED ON is decoration. So the suite checks that a parameter the plugin
 * says it will ignore cannot be modulated — from the rack (no badge) and from the console (a
 * refusal), because the console can reach a row the rack is not drawing.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

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

/*
 * A REAL PLUGIN, because this suite is entirely about what a plugin says about itself. A stub
 * that answered from a table would pass every check here while the wire was broken — the exact
 * shape of test this project keeps writing down as worthless.
 */
const Q = 960000;
const IDENTITY = resolve('build/identity_plugin_artefacts/RelWithDebInfo/VST3/Identity.vst3');
writeFileSync(`${stack.dir}/pmeta.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'pmeta', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [], clips: [],
  tracks: [{
    track_id: 0, name: 'P', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
    device_chain: [
      /*
       * A SOURCE AT POSITION 0, so the ordering guard is not what refuses the link.
       *
       * Without it every `map` here is refused with "nothing before this device to modulate
       * from", and the check below would pass on the wrong refusal — a test that verifies
       * nothing, which is the trap this project keeps writing down. The parameter that IS
       * automatable must genuinely map for the one that is not to mean anything.
       */
      { device_id: 5, kind: 'patcher_event', capability_mask: 1, patcher_node_id: 0,
        host_slot_index: 4294967295, bypass: false, vst_ref: {} },
      { device_id: 6, kind: 'vst_instrument', capability_mask: 5, patcher_node_id: 0,
        host_slot_index: 4294967294, bypass: false,
        vst_ref: { vendor: '', name: 'Identity', path: IDENTITY, uid16: '' },
      }],
    mod_links: [], placements: [],
  }],
}));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1200);

const type = async (line) => {
  const log = await page.evaluate((c) => window.__uni.run(c), line);
  await page.waitForTimeout(500);
  const at = (log || []).lastIndexOf(`in: > ${line}`);
  const mine = at >= 0 ? log.slice(at + 1) : (log || []);
  /*
   * `err:` AS WELL AS `out:`. A command rejected by the dock's own argument schema prints on the
   * error channel, and reading only `out:` made a malformed command return the empty string —
   * which reads as "the app said nothing" rather than as "you called it wrong", and cost two
   * runs of this suite to see.
   */
  return mine.filter((l) => String(l).startsWith('out:') || String(l).startsWith('err:'))
             .map((l) => String(l).slice(5)).join('\n');
};

await page.evaluate(() => window.__uni.loadProject('pmeta'));
/*
 * THE PLUGIN'S OWN CARD, NOT CARD ZERO — and this was a 1-in-4 flake for as long as the suite
 * has existed.
 *
 * This waited for `c.params[0] > 0`: the parameter count of the FIRST device in the chain. The
 * fixture above deliberately puts a patcher at position 0 (see its comment — a source has to be
 * before the plugin or every `map` is refused for the wrong reason), so once the chain has
 * settled the shape is:
 *
 *     titles: ["patcher event #5", "Identity"]      params: [0, 2]
 *
 * `params[0]` is the PATCHER's count, which is correctly and permanently 0. The suite passed only
 * when it happened to sample an intermediate publish where Identity was the only card and
 * therefore index 0 — measured at 0.0s on every passing run, and never at all on the others.
 * Waiting longer could not help: 120s failed exactly as 45s did.
 *
 * So the check now finds the card by NAME and asks whether IT published parameters. Position is
 * an accident of the fixture; the plugin is the thing under test.
 */
const loaded = await page.waitForFunction(
  () => {
    const c = window.__uni.chainProbe();
    if (!c || !c.params || !c.titles) return false;
    const i = c.titles.findIndex((t) => String(t).includes('Identity'));
    return i >= 0 && c.params[i] > 0;
  },
  null, { timeout: 45000 }).then(() => true).catch(() => false);
/*
 * WHEN THIS TIMES OUT, SAY WHAT THE PAGE ACTUALLY HAD.
 *
 * This suite flakes, roughly one sweep in three, and the engine logs cannot explain it. The
 * differential (sweep 27, the failing run against its own retry, both kept by the sweep) shows
 * the engine doing everything right in BOTH: project.plugin_resolved for Identity,
 * chain.reconciled with plugins:1, "Host: reconciled chain to 1 plugin(s)". The one difference is
 * that the failing run received NO device.params_query at all — and that request comes from the
 * PAGE, when it renders the rack. So the question is what the page had, and nothing on disk
 * records it.
 *
 * A bare `check(loaded, ...)` throws that away: it reports "the plugin loads and publishes its
 * parameters" failed, which is the one thing already known. So on failure only, dump the probe —
 * whether the chain arrived at all, which cards it holds, and what parameter counts they carry.
 * That distinguishes "no chain reached the page", "the chain reached it without Identity" and
 * "Identity is there with zero parameters", which are three different bugs wearing one symptom.
 *
 * Costs nothing on the happy path and turns the next occurrence into evidence instead of another
 * round of this.
 */
if (!loaded) {
  const probe = await page.evaluate(() => {
    const c = window.__uni.chainProbe();
    return c ? { known: c.known, version: c.version, cards: c.cards, titles: c.titles,
                 params: c.params, notice: c.notice } : null;
  }).catch(() => null);
  console.log(`  PROBE on failure: ${JSON.stringify(probe)}`);
}
check(loaded, 'the plugin loads and publishes its parameters');
await page.waitForTimeout(1500);

/*
 * THE DEVICE'S ID, read from the chain rather than assumed to be its position. They are
 * different numbers — the console addresses a device by id, and a suite that passed the position
 * got refused by the ordering guard instead, which looks exactly like the refusal it was
 * testing for.
 */
const [plugin, params] = await page.evaluate(() => {
  const all = window.__uni.deviceParams();
  // The mirror is keyed `track:device` with the DEVICE ID, which is where the id is legible from
  // outside — `chainProbe` reports cards by position and never names them.
  const k = Object.keys(all).find((x) => all[x].params && all[x].params.length);
  // The key is `track * 65536 + device` (see paramKey), so the device is the low half.
  return k ? [Number(k) % 65536, all[k].params] : [0, []];
});
const gain = params.find((p) => p.name === 'Gain');
const mode = params.find((p) => p.name === 'Mode');

// ---------------------------------------------------------------------------
// THE WIRE carries what the plugin says, and does not invent any of it.
// ---------------------------------------------------------------------------
{
  check(!!gain && !!mode, 'both parameters arrive',
        params.map((p) => p.name).join(','));
  check(gain && gain.unit === 'dB', 'a unit comes through', gain && gain.unit);
  /*
   * THE ENDPOINT TEXTS, which are the whole reason this exists. Asserted as text, not parsed
   * into numbers — the plugin's rendering IS the answer, and a suite that parsed "-60.0 dB" into
   * -60 would be re-deriving the thing it is checking arrived.
   */
  check(gain && gain.minText === '-60.0 dB' && gain.maxText === '0.0 dB',
        'and the range as the PLUGIN renders it, which is the only place it exists',
        gain && `${gain.minText} .. ${gain.maxText}`);
  check(mode && mode.steps === 3, 'a switch says how many positions it has',
        mode && String(mode.steps));
  check(mode && mode.discrete === true, 'and that it is discrete', mode && String(mode.discrete));
  check(mode && mode.automatable === false,
        'and that the plugin will IGNORE host automation for it',
        mode && String(mode.automatable));
  check(gain && gain.automatable === true, 'while the one that accepts it says so');
}

// ---------------------------------------------------------------------------
// THE RACK SHOWS IT. Carried and decoded is not the same as visible.
// ---------------------------------------------------------------------------
const rows = await page.evaluate(() =>
  [...document.querySelectorAll('.dv-p')].filter((r) => r.style.display !== 'none').map((r) => ({
    name: r.querySelector('.dv-p-n') && r.querySelector('.dv-p-n').textContent,
    title: r.title,
    stepped: r.classList.contains('stepped'),
    steps: r.style.getPropertyValue('--steps'),
    mapShown: !!r.querySelector('.dv-p-map') &&
              r.querySelector('.dv-p-map').style.display !== 'none',
  })));
{
  const g = rows.find((r) => r.name === 'Gain');
  const m = rows.find((r) => r.name === 'Mode');
  check(!!g && !!m, 'the rack draws both rows', JSON.stringify(rows.map((r) => r.name)));
  check(g && g.title === '-60.0 dB .. 0.0 dB',
        'the continuous one names its real range instead of nothing', g && g.title);
  check(g && !g.stepped, 'and is not drawn as a switch');
  check(m && m.stepped, 'the three-position one IS drawn as a switch', m && m.title);
  check(m && m.steps === '3', 'with its positions marked', m && m.steps);
  check(m && /3 positions/.test(m.title) && /not automatable/.test(m.title),
        'and says both facts about itself', m && m.title);
}

// ---------------------------------------------------------------------------
// AND IT IS ACTED ON. Metadata that changes nothing is decoration.
// ---------------------------------------------------------------------------
{
  const g = rows.find((r) => r.name === 'Gain');
  const m = rows.find((r) => r.name === 'Mode');
  check(g && g.mapShown, 'the rack offers modulation on the parameter that accepts it');
  check(m && !m.mapShown,
        'and does NOT offer it on the one the plugin ignores — a badge over an inert link is a lie');

  /*
   * THE CONSOLE TOO, and this is not redundant with the badge: the console can address a row the
   * rack is not drawing, so hiding the badge alone would leave the inert link one word away.
   */
  await type('macro 0 0 0.5');
  const refused = await type(`map 0 ${plugin} ${mode ? mode.index : 1}`);
  check(/ignores host automation/i.test(refused),
        'and the console refuses the same link, with the reason', refused.slice(0, 120));
  /*
   * AND THE POSITIVE CONTROL, asserted as what it SHOULD say rather than as the absence of the
   * refusal. "Did not print the ignore message" is satisfied by every other failure there is —
   * including the ordering refusal that was firing here before the source device existed.
   */
  const allowed = await type(`map 0 ${plugin} ${gain ? gain.index : 0}`);
  check(/^mapped /.test(allowed),
        'while the parameter that accepts automation still maps', allowed.slice(0, 120));
}

check(errors.length === 0, 'and nothing threw', errors.slice(0, 2).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
