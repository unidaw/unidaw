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
import { join, resolve } from 'node:path';
import { copyFileSync, existsSync, writeFileSync, readFileSync } from 'node:fs';
import { startStack } from './stack.mjs';
import { readWav, envelope } from './wav.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const CAPTURE_SECONDS = 50;
const WAV = join(process.cwd(), 'ui-web/test/out/sampler-device-id.wav');

const stack = await startStack({ keepDir: true, capture: WAV, captureSeconds: CAPTURE_SECONDS,
                                 runSeconds: CAPTURE_SECONDS + 12,
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
                                 numBlocks: 16 });
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

const captureT0 = Date.now();
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
check(ids[0] === 0, 'the sampler on the empty chain got device id 0', JSON.stringify(ids));
check(ids[1] !== undefined && ids[1] !== 0, 'the sampler behind a device got a non-zero id',
      JSON.stringify(ids));
check(ids[2] !== undefined, 'the hosted track has a sampler too', JSON.stringify(ids));

const dev0 = ids[0], dev1 = ids[1], dev2 = ids[2];
await run(`load-sample 0 ${dev0} break.wav`);
await run(`load-sample 1 ${dev1} break.wav`);
await run(`load-sample 2 ${dev2} break.wav`);
await page.waitForTimeout(2500);

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
           bad0: bad(s0), bad1: bad(s1), bad2: bad(s2), root0: s0[0] && s0[0].rootKey };
}, [dev0, dev1, dev2]);
check(loaded.n0 > 0 && loaded.n1 > 0 && loaded.n2 > 0 &&
      loaded.bad0 === 0 && loaded.bad1 === 0 && loaded.bad2 === 0,
      'all three samplers hold a resolved slot', JSON.stringify(loaded));

/*
 * ONE NOTE EACH, FAR APART IN TIME, so the two answers occupy separate windows of one capture
 * and neither can be credited with the other's sound. Row 0 and row 32 at 4 lines per beat and
 * 120bpm is 0s and 4s.
 */
const KEY = loaded.root0 || 36;
// TWO SECONDS EACH (4 quarters at Q=960000), not the default row length. A 0.125-second note can
// fall between two 500ms polls, so a zero voice count would be luck rather than a reading.
const DUR = 3840000;
await run('zoom 1');
await run('goto 0 0');
await run(`note ${KEY} ${DUR}`);
await run('goto 32 1');
await run(`note ${KEY} ${DUR}`);
await run('goto 64 2');
await run(`note ${KEY} ${DUR}`);
await run('goto 96 3');
await run(`note 60 ${DUR}`);
await page.waitForTimeout(1200);

const placed = await page.evaluate(() => {
  const all = window.__uni.notes() || [];
  return all.map((n) => `t${n.tr}@${n.t}:${n.p}`).join(' ');
});
check(placed.split(' ').length === 4, 'one note on each track', placed);

const playedAt = (Date.now() - captureT0) / 1000;
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
const seen = { 0: 0, 1: 0, 2: 0 }, miss = { 0: 0, 1: 0, 2: 0 };
const devs = [dev0, dev1, dev2];
for (let i = 0; i < 16; i++) {
  const v = await page.evaluate(async (ds) => {
    /*
     * ASK RAW, BECAUSE THE CACHE CANNOT ANSWER THIS.
     *
     * `requestSamplerKit` holds an answer until `samplerKitVersion` moves, and that version is
     * bumped when the KIT CHANGES — never when a voice starts. So 72 polls through the normal
     * path produced FOUR engine publications, all from before playback, and the "0 voices" they
     * reported was a cached fact about a stopped transport. A reading that cannot change is not
     * a reading. `send` goes straight out, past the cache.
     */
    for (let t = 0; t < ds.length; t++) {
      window.__uni.send({ type: 'samplerkit', track: t, device: ds[t] });
    }
    await new Promise((r) => setTimeout(r, 120));
    const out = {};
    for (let t = 0; t < ds.length; t++) {
      const k = window.__uni.samplerKitCached(t, ds[t]);
      // `activeVoices` and `unmapped`, the names the read-back actually uses. `k.voices` reads
      // undefined and `|| 0` turns that into a confident zero — a reading that cannot fail is
      // not a reading. `unmapped` is the engine's own count of notes that hit no slot.
      out[t] = { v: (k && k.activeVoices) || 0, u: (k && k.unmapped) || 0 };
    }
    return out;
  }, devs);
  for (const t of [0, 1, 2]) {
    seen[t] = Math.max(seen[t], v[t].v); miss[t] = Math.max(miss[t], v[t].u);
  }
  await page.waitForTimeout(1000);
}
console.log(`  peak live voices: t0=${seen[0]} t1=${seen[1]} t2=${seen[2]}` +
            `   notes that hit no slot: t0=${miss[0]} t1=${miss[1]} t2=${miss[2]}`);
