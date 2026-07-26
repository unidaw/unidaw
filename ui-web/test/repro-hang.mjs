#!/usr/bin/env node
// Minimal repro for the engine dying under command traffic.
//
//   tools/webstack.sh && (cd ui-web && node test/repro-hang.mjs)
//
// Lives under ui-web because that is where playwright is installed.
//
// Sends note writes and nothing else — no transport, no plugin chains in the
// project — and reports the command count at which ui_version stops advancing.
import { chromium } from 'playwright';

const b = await chromium.launch({ channel: 'chrome' });
const pg = await b.newPage();
await pg.goto(process.env.UNI_URL || 'http://127.0.0.1:8173/index.html');
await pg.waitForFunction(() => !!window.__uni);
await pg.waitForFunction(() => window.__uni.canSend(), null, { timeout: 8000 });
await pg.evaluate(() => window.__uni.loadProject('webtest'));
await pg.waitForTimeout(2000);

let sent = 0;
for (let r = 0; r < 200; r++) {
  await pg.evaluate((i) => {
    window.__uni.run('goto ' + (40 + i) + ' 0');
    window.__uni.run('note ' + (60 + (i % 12)));
  }, r);
  sent += 2;
  await pg.waitForTimeout(250);
  const s = await pg.evaluate(() => window.__uni.engineState());
  if (s.stale) {
    console.log(`engine stopped publishing after ${sent} commands (clipVersion ${s.clipVersion})`);
    await b.close();
    process.exit(1);
  }
}
console.log(`survived ${sent} commands`);
await b.close();
