/**
 * MODULATION — what moves what, on both surfaces, and PROVED BY EAR.
 *
 * The last check in this file is the only one that matters and the rest exist to make it
 * interpretable: turning a macro knob changes the sound. Everything before it asserts that
 * the link was made, named, published and drawn, and every one of those can be true while
 * the audio never changes — because a modulation link the engine ACCEPTS can still move
 * nothing, three separate ways, all of them silent:
 *
 *   1. No parameter named. The block-rate applier builds its ParamPayload from
 *      `link.target.uid16` and NEVER reads `targetId`, so a link without a uid16 is
 *      accepted, published, drawable and inert.
 *   2. No source value. The applier resolves a source by looking it up in the track's
 *      source states, and a macro nobody has turned is not in that table — the link is
 *      skipped.
 *   3. A source that is not STRICTLY EARLIER than its target. The command validator
 *      refuses only a LATER source; the applier skips a source at the same position. So
 *      the engine accepts same-device links and then never applies them.
 *
 * Three ways to build a lit badge over silence. Which is why this suite ends with a
 * capture and an RMS comparison rather than with a green DOM assertion — "reasoning about
 * a signal path is not evidence", as audible.mjs puts it.
 *
 * THE FIXTURE IS WRITTEN HERE, and it has to be, for two reasons that only showed up when
 * the capture came back silent.
 *
 * `rack.uniproj.json` looked perfect — a patcher at position 0 and Identity at position 1,
 * which is exactly the shape this needs — and its Identity has an EMPTY PATH. So the engine
 * reported `project.plugin_missing` and put whatever the plugin cache offered first into that
 * slot; on this machine, an Analog Heat with 256 parameters. Every check above the capture
 * passed against it, because a link to parameter 0 of SOMETHING is still a link. The capture
 * was silence, and it read as "modulation does nothing".
 *
 * So the project is written with a REAL PATH to our own Identity build. Its only parameter is
 * Gain and it emits a ten-sample click per note-on at that gain, which makes the measurement
 * unambiguous in a way no third-party instrument would be: macro 1 is full scale, macro 0 is
 * digital silence, and there is no timbre judgement anywhere in it.
 *
 * The patcher at position 0 is the modulation SOURCE, and it has to be there: a source must
 * sit strictly earlier in the chain than its target, so a one-device track has nothing that
 * can legally modulate it.
 */

import { chromium } from 'playwright';
import { startStack, soundGate } from './stack.mjs';
import { readWav, rmsBetween, summarise } from './wav.mjs';
import { existsSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const WAV = '/tmp/uni-mods-capture.wav';
/*
 * The capture holds the LAST `CAPTURE_SECONDS` of a run that lasts `RUN_SECONDS`, and the
 * engine EXITS when the run ends — so the run has to outlast the whole test, not just the
 * audible part. The first version set the run to 30s and the setup alone takes longer than
 * that: the engine was gone before the macro was touched, and both windows read as perfect
 * silence, which looks exactly like "modulation does nothing".
 */
/*
 * THE CAPTURE COVERS THE WHOLE RUN, which is why these two are equal.
 *
 * The capture is the LAST `captureSeconds` of a `runSeconds` run, so a short window at the
 * end of a long run holds whatever the engine was doing after the test finished — silence.
 * That is what the first two attempts measured: correct windows, into a part of the capture
 * the test had never reached. Equal values make `captureOffset` return plain seconds since
 * audio started, and there is no window to line up at all.
 *
 * Long enough for the setup, which is dominated by the project load and by twenty console
 * commands at 600ms each; the run ends when it ends and the test waits for the file.
 */
const CAPTURE_SECONDS = 60;
const RUN_SECONDS = 60;

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ capture: WAV, captureSeconds: CAPTURE_SECONDS,
                                 runSeconds: RUN_SECONDS, numBlocks: 8 });
const startedAt = Date.now();
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1000);

const mods = (t) => page.evaluate((x) => window.__uni.mods(x), t);
const chain = (t) => page.evaluate((x) => window.__uni.chains()[x] || null, t);
/**
 * Type a line and hand back EVERYTHING it printed, joined.
 *
 * All of it, not the last line: `mods` answers with one line per link and the dock logs
 * them separately, so taking the last `out:` returned the last LINK and a check looking for
 * a word anywhere in the answer failed against a perfectly correct list.
 */
