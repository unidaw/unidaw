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

const logBox = await page.evaluate(() => {
  const r = document.querySelector('.dk-log').getBoundingClientRect();
  return { x: r.x, y: r.y, w: r.width, h: r.height };
});
console.log('log box', logBox);

// Drag across the whole visible log.
await page.mouse.move(logBox.x + 3, logBox.y + 3);
await page.mouse.down();
await page.mouse.move(logBox.x + logBox.w - 6, logBox.y + logBox.h - 4, { steps: 25 });
await page.mouse.up();
await page.waitForTimeout(150);
const sel = await page.evaluate(() => String(document.getSelection()));
console.log('drag selection:', JSON.stringify(sel));
console.log('selectedText():', JSON.stringify(await page.evaluate(() => window.__uni.selectedText())));

await page.keyboard.press('Meta+c');
await page.waitForTimeout(200);
console.log('reject after cmd+c:', await page.evaluate(() => window.__uni.state().reject));
console.log('clipboard after cmd+c:', JSON.stringify(await page.evaluate(() => navigator.clipboard.readText())));

// The copy button
await page.evaluate(() => document.getSelection().removeAllRanges());
const cbox = await page.evaluate(() => { const r = document.querySelector('.dk-copy').getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; });
console.log('copy button box', cbox);
await page.mouse.click(cbox.x + cbox.w / 2, cbox.y + cbox.h / 2);
await page.waitForTimeout(400);
console.log('clipboard after button:', JSON.stringify((await page.evaluate(() => navigator.clipboard.readText())).slice(0, 120)));
console.log('dock last:', await page.evaluate(() => window.__uni.dockProbe().last));

// Does a stale selection survive a click on a user-select:none surface?
await page.evaluate(() => window.__uni.setView('tracker'));
await page.waitForTimeout(200);
const tb = await page.evaluate(() => { const r = document.getElementById('tracker').getBoundingClientRect(); return { x: r.x, y: r.y }; });
// re-select in the dock first
await page.mouse.move(logBox.x + 3, logBox.y + 3);
await page.mouse.down();
await page.mouse.move(logBox.x + logBox.w - 6, logBox.y + 40, { steps: 15 });
await page.mouse.up();
await page.waitForTimeout(120);
console.log('re-selected:', JSON.stringify((await page.evaluate(() => String(document.getSelection()))).slice(0, 60)));
await page.mouse.click(tb.x + 120, tb.y + 120);
await page.waitForTimeout(150);
console.log('selection after tracker click:', JSON.stringify(await page.evaluate(() => String(document.getSelection()))));
console.log('selectedText() after tracker click:', JSON.stringify(await page.evaluate(() => window.__uni.selectedText())));
await page.keyboard.press('Meta+c');
await page.waitForTimeout(150);
console.log('reject after cmd+c in tracker:', await page.evaluate(() => window.__uni.state().reject));

await br.close();
srv.close();
