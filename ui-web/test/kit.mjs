/**
 * THE SAMPLER READ-BACK: ask what is in a sampler and get an answer.
 *
 * Wired before any sampler COMMAND, deliberately. A sampler you can only configure by console
 * verb is one you can only verify by saving the file and reading it back — so the read-back
 * comes first and the commands follow the surface rather than leading it.
 *
 * WHAT MAKES THIS WORTH TESTING is where the answer comes from. Backend's own note on the
 * region: it is published FROM THE SNAPSHOT THE AUDIO PRODUCER READS, not from the document.
 * Publishing the model would answer "what was configured" while the audio thread plays
 * something else, and catching exactly that divergence is the point of a read-back.
 *
 * `found: false` is an ANSWER — "there is no sampler on that device" — and a client that
 * treated it as silence would spin on a question already answered. So both are asserted.
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
const Q = 960000;
const WAV = resolve('presets/audio/waveform_probe.wav');

/*
 * A SAMPLER DEVICE, authored in the project file. `DeviceKind::Sampler = 5`.
 *
 * Hand-written because there is no other way in yet — `add-device --kind sampler` exists in
 * daw-cli, and daw-cli cannot talk to this stack's engine (it maps a different segment). The
 * kit's contents come from the device's own document, which is what the read-back then reports
 * from the RUNTIME, so the two agreeing is the assertion.
 */
writeFileSync(`${stack.dir}/kitfix.uniproj.json`, JSON.stringify({
  schema_version: 4, meta: { name: 'kitfix', created_utc: 0, modified_utc: 0 },
  timebase: { nanoticks_per_quarter: Q, time_sig_numerator: 4, time_sig_denominator: 4 },
  nanoticks_per_quarter: Q, tempo_map: [{ nanotick: 0, bpm: 120 }],
  harmony_timeline: [], clips: [],
  tracks: [{
    track_id: 0, name: 'Kit', harmony_quantize: false, lines_per_beat: 4,
    mixer: { gain_db: 0, pan: 0, mute: false, solo: false },
    device_chain: [{
      device_id: 9, kind: 'sampler', capability_mask: 5, patcher_node_id: 0,
      host_slot_index: 4294967295, bypass: false, vst_ref: {},
      sampler: {
        sources: [{ local_id: 1, path: WAV }],
        slots: [
          { slot_id: 1, source_local_id: 1, key_low: 36, key_high: 36, root_key: 36 },
          { slot_id: 2, source_local_id: 1, key_low: 38, key_high: 40, root_key: 38 },
        ],
      },
    }],
    mod_links: [], placements: [],
  }],
}));

const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 } });
const errors = [];
page.on('pageerror', (e) => { if (!errors.includes(e.message)) errors.push(e.message); });

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});
await page.waitForTimeout(1200);
await page.evaluate(() => window.__uni.loadProject('kitfix'));
await page.waitForTimeout(3000);

/** Ask, then wait for the answer to land in the cache. A receipt is not an outcome. */
const ask = async (track, device) => {
  await page.evaluate(([t, d]) => window.__uni.samplerKit(t, d), [track, device]);
  await page.waitForFunction(([t, d]) => !!window.__uni.samplerKitCached(t, d),
                             [track, device], { timeout: 4000 }).catch(() => {});
  return page.evaluate(([t, d]) => window.__uni.samplerKitCached(t, d), [track, device]);
};

// ---------------------------------------------------------------------------
// A DEVICE THAT IS NOT A SAMPLER answers "no", and that is an answer.
// ---------------------------------------------------------------------------
{
  const none = await ask(0, 77);
  check(none !== null, 'asking about a device that does not exist is ANSWERED, not left hanging',
        JSON.stringify(none));
  check(none && none.found === false, 'and the answer is "no sampler there"',
        none && String(none.found));
}

// ---------------------------------------------------------------------------
// THE SAMPLER, slot by slot.
// ---------------------------------------------------------------------------
{
  const kit = await ask(0, 9);
  check(kit !== null, 'the sampler answers', JSON.stringify(kit));
  check(kit && kit.found === true, 'and says there is one there', kit && String(kit.found));
  check(kit && Array.isArray(kit.slots) && kit.slots.length === 2,
        'with both slots the document declares',
        kit && `${kit.slots && kit.slots.length} slot(s)`);
  if (kit && kit.slots && kit.slots.length === 2) {
    const [a, b] = kit.slots.sort((x, y) => x.slot - y.slot);
    // The KEY RANGE, which is what makes a kit grid drawable at all — a slot on one key and a
    // slot spanning three are the difference between a drum pad and a zone.
    check(a.keyLow === 36 && a.keyHigh === 36, 'the one-key slot spans one key',
          `${a.keyLow}-${a.keyHigh}`);
    check(b.keyLow === 38 && b.keyHigh === 40, 'and the zone spans three',
          `${b.keyLow}-${b.keyHigh}`);
    /*
     * FRAMES FROM THE RUNTIME. `lengthFrames == 0` means the source did not resolve, so the
     * slot is SILENT — and that is a different problem from an empty sample, which is why the
     * region publishes a source-missing flag rather than leaving it to be inferred.
     */
    check(a.frames > 0, 'the slot resolved its source and has audio', String(a.frames));
    check((a.flags & 4) === 0, 'and is not flagged source-missing', String(a.flags));
  }
  check(kit && kit.voiceCap > 0, 'the voice pool is published',
        kit && `${kit.activeVoices}/${kit.voiceCap}`);
  check(kit && kit.truncated === 0, 'and nothing was truncated silently',
        kit && String(kit.truncated));
}

// ---------------------------------------------------------------------------
// THE CONSOLE says the same thing, because it calls the same function.
// ---------------------------------------------------------------------------
{
  const log = await page.evaluate(() => window.__uni.run('kit 0 9'));
  await page.waitForTimeout(1200);
  const said = (log || []).join(' | ');
  check(/asking the engine|sampler t0 dev9/.test(said),
        'the console verb reaches it', said.slice(-140));
  const after = await page.evaluate(() => window.__uni.run('kit 0 9'));
  const text = (after || []).join('\n');
  check(/slot|2 slot/.test(text), 'and prints the slots once the answer is in',
        text.slice(-200));
}

// ---------------------------------------------------------------------------
// AND THE RACK NAMES IT. A device whose kind has no entry in the client's table renders as
// "kind 5 #9" with a fallback badge — which reads as a device the app does not understand,
// rather than as a table one entry short. The sampler shipped exactly that way.
// ---------------------------------------------------------------------------
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.waitForTimeout(600);
  const card = await page.evaluate(() => {
    const t = document.querySelector('.dv-card .dv-title, .dv-title');
    const b = document.querySelector('.dv-card .dv-badge, .dv-badge');
    return { title: t && t.textContent, badge: b && b.textContent };
  });
  check(card.title && !/kind 5/.test(card.title),
        'the sampler card is named, not "kind 5"', JSON.stringify(card));
  check(card.badge === 'UNI',
        'and badged UNI — it runs IN the engine, not in a host process', JSON.stringify(card));
}

check(errors.length === 0, 'and nothing threw', errors.slice(0, 3).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