const type = async (line) => {
  const log = await page.evaluate((c) => window.__uni.run(c), line);
  await page.waitForTimeout(600);
  // Everything after THIS command's own echo. `run` hands back the whole rolling log, and
  // taking the last `out:` returned the last LINE — which for `mods` is the last link, so a
  // check looking for a word anywhere in the answer failed against a correct list.
  const at = (log || []).lastIndexOf(`in: > ${line}`);
  const mine = at >= 0 ? log.slice(at + 1) : (log || []);
  return mine.filter((l) => String(l).startsWith('out:'))
             .map((l) => String(l).slice(5)).join('\n');
};
const loadAndWait = async (n) => {
  const b = await page.evaluate(() => (window.__uni.loadStatus() || {}).seq || 0);
  await page.evaluate((x) => window.__uni.loadProject(x), n);
  await page.waitForFunction((s) => {
    const l = window.__uni.loadStatus(); return l && l.seq > s && l.ok;
  }, b, { timeout: 15000 }).catch(() => {});
  await page.waitForTimeout(900);
};

// ---------------------------------------------------------------------------
// The project, written into the stack's own disposable project directory.
// ---------------------------------------------------------------------------
{
  const Q = 960000;
  const IDENTITY = resolve('build/identity_plugin_artefacts/RelWithDebInfo/VST3/Identity.vst3');
  check(existsSync(IDENTITY), 'our Identity build is where this expects it', IDENTITY);
  /*
   * SIXTEENTHS, not quarters. Identity emits TEN SAMPLES per note-on, so the RMS of a window
   * is a function of how many notes fall in it: at four notes a second a 3.5-second window
   * holds fourteen clicks and an RMS around 0.03, comfortably above the floor. At one note a
   * second it would be 0.015 and the "is it audible" check would be measuring the fixture's
   * sparseness rather than the modulation.
   */
  const notes = [];
  for (let i = 0; i < 240; i++) {
    notes.push({ nanotick: i * (Q / 4), duration: Q / 8, pitch: 60 + (i % 12),
                 velocity: 110, column: 0, note_id: i + 1 });
  }
  const doc = {
    schema_version: 4,
    meta: { name: 'modsound', created_utc: 0, modified_utc: 0 },
    timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
    nanoticks_per_quarter: Q,
    tempo_map: [{ nanotick: 0, bpm: 120.0 }],
    harmony_timeline: [],
    clips: [{ id: 1, name: 'clicks', length: Q * 240, lines_per_beat: 4, kind: 'symbolic',
              time_sig_numerator: 4, time_sig_denominator: 4, notes, chords: [] }],
    tracks: [{
      track_id: 0, name: 'Clicks', harmony_quantize: false, lines_per_beat: 4,
      mixer: { gain_db: 0.0, pan: 0.0, mute: false, solo: false },
      device_chain: [
        // The SOURCE, at position 0. A patcher device rather than a second plugin: it needs
        // to occupy a position and hold a macro, and nothing more.
        { device_id: 5, kind: 'patcher_event', capability_mask: 1, patcher_node_id: 0,
          host_slot_index: 4294967295, bypass: false, vst_ref: {} },
        // The TARGET, at position 1, with a path the engine can actually resolve.
        { device_id: 6, kind: 'vst_instrument', capability_mask: 5, patcher_node_id: 0,
          host_slot_index: 4294967294, bypass: false,
          vst_ref: { vendor: '', name: 'Identity', path: IDENTITY, uid16: '' } },
      ],
      mod_links: [],
      placements: [{ clip_id: 1, at: 0, length: Q * 240, notes: [], chords: [], mutes: [] }],
    }],
  };
  writeFileSync(join(stack.dir, 'modsound.uniproj.json'), JSON.stringify(doc));
}

