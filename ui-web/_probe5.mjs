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
const page = await ctx.newPage();
page.on('pageerror', (e) => console.log('PAGEERROR', e.message));
await page.goto(`http://127.0.0.1:${port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
await page.waitForTimeout(400);
await page.evaluate(() => { window.__uni.dock(true); window.__uni.run('tempo'); window.__uni.run('tempo'); });
await page.waitForTimeout(300);

const info = await page.evaluate(() => {
  const log = document.querySelector('.dk-log');
  const lr = log.getBoundingClientRect();
  const lines = [...log.querySelectorAll('.dk-line')].filter(e => e.style.display !== 'none')
    .map(e => { const r = e.getBoundingClientRect(); return { t: e.textContent, x: r.x, y: r.y, w: r.width, h: r.height }; });
  return { lr: { x: lr.x, y: lr.y, w: lr.width, h: lr.height }, lines: lines.slice(0, 6),
           pad: getComputedStyle(log).padding, scrollTop: log.scrollTop, scrollH: log.scrollHeight };
});
console.log(JSON.stringify(info, null, 1));

const at = (x, y) => page.evaluate(([x, y]) => {
  const e = document.elementFromPoint(x, y);
  return e ? (e.className || e.tagName) : null;
}, [x, y]);

console.log('at logTL', await at(info.lr.x + 3, info.lr.y + 3));
console.log('at first line mid', await at(info.lines[0].x + 20, info.lines[0].y + 5));
console.log('at bottom empty', await at(info.lr.x + info.lr.w - 6, info.lr.y + info.lr.h - 4));

async function drag(name, x0, y0, x1, y1) {
  await page.evaluate(() => document.getSelection().removeAllRanges());
  await page.mouse.move(x0, y0);
  await page.mouse.down();
  await page.mouse.move((x0 + x1) / 2, (y0 + y1) / 2, { steps: 10 });
  await page.waitForTimeout(30);
  await page.mouse.move(x1, y1, { steps: 10 });
  await page.waitForTimeout(30);
  await page.mouse.up();
  await page.waitForTimeout(120);
  const s = await page.evaluate(() => {
    const sel = document.getSelection();
    return { text: String(sel), anchor: sel.anchorNode ? (sel.anchorNode.nodeName + ':' + String(sel.anchorNode.textContent).slice(0, 20)) : null };
  });
  console.log(name, JSON.stringify(s));
}

const L0 = info.lines[0], L1 = info.lines[1] || L0;
await drag('logTL -> bottom', info.lr.x + 3, info.lr.y + 3, info.lr.x + info.lr.w - 6, info.lr.y + info.lr.h - 4);
await drag('inside line0 -> inside line1', L0.x + 6, L0.y + L0.h / 2, L1.x + 120, L1.y + L1.h / 2);
await drag('line0 start -> line1 end', L0.x + 1, L0.y + 1, L1.x + L1.w - 2, L1.y + L1.h - 1);

await br.close();
srv.close();
