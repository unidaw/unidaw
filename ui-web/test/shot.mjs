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
  { name: 'typed', setup: async (p) => { for (let i = 0; i < 12; i++) await p.keyboard.press('ArrowDown'); await p.keyboard.press('ArrowRight'); } },
];

let fail = 0;
const ok = (cond, msg) => { console.log(`${cond ? '  PASS' : '  FAIL'}  ${msg}`); if (!cond) fail++; };

const { server, port } = await serve(root);
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1680, height: 980 }, deviceScaleFactor: 2 });
page.on('pageerror', (e) => console.log('  [pageerror]', e.message));
await page.goto(`http://127.0.0.1:${port}/index.html`);
await page.waitForFunction(() => !!window.__uni);

for (const scene of SCENES) {
  console.log(`\n[${scene.name}]`);
  await page.evaluate(() => { window.__uni.setZoom(2); window.__uni.scrollTo(0); });
  await scene.setup(page);
  await page.waitForTimeout(60);

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
  console.log(`  ${p.zoom}  start=${p.startRow}  pool=${p.poolSize}  nodes=${p.domNodes}  clips=${p.clips}`);
}

await browser.close();
server.close();
console.log(`\n${fail === 0 ? 'ALL PASS' : `${fail} FAILURES`}`);
process.exit(fail === 0 ? 0 : 1);