await loadAndWait('modsound');
/*
 * WAIT FOR THE REAL INSTRUMENT, not merely for a card.
 *
 * The engine puts a stand-in on a track while it resolves what the project asked for, and
 * playing against the stand-in produces a capture of perfect silence — which reads as "the
 * app makes no sound". audible.mjs records this exact trap; the difference here is that our
 * instrument IS called Identity, so the name cannot be the test. The parameter COUNT is: the
 * stand-in publishes none and Identity publishes its Gain.
 */
await page.waitForFunction(() => {
  const c = window.__uni.chainProbe();
  return c && c.cards >= 2 && c.params && c.params[1] > 0;
}, null, { timeout: 45000 }).catch(() => {});
await page.evaluate(() => window.__uni.reqChain(0));
await page.waitForTimeout(900);

// ---------------------------------------------------------------------------
// THE RACK IS THE SHAPE THIS SUITE NEEDS. Asserted rather than assumed, because every
// later check reads "the device at position 1" and "the device at position 0" — and a
// preset that changed under this suite would make all of them pass against the wrong
// devices, which is the failure mode that looks most like success.
// ---------------------------------------------------------------------------
const c0 = await chain(0);
const devs = ((c0 && c0.devices) || []).slice().sort((a, b) => a.pos - b.pos);
check(devs.length >= 2, 'the fixture has two devices — a source and a target',
      JSON.stringify(devs.map((d) => `${d.id}@${d.pos}`)));
const srcDev = devs[0] ? devs[0].id : -1;
const dstDev = devs[1] ? devs[1].id : -1;

// The parameter to modulate: Identity's Gain, which is index 0 on the target device.
const gain = await page.evaluate((d) => {
  // Keyed on (track, device) — device ids are per track, so the device id alone is not a
  // key. `paramKey(0, d)` is `0 * 65536 + d`, spelled out here because the test cannot
  // import the page's module.
  const dp = window.__uni.deviceParams()[d] || {};
  const list = dp.params || [];
  const p = list.find((x) => x.index === 0);
  return p ? { index: p.index, uid: p.uid, name: p.name } : null;
}, dstDev);
check(gain && gain.uid, 'and the target publishes a parameter with a stable id',
      JSON.stringify(gain));
console.log(`  target dev${dstDev} param 0 is "${gain && gain.name}" · source dev${srcDev}`);

// ---------------------------------------------------------------------------
// A TRACK WITH NO MODULATION HAS NONE. The fixture ships no links, so this is the honest
// starting point rather than a claim about the loader — see the note in the commit about
// the engine publishing its mod registry BEFORE installing it, which makes a loaded
// project's modulation invisible until the first edit and is reported separately.
// ---------------------------------------------------------------------------
{
  const m = await mods(0);
  check((m && m.links || []).length === 0, 'the fixture starts with nothing modulated');
  // A macro turn does NOT make the engine publish: SetModSourceValue changes no links, and
  // only a link change calls emitModSnapshot. Asserted so the next person does not reach for
  // it as a way to force a publish, the way I did.
  await type(`macro 0 ${srcDev} 0.5`);
  await page.waitForTimeout(700);
  check(((await mods(0)) || { links: [] }).links.length === 0,
        'and turning a macro does not make the engine publish anything');
}

