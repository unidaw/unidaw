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

  if (scene.mixer) {
    const m = await page.evaluate(() => window.__uni.mixerProbe());
    ok(m !== null, 'mixer model built');
    ok(m.strips === 8, `strips: ${m.strips}`);
    ok(m.authoritative === false, 'mixer declares itself non-authoritative');
    ok(m.detail[3].mute && !m.detail[3].solo, 'mute flag on T04');
    ok(m.detail[4].solo && !m.detail[4].dim, 'solo on T05 and not dimmed');
    ok(m.detail[0].dim, 'unsoloed strips dimmed while something is soloed');
    ok(m.detail[2].pan === 'L40' && m.detail[5].pan === 'R60', 'pan labels');
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
