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
  { name: 'mixed-grid', setup: async (p) => p.evaluate(() => window.__uni.useMixedGrid()) },
  // Arrange: the same store, a horizontal projection. Includes an audio region,
  // which renders differently and which no other fixture exercises.
  { name: 'arrange', arrange: true, setup: async (p) => p.evaluate(() => window.__uni.useArrangeFixture()) },
  { name: 'arrange-zoomed', arrange: true, setup: async (p) => p.evaluate(() => {
      window.__uni.useArrangeFixture(); window.__uni.arrangeZoom(5); }) },
  { name: 'help', help: true, setup: async (p) => p.evaluate(() => { window.__uni.help(true); }) },
  { name: 'help-piano', help: true, setup: async (p) => p.evaluate(() => {
      window.__uni.view('piano'); window.__uni.help(true); }) },
  { name: 'patcher', patcher: true, setup: async (p) => p.evaluate(() => window.__uni.usePatcherFixture()) },
  { name: 'dock', dock: true, setup: async (p) => p.evaluate(() => window.__uni.useDockFixture()) },
  { name: 'browser', browser: true, setup: async (p) => p.evaluate(() => window.__uni.useBrowserFixture()) },
  { name: 'piano', piano: true, setup: async (p) => p.evaluate(() => window.__uni.usePianoFixture()) },
  { name: 'piano-zoomed', piano: true, setup: async (p) => p.evaluate(() => {
      window.__uni.usePianoFixture(); window.__uni.pianoZoom(5); }) },
  { name: 'mixer', mixer: true, setup: async (p) => p.evaluate(() => window.__uni.useMixerFixture()) },
  { name: 'typed', setup: async (p) => { for (let i = 0; i < 12; i++) await p.keyboard.press('ArrowDown'); await p.keyboard.press('ArrowRight'); } },
  // The token buffer is the ONLY thing that waits for Enter, and it is only ever
  // opened by `@`. Every other keystroke commits on the keydown.
  { name: 'token-entry', setup: async (p) => { await p.keyboard.press('@'); for (const c of '3^7') await p.keyboard.press(c); } },
];

let fail = 0;
const ok = (cond, msg) => { console.log(`${cond ? '  PASS' : '  FAIL'}  ${msg}`); if (!cond) fail++; };

const { server, port } = await serve(root);
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 }, deviceScaleFactor: 2 });
page.on('pageerror', (e) => console.log('  [pageerror]', e.message));
await page.goto(`http://127.0.0.1:${port}/index.html`);
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
      // localise it — a percentage does not say where
      try {
        const out = execFileSync('magick', ['compare', '-metric', 'AE', baseline, png, join(OUT, `${scene.name}.diff.png`)], { stdio: ['ignore', 'pipe', 'pipe'] });
        console.log(`  FAIL  golden differs: ${out}`);
      } catch (e) {
        const px = String(e.stderr || e.stdout || '').trim();
        console.log(`  FAIL  golden differs: ${px} px  -> ${join(OUT, `${scene.name}.diff.png`)}`);
      }
      fail++;
    }
  }
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

  await shoot(scene);

}

await browser.close();
server.close();
console.log(`\n${fail === 0 ? 'ALL PASS' : `${fail} FAILURES`}`);
process.exit(fail === 0 ? 0 : 1);