// ---------------------------------------------------------------------------
// MAP, BY POINTER — the badge on the parameter row.
//
// Driven with a real click, not `element.click()`: the handler is bound to pointerdown,
// and `element.click()` does not fire it. A test that used the latter would pass having
// never reached the code (GUIDELINES 2.15), which has happened here before with bypass.
// ---------------------------------------------------------------------------
{
  const before = ((await mods(0)) || { links: [] }).links.length;
  // The card for the TARGET device, and the row for Gain. By device id, because the cards
  // are ordered by chain position and the ids are not.
  const row = page.locator(`.dv-card[data-dev="${dstDev}"] .dv-p`).first();
  const badge = row.locator('.dv-p-map');
  const shown = await badge.count();
  check(shown === 1, 'the parameter row has a MAP badge', String(shown));
  const box = await badge.boundingBox();
  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await page.waitForTimeout(1500);

  const m = await mods(0);
  /*
   * BY UID, NOT BY COUNT.
   *
   * The first publish reveals the PRESET's link as well as the new one, so the count goes
   * from 0 to 2 and a delta assertion reads as a failure — or worse, a link identified as
   * "the first one with a uid16" is the preset's, and every assertion after it describes
   * the wrong link while passing. Named by what was asked for.
   */
  const made = (m && m.links || []).find((l) => l.uid16 === gain.uid);
  check(!!made, 'clicking MAP makes a link, and it NAMES the parameter',
        JSON.stringify((m && m.links || []).map((l) => ({ id: l.id, uid: l.uid16 }))));
  check((m && m.links.length) > before, 'the published set grew',
        `${before} -> ${m && m.links.length}`);
  if (made) {
    check(made.notForward === false && made.unnamed === false && made.orphan === false,
          'and none of the three ways a link can be inert applies to it',
          JSON.stringify({ notForward: made.notForward, unnamed: made.unnamed,
                           orphan: made.orphan }));
  }
  /*
   * ...and A LINK THAT CANNOT WORK IS REPORTED AS SUCH. Made deliberately, source and target
   * on the same device: the command validator accepts it (it refuses only a LATER source)
   * and the block-rate applier skips it (it requires strictly EARLIER). So the engine holds
   * a modulation it will never apply, and the only defence a person has is being told.
   */
  await page.evaluate((d) => window.__uni.send({ type: 'mod', op: 'add', track: 0,
      srcDevice: d, srcId: 1, dstDevice: d, dstParam: 0, depth: 1, bias: 0,
      source: 'macro', target: 'param', rate: 'block' }), dstDev);
  await page.waitForTimeout(1200);
  const m2 = await mods(0);
  const bad = (m2 && m2.links || []).find((l) => l.sourceDevice === l.targetDevice);
  check(!!bad, 'a same-device link is accepted by the engine',
        JSON.stringify((m2 && m2.links || []).map((l) => `${l.sourceDevice}->${l.targetDevice}`)));
  if (bad) {
    check(bad.notForward === true,
          'and is reported as NOT WORKING — its source is not before its target',
          JSON.stringify({ src: bad.sourceDevice, dst: bad.targetDevice }));
    const said = await type('mods 0');
    check(/NOT WORKING/.test(said), 'the console says so in words', said.slice(0, 140));
    // Removed, so it cannot confuse the depth and audible checks below.
    await type(`unmap 0 ${bad.id}`);
    await page.waitForTimeout(900);
  }
  // ...and the badge is LIT, from the DOM. The model holding a link and the badge drawing
  // it are different claims, and the second is the one a person sees.
  const lit = await badge.evaluate((n) => n.className);
  check(/\bon\b/.test(lit) && !/inert/.test(lit), 'the badge is lit, and not in the inert state', lit);
}

// ---------------------------------------------------------------------------
// The same thing from the CONSOLE, on a second parameter, so neither surface is the only
// way in. `map` takes a parameter INDEX and resolves the uid itself — nobody should have
// to type 32 hex characters to map a knob.
// ---------------------------------------------------------------------------
{
  /*
   * THE SAME PARAMETER, unmapped and mapped again. Identity publishes exactly ONE parameter,
   * so "map a second one" is not available in this fixture — and asking for parameter 1 is
   * correctly refused, which made a check that expected a new link fail for the right reason
   * and prove nothing. Round-tripping the one parameter tests both console paths instead.
   */
  const first = ((await mods(0)) || { links: [] }).links.find((l) => l.uid16 === gain.uid);
  const gone = await type(`unmap 0 ${first.id}`);
  await page.waitForTimeout(1200);
  check(!((await mods(0)) || { links: [] }).links.some((l) => l.id === first.id),
        `\`unmap\` really removes it — ${gone}`,
        JSON.stringify(((await mods(0)) || { links: [] }).links.map((l) => l.id)));
  const said = await type(`map 0 ${dstDev} 0`);
  await page.waitForTimeout(1400);
  const back = ((await mods(0)) || { links: [] }).links.find((l) => l.uid16 === gain.uid);
  check(!!back, `and \`map\` puts it back, named — ${said}`,
        JSON.stringify(((await mods(0)) || { links: [] }).links.map((l) => ({ id: l.id, uid: l.uid16 }))));
}

