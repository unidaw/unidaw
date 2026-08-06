import { chromium } from 'playwright';
import { startStack } from './stack.mjs';
const OUT = '/private/tmp/claude-501/-Users-jak-src-daw/072087ea-9515-45b9-8660-c8c34a937332/scratchpad/shots';
const stack = await startStack({ keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await page.waitForTimeout(1500);
const run = (c) => page.evaluate((x) => window.__uni.run(x), c);
const settle = (ms) => page.waitForTimeout(ms);
const shot = async (n) => { await page.screenshot({ path: `${OUT}/${n}.png` }); console.log('shot', n); };

// §1 the tracker, with the demo song loaded — what Jaakko sees first.
await run('load demo');
await settle(2500);
await run('view tracker');
await settle(800);
await shot('1-tracker');

// §1 legato: a second note column and two OVERLAPPING notes, the feature fixed tonight.
await page.evaluate(() => { const g = document.querySelector('#tracker'); if (g) g.click(); });
await settle(300);
await page.keyboard.press('Shift+BracketRight');
await settle(700);
await run('goto 0'); await settle(200);
await run('note 60 3840000'); await settle(600);
await run('goto 4'); await settle(200);
for (let i = 0; i < 3; i++) { await page.keyboard.press('ArrowRight'); await settle(100); }
await run('note 67 3840000'); await settle(700);
await shot('2-two-columns');

// §2 the piano roll — where the length drag lives.
await run('view piano');
await settle(1200);
await shot('3-roll');

// §5 the harmony lane.
await run('view harmony');
await settle(1200);
await shot('4-harmony');

// §6 the patcher, on the generator preset the runbook points at.
await run('load generator');
await settle(2500);
await run('view patcher');
await settle(1500);
await shot('5-patcher');

console.log('page errors:', JSON.stringify(errors.slice(0, 5)));
await browser.close(); await stack.stop();
