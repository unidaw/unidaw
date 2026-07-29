import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
import { readdirSync } from 'node:fs';
const stack = await startStack();
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1500, height: 900 } });
const errs = []; page.on('pageerror', e => errs.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForTimeout(1500);
await page.evaluate(() => window.__uni.loadProject('rack'));
await page.waitForTimeout(2500);
// Change something, then Cmd+S.
await page.evaluate(() => { window.__uni.run('view tracker'); window.__uni.run('goto 5 0'); });
await page.waitForTimeout(400);
await page.evaluate(() => window.__uni.run('note 64'));
await page.waitForTimeout(700);
const before = readdirSync(stack.dir).filter(f=>f.includes('rack'));
console.log('project file before:', before);
const mtimeBefore = (await import('node:fs')).statSync(stack.dir + '/rack.uniproj.json').mtimeMs;
await page.keyboard.press('Meta+s');
await page.waitForTimeout(1500);
const mtimeAfter = (await import('node:fs')).statSync(stack.dir + '/rack.uniproj.json').mtimeMs;
console.log('mtime moved:', mtimeAfter > mtimeBefore, mtimeBefore, '->', mtimeAfter);
console.log('state:', JSON.stringify(await page.evaluate(() => ({
  proj: window.__uni.state().currentProject, reject: window.__uni.state().reject,
  browserOpen: window.__uni.state().browserOpen }))));
console.log('errors:', errs.length ? errs.join(' | ') : 'none');
await browser.close(); stack.stop(); process.exit(0);