// ---------------------------------------------------------------------------
// REFUSALS, both by name.
// ---------------------------------------------------------------------------
{
  // A target at position 0 has nothing before it, and modulation flows forward. Refused
  // rather than sent, because the engine would ACCEPT it and then never apply it.
  const before = ((await mods(0)) || { links: [] }).links.length;
  const said = await type(`map 0 ${srcDev} 0`);
  await page.waitForTimeout(700);
  check(/before|forward/i.test(said),
        'mapping a parameter on the FIRST device is refused, and says why', said);
  check(((await mods(0)) || { links: [] }).links.length === before,
        'and no link was made — the refusal is not cosmetic');
}
{
  const said = await type(`map 0 ${dstDev} 9999`);
  check(/stable id|no such|not/i.test(said),
        'and so is a parameter the device does not publish', said);
}

// ---------------------------------------------------------------------------
// DEPTH — how far the source sweeps it. An add with the same id, which the engine treats
// as a replacement, so every field has to go back out or the link is re-sourced by a
// command that said "change the depth".
// ---------------------------------------------------------------------------
let workingLink = 0;
{
  const m = await mods(0);
  const l = (m && m.links || []).find((x) => x.uid16 === gain.uid);
  workingLink = l ? l.id : 0;
  check(workingLink > 0, 'the working link has an id to address');
  if (workingLink) {
    await type(`depth 0 ${workingLink} 0.25`);
    await page.waitForTimeout(1800);
    /*
     * FOUND BY UID, NOT BY THE OLD ID.
     *
     * `AddModLink` with an existing id is REFUSED (`kModErrLinkExists`), so a depth change
     * is a remove and an add — and the engine assigns the new link `max + 1`. Looking for
     * the old id would find nothing and report the depth as unchanged, which is what
     * happened when I assumed an add could replace.
     */
    const after = ((await mods(0)) || { links: [] }).links.find((x) => x.uid16 === gain.uid);
    check(after && Math.abs(after.depth - 0.25) < 0.001, 'depth sets the depth',
          after && String(after.depth));
    /*
     * ONE link, and it KEPT ITS ID. `SetModLinkDepth` (v28) updates in place, which it did not
     * used to: a depth change was a remove and an add, so the link came back with a new id and
     * NO uid16 — inert until something named it again — and the pair was not atomic, which put
     * a depth slider out of reach. Backend added the opcode on the strength of that report, so
     * the assertion is now the stronger one: same link, new depth.
     */
    check(after && after.id === workingLink, 'and it is the SAME link, updated in place',
          `${workingLink} -> ${after && after.id}`);
    const forParam = ((await mods(0)) || { links: [] }).links.filter((x) => x.uid16 === gain.uid);
    check(forParam.length === 1,
          'and there is exactly one link for it — remove-and-add, not add-on-top',
          JSON.stringify(forParam.map((x) => ({ id: x.id, depth: x.depth }))));
    check(after && after.sourceDevice === srcDev && after.targetDevice === dstDev,
          'while keeping its source and target — every field goes back out',
          after && JSON.stringify({ src: after.sourceDevice, dst: after.targetDevice }));
    check(after && after.uid16 === gain.uid,
          'and its parameter is named again — an unnamed link moves nothing',
          after && after.uid16);
    // Back to full range for the audible check below.
    await type(`depth 0 ${after ? after.id : workingLink} 1`);
    await page.waitForTimeout(1500);
    const full = ((await mods(0)) || { links: [] }).links.find((x) => x.uid16 === gain.uid);
    check(full && Math.abs(full.depth - 1) < 0.001, 'and back to full range',
          full && String(full.depth));
  }
}

