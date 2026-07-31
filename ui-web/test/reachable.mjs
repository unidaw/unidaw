/**
 * EVERYTHING IN THE FILE IS REACHABLE THROUGH THE UI.
 *
 * The other suites here ask whether a feature works. This one asks a different
 * question, and it is the question that would have caught the worst bug of the
 * night: given a document, can the interface SEE all of it?
 *
 * Notes on track 8 and above never crossed the wire. The engine had them, the saved
 * file had them, and the browser was never told — so they could not be seen, edited
 * or played, and 363 checks walked past it because every one of them asserts
 * something about material it already knows is there. Nothing asked the complement:
 * is there anything the document contains that the app cannot show?
 *
 * WHY A FIXTURE CANNOT ASK IT. No preset has more than six tracks, and none has a
 * tombstoned slot, because every preset was AUTHORED. Both of last night's data-loss
 * bugs needed a condition only a SESSION produces — more tracks than anyone wrote
 * down, ids that are not dense from zero. So this builds its document by editing, the
 * way backend's edited_roundtrip_check does for the engine, and then uses the SAVED
 * FILE as ground truth against the running UI.
 *
 * The file is the right authority precisely because it is not the wire: it is written
 * by the engine from its own document, and it is what a person would still have
 * tomorrow. If the file holds something the UI cannot reach, the UI is lying about
 * the song — whatever the reason, and whether the reason is the sidecar, a stride, a
 * count-versus-extent, or a renderer that draws sixteen lanes and no more.
 *
 * WHAT IT DELIBERATELY DOES NOT DO: assert positions, names or counts against
 * expected values. Those are the other suites' job and they are specific. This one is
 * total and shallow — for every X in the file, is there an X on screen — because
 * total and shallow is what finds the thing nobody thought of.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const MASTER = 0xFFFF0000;

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

await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1200);
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null,
                           { timeout: 12000 }).catch(() => {});

const loadAndWait = async (name) => {
  const before = await page.evaluate(() => (window.__uni.loadStatus() || {}).seq || 0);
  await page.evaluate((n) => window.__uni.loadProject(n), name);
  await page.waitForFunction(
    (s) => { const l = window.__uni.loadStatus(); return l && l.seq > s && l.ok; },
    before, { timeout: 15000 }).catch(() => {});
  await page.waitForTimeout(500);
};

// ---------------------------------------------------------------------------
// A SESSION, not a fixture.
//
// Ten tracks, because the wire carried eight; notes on the ones past the old cap,
// because that is the material that vanished; and a removal in the middle, because
// that is what makes the ids sparse and the slots tombstoned. None of this can be
// authored into a preset — a preset with ten dense tracks would not have produced
// either bug.
// ---------------------------------------------------------------------------
await page.evaluate(() => window.__uni.run('view tracker'));
await loadAndWait('meter');

const startTracks = await page.evaluate(() => window.__uni.state().tracks);
for (let i = startTracks; i < 11; i++) {
  await page.evaluate(() => window.__uni.addTrack());
  await page.waitForTimeout(400);
}
const grew = await page.evaluate(() => window.__uni.state().tracks);
check(grew >= 11, 'the session built more tracks than the wire used to carry',
      `${startTracks} -> ${grew}`);

// A note on each of several tracks, INCLUDING past the old cap of eight. Distinctive
// pitches so a lost one is identifiable rather than merely missing from a count.
const written = [];
for (const [track, pitch] of [[0, 61], [4, 71], [7, 79], [8, 88], [9, 90], [10, 95]]) {
  await page.evaluate((t) => window.__uni.run(`goto 3 ${t}`), track);
  await page.waitForTimeout(150);
  await page.evaluate((p) => window.__uni.run(`note ${p}`), pitch);
  await page.waitForTimeout(500);
  written.push({ track, pitch });
}
check(written.length === 6, 'and wrote a note on six of them, three past the old cap');

// Remove one in the MIDDLE, so ids stop being dense from zero and a slot becomes a
// tombstone. This is the shape no preset has.
page.once('dialog', (d) => d.accept());
await page.evaluate(() => window.__uni.removeTrack(2));
await page.waitForTimeout(1200);

// ---------------------------------------------------------------------------
// SAVE, and take the file as the authority.
// ---------------------------------------------------------------------------
await page.evaluate(() => window.__uni.run('save reachcheck'));
await page.waitForTimeout(2500);

const file = join(stack.dir, 'reachcheck.uniproj.json');
let doc = null;
try { doc = JSON.parse(readFileSync(file, 'utf8')); }
catch (e) { check(false, 'the session saved a file to compare against', e.message); }

if (doc) {
  /** Every authored note in the document, wherever the format keeps it. */
  const fileNotes = [];
  const byId = new Map();
  for (const c of doc.clips || []) byId.set(c.id, c);
  for (const t of doc.tracks || []) {
    if (t.track_id === MASTER) continue;
    for (const n of t.notes || []) fileNotes.push({ track: t.track_id, pitch: n.pitch });
    for (const p of t.placements || []) {
      for (const n of p.notes || []) fileNotes.push({ track: t.track_id, pitch: n.pitch });
      const clip = byId.get(p.clip_id);
      for (const n of (clip && clip.notes) || []) {
        fileNotes.push({ track: t.track_id, pitch: n.pitch });
      }
    }
  }
  const fileTracks = (doc.tracks || []).filter((t) => t.track_id !== MASTER);
  check(fileNotes.length > 8, 'the file holds notes to account for',
        `${fileNotes.length} across ${fileTracks.length} tracks`);

  // Reload it, so the UI is looking at the same bytes rather than at whatever the
  // session left in memory. A wire that drops a track on load is the case in point.
  await loadAndWait('reachcheck');

  /*
   * 1. EVERY TRACK IN THE FILE HAS A LANE.
   *
   * Not "the count matches" — WHICH tracks. A count can be right while the set is
   * wrong, and after a removal the ids are sparse, which is exactly the condition
   * that made a real track invisible and an imaginary one editable.
   */
  const seenTracks = await page.evaluate(() => {
    const tree = window.__uni.trackTree() || [];
    return tree.filter((t) => !t.absent).map((t) => t.track);
  });
  const missingTracks = fileTracks
    .map((t) => t.track_id)
    .filter((id) => !seenTracks.includes(id));
  check(missingTracks.length === 0,
        'every track in the file is a lane in the UI',
        `file ${JSON.stringify(fileTracks.map((t) => t.track_id))}`
        + ` vs UI ${JSON.stringify(seenTracks)}`);
  // And nothing the file does not have. The sparse-id bug INVENTED an editable lane
  // in an unclaimed slot, which a "nothing is missing" check passes happily.
  const invented = seenTracks.filter((id) => !fileTracks.some((t) => t.track_id === id));
  check(invented.length === 0, 'and the UI shows no lane the file does not have',
        JSON.stringify(invented));

  /*
   * 2. EVERY TRACK THAT HAS NOTES SHOWS NOTES.
   *
   * Per track rather than per note, because `notes()` is a viewport WINDOW and
   * scrolling the whole song to account for every note individually would be a slow
   * test that fails for its own reasons. What matters is the failure mode that
   * really happened: a whole track's material silently absent. One note visible on a
   * track that has notes is enough to prove the channel is open for it.
   */
  const withNotes = [...new Set(fileNotes.map((n) => n.track))].sort((a, b) => a - b);
  await page.evaluate(() => window.__uni.setZoom(4));    // the widest window
  await page.waitForTimeout(700);
  const uiTracks = await page.evaluate(() =>
    [...new Set((window.__uni.notes() || []).map((n) => n.tr))]);
  const silent = withNotes.filter((t) => !uiTracks.includes(t));
  check(silent.length === 0,
        'every track the file gives notes to has notes in the UI',
        `file ${JSON.stringify(withNotes)} vs UI ${JSON.stringify(uiTracks.sort((a, b) => a - b))}`);

  /*
   * 3. EVERY CLIP IN THE FILE IS A RAIL.
   *
   * The arrangement's own reachability. A clip the file holds and the arrangement
   * does not draw cannot be moved, trimmed or opened.
   */
  await page.evaluate(() => window.__uni.run('view arrange'));
  await page.waitForTimeout(800);
  /*
   * `c.clip`, NOT `c.id` — and no `??` chain.
   *
   * `clips()` returns `{ id: placementId, clip: clipId }`, and the first version of
   * this read `c.clipId ?? c.clip_id ?? c.id`, which fell all the way through to the
   * PLACEMENT id. Comparing placement ids against clip ids gave a near-miss overlap
   * that looked exactly like one clip lost and one invented.
   *
   * A `??` chain over guessed field names is the same mistake as filtering on a field
   * that does not exist: it finds A number and reports it confidently. The shape is
   * looked up now, and named once.
   */
  const uiClips = await page.evaluate(() => (window.__uni.clips() || []).map((c) => c.clip));
  const filePlacements = fileTracks.flatMap((t) => (t.placements || []).map((p) => p.clip_id));
  const unreachable = filePlacements.filter((id) => !uiClips.includes(id));
  check(unreachable.length === 0, 'every placed clip in the file is drawn',
        `file ${JSON.stringify(filePlacements)} vs UI ${JSON.stringify(uiClips)}`);

  /*
   * 4. EVERY DEVICE IN THE FILE IS A CARD.
   *
   * Walked track by track, because the rack shows one chain at a time. A device the
   * file holds and no card shows is a plugin you cannot bypass, remove or open —
   * which is how a generator playing notes nobody wrote stayed unexplainable for
   * three separate bug reports.
   */
  await page.evaluate(() => window.__uni.run('view tracker'));
  await page.waitForTimeout(400);
  const deviceGaps = [];
  for (const t of fileTracks) {
    const want = (t.device_chain || []).map((d) => d.device_id);
    if (!want.length) continue;
    await page.evaluate((id) => window.__uni.reqChain(id), t.track_id);
    await page.waitForTimeout(500);
    const have = await page.evaluate((id) => {
      const c = window.__uni.chains()[id];
      return ((c && c.devices) || []).map((d) => d.id);
    }, t.track_id);
    for (const id of want) {
      if (!have.includes(id)) deviceGaps.push(`t${t.track_id}/d${id}`);
    }
  }
  check(deviceGaps.length === 0, 'every device in the file is in a published chain',
        JSON.stringify(deviceGaps));
}

check(errors.length === 0, 'and nothing threw while asking', errors.slice(0, 2).join(' | '));

await browser.close();
stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
