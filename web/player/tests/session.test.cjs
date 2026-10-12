// Headless session ownership checks. The runtime fixture starts no game or GPU.
// Run with Node and Playwright on NODE_PATH; RECOMP_BROWSER_PATH is optional.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const {chromium} = require('playwright');

test('player waits for Start, prevents duplicates and releases its runtime on Stop', async () => {
  const template = fs.readFileSync(path.join(__dirname, '../index.html'), 'utf8')
    .replaceAll('@TITLE@', 'Resident Evil 4');
  const server = http.createServer((req, res) => {
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    res.setHeader('Content-Type', 'text/html');
    res.end(req.url.startsWith('/re4/runtime.html') ? '<p>Runtime fixture</p>' : template);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({headless: true,
      ...(process.env.RECOMP_BROWSER_PATH ? {executablePath: process.env.RECOMP_BROWSER_PATH} : {})});
    const context = await browser.newContext({viewport: {width: 1440, height: 900}});
    const first = await context.newPage();
    const second = await context.newPage();
    const url = `http://127.0.0.1:${server.address().port}/re4/`;
    await first.goto(url);
    await second.goto(url);
    assert.equal(await first.locator('iframe').count(), 0);
    if (process.env.RECOMP_PLAYER_SCREENSHOT)
      await first.screenshot({path: process.env.RECOMP_PLAYER_SCREENSHOT});
    await first.locator('#start').click();
    await first.locator('iframe').waitFor();
    assert.equal(await first.locator('iframe').count(), 1);
    await second.locator('#start').click();
    await second.waitForFunction(() => document.querySelector('#message').textContent.includes('another tab'));
    assert.equal(await second.locator('iframe').count(), 0);
    await first.locator('#stop').click();
    assert.equal(await first.locator('iframe').count(), 0);
    await second.locator('#start').click();
    await second.locator('iframe').waitFor();
    await second.reload();
    assert.equal(await second.locator('iframe').count(), 0);
    await first.locator('#start').click();
    await first.locator('iframe').waitFor();
    await first.locator('#stop').click();
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
});
