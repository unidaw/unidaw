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
const errs = [];
page.on('pageerror', (e) => errs.push(e.message));
await page.goto(`http://127.0.0.1:${port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
await page.waitForTimeout(400);

await page.evaluate(() => window.__uni.setView('piano'));
await page.waitForTimeout(300);
console.log('state', await page.evaluate(() => { const s = window.__uni.state(); return { view: s.view, low: s.pianoLow, start: s.pianoStart, follow: s.followPlayhead }; }));
const box = await page.evaluate(() => { const r = document.getElementById('piano').getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; });
console.log('piano box', box);

await page.mouse.move(box.x + box.w / 2, box.y + box.h / 2);
await page.mouse.wheel(0, 200);
await page.waitForTimeout(200);
console.log('after wheel down', await page.evaluate(() => { const s = window.__uni.state(); return { low: s.pianoLow, start: s.pianoStart }; }));

await page.mouse.wheel(0, -400);
await page.waitForTimeout(200);
console.log('after wheel up', await page.evaluate(() => { const s = window.__uni.state(); return { low: s.pianoLow, start: s.pianoStart }; }));

await page.keyboard.down('Shift');
await page.mouse.wheel(0, 200);
await page.keyboard.up('Shift');
await page.waitForTimeout(200);
console.log('after shift wheel', await page.evaluate(() => { const s = window.__uni.state(); return { low: s.pianoLow, start: s.pianoStart }; }));

await page.evaluate(() => { window.__uni.dock(true); window.__uni.run('help'); });
await page.waitForTimeout(300);
const dbox = await page.evaluate(() => {
  const el = document.querySelector('.dk-log');
  const r = el.getBoundingClientRect();
  const line = el.querySelector('.dk-line');
  const lr = line.getBoundingClientRect();
  return { r: { x: r.x, y: r.y, w: r.width, h: r.height }, us: getComputedStyle(el).userSelect,
           lineUs: getComputedStyle(line).userSelect, lineText: line.textContent,
           lr: { x: lr.x, y: lr.y, w: lr.width, h: lr.height } };
});
console.log('dock', JSON.stringify(dbox));

await page.mouse.move(dbox.r.x + 4, dbox.r.y + 8);
await page.mouse.down();
await page.mouse.move(dbox.r.x + dbox.r.w - 10, dbox.r.y + 60, { steps: 12 });
await page.mouse.up();
await page.waitForTimeout(150);
console.log('selection:', JSON.stringify(await page.evaluate(() => String(document.getSelection()))));
console.log('selectedText():', JSON.stringify(await page.evaluate(() => window.__uni.selectedText())));

await page.keyboard.press('Meta+c');
await page.waitForTimeout(150);
console.log('reject after cmd+c with selection:', await page.evaluate(() => window.__uni.state().reject));

await page.evaluate(() => window.__uni.setView('tracker'));
await page.waitForTimeout(200);
const tb = await page.evaluate(() => { const r = document.getElementById('tracker').getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; });
await page.mouse.click(tb.x + 100, tb.y + 100);
await page.waitForTimeout(150);
console.log('selection after clicking tracker:', JSON.stringify(await page.evaluate(() => String(document.getSelection()))));
console.log('selectedText() after tracker click:', JSON.stringify(await page.evaluate(() => window.__uni.selectedText())));

console.log('errors', errs);
await br.close();
srv.close();
