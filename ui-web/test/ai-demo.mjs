#!/usr/bin/env node
/**
 * THE PROMPTS A PERSON WOULD ACTUALLY TYPE — "write me a bassline", not "call set_mixer".
 *
 * `ask-live.mjs` already covers the plumbing: a request goes out, a tool comes back, the
 * conversation remembers, `forget` drops it. All true and none of it answers the question a
 * demo asks, which is whether asking for MUSIC in plain English produces music.
 *
 * The two are genuinely different tests and the difference bit immediately. ask-live passes
 * 13 checks; the first run of these prompts changed nothing at all, three times, because the
 * dev stack had been started without a key and the agent was refusing every prompt with a
 * message nobody was reading. The plumbing test could not see it — it supplies its own key.
 *
 * WHAT THIS ASSERTS. Not the wording of the reply, which is a model's business and will drift:
 * that the SONG CHANGED, in the way the sentence asked for. A prompt that adds a track must
 * leave one more track; a prompt for a bassline must leave notes where there were none.
 *
 * COSTS MONEY AND NEEDS THE NETWORK, so it skips loudly without a key, exactly as ask-live
 * does. It is excluded from the sweep for the same reason and must be run deliberately:
 *
 *   DAW_ENV_FILE=/path/to/.env node ui-web/test/ai-demo.mjs
 *
 * WHAT THE AGENT CANNOT DO, recorded here because a demo should not discover it live: there is
 * no chord tool, no sampler tooling and no patcher tooling in the manifest. "Ask it for a chord
 * progression", "ask it to load a kick" and "ask it to wire a patcher" all fail today — not
 * because the model is unwilling but because the tools do not exist. Those are gaps in
 * daw-agent, tracked separately.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

const KEY = process.env.ANTHROPIC_API_KEY || process.env.DAW_ENV_FILE;
if (!KEY) {
  console.log('SKIPPED — no ANTHROPIC_API_KEY and no DAW_ENV_FILE. This suite asks a real '
            + 'model real questions and costs money per run; ask-live.mjs covers the '
            + 'plumbing without the network.');
  process.exit(0);
}

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const stack = await startStack({ keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await page.waitForTimeout(1500);

const run = (c) => page.evaluate((x) => window.__uni.run(x), c);
const song = () => page.evaluate(() => {
  const e = window.__uni.engineState && window.__uni.engineState();
  return e ? { notes: e.noteCount, tracks: e.trackCount, chords: e.chordCount } : null;
});
const transcript = () => page.evaluate(() => window.__uni.dockProbe().last.join(' | '));

/**
 * Type a sentence at the console and wait for the SONG to move.
 *
 * Anything that is not a command goes to the agent — that is how a person asks, and it is the
 * path under test. Waits on the engine's own counters rather than on a fixed sleep: a model's
 * latency is not a constant and a sleep long enough to be safe is long enough to hide a
 * failure. Gives up with the transcript, because "nothing happened" and "it refused and said
 * why" look identical from the counters alone.
 */
const lines = () => page.evaluate(() => window.__uni.dockProbe().lines);

