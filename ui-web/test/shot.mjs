#!/usr/bin/env node
// The agent feedback loop: render, assert on structure, screenshot, diff.
//
//   node test/shot.mjs            render, assert, write test/out/*.png, diff vs baselines
//   node test/shot.mjs --bless    accept the current renders as the baselines
//
// Assertions are on the view-model and the DOM, not on pixels — pixels are the
// second channel, for catching what structure cannot express. A diff prints a
// bounding box, not just a percentage, because a percentage does not tell you
// where the change is.

import { chromium } from 'playwright';
import { resolve, join, dirname, extname, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mkdirSync, existsSync, readFileSync, writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { createServer } from 'node:http';

// ES modules are CORS-blocked over file://, so serve the tree. A real shell
// (Electron/Tauri/a Rust sidecar) provides this; here it is 15 lines.
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json' };
function serve(dir) {
  const s = createServer((req, res) => {
    const p = join(dir, normalize(decodeURI(req.url.split('?')[0])).replace(/^(\.\.[/\\])+/, ''));
    try {
      const body = readFileSync(p.endsWith('/') ? join(p, 'index.html') : p);
      res.writeHead(200, { 'content-type': MIME[extname(p)] || 'application/octet-stream' });
      res.end(body);
    } catch { res.writeHead(404); res.end('not found'); }
  });
  return new Promise((r) => s.listen(0, '127.0.0.1', () => r({ server: s, port: s.address().port })));
}

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');
const OUT = join(here, 'out');
const BASE = join(here, 'baselines');
const BLESS = process.argv.includes('--bless');
mkdirSync(OUT, { recursive: true });
mkdirSync(BASE, { recursive: true });

const SCENES = [
  { name: 'boot', setup: async () => {} },
  { name: 'scrolled', setup: async (p) => p.evaluate(() => window.__uni.scrollTo(4096)) },
  { name: 'deep', setup: async (p) => p.evaluate(() => window.__uni.scrollTo(99000)) },
  { name: 'zoom-aggregate', setup: async (p) => p.evaluate(() => { window.__uni.setZoom(4); window.__uni.scrollTo(512); }) },
  // Lanes that DISAGREE about their grid. The one scene that can tell a correct
  // projection from a plausible one; see __uni.useMixedGrid.
  { name: 'mixed-grid', contour: true,
    setup: async (p) => p.evaluate(() => window.__uni.useMixedGrid()) },
  // Arrange: the same store, a horizontal projection. Includes an audio region,
  // which renders differently and which no other fixture exercises.
  { name: 'arrange', arrange: true, setup: async (p) => p.evaluate(() => window.__uni.useArrangeFixture()) },
  { name: 'arrange-zoomed', arrange: true, setup: async (p) => p.evaluate(() => {
      window.__uni.useArrangeFixture(); window.__uni.arrangeZoom(5); }) },
  // Audio regions with their material drawn inside them. Its own scene rather
  // than a flag on the two above, because the arrange goldens must not move when
  // the waveform changes and vice versa — and because this one asserts on PIXELS,
  // which is the only place a peak renderer's mistakes actually show up.
  { name: 'arrange-waves', arrange: true, waves: true,
    setup: async (p) => p.evaluate(() => window.__uni.useWaveFixture()) },
  { name: 'help', help: true, setup: async (p) => p.evaluate(() => { window.__uni.help(true); }) },
  { name: 'help-piano', help: true, setup: async (p) => p.evaluate(() => {
      window.__uni.view('piano'); window.__uni.help(true); }) },
  { name: 'patcher', patcher: true, setup: async (p) => p.evaluate(() => window.__uni.usePatcherFixture()) },
  { name: 'dock', dock: true, setup: async (p) => p.evaluate(() => window.__uni.useDockFixture()) },
  { name: 'browser', browser: true, setup: async (p) => p.evaluate(() => window.__uni.useBrowserFixture()) },
  // Clips that each carry their own meter, at their own origins. See the fixture
  // in index.html: five lanes, five different questions, and the only place in the
  // repo where a clip's bar 1 is not the song's bar 1.
  { name: 'clip-meters', clipMeters: true,
    setup: async (p) => p.evaluate(() => window.__uni.useClipMeters()) },
  // Driven with REAL keystrokes, which is the whole point of this scene. The
  // palette had a bug where every non-printable key was applied twice — ArrowDown
  // moved the selection two rows, Enter ran a command and then ran whatever had
  // become selected — because the app's keydown listener is in the capture phase
  // and the palette input's own listener runs afterwards on the way back up. Both
  // called feed(). Nothing that drives feed() DIRECTLY can see that: the alloc
  // test's `paletteMove` and any __uni helper both bypass the wiring that was
  // broken. Only a real key press goes through both handlers.
  { name: 'palette', palette: true, setup: async (p) => {
      await p.keyboard.press('Meta+k');
      // WAIT FOR FOCUS before pressing anything. Without this the arrows arrive
      // while the input has not been focused yet, so the event's target is the
      // body, the input's own handler never runs, and only the app's handler acts
      // — one move, the right answer, for entirely the wrong reason. Written
      // without this line the scene passed against the BUGGY build too, which is
      // the failure GUIDELINES 2.1.1 is about: the fixture only tests where you
      // point it, and it was pointed a few milliseconds short of the defect.
      await p.waitForFunction(() => document.activeElement
        && document.activeElement.classList.contains('pl-input'));
      await p.keyboard.press('ArrowDown');
      await p.keyboard.press('ArrowDown');
    } },
  { name: 'piano', piano: true, setup: async (p) => p.evaluate(() => window.__uni.usePianoFixture()) },
  { name: 'piano-zoomed', piano: true, setup: async (p) => p.evaluate(() => {
      window.__uni.usePianoFixture(); window.__uni.pianoZoom(5); }) },
  { name: 'mixer', mixer: true, setup: async (p) => p.evaluate(() => window.__uni.useMixerFixture()) },
  { name: 'typed', setup: async (p) => { for (let i = 0; i < 12; i++) await p.keyboard.press('ArrowDown'); await p.keyboard.press('ArrowRight'); } },
  // The token buffer is the ONLY thing that waits for Enter, and it is only ever
  // opened by `@`. Every other keystroke commits on the keydown.
  { name: 'token-entry', setup: async (p) => { await p.keyboard.press('@'); for (const c of '3^7') await p.keyboard.press(c); } },
  // A token that means nothing STAYS, in the cell it was typed into.
  // ARCHITECTURE_REVIEW Movement 1 item 12: a red cell, never a silent no-op.
  { name: 'bad-token', badToken: true, setup: async (p) => {
      // Scenes share one page, and `token-entry` above deliberately leaves its
      // buffer OPEN. Starting from whatever it left behind made this scene type
      // into that buffer and commit something else entirely — which is how it
      // first "failed": not because the feature was broken, but because the
      // scene was measuring a different keystroke sequence than it described.
      await p.keyboard.press('Escape');
      await p.evaluate(() => window.__uni.goto(0, 0));
      await p.keyboard.press('@');
      for (const c of '99') await p.keyboard.press(c);
      await p.keyboard.press('Enter');
    } },
];

/** Which lanes show their bar readout, as an exact list. */
function assert_shown(got, want) {
  ok(JSON.stringify(got) === JSON.stringify(want),
     `the column appears on exactly the deviating lanes: ${JSON.stringify(got)}`,
     );
}

/** Above this many perceptibly-differing pixels it is a change, not antialiasing. */
const NOISE_PX = 16;

/**
 * Count of pixels differing perceptibly, or null if it could not be measured.
 *
 * NOT `compare -metric AE`. That returns 1.24 MILLION for two frames a human
 * cannot tell apart — the whole image routinely differs by 1/255 per channel
 * from GPU dithering — and adding `-fuzz 1%` made it return 131,070, which
 * contradicted its own 3x3 difference bounding box. I chased a "regression"
 * twice on those numbers.
 *
 * This builds the difference image, thresholds it, and counts what survives:
 * for the same pair, TWO pixels, in one icon's antialiasing. That is a number
 * with a meaning.
 */
function pixelDiff(a, b, out) {
  try {
    const n = execFileSync('magick', [a, b, '-compose', 'difference', '-composite',
      '-colorspace', 'Gray', '-threshold', '2%', '-format', '%[fx:int(mean*w*h)]', 'info:'],
      { stdio: ['ignore', 'pipe', 'pipe'] });
    // Written only when it matters, so a passing run leaves no stale artefact.
    const count = Number(String(n).trim());
    if (count > 0) {
      try {
        execFileSync('magick', ['compare', '-metric', 'AE', a, b, out],
                     { stdio: ['ignore', 'pipe', 'pipe'] });
      } catch (e) { /* compare exits non-zero when images differ; the file is written */ }
    }
    return Number.isFinite(count) ? count : null;
  } catch (e) {
    return null;
  }
}

let fail = 0;
const ok = (cond, msg) => { console.log(`${cond ? '  PASS' : '  FAIL'}  ${msg}`); if (!cond) fail++; };

const { server, port } = await serve(root);
// Rasterisation pinned, because the default is not reproducible. Chrome
// re-rasterises only the tiles it thinks are dirty and reuses what it already
// has, so the same DOM can come out with different antialiasing depending on
// what was invalidated before it — and this UI mutates its surfaces in place
// between scenes by design, which is precisely the case that varies. It showed
// up as `patcher` differing from its own baseline by 23px in five runs out of
// six, bistable rather than drifting, entirely on the rounded corners of one
// pill and two diagonal cable pixels: straight edges land on the same pixel
// either way, so curves were the only witness.
//
// srgb as well, so the goldens do not depend on the colour profile of whichever
// display the machine happens to have attached.
const browser = await chromium.launch({
  channel: 'chrome',
  args: ['--disable-partial-raster', '--disable-gpu-rasterization',
         '--force-color-profile=srgb', '--disable-lcd-text'],
});
const page = await browser.newPage({ viewport: { width: 1680, height: 980 }, deviceScaleFactor: 2 });
page.on('pageerror', (e) => console.log('  [pageerror]', e.message));
await page.goto(`http://127.0.0.1:${port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
// Wait for the vendored fonts to be ready before capturing anything. Without
// this the first scene is shot mid-swap and differs from every later run by a
// fixed 72,299 px — deterministic, so it reads as a real regression rather than
// a harness bug. Font loading is the largest source of screenshot instability;
// it bit the harness itself.
await page.evaluate(() => document.fonts.ready);
// Pin the fixture: a golden that changes depending on whether the sidecar is
// running is not a golden.
await page.evaluate(() => window.__uni.useFixture());
await page.waitForTimeout(120);

/** Capture, then bless or compare. Shared by every surface — a second copy is
 *  how two goldens end up with different staleness rules. */
async function shoot(scene) {
  const png = join(OUT, `${scene.name}.png`);
  await page.screenshot({ path: png });

  const baseline = join(BASE, `${scene.name}.png`);
  if (BLESS || !existsSync(baseline)) {
    writeFileSync(baseline, readFileSync(png));
    console.log(`  ${BLESS ? 'blessed' : 'created'} baseline`);
  } else {
    const a = readFileSync(baseline), b = readFileSync(png);
    if (a.equals(b)) console.log('  PASS  golden byte-identical');
    else {
      // Byte-identical stays the goal, but a handful of perceptibly-differing
      // pixels is antialiasing, not a regression. Failing on that teaches people
      // to re-run rather than to look, which is worse than the noise. Anything
      // above the band still fails, and the count is always printed.
      const px = pixelDiff(baseline, png, join(OUT, `${scene.name}.diff.png`));
      if (px !== null && px <= NOISE_PX) {
        console.log(`  WARN  golden differs by ${px} px — within antialiasing noise`);
        return;
      }
      console.log(`  FAIL  golden differs: ${px === null ? '?' : px} px`
                + `  -> ${join(OUT, `${scene.name}.diff.png`)}`);
      fail++;
    }
  }
}

/**
 * What the waveform layer actually PAINTED, read back off the canvas.
 *
 * Everything below is a pixel assertion on purpose. A waveform is the one surface
 * where the view-model can be perfectly right and the picture perfectly wrong —
 * every mistake this fixture exists to catch (averaging away a transient,
 * storing one magnitude instead of a min and a max, mirroring +/-|peak| around a
 * centre line, downmixing a stereo pair) produces a model that looks correct and
 * a drawing that is not. `useWaveFixture` publishes the same eight one-second
 * sections `tools/gen_audio_fixture.py` writes, so every number here is known in
 * advance rather than blessed from whatever came out.
 *
 * Geometry is derived from the probe rather than hardcoded: the fixture is at
 * 120 BPM and 44,100 Hz, so one second of source is 1,920,000 nanoticks, and
 * everything else follows from `ticksPerPixel` and the device pixel ratio. A test
 * that hardcoded 64 px per second would pass on this machine and lie on any other.
 */
async function waveAssertions(page, a) {
  const w = a.wave;
  ok(w.bound && w.themed, `waveform layer wired and themed: ${w.bound}/${w.themed}`);
  ok(w.wanted > 0 && w.held === w.wanted && !w.incomplete,
     `every window on screen has data: ${w.held}/${w.wanted}`);
  ok(w.repaints > 0 && w.deviceWidth > 0 && w.deviceHeight > 0,
     `canvas painted: ${w.deviceWidth}x${w.deviceHeight} device px, ${w.repaints} repaints`);

  // One second of the fixture's audio, in canvas device pixels.
  const TICKS_PER_AUDIO_SECOND = 1920000;
  const secPx = (TICKS_PER_AUDIO_SECOND / a.ticksPerPixel) * w.dpr;
  const laneDev = w.laneHeight * w.dpr;

  /** The lit rows of one lane-relative column, and where the bands are. */
  const column = (track, clipStartTick, second) => page.evaluate(
    ([track, clipStartTick, second, secPx, tpp, dpr, spanX, laneDev]) => {
      const cv = document.querySelector('.ar-wave');
      const x = Math.round((clipStartTick / tpp - spanX) * dpr + secPx * (second + 0.5));
      const y0 = Math.round(track * laneDev);
      const d = cv.getContext('2d').getImageData(x, y0, 1, Math.round(laneDev)).data;
      const rows = [];
      let r = 0, g = 0, b = 0;
      for (let i = 0; i < Math.round(laneDev); i++) {
        if (d[i * 4 + 3] > 40) { rows.push(i); r = d[i * 4]; g = d[i * 4 + 1]; b = d[i * 4 + 2]; }
      }
      return { x, rows, rgb: [r, g, b] };
    },
    [track, clipStartTick, second, secPx, a.ticksPerPixel, w.dpr, w.spanX, laneDev]);

  const B = 3840000;                       // one bar, as the fixture lays it out
  const mid = laneDev / 2;
  const inset = w.inset;

  // --- mono, track 0, starting at bar 1 -----------------------------------
  const silence = await column(0, 0, 0);
  ok(silence.rows.length === 1 && Math.abs(silence.rows[0] - mid) <= 1,
     `silence is one device pixel at the centre: rows ${JSON.stringify(silence.rows)}, centre ${mid}`);

  const loud = await column(0, 0, 1);
  ok(loud.rows[0] <= inset + 1 && loud.rows[loud.rows.length - 1] >= laneDev - inset - 2,
     `full scale reaches the lane edges: ${loud.rows[0]}..${loud.rows[loud.rows.length - 1]}`
     + ` of ${inset}..${laneDev - inset - 1}`);

  const quiet = await column(0, 0, 2);
  const quietSpan = quiet.rows[quiet.rows.length - 1] - quiet.rows[0] + 1;
  const loudSpan = loud.rows[loud.rows.length - 1] - loud.rows[0] + 1;
  ok(Math.abs(quietSpan / loudSpan - 0.25) < 0.06,
     `a quarter of full scale draws a quarter as tall: ${quietSpan} vs ${loudSpan}`);

  // THE ONE THIS FIXTURE EXISTS FOR. The DC second is a constant +0.5: every
  // sample is above zero, so every drawn pixel must be. A renderer that mirrors
  // +/-|peak| about the centre draws a band straddling it, which is a picture of
  // audio that is not in the file.
  const dc = await column(0, 0, 5);
  ok(dc.rows.length > 0 && dc.rows[dc.rows.length - 1] < mid,
     `DC +0.5 draws entirely ABOVE the centre: rows ${JSON.stringify(dc.rows)}, centre ${mid}`);
  const dcHeight = mid - dc.rows[dc.rows.length - 1];
  ok(Math.abs(dcHeight - (mid - inset) * 0.5) <= 1.5,
     `and at half of full deflection: ${dcHeight} px above centre, half-scale is ${(mid - inset) * 0.5}`);

  // The alternating section has mean 0 and |peak| 1. One magnitude per bucket
  // cannot express it; a min AND a max can.
  const alt = await column(0, 0, 6);
  ok(alt.rows[0] <= inset + 1 && alt.rows[alt.rows.length - 1] >= laneDev - inset - 2,
     `alternating +/-1 spans the lane: ${alt.rows[0]}..${alt.rows[alt.rows.length - 1]}`);

  // --- stereo, track 1, starting at bar 5 ---------------------------------
  // Right is the exact negation of left, so a downmix is identically zero: the
  // loudest second in the file would draw as a flat line.
  const stLoud = await column(1, B * 4, 1);
  const half = (laneDev - inset * 2) / 2;
  const band0 = stLoud.rows.filter((r) => r < mid);
  const band1 = stLoud.rows.filter((r) => r >= mid);
  ok(band0.length >= half - 2 && band1.length >= half - 2,
     `stereo draws TWO full-height bands, not a downmixed line: ${band0.length} + ${band1.length}`
     + ` of ${half} each`);

  const stDc = await column(1, B * 4, 5);
  const up = stDc.rows.filter((r) => r < mid);
  const down = stDc.rows.filter((r) => r >= mid);
  ok(up.length > 0 && down.length > 0, `both channels drawn: ${JSON.stringify(stDc.rows)}`);
  // Mirror images: channel 1 is -channel 0, so its deflection is the same size in
  // the opposite direction. Compared as DISTANCE from the lane's centre line —
  // the two bands are drawn from their own centres and a rectangle with a
  // one-pixel floor cannot land on the exact reflection of another.
  const aboveNear = mid - up[up.length - 1], belowNear = down[0] - mid;
  const aboveFar = mid - up[0], belowFar = down[down.length - 1] - mid;
  ok(Math.abs(aboveNear - belowNear) <= 1 && Math.abs(aboveFar - belowFar) <= 1,
     `the two bands mirror: ${aboveNear}/${aboveFar} above vs ${belowNear}/${belowFar} below`);

  // --- a source that would not decode, track 2, bar 9 ----------------------
  const bad = await column(2, B * 8, 1);
  ok(bad.rows.length > 0, `a failed source is not silently blank: ${bad.rows.length} lit rows`);
  ok(bad.rgb[0] > bad.rgb[2],
     `and it is drawn in the warning colour, not the waveform's: rgb(${bad.rgb.join(',')})`);
  const marked = await page.evaluate(() => document.querySelectorAll('.ar-clip.audio.failed').length);
  ok(marked > 0, `the block says so too: ${marked} clips marked failed`);
}

for (const scene of SCENES) {
  console.log(`\n[${scene.name}]`);
  await page.evaluate(() => window.__uni.reset());
  await scene.setup(page);
  // Wait two animation frames, not a timeout. The renderer deliberately defers
  // overscan binding by one frame on a zoom (GUIDELINES 3.9), so a screenshot
  // taken before that lands catches a partially-bound band — it showed up as
  // zoom-aggregate differing by 129 px roughly one run in three. A timeout only
  // makes that less likely; waiting for the frame makes it impossible.
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  // Fonts again, per scene, not only at boot. `document.fonts.ready` resolves for
  // the faces wanted AT THAT MOMENT, and a face a later scene is the first to use
  // is requested when that scene first paints a glyph in it — so the boot await
  // does not cover it. patcher differed from its own baseline by 23px in five runs
  // out of six: not a fade but a bistable pair of layouts, the difference confined
  // to the rounded corners of the PATCHER pill and two diagonal cable pixels, which
  // is what a sub-pixel width shift looks like when every straight edge still lands
  // on the same pixel. Straight edges hide it; curves are the only witness.
  await page.evaluate(() => document.fonts.ready);
  // Then jump every running CSS transition to its end state. Two frames is ~32ms
  // and `.ch-reject` fades over 120ms, so a scene that ends in a rejection was
  // photographed partway through the fade — same text, different opacity, a
  // different image every run. bad-token differed from ITSELF by ~1900px between
  // two consecutive runs, and blessing it just froze one arbitrary point on the
  // ramp; the next run picked another.
  //
  // Finished, not disabled: the fade is real product behaviour (a rejection that
  // eases in reads as feedback, one that pops reads as a glitch) and a harness
  // that turns off the thing it is photographing tests a UI nobody uses. What the
  // golden should capture is where the transition ENDS, which is deterministic.
  await page.evaluate(() => {
    for (const a of document.getAnimations()) {
      // An infinite animation has no end to jump to and finish() throws on one.
      // There are none today; the guard is so that adding one later degrades to
      // the old flake in one scene instead of throwing and failing every scene.
      try { a.finish(); } catch { /* not finishable — leave it running */ }
    }
  });
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(r)));

  // Arrange is a different surface with a different structure. Asserting tracker
  // cells against it does not weakly pass — it fails on geometry that was never
  // supposed to exist, which is noise. Each surface gets its own assertions.
  if (scene.arrange) {
    const a = await page.evaluate(() => window.__uni.arrangeProbe());
    ok(a !== null, 'arrange model built');
    ok(a.lanes > 0 && a.lanes <= 16, `lanes bounded: ${a.lanes}`);
    ok(a.clips > 0, `clips visible: ${a.clips}`);
    ok(a.audioClips > 0, `audio regions distinguished: ${a.audioClips}`);
    ok(a.rulerTicks > 0 && a.firstBar === 1, `ruler starts at bar 1: ${a.firstBar}, ${a.rulerTicks} ticks`);
    ok(a.domNodes < 1200, `arrange dom bounded: ${a.domNodes}`);
    ok(a.playheadX >= 0, `playhead placed: ${a.playheadX}`);
    console.log(`  ${a.zoom}  ${a.clips} clips  ${a.gridLines} gridlines  ${a.domNodes} nodes`);
    if (scene.waves) await waveAssertions(page, a);
    await shoot(scene);
    continue;
  }

  if (scene.help) {
    const hp = await page.evaluate(() => window.__uni.helpProbe());
    ok(hp.sections === 2, `surface keys and global keys: ${hp.sections} sections`);
    ok(hp.rows > 8, `keys documented: ${hp.rows}`);
    // Every DECLARED key renders. Asserting an exact count instead just means
    // the test fails whenever a key is documented, which teaches you to edit the
    // number rather than to look.
    const km = await page.evaluate(() => window.__uni.keymap());
    ok(hp.rows === km.global + km.surface,
       `overlay renders the whole keymap: ${hp.rows} of ${km.global + km.surface}`);
    const shown = await page.evaluate(() => document.querySelector('.ch-view')?.textContent);
    ok(shown === hp.surface.toUpperCase(), `chrome names the surface: ${shown} vs ${hp.surface}`);
    console.log(`  ${hp.surface}: ${hp.rows} keys in ${hp.sections} sections`);
    await shoot(scene);
    continue;
  }

  if (scene.patcher) {
    const g = await page.evaluate(() => window.__uni.patcherProbe());
    ok(g.nodes === 5 && g.edges === 4, `graph: ${g.nodes} nodes, ${g.edges} edges`);
    ok(g.types.join(',') === 'euclidean,random,lfo,kernel,out', `node types: ${g.types.join(',')}`);
    // A type with no config table shows nothing, not eight anonymous numbers.
    ok(g.configs.length === 3, `only typed configs described: ${g.configs.length}`);
    ok(g.configs[2].startsWith('freq 2.50Hz'), `milli-units scaled: ${g.configs[2]}`);
    const kinds = await page.evaluate(() => ({
      event: document.querySelectorAll('.pt-edge.event').length,
      audio: document.querySelectorAll('.pt-edge.audio').length,
      control: document.querySelectorAll('.pt-edge.control').length,
    }));
    ok(kinds.event === 2 && kinds.audio === 1 && kinds.control === 1,
       `edge kinds drawn apart: ${JSON.stringify(kinds)}`);
    console.log(`  ${g.nodes} nodes, ${g.edges} edges, v${g.version}`);
    await shoot(scene);
    continue;
  }

  if (scene.dock) {
    const d = await page.evaluate(() => window.__uni.dockProbe());
    ok(d.lines === 12, `log lines: ${d.lines}`);
    ok(d.commands.includes('note') && d.commands.includes('view') && d.commands.includes('gain'),
       `command grammar covers edit, view and mix: ${d.commands.length} commands`);
    const kinds = await page.evaluate(() => ({
      in: document.querySelectorAll('.dk-line.in').length,
      out: document.querySelectorAll('.dk-line.out').length,
      err: document.querySelectorAll('.dk-line.err').length,
    }));
    // Typed, returned and failed are three different things. A console that
    // renders them alike is a log you have to re-read to understand.
    ok(kinds.in === 5 && kinds.out === 5 && kinds.err === 2,
       `line kinds distinguished: ${JSON.stringify(kinds)}`);
    console.log(`  ${d.lines} lines, ${d.commands.length} commands`);
    await shoot(scene);
    continue;
  }

  if (scene.clipMeters) {
    const lanes = await page.evaluate(() => {
      const rows = [...document.querySelectorAll('.tk-row')]
        .sort((a, b) => Number(a.dataset.row) - Number(b.dataset.row));
      const read = (r, t) => {
        const row = rows.find((x) => Number(x.dataset.row) === r);
        if (!row) return null;
        const el = row.querySelector(`.tk-lane-bar[data-track="${t}"]`);
        return el ? { text: el.textContent,
                      bar: el.parentElement.classList.contains('lbar') } : null;
      };
      return { t0r1: read(1, 0), t0r17: read(17, 0), t1r2: read(2, 1), t1r14: read(14, 1),
               t2r0: read(0, 2), t3gap: read(18, 3), t3r2: read(2, 3), t4r0: read(0, 4),
               // Which lanes actually SHOW the column, measured rather than read
               // off a class: an element with display:none still reports its
               // textContent, so every assertion above passes on a hidden lane.
               shown: [0, 1, 2, 3, 4].map((t) => {
                 const e = document.querySelector(`.tk-lane-bar[data-track="${t}"]`);
                 return e ? (e.getBoundingClientRect().width > 0 ? 1 : 0) : -1;
               }),
               // Track boxes, to prove they are genuinely different widths now.
               trackW: [0, 1, 2, 3, 4].map((t) => {
                 const e = document.querySelector(`.tk-row .tk-track:nth-child(${t + 3})`);
                 return e ? Math.round(e.getBoundingClientRect().width) : -1;
               }),
               width: (() => { const e = document.querySelector('.tk-lane-bar[data-track="0"]');
                               return e ? Math.round(e.getBoundingClientRect().width) : -1; })() };
    });
    // A 4/4 clip starting a 1/16 in: its bar 1 beat 1 is row 1, not row 0. A build
    // that reprinted the song gutter per lane would say "1:1" at row 0.
    ok(lanes.t0r1 && lanes.t0r1.text === '1:1' && lanes.t0r1.bar,
       `lane 0 starts its own bar 1 where the CLIP starts: ${JSON.stringify(lanes.t0r1)}`);
    ok(lanes.t0r17 && lanes.t0r17.text === '2:1',
       `and its bar 2 is 16 rows on, in 4/4: ${JSON.stringify(lanes.t0r17)}`);
    // 3/4 at a different origin: bar 2 arrives 12 rows in, not 16.
    ok(lanes.t1r2 && lanes.t1r2.text === '1:1' && lanes.t1r2.bar,
       `lane 1 has its own origin: ${JSON.stringify(lanes.t1r2)}`);
    ok(lanes.t1r14 && lanes.t1r14.text === '2:1',
       `and counts 3/4, so bar 2 is 12 rows on: ${JSON.stringify(lanes.t1r14)}`);
    // grid null means "count me in the SONG's meter", which here is 7/8 — a
    // different answer from 4/4, which is the whole reason the sentinel exists.
    ok(lanes.t2r0 && lanes.t2r0.text === '1:1',
       `a clip with no grid still reads: ${JSON.stringify(lanes.t2r0)}`);
    // The gap between two clips, and the audio lane, both read blank.
    // Track 3 holds two clips with a gap: rows 0-15 are the first, 16-23 are
    // nothing, 24 on are the second. The gap is the case that proves the readout
    // follows the CLIPS and not the track.
    ok(lanes.t3r2 && lanes.t3r2.text !== '',
       `track 3 reads inside its first clip: ${JSON.stringify(lanes.t3r2)}`);
    ok(lanes.t3gap && lanes.t3gap.text === '',
       `and says nothing in the gap between them: ${JSON.stringify(lanes.t3gap)}`);
    ok(lanes.t4r0 && lanes.t4r0.text === '',
       `nor does an audio region, whose meter is an engine default: ${JSON.stringify(lanes.t4r0)}`);
    // THE POINT OF THE COLUMN BEING CONDITIONAL. A lane earns it by disagreeing
    // with the gutter, in one of two ways: a different meter, or an origin that is
    // not on a song bar. The song here is 7/8.
    //   0  4/4 at tick 240000 — same-length bars in the wrong place, and a
    //      meter-only test would wrongly call this "no deviation"
    //   1  3/4 — a different meter outright
    //   2  no grid, origin 0 — genuinely the song's own bars, so no column
    //   3  a second clip at 5,760,000, which is not a multiple of a 7/8 bar
    //   4  audio, whose packed meter is an engine default
    assert_shown(lanes.shown, [1, 1, 0, 1, 0]);
    // And the tracks are therefore NOT all one width, which is what forced the
    // renderer's single `trackStride` to become a per-track array.
    ok(lanes.trackW[0] !== lanes.trackW[2],
       `tracks are no longer a uniform width: ${JSON.stringify(lanes.trackW)}`);
    ok(lanes.width === 40, `a shown readout is its full width: ${lanes.width}px`);

    // THE HEADER MUST FOLLOW. Non-uniform track widths are exactly the case where
    // a header built from one shared stride drifts: every header after the first
    // deviating track sits 40px out, and it compounds. Compared as left edges,
    // which is what a user sees line up or not.
    const align = await page.evaluate(() => {
      const row = document.querySelector('.tk-row');
      const trs = [...row.querySelectorAll('.tk-track')];
      const hds = [...document.querySelectorAll('.htrack')];
      const out = [];
      for (let t = 0; t < Math.min(trs.length, hds.length); t++) {
        out.push(Math.round(hds[t].getBoundingClientRect().left
                            - trs[t].getBoundingClientRect().left));
      }
      return out;
    });
    const worst = align.reduce((a, b) => Math.max(a, Math.abs(b)), 0);
    ok(worst <= 1, `every header sits over its track: worst drift ${worst}px`,
       JSON.stringify(align));

    // AND THE HIT TEST. It used to divide by one stride; with ragged tracks it has
    // to find the track by its measured box, and subtract that track's OWN readout
    // width before resolving a column. Driven through the real hitTest with real
    // coordinates taken off the rendered cells, so it cannot agree with the
    // renderer by sharing its arithmetic.
    const hits = await page.evaluate(() => {
      const probe = (track, col) => {
        const c = document.querySelector(
          `.tk-row[data-row="4"] .tk-cell[data-track="${track}"][data-col="${col}"]`);
        if (!c) return null;
        const b = c.getBoundingClientRect();
        const h = window.__uni.clickAt(b.left + b.width / 2, b.top + b.height / 2);
        return h ? `${h.track},${h.col}` : 'null';
      };
      // A lane WITH a readout and a lane without, so a single-stride hit test
      // cannot pass both.
      const withCol = [probe(1, 0), probe(1, 2)];
      const without = [probe(2, 0), probe(2, 2)];
      // And the readout itself: pointing at a bar number is not pointing at a cell.
      const lb = document.querySelector('.tk-row[data-row="4"] .tk-lane-bar[data-track="1"]');
      const r = lb.getBoundingClientRect();
      const onReadout = window.__uni.clickAt(r.left + r.width / 2, r.top + r.height / 2);
      return { withCol, without, onReadout: onReadout ? `${onReadout.track},${onReadout.col}` : 'null' };
    });
    ok(hits.withCol.join(' ') === '1,0 1,2',
       `a click lands in the right cell on a lane WITH a readout: ${JSON.stringify(hits.withCol)}`);
    ok(hits.without.join(' ') === '2,0 2,2',
       `and on a lane without one: ${JSON.stringify(hits.without)}`);
    ok(hits.onReadout === 'null',
       `a click on the readout is not a click on a cell: ${hits.onReadout}`);
  }

  if (scene.palette) {
    const pal = await page.evaluate(() => {
      const items = [...document.querySelectorAll('.pl-item')];
      const sel = document.querySelector('.pl-item.sel');
      return { count: items.length, index: sel ? items.indexOf(sel) : -1,
               open: !!document.querySelector('.pl-item') };
    });
    ok(pal.open, 'the palette opened on the app shortcut');
    ok(pal.count > 0, `commands listed: ${pal.count}`);
    // TWO presses, so the selection must be on row 2. This is the assertion that
    // catches the double-apply: it landed on row 4, which reads as "a list that
    // scrolls fast" rather than as a bug, and is exactly the kind of wrong a
    // screenshot cannot report. It is stated as the count of presses rather than
    // as a literal so the intent survives a change to how many rows are visible.
    const presses = 2;
    ok(pal.index === presses,
       `${presses} presses move the selection ${presses} rows, not ${presses * 2}: row ${pal.index}`);
  }

  if (scene.browser) {
    const bp = await page.evaluate(() => window.__uni.browserProbe());
    ok(bp.items.length === 5, `projects listed: ${bp.items.length}`);
    ok(bp.selected === 2, `selection: ${bp.selected}`);
    ok(bp.current === 'maximal', `loaded project marked: ${bp.current}`);
    const marks = await page.evaluate(() => ({
      sel: document.querySelectorAll('.br-item.sel').length,
      cur: document.querySelectorAll('.br-item.cur').length,
    }));
    // These are different things — the highlighted row is not necessarily the
    // loaded one — and a rail that conflates them tells you nothing.
    ok(marks.sel === 1 && marks.cur === 1, `selection and loaded drawn apart: ${JSON.stringify(marks)}`);
    // Save-as is a mode inside the rail, not a dialog, so it has to be visible
    // as a mode rather than as the list looking slightly different.
    const saving = await page.evaluate(() => { window.__uni.browserProbe();
      return document.querySelectorAll('.br-save.on').length; });
    ok(saving === 0, 'save field hidden until asked for');
    console.log(`  ${bp.items.length} projects, selected ${bp.selected}, current ${bp.current}`);
    await shoot(scene);
    continue;
  }

  if (scene.piano) {
    const q = await page.evaluate(() => window.__uni.pianoProbe());
    ok(q !== null, 'piano model built');
    ok(q.notes === 13, `all 13 notes drawn: ${q.notes}`);
    ok(q.pitchRange && q.pitchRange[0] === 52 && q.pitchRange[1] === 79,
       `pitch range ${JSON.stringify(q.pitchRange)}`);
    ok(q.keys > 40, `keyboard drawn: ${q.keys} keys`);
    ok(q.playheadX > 0, `playhead placed: ${q.playheadX}`);
    ok(q.domNodes < 1500, `piano dom bounded: ${q.domNodes}`);
    const styled = await page.evaluate(() => ({
      muted: document.querySelectorAll('.pr-note.muted').length,
      add: document.querySelectorAll('.pr-note.add').length,
      sel: document.querySelectorAll('.pr-note.sel').length,
    }));
    ok(styled.muted === 1 && styled.add === 1 && styled.sel === 1,
       `provenance styling: ${JSON.stringify(styled)}`);
    console.log(`  ${q.zoom}  ${q.notes} notes  ${q.keys} keys  ${q.domNodes} nodes`);
    await shoot(scene);
    continue;
  }

  if (scene.mixer) {
    const m = await page.evaluate(() => window.__uni.mixerProbe());
    ok(m !== null, 'mixer model built');
    ok(m.strips === 8, `strips: ${m.strips}`);
    ok(m.authoritative === true, 'mixer reads the engine, not local state');
    ok(m.detail[3].mute && !m.detail[3].solo, 'mute flag on T04');
    ok(m.detail[4].solo && !m.detail[4].dim, 'solo on T05 and not dimmed');
    ok(m.detail[0].dim, 'unsoloed strips dimmed while something is soloed');
    ok(m.detail[2].pan === 'L40' && m.detail[5].pan === 'R60', 'pan labels from the engine');
    ok(m.detail[0].meter > m.detail[3].meter, 'meters map level to height');
    // The scope is a canvas, so assert on the ring and on painted pixels rather
    // than on the DOM — there is no DOM to assert on, which is the point.
    const sc = await page.evaluate(() => window.__uni.feedScope(600));
    ok(sc && sc.head > 0, `scope ring advanced: head ${sc && sc.head}`);
    const ink = await page.evaluate(() => {
      const c = document.querySelector('.mx-scope');
      const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
      let lit = 0;
      for (let i = 0; i < d.length; i += 4) if (d[i + 2] > 90) lit++;
      return lit;
    });
    ok(ink > 1000, `scope actually painted: ${ink} lit pixels`);
    console.log(`  ${m.strips} strips  dB ${m.detail.slice(0,3).map(s=>s.db).join(' ')}`);
    await shoot(scene);
    continue;
  }

  const p = await page.evaluate(() => {
    const q = window.__uni.probe();
    return {
      startRow: q.startRow, poolSize: q.poolSize, domNodes: q.domNodes, zoom: q.zoom,
      tracks: q.tracks, columns: q.columns, cursor: q.cursor, clips: q.clips,
      // read a cell the same way an agent would
      sample: q.cellText(q.startRow + 3, 2, 0),
      rect: q.cellRect(q.startRow + 3, 2, 0),
    };
  });

  // Structural assertions — these are the ones that catch logic bugs.
  ok(p.domNodes < 12000, `dom nodes bounded: ${p.domNodes}`);
  ok(p.poolSize < 90, `pool is viewport-sized, not timeline-sized: ${p.poolSize}`);
  ok(p.rect !== null && p.rect.w > 0, `cell (${p.startRow + 3},2,0) has geometry: ${JSON.stringify(p.rect)}`);
  ok(typeof p.sample === 'string', `cell text readable via data attributes: ${JSON.stringify(p.sample)}`);

  if (scene.contour) {
    /**
     * The pitch ribbon's marks sit at a height PROPORTIONAL TO PITCH.
     *
     * A screenshot cannot tell a ribbon that tracks the melody from one that
     * draws every mark at the same height — both look like a tidy column of
     * ticks — and the second is what you get if the mark is decorative. So this
     * reads the marks back with the pitch of the cell they are in and asserts
     * the two agree in ORDER: higher note, higher mark.
     *
     * Order rather than exact values, because the mapping clamps MIDI 24..96 and
     * rounds to whole percent, so two nearby pitches can legitimately land on the
     * same percent. Monotonicity is the property that makes the ribbon readable;
     * exact heights are an implementation detail.
     */
    const ribbon = await page.evaluate(() => {
      const vm = window.__uni.probe();
      const out = [];
      for (const cellEl of document.querySelectorAll('.tk-cell[data-kind="note"]')) {
        const bar = cellEl.querySelector('.tk-bar');
        if (!bar || bar.style.display === 'none') continue;
        const text = cellEl.textContent.trim();
        if (!text) continue;
        out.push({ text, bottom: parseFloat(bar.style.bottom) });
        if (out.length >= 24) break;
      }
      return { marks: out, tracks: vm.tracks };
    });
    ok(ribbon.marks.length > 8, `notes carry a pitch mark: ${ribbon.marks.length}`);
    // Turn each note name back into a pitch and check the marks agree in order.
    const NOTES = { 'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11 };
    const pitchOfName = (t) => {
      const m = /^([A-G])([#-])(-?\d)$/.exec(t);
      if (!m) return null;
      return 12 * (Number(m[3]) + 1) + NOTES[m[1]] + (m[2] === '#' ? 1 : 0);
    };
    const pairs = ribbon.marks.map((m) => ({ p: pitchOfName(m.text), b: m.bottom }))
                              .filter((x) => x.p !== null);
    ok(pairs.length > 8, `note names parsed back to pitches: ${pairs.length}`);
    let inverted = 0;
    for (let i = 0; i < pairs.length; i++) {
      for (let j = i + 1; j < pairs.length; j++) {
        if (pairs[i].p < pairs[j].p && pairs[i].b > pairs[j].b) inverted++;
        if (pairs[i].p > pairs[j].p && pairs[i].b < pairs[j].b) inverted++;
      }
    }
    ok(inverted === 0, `every mark is at or above every lower note's mark: ${inverted} inversions`);
    // ...and it must actually VARY, or a constant height would pass the order
    // check vacuously. That is the failure this whole assertion exists for.
    const lows = new Set(pairs.map((x) => x.b));
    ok(lows.size > 2, `and the marks are at different heights: ${lows.size} distinct`);
  }

  if (scene.badToken) {
    /**
     * A rejected token is a RED CELL, at the cursor, holding what was typed.
     *
     * Asserted rather than left to the picture, because a screenshot cannot tell
     * "the cell is marked" from "the cell shows what was there before" — which is
     * the whole failure this replaces. Before this, an unrecognised token put a
     * sentence in the chrome and left the grid untouched, so the evidence was
     * gone the moment you looked away from a status line.
     */
    const bad = await page.evaluate(() => {
      const els = [...document.querySelectorAll('.tk-cell[data-kind="bad"]')];
      const st = window.__uni.state();
      return {
        count: els.length,
        text: els[0] ? els[0].textContent : null,
        track: els[0] ? Number(els[0].dataset.track) : -1,
        col: els[0] ? Number(els[0].dataset.col) : -1,
        row: els[0] ? Number(els[0].closest('.tk-row').dataset.row) : -1,
        cursor: { row: st.cursor.row, track: st.cursor.track, col: st.cursor.col },
        reject: document.querySelector('.ch-reject')?.textContent ?? '',
      };
    });
    ok(bad.count === 1, `exactly one cell is marked bad: ${bad.count}`);
    // The buffer keeps the '@' that opened it, so that is what was typed.
    ok(bad.text === '@99', `and it holds what was typed: ${JSON.stringify(bad.text)}`);
    // At the CURSOR, which stays put so the thing to fix is under the thing that
    // is blinking.
    ok(bad.row === bad.cursor.row && bad.track === bad.cursor.track
       && bad.col === bad.cursor.col,
       `at the cursor: ${bad.row},${bad.track},${bad.col} vs ` +
       `${bad.cursor.row},${bad.cursor.track},${bad.cursor.col}`);
    ok(bad.reject.length > 0, `and the chrome says why: ${JSON.stringify(bad.reject)}`);
  }

  /**
   * Each track's header names THAT track.
   *
   * It did not. `paintHeadLpb` walked `headRow.children[t + 1]`, which was right
   * when the gutter was the only fixed column and wrong the moment the harmony
   * header was added beside it: every header then carried the NEXT track's name
   * and the last one was left blank. It survived because the goldens were
   * blessed with it in place, and a picture of sixteen plausible labels looks
   * exactly like a picture of sixteen correct ones.
   *
   * So this asserts the correspondence rather than the appearance. It compares
   * against the tracker's own track list, not against a hardcoded "T01" — the
   * engine publishes real names and the header must follow them.
   */
  const heads = await page.evaluate(() => ({
    labels: [...document.querySelectorAll('#head .htrack')].map((e) => e.textContent.trim()),
    names: window.__uni.probe().tracks,
  }));
  ok(heads.labels.length === heads.names, `a header per track: ${heads.labels.length}`);
  ok(heads.labels.every((s) => s.length > 0), `no header left unnamed: ${JSON.stringify(heads.labels.slice(-2))}`);
  ok(heads.labels.every((s, i) => s.startsWith(`T${String(i + 1).padStart(2, '0')}`)),
     `header n names track n: ${JSON.stringify(heads.labels.slice(0, 3))}`);

  await shoot(scene);

}

await browser.close();
server.close();
console.log(`\n${fail === 0 ? 'ALL PASS' : `${fail} FAILURES`}`);
process.exit(fail === 0 ? 0 : 1);
