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
import { execFileSync } from 'node:child_process';

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
 * The slot key is `id`, not `slot_id` — `sampler_serialize.h:294` reads `get<uint32_t>("id")`.
 * The first version used `slot_id`, both slots silently defaulted to 0, and the card drew two
 * rows both labelled 0. That is the third fixture-key error of the night, all the same shape:
 * an unread key does not error, it defaults.
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
          { id: 1, source_local_id: 1, key_low: 36, key_high: 36, root_key: 36 },
          { id: 2, source_local_id: 1, key_low: 38, key_high: 40, root_key: 38 },
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

// ---------------------------------------------------------------------------
// AND THE RACK DRAWS IT. A sampler has no plugin parameters at all, so its card was a title
// over an empty body — the engine had a whole instrument in it and the rack said nothing.
//
// The slots ride the parameter rows: same virtualized ring, same name-and-value shape, no bar.
// A kit grid is a list of slots, and the rack already knows how to draw a list.
// ---------------------------------------------------------------------------
{
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.waitForTimeout(1200);
  const rows = await page.evaluate(() =>
    [...document.querySelectorAll('.dv-p')].filter((r) => r.style.display !== 'none').map((r) => ({
      name: r.querySelector('.dv-p-n') && r.querySelector('.dv-p-n').textContent,
      val: r.querySelector('.dv-p-v') && r.querySelector('.dv-p-v').textContent,
      title: r.title,
      map: !!r.querySelector('.dv-p-map') &&
           r.querySelector('.dv-p-map').style.display !== 'none',
    })));
  check(rows.length === 2, 'the card lists both slots', JSON.stringify(rows));
  const one = rows.find((r) => r.name && r.name.startsWith('1'));
  const zone = rows.find((r) => r.name && r.name.startsWith('2'));
  /*
   * THE KEY IS THE NAME, because that is how a person finds a pad — and a one-key slot names
   * its key while a zone names its span, which is the difference between a drum pad and a
   * sampled instrument and the one thing a kit grid must not blur.
   */
  // The app's own tracker notation — `C-2`, note then octave — not the `C1` spelling I assumed.
  // Asserted as a SHAPE rather than an exact octave, so this checks the naming and not the
  // middle-C convention, which is a different argument and not this suite's.
  check(one && /^1\s+[A-G]#?-?\d$/.test(one.name), 'a one-key slot is named by its key',
        one && JSON.stringify(one.name));
  check(zone && zone.name.includes('-'), 'and a zone by its span',
        zone && JSON.stringify(zone.name));
  check(one && /f$/.test(one.val || ''), 'the value is its length in FRAMES — the sample rate '
        + 'is not on this side of the wire, and a wrong duration is worse than an honest count',
        one && JSON.stringify(one.val));
  check(one && /frames/.test(one.title || ''), 'with the detail in the title', one && one.title);
  /*
   * AND NO MAP BADGE. Nothing modulates a slot, so offering one would put a badge over a link
   * that could never move anything — the same lie the rack already refuses for a plugin
   * parameter whose `automatable` is false.
   */
  check(rows.every((r) => !r.map), 'and no modulation badge — nothing modulates a slot',
        JSON.stringify(rows.map((r) => r.map)));
}

