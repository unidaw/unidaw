import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { join, extname, normalize } from 'node:path';

const root = '/Users/jak/src/daw-web/ui-web';
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.woff2': 'font/woff2' };
const srv = createServer((q, r) => {
  const p = join(root, normalize(decodeURI(q.url.split('?')[0])));
  let b; try { b = readFileSync(p); } catch { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': MIME[extname(p)] || 'application/octet-stream', 'cache-control': 'no-store' }); r.end(b);
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const port = srv.address().port;

const br = await chromium.launch({ channel: 'chrome' });
const ctx = await br.newContext({ viewport: { width: 1500, height: 1200 } });
await ctx.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: `http://127.0.0.1:${port}` });
const page = await ctx.newPage();
page.on('pageerror', (e) => console.log('PAGEERROR', e.message));
await page.goto(`http://127.0.0.1:${port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
await page.waitForTimeout(400);
await page.evaluate(() => { window.__uni.dock(true); window.__uni.run('tempo'); window.__uni.run('tempo'); });
await page.waitForTimeout(300);

/** The visible transcript lines, freshly measured. */
const lines = () => page.evaluate(() => {
  const log = document.querySelector('.dk-log');
  const lr = log.getBoundingClientRect();
  return [...log.querySelectorAll('.dk-line')]
    .filter((e) => e.style.display !== 'none' && e.textContent.trim())
    .map((e) => { const r = e.getBoundingClientRect(); return { t: e.textContent, x: r.x, y: r.y, w: r.width, h: r.height }; })
    .filter((r) => r.y >= lr.top && r.y + r.h <= lr.bottom);
});

const L = await lines();
console.log('lines', L.map(l => [l.t, l.x, l.y]));
const a = L[0], b = L[L.length - 1];
await page.mouse.move(a.x + 8, a.y + a.h / 2);
await page.mouse.down();
await page.mouse.move(a.x + 60, a.y + a.h / 2, { steps: 5 });
await page.waitForTimeout(40);
await page.mouse.move(b.x + b.w - 20, b.y + b.h / 2, { steps: 15 });
await page.waitForTimeout(40);
await page.mouse.up();
await page.waitForTimeout(150);
console.log('selection:', JSON.stringify(await page.evaluate(() => String(document.getSelection()))));
console.log('selectedText():', JSON.stringify(await page.evaluate(() => window.__uni.selectedText())));

await page.keyboard.press('Meta+c');
await page.waitForTimeout(250);
console.log('reject:', await page.evaluate(() => window.__uni.state().reject));
console.log('clipboard:', JSON.stringify(await page.evaluate(() => navigator.clipboard.readText())));

// stale selection across a click on a non-selectable surface
await page.evaluate(() => window.__uni.setView('tracker'));
await page.waitForTimeout(250);
const tb = await page.evaluate(() => { const r = document.getElementById('tracker').getBoundingClientRect(); return { x: r.x, y: r.y }; });
await page.mouse.click(tb.x + 150, tb.y + 150);
await page.waitForTimeout(200);
console.log('selection after tracker click:', JSON.stringify(await page.evaluate(() => String(document.getSelection()))));
await page.keyboard.press('Meta+c');
await page.waitForTimeout(150);
console.log('reject after tracker cmd+c:', await page.evaluate(() => window.__uni.state().reject));

await br.close();
srv.close();