// ---------------------------------------------------------------------------
// THE AUDIBLE PROOF.
//
// Gain at depth 1 from a macro: macro 1 is full scale and macro 0 is silence. Two windows
// of the same playing song, one at each, and the RMS between them has to differ by more
// than any interpretation of the material could account for.
//
// This is the check that cannot be satisfied by a link that is merely accepted. All three
// silent failure modes produce IDENTICAL audio in both windows.
// ---------------------------------------------------------------------------
await type(`macro 0 ${srcDev} 1`);
await type('play');
// Long enough for the notes to be sounding well inside the window, and for the parameter
// change to have been applied on a block boundary rather than at the edge of one.
await page.waitForTimeout(6000);
// `captureOffset` maps a wall-clock instant to a second inside the WAV, anchored on
// "Audio output started" in the engine's log. Doing that arithmetic by hand here is how the
// first version produced a window starting at -2.8 seconds.
const loudAt = stack.captureOffset(Date.now());
await type(`macro 0 ${srcDev} 0`);
await page.waitForTimeout(6000);
const quietAt = stack.captureOffset(Date.now());
await type('stop');
await page.waitForTimeout(1500);

check(errors.length === 0, 'and nothing threw while asking', errors.slice(0, 2).join(' | '));
await browser.close();

/*
 * The engine writes the capture when its own window closes, so this waits for the RUN to
 * end rather than killing it: a SIGTERM before the window closes leaves no file at all, and
 * "no capture" and "a silent capture" are different failures worth telling apart.
 */
// Wait for the RUN to end, not a fixed pause: the engine writes the file when its own
// window closes, and a SIGTERM before that leaves no file — "no capture" and "a silent
// capture" are different failures and both deserve to be reachable.
{
  const left = RUN_SECONDS * 1000 - (Date.now() - startedAt) + 4000;
  if (left > 0) await new Promise((r) => setTimeout(r, left));
}
stack.stop();
await new Promise((r) => setTimeout(r, 1500));

/* See stack.mjs's soundGate. This suite read a 39-hour-old capture and reported ALL PASS
   off it until the stack started deleting the file — a green on evidence from another run. */
const { soundCheck, banner } = soundGate(stack, check);
if (!stack.audioRunning || !existsSync(WAV)) {
  soundCheck(false, 'the engine wrote a capture to measure', WAV);
} else {
  const { rate, mono } = readWav(WAV);
  const dur = mono.length / rate;
  /*
   * IS THERE ANY SOUND IN THE FILE AT ALL?
   *
   * First, because "the whole capture is silent" and "the windows point at the wrong part
   * of it" produce the same two failing RMS numbers, and they are entirely different
   * problems — one is the audio path, the other is arithmetic in this file. Both of the
   * previous attempts here were the second and I could not tell from the output.
   */
  const whole = summarise(mono, rate);
  check(whole.peak > 0.01, 'the capture contains sound at all',
        `peak ${whole.peak.toFixed(4)} over ${dur.toFixed(1)}s`);
  // The last four seconds before each reading, less half a second of slack at the boundary
  // so a block that straddles the parameter change lands in neither window.
  const loudFrom = loudAt - 4.0, loudTo = loudAt - 0.5;
  const quietFrom = quietAt - 4.0, quietTo = quietAt - 0.5;
  const loud = rmsBetween(mono, rate, Math.max(0, loudFrom), Math.max(0, loudTo));
  const quiet = rmsBetween(mono, rate, Math.max(0, quietFrom), Math.max(0, quietTo));
  console.log(`  capture ${dur.toFixed(1)}s  loud[${loudFrom.toFixed(1)}..${loudTo.toFixed(1)}]`
            + `=${loud.toFixed(5)}  quiet[${quietFrom.toFixed(1)}..${quietTo.toFixed(1)}]`
            + `=${quiet.toFixed(5)}`);
  check(loud > 0.002, 'the song is audible with the macro at full',
        `rms ${loud.toFixed(5)}`);
  /*
   * A FACTOR, not a difference. The absolute level depends on the instrument, the mix and
   * the device, and none of those is what this measures — the claim is that turning the
   * knob changed the gain, so the ratio is the assertion. Four times is far outside any
   * variation the material could produce between two windows of the same loop.
   */
  check(loud > quiet * 4, 'and turning the macro to zero SILENCES it — the link works',
        `loud ${loud.toFixed(5)} vs quiet ${quiet.toFixed(5)}`);
}

console.log(banner(fail, pass));
process.exit(fail === 0 ? 0 : 1);
