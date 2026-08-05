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
import { writeFileSync, copyFileSync } from 'node:fs';
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
// A MODULATOR DRIVEN INTO EXISTENCE — and the one the product cannot make live.
//
// This was deferred once: I could not get a non-zero modMask through daw-cli and said so rather
// than ship a green check that was really testing my ability to guess a CLI. Backend answered
// that the invocation did not exist — a sampler with no mod sets made `sampler-env` apply to
// nothing, the handler iterating an empty vector and logging `no_such_mod_set` to its own log
// while the CLI printed `{"sent": ...}`. Fixed there; testable here.
//
// AND THE PART WORTH THE WHOLE FILE: nothing in the engine writes `modSet.filterType`. Not one
// site — the only reference anywhere is the read at the kit publish point, which is the value
// this rack draws. So every cutoff and resonance modulator reachable from any surface is inert
// BY CONSTRUCTION, and the `!` badge is not showing an unlucky fixture. It is showing the only
// state the product can currently reach.
//
// Which is the argument for showing inert modulators rather than hiding them, twice over:
// hiding would make this surface agree with the bug, and the bug is that nothing can disagree.
// ---------------------------------------------------------------------------
{
  await page.evaluate(() => window.__uni.run('add-track'));
  await page.waitForTimeout(1200);
  const mt = await page.evaluate(() => window.__uni.state().tracks - 1);
  await page.evaluate((t) => window.__uni.run(`sampler ${t}`), mt);
  await page.waitForTimeout(1500);
  copyFileSync(resolve('presets/audio/waveform_probe.wav'), `${stack.dir}/mod.wav`);

  const cli = (args) => {
    try {
      return execFileSync('./ui/target/release/daw-cli', args,
        { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm }, encoding: 'utf8' }).trim();
    } catch (e) { return 'FAILED: ' + String(e.stdout || e.message).slice(0, 160); }
  };
  const load = cli(['do', 'sampler-load', '--track', String(mt), '--file', 'mod.wav',
                    '--root', '60']);
  check(!/FAILED/.test(load), 'a sample loads', load.slice(0, 100));

  // The AMP envelope — bit 0. Nothing to do with the filter, so it moves whatever the filter is.
  const amp = cli(['do', 'sampler-env', '--track', String(mt), '--attack', '100000',
                   '--decay', '200000', '--sustain', '500', '--release', '300000']);
  // The CUTOFF envelope — bit 6. Real, stored, round-tripping, and silent, because the filter
  // is off and no command in the product can turn it on.
  const cut = cli(['do', 'sampler-env', '--track', String(mt), '--target', 'cutoff',
                   '--attack', '100000', '--decay', '200000', '--sustain', '500',
                   '--release', '300000', '--depth', '900']);
  check(!/FAILED/.test(amp) && !/FAILED/.test(cut), 'both envelopes are accepted',
        (amp + ' | ' + cut).slice(0, 120));

  const slot = await page.evaluate(async (t) => {
    for (let i = 0; i < 50; i++) {
      window.__uni.samplerKit(t, 0);
      const k = window.__uni.samplerKitCached(t, 0);
      if (k && k.slots && k.slots.length && k.slots[0].modMask) return k.slots[0];
      await new Promise((r) => setTimeout(r, 150));
    }
    const k = window.__uni.samplerKitCached(t, 0);
    return k && k.slots ? k.slots[0] : null;
  }, mt);

  check(slot && (slot.modMask & 1) !== 0, 'the amp envelope sets its bit',
        slot && `mask ${slot.modMask}`);
  check(slot && (slot.modMask & (1 << 6)) !== 0, 'and so does the cutoff envelope',
        slot && `mask ${slot.modMask}`);
  check(slot && slot.filterType === 0,
        'while the filter is OFF — which is now a STATE rather than the only value reachable. '
        + 'It was the latter for a night: no site in the engine wrote filterType, so every '
        + 'cutoff modulator was inert by construction. Opcode 86 changed that; the check below '
        + 'turns it on.',
        slot && String(slot.filterType));

  /*
   * THE CURSOR HAS TO BE ON THAT TRACK. The rack draws the chain of `state.cursor.track`, so
   * reading `.dv-p` rows while the cursor sits elsewhere reads a DIFFERENT device's rows — which
   * is what happened first, and it reported the modulator marks missing from a card that was
   * never showing the modulated slot.
   */
  await page.evaluate((t) => window.__uni.run(`goto 0 ${t}`), mt);
  await page.waitForTimeout(1200);
  const row = await page.evaluate(() =>
    [...document.querySelectorAll('.dv-p')].filter((r) => r.style.display !== 'none')
      .map((r) => ({ v: r.querySelector('.dv-p-v') ? r.querySelector('.dv-p-v').textContent : '',
                     t: r.title || '' }))[0]);
  /*
   * `~!` AND NOT `~`, WHILE THE FILTER IS OFF. One modulator moves, one cannot, and the row says
   * both. Marking the cutoff envelope live here would make this surface agree with a state the
   * product can genuinely be in.
   */
  check(row && /~/.test(row.v) && /!/.test(row.v),
        'and the row shows BOTH — something moves, something cannot', row && JSON.stringify(row.v));
  check(row && /filter is off/.test(row.t),
        'naming the reason, because the reason is the fixable part', row && row.t);

  /*
   * NOW TURN THE FILTER ON — the half that could not be written until opcode 86 existed.
   *
   * This was an inverted gap check for most of a night: nothing in the engine wrote
   * `modSet.filterType`, so every cutoff modulator in the file format was inert BY CONSTRUCTION
   * and `!` was the only badge the product could reach. The command exists now, so the same
   * cutoff envelope becomes live and the `!` must go — which is a claim about the engine, the
   * sidecar, the console verb and the badge rule all at once, and the only reason to trust any
   * of them is that this fails when any one is wrong.
   */
  const set = await page.evaluate((t) => window.__uni.run(`filter ${t} 0 lp24 700 200`), mt);
  await page.waitForTimeout(1500);
  const lit = await page.evaluate(async (t) => {
    for (let i = 0; i < 40; i++) {
      window.__uni.send({ type: 'samplerkit', track: t, device: 0 });
      await new Promise((r) => setTimeout(r, 200));
      const k = window.__uni.samplerKitCached(t, 0);
      if (k && k.slots && k.slots.length && k.slots[0].filterType) return k.slots[0];
    }
    const k = window.__uni.samplerKitCached(t, 0);
    return k && k.slots ? k.slots[0] : null;
  }, mt);
  check(lit && lit.filterType === 2,
        'the console turns the filter ON, and the kit read-back says so',
        `${String(set).slice(-40)} / filterType ${lit && lit.filterType}`);

  await page.waitForTimeout(900);
  const row2 = await page.evaluate(() =>
    [...document.querySelectorAll('.dv-p')].filter((r) => r.style.display !== 'none')
      .map((r) => ({ v: r.querySelector('.dv-p-v') ? r.querySelector('.dv-p-v').textContent : '',
                     t: r.title || '' }))[0]);
  check(row2 && /~/.test(row2.v) && !/!/.test(row2.v),
        'and the row drops the inert mark — the cutoff envelope is live now',
        row2 && JSON.stringify(row2.v));

  /*
   * AND THE SAME THING WITH THE POINTER.
   *
   * "Console and UI should both have all the functionality" is a standing rule here, and a
   * console verb alone leaves the pointer unable to do something the keyboard can. The button
   * cycles, so pressing it from lp24 must land on the NEXT type and the read-back must agree —
   * which also checks that it names the state it wants rather than toggling, because a cycle
   * driven from stale state lands somewhere nobody asked for.
   */
  /*
   * THE VISIBLE one. The card pool is reused as the rack changes shape and every card carries a
   * filter button, hidden on anything that is not a sampler — so `querySelector` returns the
   * first card's, which is a hidden button still holding whatever text it last had. It reported
   * `{"text":"lp24","shown":false}`: the right label on the wrong element.
   */
  const btn = await page.evaluate(() => {
    const b = [...document.querySelectorAll('.dv-card .dv-flt')]
      .find((x) => x.style.display !== 'none');
    return b ? { text: b.textContent, shown: true } : null;
  });
  check(btn && btn.text === 'lp24',
        'the sampler card shows the filter it is set to', JSON.stringify(btn));

  /*
   * A REAL CLICK, not `element.click()`.
   *
   * The rack listens on POINTERDOWN — it has to, because the same handler starts a card drag —
   * and a synthetic `.click()` dispatches no pointer events at all. The button was drawn right,
   * wired right and did nothing, which is indistinguishable from a broken handler.
   *
   * Dispatched directly rather than through Playwright's click, which applies actionability
   * checks — visibility, stability, hit-testing — and timed out on a button inside a scrolling
   * rack. Those checks are worth having when the question is "can a person press this"; the
   * question here is "does pressing it do the right thing", and the event is the whole of it.
   */
  await page.evaluate(() => {
    const b = [...document.querySelectorAll('.dv-card .dv-flt')]
      .find((x) => x.style.display !== 'none');
    b.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true }));
  });
  await page.waitForTimeout(1500);
  const cycled = await page.evaluate(async (t) => {
    for (let i = 0; i < 40; i++) {
      window.__uni.send({ type: 'samplerkit', track: t, device: 0 });
      await new Promise((r) => setTimeout(r, 200));
      const k = window.__uni.samplerKitCached(t, 0);
      if (k && k.slots && k.slots.length && k.slots[0].filterType === 3) return k.slots[0];
    }
    const k = window.__uni.samplerKitCached(t, 0);
    return k && k.slots ? k.slots[0] : null;
  }, mt);
  check(cycled && cycled.filterType === 3,
        'clicking it cycles to the next type, and the engine agrees',
        `filterType ${cycled && cycled.filterType} (3 = hp, after lp24)`);
}

