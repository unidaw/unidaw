#!/usr/bin/env node
/**
 * DOES A SAMPLER'S DEVICE ID DECIDE WHETHER IT PLAYS?
 *
 * chop-audible.mjs found a sampler that loads a file, chops it into eight slots, resolves every
 * source, sits under two notes that fall inside its key ranges, and produces PERFECT SILENCE
 * while the transport runs inside the capture window. Zebralette on the same stack captures at
 * peak 0.0568, so the capture path is not the problem.
 *
 * Reading the engine gives a candidate cause:
 *
 *   apps/device_chain.cpp:24    nextDeviceId() returns 0 for an EMPTY chain
 *   apps/daw_engine_main.cpp:2273   uint32_t samplerDeviceId = 0;  // 0 = this track has no sampler
 *   apps/daw_engine_main.cpp:5599   rt.samplerDeviceId = found->id;
 *
 * A valid id and the "none" sentinel are the same value. So a sampler added to an empty chain
 * gets id 0, and every one of the ten `samplerDeviceId != 0` guards — the note tee at :14676 and
 * the render at :16181 among them — reads "this track has no sampler".
 *
 * That is a hypothesis from reading, and reading is not evidence. This is the experiment: the
 * SAME sampler, the SAME sample, the SAME note, twice — once as the first device on its track,
 * once behind a filler device that takes id 0 first. One variable. If only the second sounds,
 * the id is deciding, and nothing else can be blamed for it.
 */