await page.evaluate(() => window.__uni.transport('stop'));
check(playedAt + 16 < CAPTURE_SECONDS, 'the playback happens inside the capture window',
      `played at ${playedAt.toFixed(1)}s of ${CAPTURE_SECONDS}s`);
await page.waitForTimeout(1000);
await browser.close();

for (let i = 0; i < 240 && !existsSync(WAV); i++) {
  await new Promise((r) => setTimeout(r, 250));
}
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
const worst = (engineLog.match(/\((\d+) total/g) || []).pop() || '';
check(underruns === 0, 'the producer kept up (no underruns)',
      `${underruns} underrun report(s) ${worst} — a starved producer outputs silence, so every ` +
      `sound check below is inconclusive rather than failing`);

if (!existsSync(WAV)) {
  check(false, 'the engine wrote a capture', WAV);
} else {
  const { rate, mono } = readWav(WAV);
  const env = envelope(mono, rate, 0.05);
  const per = 0.05;
  // Window A is the first track's note, window B the second's, located from when playback began.
  const at = (t) => Math.round((playedAt + t) / per);
  const peakBetween = (t0, t1) => {
    let m = 0;
    for (let i = Math.max(0, at(t0)); i < Math.min(env.length, at(t1)); i++) m = Math.max(m, env[i]);
    return m;
  };
  const first = peakBetween(0.2, 3.5);     // t0: sampler alone, device id 0
  const behind = peakBetween(4.2, 7.5);    // t1: sampler alone, device id non-0
  const hosted = peakBetween(8.2, 11.5);   // t2: sampler on a track that also has a VST
  const control = peakBetween(12.2, 15.5); // t3: a plain VST instrument, no sampler involved
  console.log(`  t0 sampler-only(dev ${dev0}) ${first.toFixed(4)}   ` +
              `t1 sampler-only(dev ${dev1}) ${behind.toFixed(4)}   ` +
              `t2 with a VST(dev ${dev2}) ${hosted.toFixed(4)}   ` +
              `t3 CONTROL (a VST instrument) ${control.toFixed(4)}`);
  check(control > 0.01, "this project's master carries audio at all (control track)",
        `peak ${control.toFixed(4)} — if this is 0 the readings above say nothing about the sampler`);
  check(hosted > 0.01, 'a sampler on a track that ALSO has a VST sounds',
        `peak ${hosted.toFixed(4)}`);
  /*
   * THE CLAIM. Both notes are the same sample at the same key through the same master bus, so
   * they must both sound. If only the one behind a filler device sounds, the device id is what
   * decided — which is the bug, stated as the check that fails while it is present.
   */
  check(behind > 0.01, 'a sampler with no VST on the track sounds (non-zero device id)',
        `peak ${behind.toFixed(4)} vs ${hosted.toFixed(4)} for the same sample on a hosted track`);
  check(first > 0.01, 'a sampler with no VST on the track sounds (device id 0)',
        `peak ${first.toFixed(4)} vs ${hosted.toFixed(4)} for the same sample on a hosted track`);
}

check(errors.length === 0, 'nothing threw', errors.join(' | '));
console.log(fail ? `\n${fail} of ${pass + fail} FAILED` : `\nALL PASS (${pass})`);
process.exit(fail ? 1 : 0);