// ---------------------------------------------------------------------------
// THE WHOLE CHOP, FROM THE UI ALONE. No daw-cli anywhere in this block.
//
// This is the workflow the entire per-note-op design was drawn around — the amen break,
// one snare at five pitches, a row addressing a hit by `s04`. Every piece of it existed and none
// of it was reachable: `add-device --kind sampler`, `sampler-load` and `sampler-slice` were
// daw-cli verbs, so the surface could draw a chop that only a command line could make.
//
// make the sampler -> load the break -> chop it -> the slots appear, one per slice, on
// consecutive keys, and a note can name one.
// ---------------------------------------------------------------------------
{
  await page.evaluate(() => window.__uni.run('add-track'));
  await page.waitForTimeout(1200);
  const ct = await page.evaluate(() => window.__uni.state().tracks - 1);
  copyFileSync(resolve('presets/audio/waveform_probe.wav'), `${stack.dir}/break.wav`);

  await page.evaluate((t) => window.__uni.run(`sampler ${t}`), ct);
  await page.waitForTimeout(1500);
  await page.evaluate((t) => window.__uni.run(`load-sample ${t} 0 break.wav`), ct);
  const one = await page.waitForFunction(async (t) => {
    window.__uni.samplerKit(t, 0);
    const k = window.__uni.samplerKitCached(t, 0);
    return !!(k && k.slots && k.slots.length === 1);
  }, ct, { timeout: 8000 }).then(() => true).catch(() => false);
  check(one, 'the break loads as one slot');

  const said = await page.evaluate((t) => window.__uni.run(`slice ${t} 0 8`), ct);
  check(/8 equal slices/.test((said || []).join(' ')), 'the console chops it',
        (said || []).join(' ').slice(-80));

  const chopped = await page.evaluate(async (t) => {
    for (let i = 0; i < 60; i++) {
      window.__uni.samplerKit(t, 0);
      const k = window.__uni.samplerKitCached(t, 0);
      if (k && k.slots && k.slots.length >= 8) return k;
      await new Promise((r) => setTimeout(r, 150));
    }
    return window.__uni.samplerKitCached(t, 0);
  }, ct);
  /*
   * EIGHT ASKED FOR, EIGHT MADE, plus the whole-file slot the load left behind.
   *
   * This was 8-asked-for, SEVEN-made for a night, and the reason is worth keeping: `divideEqually`
   * returned only the INTERIOR boundaries, so the region from frame 0 to the first marker had no
   * index, no id and no way to be played. Backend's own words: "a comment called frame 0 'the
   * first slice's implicit start' — it was not implicit, it was UNREACHABLE." For a transient
   * chop that region is pre-roll and losing it is right; for an equal division it is the
   * DOWNBEAT. Frame 0 is a legal marker now and N parts means N slices.
   *
   * The check moved the day that changed, which is what an inverted gap check is for — it was
   * written to fail on the fix rather than to be remembered.
   *
   * Asserted exactly rather than loosely: `>= 8` would pass on nine, and a check that cannot
   * tell an off-by-one from a fix is not worth having.
   */
  check(chopped && chopped.slots.length === 9,
        'the chop adds eight slice slots beside the whole-file slot the load made',
        chopped && String(chopped.slots.length));

  if (chopped && chopped.slots.length === 9) {
    const sorted = [...chopped.slots].sort((a, b) => a.keyLow - b.keyLow);
    /*
     * CONSECUTIVE KEYS FROM C1. That is what makes a chop playable rather than merely stored:
     * the break lands under the fingers IN ORDER, so a pattern can address a hit by pitch alone
     * and never mention a slot id at all. It is also why `sound == 0` meaning "pitch picks the
     * slot" is the common case and the ops cell is empty on an ordinary kit track.
     */
    // The whole-file slot from the load shares key 36 with the chop's first slice, so the chop's
    // own slots are the ones naming a slice.
    const cut = sorted.filter((x) => x.slice > 0);
    const keys = cut.map((x) => x.keyLow);
    const consecutive = keys.every((k, i) => i === 0 || k === keys[i - 1] + 1);
    // EIGHT, since frame 0 became a legal marker: `slice 8` is eight slices on eight keys.
    check(cut.length === 8 && consecutive,
          'the slices are on consecutive keys, so the break is playable in order',
          JSON.stringify(keys));
    check(cut.every((x, i) => x.slice === i + 1), 'each naming the slice it plays, in order',
          JSON.stringify(cut.map((x) => x.slice)));
    /*
     * EVERY SLOT STILL REPORTS THE SOURCE'S LENGTH IN `frames`, and that is now correct rather
     * than a limitation.
     *
     * This was an inverted gap check — "the extent is derived at note-on and never published" —
     * and kShmVersion 35 published it. `frames` was deliberately left alone in that change: a
     * waveform needs the source's scale even while drawing a slice inside it, so a slot carries
     * two facts rather than one field that means different things depending on whether a slice
     * is set. So the assertion stays and its reason inverts.
     */
    check(cut.every((x) => x.frames === cut[0].frames),
          'every slot reports the SOURCE length in `frames` — one scale for the whole kit, with '
          + 'the slice bounds carried separately',
          JSON.stringify(cut.map((x) => x.frames)));

    /*
     * THE SLICES TILE THE SOURCE — the assertion this suite asked backend for, run here from the
     * UI's own read-back.
     *
     * Four claims, separately, so a failure says WHICH: the first slice starts at frame 0, the
     * last ends at the source's length, every slice's end is the next one's begin, and none is
     * empty. It knows nothing about the implementation, which is why it caught a bug the sound
     * checks could not: "slice 1 sounds different from slice 8" proves the chop works and says
     * nothing about whether it is CONTIGUOUS, and both of the bugs this found lived there.
     *
     * It found the frame-0 gap in the transient detector on its first run engine-side (#111), on
     * a file whose first hit is at frame 0 — the most common thing anyone chops.
     */
    const begins = cut.map((x) => x.begin), ends = cut.map((x) => x.end);
    check(begins[0] === 0, 'the first slice starts at frame 0 — nothing before the downbeat is '
          + 'left unreachable', String(begins[0]));
    check(ends[ends.length - 1] === cut[0].frames,
          'and the last one ends at the source length — nothing after the final hit is dropped',
          `${ends[ends.length - 1]} vs ${cut[0].frames}`);
    check(cut.every((x, i) => i === 0 || x.begin === ends[i - 1]),
          'and each slice begins exactly where the previous one ended — no gaps, no overlaps',
          JSON.stringify(cut.map((x) => [x.begin, x.end])));
    check(cut.every((x) => x.end > x.begin), 'and none of them is empty',
          JSON.stringify(cut.map((x) => x.end - x.begin)));

    /*
     * AND THE SLOTS ARE NAMED, which they were not until kShmVersion 36.
     *
     * `sampler-slice` seeds `<stem> NN` so a chop is readable before anybody renames anything,
     * and the stem rather than a bare "slice NN" so two breaks chopped into one kit stay apart.
     * Distinctness is the half worth asserting: eight pads all called the same thing would be a
     * seed that ran once instead of per slot, and it would look perfectly reasonable.
     */
    /*
     * ...AND THE CARD DRAWS THE EXTENTS AS A ROW OF SPANS.
     *
     * The read-back checks above prove the engine publishes the tiling. This proves the RACK
     * shows it, which is the whole reason the extents were asked for: "slice 3 is twice slice 4"
     * has to be visible without opening anything.
     *
     * Measured off the rendered boxes, not off the model — the model could be perfect while the
     * fill draws at the wrong offset, and a bar that is always full-width looks like a correct
     * bar rather than a missing feature. The offsets must ASCEND with the slices, which is the
     * one property a stubbed or always-zero `margin-left` cannot fake.
     */
    /*
     * THE RACK DRAWS THE CURSOR'S TRACK, and the chop is on a track this suite ADDED — so
     * without this the measurement is taken against track 0's two-slot kit and reports one
     * full-width span, which reads as the feature not working. The same trap the gate block
     * below already carries a comment about; it is the second time in this file.
     */
    await page.evaluate((t) => window.__uni.run(`goto 0 ${t}`), ct);
    await page.waitForTimeout(900);
    const spans = await page.evaluate(() => {
      const rows = [...document.querySelectorAll('.dv-p.slot')];
      return rows.map((r) => {
        const bar = r.querySelector('.dv-p-bar'), fill = r.querySelector('.dv-p-fill');
        if (!bar || !fill) return null;
        const b = bar.getBoundingClientRect(), f = fill.getBoundingClientRect();
        return b.width > 0
          ? { left: Math.round(((f.left - b.left) / b.width) * 100),
              w: Math.round((f.width / b.width) * 100) }
          : null;
      }).filter(Boolean);
    });
    check(spans.length >= 8, 'the card draws a span per pad', JSON.stringify(spans.slice(0, 3)));
    const sliced = spans.filter((x) => x.w < 95);
    check(sliced.length >= 7,
          'and a sliced pad is drawn NARROWER than the bar — an always-full fill looks like a '
          + 'correct bar rather than a missing feature',
          JSON.stringify(spans.map((x) => x.w)));
    const lefts = sliced.map((x) => x.left);
    check(lefts.some((v, i) => i > 0 && v > lefts[i - 1]),
          'and the spans STEP ACROSS the bar — the one property a stubbed offset cannot fake',
          JSON.stringify(lefts));

    const names = cut.map((x) => x.name);
    check(names.every((n) => /^break\s+\d+$/.test(n)),
          'each slice is seeded with its source stem and its number, so a fresh chop is readable',
          JSON.stringify(names.slice(0, 3)));
    check(new Set(names).size === names.length,
          'and no two share a name — a seed that ran once rather than per slot would look fine '
          + 'and be useless', JSON.stringify(names));
  }

  /*
   * THE SAMPLE VIEW: the chop, drawn.
   *
   * The extents were published so a chop could be SEEN, and until this the card could only
   * LIST it — "slice 3 of a 352800-frame source" is a fact, not a picture. `bank <t> <d>
   * default-view 1` swaps the pad list for the source's waveform with a boundary at every
   * slice, and the setting is persisted per device, so a kit set up as a break opens as the
   * break.
   *
   * Asserted on the CANVAS's box rather than on the model, and on the LIST being gone: a
   * card that drew both would be two things in one space, and a canvas with no size is
   * indistinguishable from one that has not painted.
   */
  {
    await page.evaluate((t) => window.__uni.run(`goto 0 ${t}`), ct);
    await page.waitForTimeout(600);
    const before = await page.evaluate(() => {
      const c = document.querySelector('.dv-card .dv-wave');
      const l = document.querySelector('.dv-card .dv-plist');
      return { wave: c ? c.getBoundingClientRect().width : -1,
               list: l ? l.getBoundingClientRect().width : -1 };
    });
    check(before.wave === 0 && before.list > 0,
          'a sampler shows its pad list by default, and no waveform', JSON.stringify(before));

    await page.evaluate((t) => window.__uni.run(`bank ${t} 0 default-view 1`), ct);
    await page.waitForTimeout(2500);
    const model = await page.evaluate(() => {
      const p = window.__uni.chainProbe();
      return p ? { views: p.views, painted: p.painted, samples: p.samples } : null;
    });
    /*
     * THE MODEL IS READY AND THE PICTURE IS NOT, and the reason is an engine contract rather
     * than anything here.
     *
     * The card knows which view the device remembers, which source to draw, how long it is and
     * where all nine slices begin and end. What it cannot get is the AUDIO: the engine's
     * waveform store is keyed BY PATH (`sourceIdForPath`) and interned for audio-CLIP sources
     * only, so a sampler's `sourceLocalId` — a per-device counter — addresses nothing in it.
     * Every window request answers no window, forever.
     *
     * So the swap deliberately does not happen: `_paintWave` returns false when it has drawn
     * nothing, and the pad list stays up. A canvas with no audio on it is indistinguishable from
     * a file that decoded to silence, and this app does not ship that distinction unresolved.
     *
     * Asserted as the CURRENT state so it goes red the day backend gives sampler sources a
     * waveform id — which is asked for on the channel. Both halves matter: the model must be
     * complete (or the fix would land on nothing) and the paint must be absent (or this is
     * already working and the note is a lie).
     */
    check(model && model.views[0] === 1 && model.samples[0]
          && model.samples[0].cuts === 9 && model.samples[0].frames > 0,
          // NINE, not eight: the load left a whole-file slot behind and the chop added eight, and
          // every one of them has an extent on this source. The number is asserted exactly for
          // the reason the chop count above is — `>= 8` would pass on the wrong picture.
          'the sample view knows what to draw — the source, its length and every slice boundary',
          JSON.stringify(model && model.samples));

    /*
     * ...AND IT DRAWS IT. Counting non-transparent pixels on the VISIBLE card's canvas, which is
     * the only honest test of a painter: a box has a size whether or not anything ran, and this
     * whole feature spent three attempts looking broken while the model was perfect.
     *
     * THE VISIBLE ONE. The rack pools its cards and hides the spares, so `.dv-card` is whichever
     * happens to be first in DOM order — it measured 0x0 and read as a dead feature while the
     * real card had 7,524 lit pixels on it. The same trap the tracker's ruler set earlier.
     */
    /*
     * WAIT FOR INK, do not sleep and hope. The window comes from the engine, so how long it
     * takes is a fact about the machine — the same reason `playUntilAudible` waits on the meter
     * rather than on a clock. The timeout still fails, and it fails having given the box a fair
     * chance rather than a constant somebody guessed.
     */
    const drawn = await page.evaluate(() => new Promise((done) => {
      const read = () => {
        const card = [...document.querySelectorAll('.dv-card')]
          .find((e) => e.getBoundingClientRect().width > 4 && e.offsetParent !== null);
        const c = card && card.querySelector('.dv-wave');
        if (!c || !c.width) return { ink: -1 };
        const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
        let ink = 0;
        for (let i = 3; i < d.length; i += 4) if (d[i] > 8) ink++;
        return { ink, w: c.width, h: c.height };
      };
      const t0 = Date.now();
      const tick = setInterval(() => {
        const r = read();
        if (r.ink > 500 || Date.now() - t0 > 15000) {
          clearInterval(tick);
          r.cache = window.__uni.waveProbe().map((e) => e.key);
          done(r);
        }
      }, 200);
    }));
    check(drawn.ink > 500,
          'and DRAWS it — the sampler source resolves through the engine\'s path-keyed store '
          + 'now (kWaveformRequestSamplerSource), so the audio a pad plays is finally visible',
          JSON.stringify(drawn));

    // Back to the kit, because the rest of this file reads slot rows.
    await page.evaluate((t) => window.__uni.run(`bank ${t} 0 default-view 0`), ct);
    await page.waitForTimeout(900);
  }

  /*
   * THE BANK'S NOTE-OFF DEFAULT, FROM THE POINTER.
   *
   * "ignore note-offs" was asked for as a per-bank setting and it is one — engine-side as
   * SamplerSetDevice field 1, and on the console as `bank <track> <device> default-gate 1`. This
   * is the other half of the rule that a console verb alone does not count: the card carries a
   * two-state button and it calls the SAME function the verb calls.
   *
   * The label says what a NEW pad will do rather than describing the kit, because that is what
   * the setting controls — it seeds a slot at mint and leaves the ones already there alone, so
   * "this kit is gated" would be a lie the moment one pad differs.
   */
  /*
   * THE RACK DRAWS THE CURSOR'S TRACK, so put the cursor on the one being tested.
   *
   * Without this the first visible card belongs to whichever track the cursor was left on by an
   * earlier block, and the click sets that sampler's default while the poll asks about this one
   * — the button said `gate` and the read-back said 0, which reads as the command not landing
   * when both were working on different devices. The same lesson as the two cache keys above,
   * one layer up: name the thing you mean.
   */
  await page.evaluate((t) => window.__uni.run(`goto 0 ${t}`), ct);
  await page.waitForTimeout(1200);

  const gateBtn = () => page.evaluate(() => {
    const b = [...document.querySelectorAll('.dv-card .dv-gat')].find((x) => x.style.display !== 'none');
    return b ? b.textContent : null;
  });
  check(await gateBtn() === '1shot',
        'the sampler card shows the bank default, and it starts at one-shot',
        JSON.stringify(await gateBtn()));

  await page.evaluate(() => {
    [...document.querySelectorAll('.dv-card .dv-gat')].find((x) => x.style.display !== 'none')
      .dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true }));
  });
  await page.waitForTimeout(1500);
  /*
   * ASK ABOUT THE DEVICE THE CARD ASKED ABOUT.
   *
   * The cache is keyed on the QUESTION — `track:device` — and the rack asks by the sampler's
   * real device id while `0` is the "first sampler" wildcard. Those are two entries, so polling
   * device 0 read a stale answer while the card, keyed on the real id, was already showing the
   * new value: the button said `gate` and the poll said 0. Not a race, two caches.
   */
  const devId = await page.evaluate((t) => {
    const ch = Object.values(window.__uni.chains()).find((c) => c && c.track === t);
    const d = ch && ch.devices ? ch.devices.find((x) => x.kind === 5) : null;
    return d ? d.id : 0;
  }, ct);
  const flipped = await page.evaluate(async ([t, d]) => {
    for (let i = 0; i < 40; i++) {
      window.__uni.send({ type: 'samplerkit', track: t, device: d });
      await new Promise((r) => setTimeout(r, 200));
      const k = window.__uni.samplerKitCached(t, d);
      if (k && k.defaultGate) return k.defaultGate;
    }
    const k = window.__uni.samplerKitCached(t, d);
    return k ? k.defaultGate : null;
  }, [ct, devId]);
  check(flipped === 1, 'clicking it sets the bank default, and the engine agrees',
        `defaultGate ${flipped}`);
  check(await gateBtn() === 'gate', 'and the button says so', JSON.stringify(await gateBtn()));

  /*
   * A REFUSED SAMPLER COMMAND SAYS SO — UiDiffType::SamplerRejected (17).
   *
   * Twenty sites across seven sampler verbs used to refuse into the engine's log and nowhere
   * else: from a browser they were commands that reported success and did nothing. I found one
   * of the twenty by accident, because the sound kept playing — `slot 0` is not a wildcard, the
   * engine answered `no_such_slot`, and a half-second note ran the sample's full eight seconds
   * while the console said the command had worked.
   *
   * A slot id that cannot exist is the cheapest way to provoke one, and the check is on the
   * WORDING as well as the fact: "no slot 999" tells a caller its idea of the kit is stale,
   * where a bare "rejected" would only tell it that it was wrong about something.
   */
  await page.evaluate((t) => { window.__uni.state().reject = null;
                               window.__uni.run(`slot ${t} 0 999 gate 1`); }, ct);
  await page.waitForTimeout(1500);
  const refusal = await page.evaluate(() => (window.__uni.state() || {}).reject);
  check(refusal && /no slot 999/.test(refusal),
        'a sampler command refused by the engine is REPORTED, and says which slot',
        JSON.stringify(refusal));

  /*
   * AND A VALID ONE IS QUIET. Without this the check above passes on a channel that fires for
   * every command, which reports nothing while looking like it reports everything — the same
   * shape as a metric that cannot fail.
   */
  await page.evaluate((t) => { window.__uni.state().reject = null;
                               window.__uni.run(`slot ${t} 0 1 gate 1`); }, ct);
  await page.waitForTimeout(1500);
  const quiet = await page.evaluate(() => (window.__uni.state() || {}).reject);
  check(!quiet || !/no slot/.test(quiet), 'and a VALID one says nothing', JSON.stringify(quiet));
}

check(errors.length === 0, 'and nothing threw', errors.slice(0, 3).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
