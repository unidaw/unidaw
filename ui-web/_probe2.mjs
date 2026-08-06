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
const page = await br.newPage({ viewport: { width: 1500, height: 900 } });
page.on('pageerror', (e) => console.log('PAGEERROR', e.message));
await page.goto(`http://127.0.0.1:${port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
await page.waitForTimeout(400);

await page.evaluate(() => { window.__uni.dock(true); window.__uni.run('tempo'); });
await page.waitForTimeout(300);

const vis = await page.evaluate(() => {
  const log = document.querySelector('.dk-log');
  const lr = log.getBoundingClientRect();
  const out = [];
  for (const el of log.querySelectorAll('.dk-line')) {
    const r = el.getBoundingClientRect();
    if (r.top >= lr.top && r.bottom <= lr.bottom && el.textContent.trim())
      out.push({ text: el.textContent, x: r.x, y: r.y, w: r.width, h: r.height });
  }
  return { logRect: { x: lr.x, y: lr.y, w: lr.width, h: lr.height }, lines: out.slice(0, 5), count: out.length };
});
console.log('visible lines', JSON.stringify(vis, null, 1));

const L = vis.lines[0];
if (L) {
  await page.mouse.move(L.x + 2, L.y + L.h / 2);
  await page.mouse.down();
  await page.mouse.move(L.x + Math.min(L.w - 4, 200), L.y + L.h / 2, { steps: 10 });
  await page.waitForTimeout(60);
  await page.mouse.up();
  await page.waitForTimeout(120);
  console.log('drag selection:', JSON.stringify(await page.evaluate(() => String(document.getSelection()))));

  await page.mouse.dblclick(L.x + 20, L.y + L.h / 2);
  await page.waitForTimeout(120);
  console.log('dblclick selection:', JSON.stringify(await page.evaluate(() => String(document.getSelection()))));
}

console.log('programmatic:', JSON.stringify(await page.evaluate(() => {
  const el = document.querySelector('.dk-line');
  const r = document.createRange();
  r.selectNodeContents(el);
  const s = document.getSelection();
  s.removeAllRanges(); s.addRange(r);
  return String(s);
})));

console.log('computed on app/dk/log/line:', await page.evaluate(() => {
  const g = (sel) => { const e = document.querySelector(sel); return e ? getComputedStyle(e).userSelect : null; };
  return { app: g('#app'), rdock: g('#rdock'), dk: g('.dk'), log: g('.dk-log'), line: g('.dk-line') };
}));

await br.close();
srv.close();