// ---------------------------------------------------------------------------
// A DRAWN KIT IS A SUBSCRIPTION, NOT A SNAPSHOT.
//
// The region carries a version now, bumped when a kit CHANGES rather than when one is
// REQUESTED. That distinction is the whole value: "did anyone ask recently" is not the question
// a drawn kit has, and a counter that ticked on request would make a polling reader re-fetch
// for ever while looking perfectly correct.
//
// Driven through daw-cli against THIS stack's segment, because nothing in the UI can mutate a
// sampler yet — the commands are recorded as gaps on purpose, since a sampler you can only
// configure by verb is one you can only verify by saving the file. So the mutation comes from
// outside and the UI's job is to notice, which is exactly the claim under test.
// ---------------------------------------------------------------------------
{
  const version = () => page.evaluate(() => window.__uni.engine().samplerKitVersion | 0);
  const before = await version();
  check(before > 0, 'the engine publishes a kit version — 0 would mean it does not', String(before));

  const slotCount = () => page.evaluate(() =>
    document.querySelectorAll('.dv-p:not([style*="display: none"])').length);
  check(await slotCount() === 2, 'the card is drawing two slots to begin with');

  let cli = null;
  try {
    cli = execFileSync('./ui/target/release/daw-cli',
      // `--slots` is the part that matters: it mints one playable SLOT per slice, which is
      // what the card lists. A chop with no slots changes the slice set and not the kit.
      ['do', 'sampler-slice', '--track', '0', '--source', '1',
       '--mode', 'equal', '--count', '4', '--slots'],
      { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm }, encoding: 'utf8' });
  } catch (e) { cli = 'FAILED: ' + String(e.stdout || e.message).slice(0, 160); }
  check(!/FAILED/.test(cli), 'daw-cli re-chops the sampler from outside the UI', cli.trim().slice(0, 120));

  if (!/FAILED/.test(cli)) {
    /*
     * NOBODY ASKED. The card is not re-requested by the test — `requestSamplerKit` notices the
     * version moved and re-fetches on its own, which is the difference between a subscription
     * and a snapshot and the only thing worth asserting here.
     */
    const moved = await page.waitForFunction(
      (was) => (window.__uni.engine().samplerKitVersion | 0) !== was, before,
      { timeout: 6000 }).then(() => true).catch(() => false);
    check(moved, 'the kit version moves when the kit changes', `was ${before}`);
    const grew = await page.waitForFunction(
      () => document.querySelectorAll('.dv-p:not([style*="display: none"])').length > 2,
      null, { timeout: 6000 }).then(() => true).catch(() => false);
    check(grew, 'and the card follows WITHOUT being asked — a slice per slot, drawn',
          `${await slotCount()} rows`);
  }
}

// ---------------------------------------------------------------------------
// THE MODULATOR FIELDS REACH THE WIRE.
//
// `modMask` reports what WOULD move — an envelope with no points and an LFO with zero swing
// both save and both do nothing, and neither sets a bit. `filterType` comes with it because the
// two are only useful together: a cutoff or resonance modulator on a filter that is OFF is
// silent, so a row drawing one without the other shows a live control over a dead one.
//
// WHAT THIS CHECKS AND WHAT IT DOES NOT. It asserts the two fields arrive, are numeric, and
// that a kit with no modulators claims none — the plumbing, end to end, through a real engine.
// It does NOT drive a modulator into existence: reaching a modulated slot through daw-cli means
// steering the engine's mod-set system by guesswork (`--mod-set`, `--modulator`, `--amp` and
// `--target` interact, and a chop mints slots whose mod set I did not choose), and a test built
// on a guessed invocation proves less than it appears to.
//
// The DECODING — live, inert, both at once, every target and kind, and the filter-off trap — is
// covered exhaustively in unit.mjs against `modSummary`, which is the part that is mine.
// ---------------------------------------------------------------------------
{
  const slots = await page.evaluate(() => {
    const k = window.__uni.samplerKitCached(0, 9);
    return k && k.slots ? k.slots.map((s) => ({ mm: s.modMask, ft: s.filterType })) : null;
  });
  check(Array.isArray(slots) && slots.length > 0, 'the kit has slots to ask about',
        JSON.stringify(slots));
  check(slots.every((s) => typeof s.mm === 'number'),
        'every slot publishes a modMask — undefined would mean the wire dropped it',
        JSON.stringify(slots.map((s) => s.mm)));
  check(slots.every((s) => typeof s.ft === 'number'),
        'and a filterType, without which a cutoff modulator cannot be judged',
        JSON.stringify(slots.map((s) => s.ft)));
  // This fixture configures no modulators, so every mask must be empty. A non-zero one here
  // would mean the field is being read from the wrong offset — the failure a numeric check
  // alone cannot see.
  check(slots.every((s) => s.mm === 0),
        'a kit with no modulators claims none — a stray bit would mean a bad offset',
        JSON.stringify(slots.map((s) => s.mm)));
  const marks = await page.evaluate(() =>
    [...document.querySelectorAll('.dv-p')].filter((r) => r.style.display !== 'none')
      .map((r) => r.querySelector('.dv-p-v') ? r.querySelector('.dv-p-v').textContent : ''));
  check(marks.every((m) => !/[~!]/.test(m)),
        'and no row draws a modulator mark it has no modulator for', JSON.stringify(marks));
}

check(errors.length === 0, 'and nothing threw', errors.slice(0, 3).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