import { chromium } from 'playwright';
import { execFileSync } from 'node:child_process';
import { join, resolve } from 'node:path';
import { copyFileSync, existsSync, unlinkSync, writeFileSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { startStack } from './stack.mjs';
import { readWav, envelope } from './wav.mjs';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

/*
 * MEASURED FROM AN OFFLINE RENDER, NOT FROM A CAPTURE OF THE AUDIO DEVICE.
 *
 * This file used to record 50 seconds off the device and read three windows out of it, and the
 * verdict it produced — "a sampler alone on an empty chain starts a voice and reaches no output"
 * — did not survive. Backend ran the same shape offline, including this file's exact duplicated
 * device-id numbering, and all three tracks sounded. So the silence was in the measurement.
 *
 * The window origin is why. A capture begins when the AUDIO DEVICE starts, not when the transport
 * does, and the offset lands almost entirely on the FIRST window — which is precisely the one
 * that read silent here. The same fault has produced a false silent-window reading three times
 * in this repo, and one of those suites PASSED with its fix reverted, which is the only reason
 * anyone found out the windows were meaningless.
 *
 * Anchoring to the control's first loud window was an attempt to fix that and is not enough: it
 * finds where the control BECAME AUDIBLE, which is the same thing as song zero only if the tap
 * was already recording. If it armed mid-note, every window slides by the difference.
 *
 * A render has no origin to get wrong. Sample zero is song zero, there is no device, and it is
 * byte-identical run to run — so a silent window means silence and nothing else. The structural
 * half of this test still needs the live stack (device ids, kit answers, voice counts, meters
 * are all live readings), so the stack stays and only the VERDICT moves offline.
 */
const RENDER_SECONDS = 18;

const stack = await startStack({ keepDir: true,
                                 /*
                                  * A DEEPER PIPELINE THAN audible.mjs USES.
                                  *
                                  * That test runs one track; this one runs four, three of them
                                  * with plugin hosts, alongside Chrome. At numBlocks 8 the
                                  * producer starved outright — 184 dropout callbacks per two
                                  * seconds, which at 86 callbacks a second is EVERY ONE — so the
                                  * device output silence and the capture faithfully recorded it.
                                  * Every reading in that run said "the sampler makes no sound"
                                  * about a machine that made no sound about anything.
                                  */
                                 numBlocks: 32 });
copyFileSync(resolve('presets/audio/waveform_probe.wav'), `${stack.dir}/break.wav`);

/*
 * TWO TRACKS, IDENTICAL BUT FOR THE CHAIN THEY START WITH.
 *
 * Both get a placed clip, because a track with no placement accepts `note` commands and keeps
 * nothing. Both get 0 dB and no mute. The only difference is that track 1 already holds a
 * device, so the sampler added to it cannot be id 0.
 */
const Q = 960000;
const clip = (id, name) => ({ id, name, length: Q * 240, lines_per_beat: 4, kind: 'symbolic',
                              time_sig_numerator: 4, time_sig_denominator: 4, notes: [], chords: [] });
const track = (id, name, chain) => ({
  track_id: id, name, harmony_quantize: false, lines_per_beat: 4,
  mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
  device_chain: chain, mod_links: [],
  placements: [{ clip_id: id + 1, at: 0, length: Q * 240, notes: [], chords: [], mutes: [] }],
});
writeFileSync(`${stack.dir}/devid.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'devid', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [],
  clips: [clip(1, 'a'), clip(2, 'b'), clip(3, 'c'), clip(4, 'd')],
  /*
   * THREE CHAINS, one variable each.
   *   t0  nothing        — the sampler will be device 0
   *   t1  a patcher      — the sampler cannot be device 0
   *   t2  a VST EFFECT   — the track has a plugin HOST, and so a segment
   *
   * The third is what separates "the id decides" from "the host decides": `segments` is built
   * only from VST devices (daw_engine_main.cpp:16109), and the sampler's audio is copied into
   * the host input plane only INSIDE the segment loop (:16398). A chain with no VST therefore
   * has nowhere to put it. Identity is the repo's own pass-through, so it colours nothing.
   */
  tracks: [track(0, 'First', []), track(1, 'Behind', []),
           track(2, 'Hosted', [{
             device_id: 0, kind: 'vst_effect', capability_mask: 5, patcher_node_id: 0,
             host_slot_index: 4294967294, bypass: false,
             /*
              * A REAL EFFECT, and deliberately not the repo's Identity.
              *
              * `host_controller.cpp:165` substitutes an in-process FAKE for any plugin whose
              * FILENAME is Identity.vst3, and its own comment says the fixture "has no audio
              * input" — "exactly how the master-FX path came to output silence for hours". A
              * pass-through that cannot pass anything through is not a control; it is the
              * silence being tested for, wearing the answer's clothes. kHs Gain is unity by
              * default and really opens.
              */
             vst_ref: { vendor: '', name: 'kHs Gain',
                        path: '/Library/Audio/Plug-Ins/VST3/Kilohearts/kHs Gain.vst3',
                        uid16: '' },
           }]),
           /*
            * THE CONTROL, INSIDE THIS PROJECT.
            *
            * audible.mjs proves the capture path works, but it proves it about a DIFFERENT
            * project. If this fixture's master were dead — a routing default, a gain, a mute —
            * every sampler reading above would be silent for a reason that has nothing to do
            * with the sampler. A plain VST instrument on a fourth track answers that here, in
            * the same run, through the same master, in its own window of the same capture.
            */
           track(3, 'Control', [{
             device_id: 0, kind: 'vst_instrument', capability_mask: 5, patcher_node_id: 0,
             host_slot_index: 4294967294, bypass: false,
             vst_ref: { vendor: '', name: 'Zebralette',
                        path: '/Library/Audio/Plug-Ins/VST3/Zebra2.vst3', uid16: '' },
           }])],
}));

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1500);
const run = (c) => page.evaluate((x) => window.__uni.run(x), c);

await page.evaluate(() => window.__uni.loadProject('devid'));
await page.waitForTimeout(2500);

/*
 * TRACK 1 GETS A FILLER FIRST, so its sampler cannot land on id 0. An AUDIO patcher, because
 * `addDevice` refuses a second instrument on a chain that already has one — and by its exact
 * kind name: the kinds are 'patcher event' / 'patcher audio' / 'VST instrument' / …, so a plain
 * 'patcher' was refused and both samplers came out id 0, which is an experiment with no variable
 * in it. The check below is what caught that; it is why the ids are read rather than assumed.
 */
const filled = await page.evaluate(() => window.__uni.addDevice(1, 'patcher audio'));
check(filled === true, 'a filler device was added to track 1', String(filled));
await page.waitForTimeout(1500);

await run('sampler 0');
await page.waitForTimeout(1200);
await run('sampler 1');
await page.waitForTimeout(1200);
await run('sampler 2');
await page.waitForTimeout(1500);

/*
 * THE IDS THE ENGINE ACTUALLY ASSIGNED, not the ones this test hopes for. If they do not come
 * out 0 and non-0 the experiment has no variable in it and everything below is noise.
 */
const ids = await page.evaluate(() => {
  // `chains()` is an object keyed by track, and a device's `kind` is the DeviceKind INDEX —
  // 5 is Sampler. Both cost a run: `for...of` on the object threw, and 'sampler' matched nothing.
  const out = {};
  for (const c of Object.values(window.__uni.chains())) {
    if (!c || !c.devices) continue;
    const s = c.devices.find((d) => d.kind === 5);
    if (s) out[c.track] = s.id;
  }
  return out;
});
/*
 * ZERO IS NOT A DEVICE ID. IT IS THE ABSENCE OF ONE — and that is now the assertion.
 *
 * This file was written to isolate the opposite: `nextDeviceId()` started at 0, so the first
 * device on an empty chain got id 0, which is what "there is no device" means everywhere else in
 * the engine. `TrackRuntime::samplerDeviceId` is documented "0 = this track has no sampler" and
 * guarded that way at nine sites, so a sampler that was the FIRST device on its track was never
 * sent a note. The wire overloads it the same way — deviceId 0 on a command means "the first
 * sampler on this track" — so it was unaddressable by every command too.
 *
 * Backend fixed it engine-side (nextDeviceId starts at 1). The check inverts rather than being
 * deleted: what was the reproducer is now the regression guard, and it fails again the day
 * anything starts handing out zero.
 */
check(ids[0] !== undefined && ids[0] !== 0,
      'a sampler on an EMPTY chain gets a real device id, not the no-device sentinel',
      JSON.stringify(ids));
check(ids[1] !== undefined && ids[1] !== 0, 'the sampler behind a device got a non-zero id',
      JSON.stringify(ids));
check(ids[2] !== undefined && ids[2] !== 0, 'the hosted track has a sampler too',
      JSON.stringify(ids));

const dev0 = ids[0], dev1 = ids[1], dev2 = ids[2];
await run(`load-sample 0 ${dev0} break.wav`);
await run(`load-sample 1 ${dev1} break.wav`);
await run(`load-sample 2 ${dev2} break.wav`);
await page.waitForTimeout(2500);
/*
 * ONE KIT DELIBERATELY UNLIKE THE OTHERS.
 *
 * The kit read-back is a request/answer pair matched on an echoed sequence number, and backend
 * found the CLI defaulting that sequence to a constant: every request matched the slot's existing
 * contents immediately, so each read returned the PREVIOUS question's answer — ask for track 1,
 * get track 0's kit. It survived because every fixture in both repos has ONE sampler track, and
 * with one track the previous answer and the current one are the same kit.
 *
 * This file has three, so it is the right place to hold the line. Slicing one of them means a
 * swapped answer is wrong in its CONTENT and not only in its label — a check that only reads the
 * `track` field would pass against an engine that echoed the label correctly and sent the wrong
 * slots.
 */
await run(`slice 2 ${dev2} 4`);
await page.waitForTimeout(2000);

// Both must be loaded and resolved, or a silence means "no sample" rather than "not played".
const loaded = await page.evaluate(async ([a, b, c]) => {
  const get = async (t, d) => {
    for (let i = 0; i < 40; i++) {
      window.__uni.samplerKit(t, d);
      const k = window.__uni.samplerKitCached(t, d);
      if (k && k.slots && k.slots.length) return k.slots;
      await new Promise((r) => setTimeout(r, 150));
    }
    return [];
  };
  const s0 = await get(0, a), s1 = await get(1, b), s2 = await get(2, c);
  const bad = (s) => s.filter((x) => (x.flags & 4) !== 0).length;
  return { n0: s0.length, n1: s1.length, n2: s2.length,
           /*
            * `root`, NOT `rootKey`. The wire spells it `root` (wire.js:385) and reading the
            * other name yielded `undefined` on every run, so KEY below silently fell back to 36
            * against a slot rooted at 60 — every note played two octaves down, which is quarter
            * speed, which turns this sample's one silent second into FOUR under a two-second
            * note. The track whose window happened to sit entirely inside that stretched silence
            * was read as "a sampler that starts a voice and reaches no output" and reported as an
            * engine bug. A misspelt field name with a plausible fallback is invisible: it never
            * throws, and the number it produces is a legal pitch.
            */
           bad0: bad(s0), bad1: bad(s1), bad2: bad(s2), root0: s0[0] && s0[0].root };
}, [dev0, dev1, dev2]);
check(loaded.n0 > 0 && loaded.n1 > 0 && loaded.n2 > 0 &&
      loaded.bad0 === 0 && loaded.bad1 === 0 && loaded.bad2 === 0,
      'all three samplers hold a resolved slot', JSON.stringify(loaded));

/*
 * EACH ANSWER IS ABOUT THE TRACK IT WAS ASKED ABOUT. Structural, with no timing in it: ask all
 * three in turn and require each answer to name its own track and device AND to carry that
 * track's own slot count. Backend's bug returned the previous answer, so a suite that asks about
 * one track can never see it; asking about three in a row is the whole test.
 */
const identity = await page.evaluate(async ([a, b, c]) => {
  const out = [];
  const devs = [a, b, c];
  for (let t = 0; t < 3; t++) {
    window.__uni.send({ type: 'samplerkit', track: t, device: devs[t] });
    await new Promise((r) => setTimeout(r, 350));
    const k = window.__uni.samplerKitCached(t, devs[t]);
    out.push(k ? { track: k.track, device: k.device, slots: (k.slots || []).length } : null);
  }
  return out;
}, [dev0, dev1, dev2]);
const askedDevs = [dev0, dev1, dev2];
check(identity.every((k, t) => k && k.track === t && k.device === askedDevs[t]),
      "each kit answer names the track and device it was asked about",
      JSON.stringify(identity));
check(identity[2] && identity[2].slots > identity[0].slots,
      'the sliced track reports MORE slots than the unsliced ones — the answers are not swapped',
      JSON.stringify(identity.map((k) => k && k.slots)));

/*
 * ONE NOTE EACH, FAR APART IN TIME, so the two answers occupy separate windows of one capture
 * and neither can be credited with the other's sound. Row 0 and row 32 at 4 lines per beat and
 * 120bpm is 0s and 4s.
 */
const KEY = loaded.root0 || 60;
check(KEY === 60, 'the notes are played AT the slot root, so the sample plays at its own rate',
      `KEY ${KEY} — anything but the root resamples, and a slow enough rate is indistinguishable ` +
      `from a sampler that never sounded`);
// TWO SECONDS EACH (4 quarters at Q=960000), not the default row length. A 0.125-second note can
// fall between two 500ms polls, so a zero voice count would be luck rather than a reading.
const DUR = 3840000;
await run('zoom 1');
/*
 * THE CONTROL PLAYS FIRST, AT SONG ZERO, AND IT IS THE CLOCK.
 *
 * The capture does not begin when the engine process does — it begins when the AUDIO DEVICE
 * does, several seconds later, after the plugin hosts have launched. So wall-clock time since
 * startStack says nothing about where a moment lands in the file, and every window this test used
 * to compute from it was in the wrong place. Twice that meant reporting silence about audio that
 * was in the capture, and once it meant playing entirely BEFORE the recording started.
 *
 * A note that is known to sound, at song position 0, fixes that: whatever the offset turns out to
 * be, the first loud window IS the downbeat, and everything else is measured from it. The control
 * earns its place twice — it proves the master carries audio, and it tells the test where zero is.
 */
await run('goto 0 3');
await run(`note 60 ${DUR}`);
await run('goto 32 0');
await run(`note ${KEY} ${DUR}`);
await run('goto 64 1');
await run(`note ${KEY} ${DUR}`);
await run('goto 96 2');
await run(`note ${KEY} ${DUR}`);
await page.waitForTimeout(1200);

const placed = await page.evaluate(() => {
  const all = window.__uni.notes() || [];
  return all.map((n) => `t${n.tr}@${n.t}:${n.p}`).join(' ');
});
check(placed.split(' ').length === 4, 'one note on each track', placed);

/*
 * REWIND FIRST. THE TRANSPORT STARTS WHERE THE CURSOR IS.
 *
 * The notes are written by moving the cursor to each row, so when the last one is written the
 * cursor is sitting on the LAST row of the song — and play from there skips every note before it.
 * The capture proved it: one note sounded, 0.25s after play, for exactly the 2 seconds of the
 * note that happened to be under the cursor, and the other three never played at all. Read as
 * "the sampler makes no sound" for hours.
 */
/*
 * PLAYED FOR THE LIVE READINGS ONLY — the voice counts and the device meters. The audio verdict
 * comes from the render at the bottom, so nothing here has to line up with a recording.
 */
await page.evaluate(() => {
  const b = [...document.querySelectorAll('.ch-btn')].find((e) => /stop/i.test(e.title || ''));
  if (b) b.click();                       // stop halts AND rewinds; `goto` only moves the cursor
});
await page.waitForTimeout(2500);
await page.evaluate(() => window.__uni.transport('play'));
/*
 * WATCH THE VOICES WHILE IT PLAYS.
 *
 * `note.emit` proves the producer emitted all three notes at the right ticks, and the capture
 * proves nothing came out. The voice count is the one reading that separates those two: a voice
 * that never STARTS means the note did not match a slot, and a voice that starts into silence
 * means the audio is not reaching the master. Polled during playback because the count is live —
 * every kit read taken before or after shows the zero it is supposed to show.
 */
/*
 * ONE REQUEST PER TRACK, TIMED INTO ITS OWN NOTE — and read the answer from the ENGINE'S LOG.
 *
 * The first version polled all three tracks every 500ms and read `activeVoices` back through the
 * UI. Both halves were wrong. The reading was wrong because `requestSamplerKit` holds its answer
 * until `samplerKitVersion` moves, and that version moves when the KIT CHANGES, never when a
 * voice starts — 72 polls produced four engine publications, all from before playback, so the
 * "0 voices" was a cached fact about a stopped transport. The LOAD was wrong because 48 raw
 * requests and the 101 kit publications they caused are themselves work on the machine whose
 * spare capacity is the thing being measured.
 *
 * So: three requests, each one second into the note it is asking about, and the truth taken from
 * `sampler.kit_published` in engine.log afterwards. The engine's own number, costing three
 * messages.
 */
const devs = askedDevs;
/*
 * THE DEVICE METERS, WHILE THE NOTES SOUND.
 *
 * The voice count says the sampler is running and the capture says nothing reaches the master;
 * the meters say WHERE it stops. The sampler renders into the host INPUT plane ahead of the
 * chain, so on the track that carries a real plugin, that plugin's `inPeak` is the exact
 * question — non-zero means the audio reached the chain and is lost after it, zero means it
 * never arrived. One reading tells those apart, and no amount of reasoning about the routing
 * does.
 */
/*
 * THE ENVELOPE IS WHAT MAKES A LOADED SAMPLE AUDIBLE AT ALL. This is the finding, and these
 * three lines are the difference between silence and sound:
 *
 *   without         t0 0.0000   t1 0.0000   t2 0.0000     (control 0.043, three voices running)
 *   with            t0 0.2784   t1 0.3998   t2 0.3998
 *
 * A sampler that has loaded a file and been sent a note STARTS A VOICE — the engine publishes
 * voices:1 while the note sounds — and renders silence, because the amp envelope a freshly
 * loaded slot gets by default produces no level. The plugin on the hosted track sees an input
 * peak of exactly 0 throughout, so it is the render and not the routing.
 *
 * That is the whole chop workflow: `sampler`, `load-sample`, `slice`, play — nothing in it sets
 * an envelope, so the kit is structurally perfect and mute. Reported; when the default becomes
 * audible these three lines can go and this file should still pass, which is how the fix will be
 * checked.
 *
 * Through daw-cli because there is no envelope verb on this side yet; if the answer is that a
 * UI must set one, that verb is the fix and it belongs here.
 */
/*
 * NO ENVELOPE IS SENT, deliberately. The three tracks are loaded and played as they come.
 *
 * A block here used to set one, because a freshly minted default put three of its four points at
 * t=0 and every slot was silent. Fixed engine-side; its absence is what checks the fix.
 */

const meterPeak = {};
const sampleMeters = async () => {
  const m = await page.evaluate(() => window.__uni.deviceMeters());
  for (const x of m || []) {
    const k = `t${x.track}d${x.device}`;
    const prev = meterPeak[k] || { in: 0, out: 0 };
    meterPeak[k] = { in: Math.max(prev.in, x.inPeak || 0),
                     out: Math.max(prev.out, x.outPeak || 0) };
  }
};
await page.waitForTimeout(5000);          // the control's note is at 0s; the samplers start at 4s
for (let t = 0; t < 3; t++) {
  await page.evaluate(([tr, d]) => window.__uni.send({ type: 'samplerkit', track: tr, device: d }),
                      [t, devs[t]]);
  for (let i = 0; i < 8; i++) { await sampleMeters(); await page.waitForTimeout(500); }
}
await page.waitForTimeout(2000);
console.log('  device meters while the notes sounded:',
            JSON.stringify(meterPeak));
await page.evaluate(() => window.__uni.transport('stop'));
await page.waitForTimeout(600);

/*
 * SAVE, AND RENDER WHAT WAS SAVED. Everything above was built by driving the UI, so the project
 * on disk is what the UI produced — the render is not a second fixture that could drift from it.
 */
await run(`save ${'devid'}`);
await page.waitForTimeout(2000);
await browser.close();

const RENDER = join(stack.dir, 'take.wav');
try { unlinkSync(RENDER); } catch { /* nothing to remove is the normal case */ }
let renderErr = '';
try {
  execFileSync(join(ROOT, 'build', 'daw_engine'),
               ['--project', 'devid', '--render', 'take', '--run-seconds', String(RENDER_SECONDS)],
               { cwd: join(ROOT, 'build'),
                 env: { ...process.env, DAW_PROJECT_DIR: stack.dir,
                        DAW_HOST_BINARY: join(ROOT, 'build', 'juce_host_process'),
                        DAW_UI_SHM_NAME: `/devid_${process.pid}` },
                 stdio: ['ignore', 'pipe', 'pipe'], timeout: 240000 });
} catch (e) { renderErr = String(e).slice(0, 200); }
check(!renderErr, 'the saved project renders offline', renderErr);

stack.stop();

/*
 * WAS THE MACHINE KEEPING UP? ASK BEFORE BELIEVING ANY SILENCE.
 *
 * A starved producer hands the device nothing and the device outputs zeros, so a capture of
 * silence is what BOTH a broken instrument and an overloaded laptop look like. The engine says
 * which, in as many words, and this run cost several hours by not reading it: "Engine: audio
 * underrun — 184 dropout callback(s) in the last ~2s". Until that line is absent, no claim below
 * about what the app played means anything at all.
 */
const engineLog = (() => { try { return readFileSync(join(stack.root, 'engine.log'), 'utf8'); }
                           catch { return ''; } })();
const underruns = (engineLog.match(/audio underrun/g) || []).length;
const conclusive = underruns === 0;
/*
 * WHAT THE UNDERRUN GATE STILL COVERS, NOW THAT THE VERDICT COMES FROM A RENDER.
 *
 * It used to gate the audio: a starved producer hands the device nothing, the device outputs
 * zeros, and a capture of that is indistinguishable from a broken instrument. An OFFLINE RENDER
 * has no device and no deadline — it cannot be starved — so gating the sound checks on it now
 * would suppress a real finding for an irrelevant reason. It did exactly that on the run that
 * found the pitch bug above: four BLOCKED lines over one underrun report, hiding the numbers
 * that turned out to be the answer.
 *
 * The LIVE readings are a different matter. Voice counts are published while the transport runs
 * on a real device, so a starved producer really can make them meaningless, and those stay
 * gated.
 *
 * And the underrun itself is REPORTED, not failed. With the audio measured offline it is a fact
 * about this machine — Chrome, four plugin hosts and a render at once — not about the product,
 * and failing a suite on it would be failing it for being run on a busy laptop.
 */
let blocked = 0;
const liveCheck = (ok, what, detail) => {
  if (conclusive) return check(ok, what, detail);
  blocked++;
  console.log('  BLOCK', what, '— the producer underran, so this LIVE reading cannot be trusted');
};
const soundCheck = check;

/*
 * THE ENGINE'S OWN VOICE COUNTS, taken from what it published while the notes were sounding.
 * `found` and `slots` are structure and were always right; `voices` is the one number that says
 * whether a note actually reached the instrument.
 */
const voicesByTrack = {};
for (const m of engineLog.matchAll(/"event":"sampler\.kit_published","track":(\d+),[^}]*"voices":(\d+)/g)) {
  const t = Number(m[1]), v = Number(m[2]);
  voicesByTrack[t] = Math.max(voicesByTrack[t] || 0, v);
}
console.log(`  voices the ENGINE published while the notes sounded: ` +
            `t0=${voicesByTrack[0] || 0} t1=${voicesByTrack[1] || 0} t2=${voicesByTrack[2] || 0}`);
/*
 * EVERY SAMPLER STARTS A VOICE, whatever its chain. This was 0 / 1 / 1 before the id fix — the
 * track whose sampler landed at id 0 was the one that never heard a note — and it is the reading
 * that proved the cause, because the three tracks are otherwise identical.
 */
liveCheck((voicesByTrack[1] || 0) > 0,
      'a sampler behind another device starts a voice', JSON.stringify(voicesByTrack));
liveCheck((voicesByTrack[0] || 0) > 0,
      'a sampler alone on an empty chain starts a voice too',
      `t0=${voicesByTrack[0] || 0} against t1=${voicesByTrack[1] || 0} on an identical track`);
const worst = (engineLog.match(/\((\d+) total/g) || []).pop() || '';
console.log(`  live playback: ${underruns} underrun report(s) ${worst}` +
            (underruns ? ' — the voice counts above are gated on this; the render below is not'
                       : ''));

if (!existsSync(RENDER)) {
  check(false, 'the engine wrote a render', RENDER);
} else {
  const { rate, mono } = readWav(RENDER);
  const per = 0.05;
  const env = envelope(mono, rate, per);

  /*
   * WINDOWS STRAIGHT FROM THE SONG. Sample zero IS song zero in a render, so these are the note
   * positions and nothing has to be located: control at 0s, then the samplers at 4s, 8s and 12s
   * (rows 32/64/96 at 4 lines per beat and 120bpm), each note two seconds long.
   *
   * THE SECOND HALF OF EACH NOTE, NOT THE FIRST. `waveform_probe.wav` is the peak-pyramid probe
   * asset and its first second is digital silence by construction, so a window over a sampler's
   * downbeat measures the file's silent opening and reads exactly like a sampler that did not
   * play. That mistake has already cost one wrong bug report this week.
   */
  const peakBetween = (t0, t1) => {
    let m = 0;
    const lo = Math.max(0, Math.round(t0 / per));
    const hi = Math.min(env.length, Math.round(t1 / per));
    for (let i = lo; i < hi; i++) m = Math.max(m, env[i]);
    return m;
  };
  let gpeak = 0;
  for (let i = 0; i < env.length; i++) if (env[i] > gpeak) gpeak = env[i];
  const FLOOR = 0.004;
  /*
   * A VOICE OUTLIVES ITS NOTE, MEASURED. Each note is two seconds and each voice renders the
   * whole four-second file: t0's note at 4s produces level until 8s, t1's at 8s until 12s. So
   * the windows below are deliberately inside the note rather than merely near it — a wider one
   * would catch the PREVIOUS track's tail and credit it to this one.
   *
   * The plateau levels differ between t0 and t1 at the same offset into the same file (0.278 vs
   * 0.341) and I have not established why. It does not affect the verdict, which is against a
   * floor, so it is written down as an observation and not as an explanation.
   */
  const control = peakBetween(0, 2.0);      // Zebralette, song 0s
  const first   = peakBetween(5.0, 6.2);    // t0: sampler alone, empty chain
  const behind  = peakBetween(9.0, 10.2);   // t1: sampler alone, behind a filler device
  const hosted  = peakBetween(13.0, 14.2);  // t2: sampler on a track that also has a VST
  console.log(`  render ${(mono.length / rate).toFixed(1)}s, peak ${gpeak.toFixed(4)}`);
  console.log(`  control(VST) ${control.toFixed(4)}   t0 sampler-only(dev ${dev0}) ` +
              `${first.toFixed(4)}   t1 sampler-only(dev ${dev1}) ${behind.toFixed(4)}   ` +
              `t2 with a VST(dev ${dev2}) ${hosted.toFixed(4)}`);

  soundCheck(control > FLOOR, "this project's master carries audio at all (control track)",
        `peak ${control.toFixed(4)} — if this fails, nothing below says anything about the sampler`);
  soundCheck(hosted > FLOOR, 'a sampler on a track that ALSO has a VST sounds',
        `peak ${hosted.toFixed(4)} against ${control.toFixed(4)} for the control`);
  soundCheck(behind > FLOOR, 'a sampler alone on its track sounds (non-zero device id)',
        `peak ${behind.toFixed(4)} against ${control.toFixed(4)} for the control — the sampler ` +
        `renders into the host input plane, which is only read inside the segment loop, and ` +
        `segments are built only from VST devices (daw_engine_main.cpp:16109)`);
  /*
   * THE ONE THAT WAS REPORTED AS A BUG AND WAS NOT ONE. Same track shape as `behind` but with the
   * sampler first on an empty chain; it read silent off the capture and sounds off the render.
   */
  soundCheck(first > FLOOR, 'a sampler alone on an EMPTY chain sounds',
        `peak ${first.toFixed(4)} against ${behind.toFixed(4)} for the identical track that ` +
        `has a filler device in front of the sampler`);
}

check(errors.length === 0, 'nothing threw', errors.join(' | '));
const note = blocked ? ` · ${blocked} BLOCKED (the producer underran, so those LIVE readings are unanswerable on this run)` : '';
console.log(fail ? `\n${fail} of ${pass + fail} FAILED${note}` : `\nALL PASS (${pass})${note}`);
process.exit(fail ? 1 : 0);
