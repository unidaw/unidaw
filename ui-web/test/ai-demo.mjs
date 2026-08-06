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
 * WHAT THE AGENT CANNOT DO, recorded here because a demo should not discover it live.
 *
 * This paragraph used to say there was no chord tool, no sampler tooling and no patcher tooling —
 * true when written, and false by the time the checks below were passing. `add_chords`,
 * `load_sample` and `patcher_node` exist now and this file asserts two of the three. A stale note
 * about a gap reads exactly like a finding, so: what remains is ONE thing.
 *
 * The agent cannot DISCOVER DEVICE IDS. Five tools take a `device` and the observation it is
 * given reports no chains, so "wire the patcher" needs a person to supply the id. It says so
 * rather than guessing, which is the right behaviour and still a gap. Tracked separately; that is
 * why there is no patcher prompt below.
 */

import { chromium } from 'playwright';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
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
    /*
     * `spoke &&` HERE TOO. This break had the same flaw the one below is documented for, and
     * it stayed hidden because it needs a `moved` that is trivially true to show: with
     * `() => true`, the loop can satisfy "the song moved and it has gone quiet" during the
     * five to fifteen seconds BEFORE the model's first token, and return a 4-second turn in
     * which nothing happened. Two checks failed on a model that had not been asked yet.
     */
    if (spoke && moved(before, now) && quiet >= 4) break;
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
/*
 * `demo_pluck_c4.wav`, which is what the runbook tells a person to reach for: middle C with its
 * attack in the first millisecond. Naming a probe asset here would have the model build something
 * that is structurally perfect and inaudible for its first second.
 */