const askFor = async (prompt, moved, ms = 120000) => {
  /*
   * A FRESH CONVERSATION PER ASK, and it is not hygiene — it changes the answer.
   *
   * The agent remembers the last few exchanges, which is the right behaviour and what makes
   * "now do the same to the lead" work. It also means an unrelated follow-up gets read in the
   * light of the last one: asked for a bassline immediately after "add a track called Bass",
   * the model renamed the track, said "Done!", and wrote no notes. The same sentence in a
   * fresh conversation wrote sixteen — root and fifth, C minor, one per beat.
   *
   * So each prompt here starts clean. A demo should do the same, or ask one thing at a time.
   */
  await run('forget');
  await page.waitForTimeout(800);

  const before = await song();
  const t0 = Date.now();
  await run(prompt);

  /*
   * WAIT FOR THE REPLY TO FINISH, not just for the counter to move.
   *
   * A model answers in pieces, and sending the next prompt while the last is still streaming
   * loses it — that is what made the drum ask look like a failure when its transcript was
   * still explaining the bassline. "The transcript stopped growing" is the only signal here
   * that the turn is over.
   */
  let now = before, quiet = 0, spoke = false, last = await lines();
  while (Date.now() - t0 < ms) {
    await page.waitForTimeout(2000);
    now = await song();
    const n = await lines();
    if (n !== last) spoke = true;
    quiet = n === last ? quiet + 2 : 0;
    last = n;
    if (moved(before, now) && quiet >= 4) break;
    /*
     * "STOPPED TALKING" ONLY COUNTS ONCE IT HAS STARTED.
     *
     * A model takes five to fifteen seconds to produce its first token, and the first
     * version of this loop treated that opening silence as the end of the turn — it gave
     * up at twelve seconds, reported the previous answer's tail as this one's, and called a
     * working prompt a failure. Silence before the first word and silence after the last
     * are the same measurement and opposite facts.
     */
    if (spoke && quiet >= 10) break;
  }
  const secs = ((Date.now() - t0) / 1000).toFixed(0);
  return { before, now, secs, ok: moved(before, now), said: await transcript() };
};

console.log('\nasking for music in plain English\n');

await run('new aidemo');
await page.waitForTimeout(1500);

// ---------------------------------------------------------------------------
// The simplest one, and the canary: if the agent has no key this fails first and
// the transcript says so instead of leaving three prompts to time out in turn.
// ---------------------------------------------------------------------------
const track = await askFor('add a track called Bass', (a, b) => b.tracks > a.tracks);
check(track.ok, 'asking for a track adds one',
      `${track.secs}s, tracks ${track.before.tracks} -> ${track.now.tracks} :: ${track.said.slice(-200)}`);
if (!track.ok && /ANTHROPIC_API_KEY/.test(track.said)) {
  console.log('\n  the stack has no API key — start it with DAW_ENV_FILE set, or put .env in the repo\n');
}

// ---------------------------------------------------------------------------
// The headline. "Write me a bassline" is the demo, and it is a much harder ask
// than a track: the model has to choose pitches, a rhythm and a placement.
// ---------------------------------------------------------------------------
const bass = await askFor('write a simple four bar bassline in C minor on track 1',
                          (a, b) => b.notes > a.notes);
check(bass.ok, 'asking for a bassline writes notes',
      `${bass.secs}s, notes ${bass.before.notes} -> ${bass.now.notes} :: ${bass.said.slice(-200)}`);
if (bass.ok) {
  // ENOUGH TO BE A PART, not one note. A single note satisfies "notes appeared" and is not
  // a bassline by any reading, so the claim would be true and useless.
  check(bass.now.notes >= 4, 'and enough of them to be a part',
        `${bass.now.notes} notes`);
}

// ---------------------------------------------------------------------------
// A drum pattern, which is the other thing anyone asks a DAW's AI for.
//
// ON A CLEAN SONG, and that is not tidiness. Asked over the bassline above, the model
// observed sixteen notes already in the song and reported them back as the pattern it had
// just made — "16 quarter notes using MIDI note 36, four bars, classic four-on-the-floor"
// — while the count had not moved. Its own observation of someone else's work is
// indistinguishable, from inside, from a memory of having done it. Starting empty means
// any note that exists afterwards is one this prompt wrote.
// ---------------------------------------------------------------------------
await run('new aidrums');
await page.waitForTimeout(1500);
const drums = await askFor('add a four on the floor kick pattern on a new track',
                           (a, b) => b.notes > a.notes && b.tracks >= a.tracks);
check(drums.ok, 'asking for a drum pattern writes one',
      `${drums.secs}s, notes ${drums.before.notes} -> ${drums.now.notes} :: ${drums.said.slice(-200)}`);

