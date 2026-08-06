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

// Make the dock tall: open it and see what governs its height.
await page.evaluate(() => { window.__uni.dock(true); });
await page.waitForTimeout(200);
console.log('rdock children heights', await page.evaluate(() => {
  const out = {};
  for (const id of ['harmony', 'inspect', 'pending', 'dock']) {
    const e = document.getElementById(id); const r = e.getBoundingClientRect();
    out[id] = { h: Math.round(r.height), y: Math.round(r.y) };
  }
  const l = document.querySelector('.dk-log').getBoundingClientRect();
  out.log = { h: Math.round(l.height), y: Math.round(l.y) };
  return out;
}));

await page.evaluate(() => { window.__uni.run('tempo'); window.__uni.run('tempo'); window.__uni.run('tempo'); });
await page.waitForTimeout(300);

const L = await page.evaluate(() => {
  const log = document.querySelector('.dk-log');
  const lr = log.getBoundingClientRect();
  for (const el of log.querySelectorAll('.dk-line')) {
    const r = el.getBoundingClientRect();
    if (r.top >= lr.top - 1 && r.bottom <= lr.bottom + 1 && el.textContent.trim())
      return { text: el.textContent, x: r.x, y: r.y, w: r.width, h: r.height };
  }
  return null;
});
console.log('line', JSON.stringify(L));

async function trial(name, fn) {
  await page.evaluate(() => document.getSelection().removeAllRanges());
  await fn();
  await page.waitForTimeout(150);
  console.log(name, JSON.stringify(await page.evaluate(() => String(document.getSelection()))));
}

await trial('slow drag mid-text', async () => {
  await page.mouse.move(L.x + 10, L.y + L.h / 2);
  await page.mouse.down();
  for (let i = 1; i <= 8; i++) {
    await page.mouse.move(L.x + 10 + i * 20, L.y + L.h / 2);
    await page.waitForTimeout(20);
  }
  await page.mouse.up();
});

await trial('drag with vertical spread', async () => {
  await page.mouse.move(L.x + 5, L.y + 1);
  await page.mouse.down();
  await page.mouse.move(L.x + 100, L.y + L.h - 1, { steps: 20 });
  await page.mouse.move(L.x + 220, L.y + L.h - 1, { steps: 20 });
  await page.mouse.up();
});

await trial('dblclick then shift-click', async () => {
  await page.mouse.dblclick(L.x + 20, L.y + L.h / 2);
});

await br.close();
srv.close();