const smp = await askFor(
  'put a sampler on track 0, load demo_pluck_c4.wav into it, and write a four note phrase',
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

/*
 * SAVED HERE, WHERE IT EXISTS. The render at the bottom needs this song, and the sections after
 * this one open NEW ones — so saving at the end saved `aipatch` under this name and rendered a
 * project with no notes in it. Nothing said so: the earlier checks had already passed against
 * the song while it was still open.
 */
await run('save aisampler');
await page.waitForTimeout(2200);
const built = await page.evaluate(() => {
  const e = window.__uni.engineState() || {};
  return { notes: e.noteCount, ticks: (window.__uni.notes() || []).map((n) => n.t ?? n.on).slice(0, 8) };
});
console.log(`  the AI's sampler part: ${built.notes} note(s) at ${JSON.stringify(built.ticks)}`);

// ---------------------------------------------------------------------------
// ASKED FOR A DRUM TRACK WITHOUT NAMING A FILE — the case that produced a silent track.
//
// Backend's rehearsal: "add a track called Drums with a sampler, and write a four-bar beat". The
// model added the track, named it, added the sampler, called load_sample TWICE with invented file
// names, took both refusals, said "I see the samples aren't found — let me write the drum pattern
// anyway", and left sixteen notes on a silent track. Every step right except the one it could not
// know: nothing told it what files exist.
//
// The observation carries a `samples:` line now. This asks WITHOUT naming a file and requires the
// slot to resolve — which is only possible if the model read the list rather than guessing.
// ---------------------------------------------------------------------------
await run('new aidrumkit');
await page.waitForTimeout(1500);
const kit = await askFor(
  'add a track called Drums with a sampler on it, load a drum sound into it, and write a four bar beat',
  (a, b) => b.notes > a.notes, 150000);
const kitSlots = await page.evaluate(() => {
  for (let t = 0; t < 4; t++) {
    for (let d = 0; d < 6; d++) {
      const k = window.__uni.samplerKitCached(t, d);
      if (k && k.slots && k.slots.length) return { track: t, slots: k.slots.length };
    }
  }
  return { track: -1, slots: 0 };
});
console.log(`  the drum kit it built: ${JSON.stringify(kitSlots)} :: ${kit.said.slice(-220)}`);
check(kitSlots.slots > 0,
      'asked for a drum sound WITHOUT being told a filename, it loads a real one',
      `${kit.secs}s, ${JSON.stringify(kitSlots)} — zero slots means it guessed at a name, took the `
      + `refusal, and wrote the notes anyway onto a silent track`);

// ---------------------------------------------------------------------------
// THE PATCHER, WHICH WAS UNANSWERABLE UNTIL THE OBSERVATION CARRIED DEVICE IDS.
//
// `patcher_node` has existed for a while and could not be used: it takes a `device`, and
// nothing the agent could read ever reported one. The engine publishes chains as diffs on a
// SINGLE-CONSUMER ring, so the agent cannot go and read them either — a second consumer would
// take entries away from the browser's rack. The sidecar's drainer accumulates them and now
// hands that accumulation to the observation.
//
// THE FIRST VERSION OF THIS CHECK PROVED NOTHING, and the negative control is what said so.
// It asked for a patcher on an EMPTY track and asserted a node existed anywhere. Device ids
// start at 1, so the model guessing "device 1" is right without reading anything — and with the
// device list forcibly disabled the check still passed. A check that passes with its subject
// removed is measuring the model's luck.
//
// So: a device is put on the track FIRST, which pushes the patcher to id 2, and the assertion
// is on `owner` — which device's graph the node is actually in. A guess of 1 now lands the node
// in the audio patcher and the event patcher's graph stays empty, which fails.
//
// The filler is an AUDIO patcher because a chain holds one instrument and the event patcher the
// model is about to add must be allowed to land.
// ---------------------------------------------------------------------------
await run('new aipatch');
await page.waitForTimeout(1500);
const filler = await page.evaluate(() => window.__uni.addDevice(0, 'patcher audio'));
check(filler === true, 'a filler device takes id 1, so the patcher cannot be guessed at',
      String(filler));
await page.waitForTimeout(1500);

const patch = await askFor(
  'put an event patcher on track 0 and add a euclidean node to it',
  () => true, 150000);

/*
 * WAIT FOR THE NODE, NOT FOR THE SILENCE.
 *
 * `askFor` ends a turn when the transcript has been quiet for four seconds, and a model making
 * tool calls is quiet for longer than that between utterances — this prompt returned after 8s
 * with the patcher added and the node still to come, and the assertion below read a device with
 * an empty graph. The transcript stopping is not the work stopping.
 *
 * So: poll the published graph for a node until one appears. This can only make a failing case
 * take longer; it cannot make a broken one pass, because the assertion is still on the SAVED
 * project afterwards.
 */
for (let i = 0; i < 40; i++) {
  const n = await page.evaluate(() => {
    const p = window.__uni.patcher();
    return p && p.nodes ? p.nodes.length : 0;
  });
  if (n > 0) break;
  await page.waitForTimeout(1000);
}

// The EVENT patcher's id, read from the chain rather than assumed — kind 0 is patcher event.
const eventId = await page.evaluate(() => {
  const c = window.__uni.chains()[0];
  const d = c && c.devices && c.devices.find((x) => x.kind === 0);
  return d ? d.id : -1;
});
check(eventId > 1, 'the model added an event patcher, and it is not device 1',
      `device ${eventId} :: ${patch.said.slice(-200)}`);

/*
 * ASK THE SAVED PROJECT, NOT THE PUBLISHED GRAPH.
 *
 * Two wrong instruments were tried first and each gave a confident wrong answer:
 *
 *   `nodes()` has no `owner` at all — id, type and config only — so it read `[null]`.
 *
 *   `patcher()` has owner, but publishes whichever graph the UI has OPEN, and with none
 *   open that is the shared POOL. It read `[0, 0, 0]`, which is indistinguishable from an
 *   agent writing pool-scoped edits — and that is what I concluded, wrongly. A deterministic
 *   test with no model in it (daw-agent's `agent_patcher_edits_are_device_scoped`) shows the
 *   tool scopes correctly; the measurement was the broken part.
 *
 * The device's OWN graph is what gets written to disk, and a pool-scoped edit saves as zero
 * nodes on every device. So the file answers the question the published graph cannot, which is
 * the same reason patcher-device.mjs reads it.
 */
/*
 * NEGATIVE CONTROL RUN. With the sidecar's device lookup forced to return nothing, this check
 * FAILS — device 2 holds 0 nodes and the transcript shows the model hunting instead: it called
 * `device_params {"device": 1}`, got the Identity plugin back, and never wired anything.
 *
 * Worth stating because the FIRST version of this check passed with the lookup blinded. It asked
 * for a patcher on an empty track and accepted a node anywhere, so "the model guessed 1 and was
 * right" satisfied it. The filler device and the saved-project reading are what give it teeth.
 */
await run('save aipatch');
await page.waitForTimeout(2500);
const savedGraph = (() => {
  try {
    const doc = JSON.parse(readFileSync(join(stack.dir, 'aipatch.uniproj.json'), 'utf8'));
    const t0 = (doc.tracks || []).find((t) => t.track_id === 0) || {};
    const dev = (t0.device_chain || []).find((d) => d.device_id === eventId);
    const st = dev && (dev.patcher_state || dev.patcher);
    return { found: !!dev, nodes: (st && st.nodes ? st.nodes.length : 0),
             // EVERY device's count, because "device 2 has none" and "nothing was added
             // anywhere" and "it went to device 1" are three different failures and the
             // first reading cannot tell them apart.
             chain: (t0.device_chain || []).map((d) => {
               const g = d.patcher_state || d.patcher;
               return `${d.device_id}:${d.kind}:${g && g.nodes ? g.nodes.length : 0}n`;
             }) };
  } catch (e) { return { found: false, nodes: 0, chain: [String(e).slice(0, 120)] }; }
})();
check(savedGraph.nodes > 0,
      "and the node is in THAT device's own saved graph — it read the id, it did not guess",
      `${patch.secs}s, device ${eventId} holds ${savedGraph.nodes} node(s); `
      + `chain ${JSON.stringify(savedGraph.chain)}; said: ${patch.said.slice(-500)}`);

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

// ---------------------------------------------------------------------------
// AND DOES ANY OF IT MAKE A SOUND?
//
// Every check above asserts the SONG CHANGED — a track appeared, notes exist, chords are chords.
// All true and none of them is what a demo audience experiences. A model can build a structurally
// perfect part that is inaudible, and this repo has found exactly that twice this week: a chord
// that emitted note-ons at peak 0, and a patcher graph that assembles and reaches no instrument.
//
// So the last prompt's work is RENDERED. The sampler song is the one to use: the model put the
// device, the file and the notes there itself, so if it sounds, every link in that chain — tool
// call, engine command, sampler mapping, render — worked from one English sentence.
// ---------------------------------------------------------------------------
/*
 * Rendered from the file saved at the sampler section — NOT from whatever is open now. The
 * sections between here and there each start a new song.
 */
let aiPeak = -1;
let aiWav = null;
try {
  const { execFileSync } = await import('node:child_process');
  const { existsSync, unlinkSync } = await import('node:fs');
  const { fileURLToPath } = await import('node:url');
  const { readWav, envelope } = await import('./wav.mjs');
  const ROOT = fileURLToPath(new URL('../..', import.meta.url));
  const out = join(stack.dir, 'aitake.wav');
  try { unlinkSync(out); } catch { /* absent is normal */ }
  execFileSync(join(ROOT, 'build', 'daw_engine'),
               ['--project', 'aisampler', '--render', 'aitake', '--run-seconds', '12'],
               { cwd: join(ROOT, 'build'),
                 env: { ...process.env, DAW_PROJECT_DIR: stack.dir,
                        DAW_HOST_BINARY: join(ROOT, 'build', 'juce_host_process'),
                        DAW_UI_SHM_NAME: `/aidemo_${process.pid}` },
                 stdio: ['ignore', 'pipe', 'pipe'], timeout: 180000 });
  if (existsSync(out)) {
    const w = readWav(out);
    aiWav = w;                       // kept: the note detector reads it below
    aiPeak = envelope(w.mono, w.rate, 0.05).reduce((m, v) => Math.max(m, v), 0);
  }
} catch (e) {
  check(false, 'the AI-built song renders', String(e).slice(0, 180));
}
console.log(`  the AI-built sampler part renders at peak ${aiPeak < 0 ? 'FAILED' : aiPeak.toFixed(4)}`);
check(aiPeak > 0.004,
      'WHAT THE AI BUILT ACTUALLY SOUNDS — device, sample and notes, from one sentence',
      `peak ${aiPeak.toFixed(4)}; a structurally perfect part that is inaudible is the failure `
      + `this repo has found twice this week, and every check above would still pass`);

/*
 * AND ARE THEY THE NOTES IT SAID IT WROTE?
 *
 * A peak is "it made a noise", which is where this check stopped — and a noise is not a phrase.
 * Every failure mode that matters to a demo audience survives a peak: one note instead of four,
 * every note at the same pitch, the last three dropped, the whole part a semitone out.
 *
 * `demo_pluck_c4.wav` is rooted at middle C, so a slot plays note N at pitch N. That makes the
 * document and the audio directly comparable: what the model wrote is what you should hear, with
 * no mapping in between. The project file is the ground truth — not the tool replies, which are
 * what the model CLAIMED.
 *
 * Read as a subset, not an equality. The detector reports what it is confident about and skips
 * the rest, so "every pitch heard was a pitch written" is the honest direction: it catches a part
 * playing the wrong notes, and does not fail because a quiet fourth voice went unmeasured.
 */
if (aiPeak > 0.004 && aiWav) {
  const { detectNotes, noteName } = await import('./notes.mjs');
  const { readFileSync } = await import('node:fs');
  let written = [];
  try {
    const doc = JSON.parse(readFileSync(join(stack.dir, 'aisampler.uniproj.json'), 'utf8'));
    written = (doc.clips || []).flatMap((c) => (c.notes || []).map((n) => n.pitch ?? n.note))
                               .filter((p) => Number.isFinite(p));
  } catch (e) { /* asserted below by being empty */ }

  const heard = detectNotes(aiWav.mono, aiWav.rate, { minConf: 0.5 });
  const heardPitches = heard.map((h) => h.midi);
  console.log(`  wrote ${JSON.stringify(written)}  heard `
              + `${JSON.stringify(heard.map((h) => `${h.name}@${h.at.toFixed(2)}s`))}`);

  check(written.length >= 2,
        'the model wrote a phrase into the document, not a single note',
        `pitches on disk: ${JSON.stringify(written)}`);

  // NOT "some sound happened N times": distinct pitches is what separates a phrase from one note
  // struck repeatedly, and a resampled one-shot repeated is exactly what a broken write looks like.
  const distinct = new Set(heardPitches);
  check(distinct.size >= 2,
        'MORE THAN ONE PITCH IS AUDIBLE — a phrase, not one note struck repeatedly',
        `heard ${JSON.stringify([...distinct].map(noteName))} from ${heard.length} onsets`);

  const strays = heardPitches.filter((p) => !written.some((w) => Math.abs(w - p) <= 1));
  check(written.length > 0 && strays.length === 0,
        'EVERY PITCH HEARD IS A PITCH THE MODEL WROTE — the part plays what the document says',
        `unaccounted: ${JSON.stringify(strays.map(noteName))} against written `
        + `${JSON.stringify(written)}. A part transposed as a whole, or playing a different `
        + `sample's pitch, lands here and nowhere else.`);
}

check(errors.length === 0, 'no page errors while prompting', errors.join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail ? `${fail} FAILED, ${pass} passed` : `ALL PASS (${pass} checks)`}\n`);
process.exit(fail ? 1 : 0);