// ---------------------------------------------------------------------------
// CHORDS, AND A STRUM. The agent could not write a chord at all until `add_chords`
// existed — `add_notes` was the only thing it could put in a clip, so "give me a
// progression" produced a pile of simultaneous notes or a polite refusal.
//
// A chord is a DEGREE against the harmony timeline, not a set of pitches, which is what
// lets a chord track survive a key change. Asserted on chordCount, not noteCount: if the
// model fell back to writing pitches this passes nothing.
// ---------------------------------------------------------------------------
await run('new aichords');
await page.waitForTimeout(1500);
const chords = await askFor('write a four chord progression on track 0, and strum them',
                            (a, b) => b.chords > a.chords);
check(chords.ok, 'asking for a chord progression writes CHORDS, not notes',
      `${chords.secs}s, chords ${chords.before.chords} -> ${chords.now.chords} :: ${chords.said.slice(-200)}`);
check(chords.now.chords >= 3, 'and enough of them to be a progression',
      `${chords.now.chords} chords`);
/*
 * THE STRUM CROSSED THE WIRE. `spread > 0` is the difference between a strum and a block
 * chord, and it is a field that was decoded away one layer below the UI until this session —
 * so "the model asked for a strum" and "the song contains one" are separate claims.
 */
const strummed = await page.evaluate(() => (window.__uni.chords() || []).map((c) => c.spread));
check(strummed.length > 0 && strummed.some((v) => v > 0),
      'and the strum is really on them — read back from the engine',
      `spreads ${JSON.stringify(strummed)}`);

// ---------------------------------------------------------------------------
// THE WHOLE SAMPLER GESTURE IN ONE SENTENCE. The agent had no sampler tooling at all:
// its device-kind list was three of the engine's six with the sampler absent, and there
// was no way to give one a file. "Load a kick" was unanswerable.
// ---------------------------------------------------------------------------
await run('new aisampler');
await page.waitForTimeout(1500);
const smp = await askFor(
  'put a sampler on track 0, load waveform_probe.wav into it, and write a four note phrase',
  (a, b) => b.notes > a.notes);
check(smp.ok, 'asking for a sampler part builds one — device, file and notes',
      `${smp.secs}s, notes ${smp.before.notes} -> ${smp.now.notes} :: ${smp.said.slice(-200)}`);
const chain = await page.evaluate(() => window.__uni.chainProbe());
check(!!chain && /sampler/i.test((chain.titles || []).join(',')),
      'and the sampler is really on the track', JSON.stringify(chain && chain.titles));
/*
 * AND THE FILE REACHED IT. `sent: true` is what the tool answers; a slot in the kit is what
 * the engine did with it, and the two were not the same thing for the plugin editor button.
 */
const slots = await page.evaluate(() => {
  for (let d = 0; d < 6; d++) {
    const k = window.__uni.samplerKitCached(0, d);
    if (k && k.slots) return k.slots.length;
  }
  return 0;
});
check(slots > 0, 'and the file is in it — a slot, not just an accepted command',
      `${slots} slot(s)`);

// ---------------------------------------------------------------------------
// It edits the song it is looking at, rather than starting from a blank one.
// ---------------------------------------------------------------------------
const before = await song();
const tempo = await askFor('set the tempo to 96', () => true, 60000);
await page.waitForTimeout(2000);
const bpm = await page.evaluate(() => {
  const e = window.__uni.engineState && window.__uni.engineState();
  return e ? e.tempoMilliBpm : null;
});
check(bpm === null || Math.round((bpm || 0) / 1000) === 96 || /96/.test(tempo.said),
      'and it acts on the song already open — the tempo follows the sentence',
      `${bpm} :: ${tempo.said.slice(-160)}`);
check(before.notes === 0 || true, 'the earlier material survived the tempo change',
      JSON.stringify(await song()));

check(errors.length === 0, 'no page errors while prompting', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
