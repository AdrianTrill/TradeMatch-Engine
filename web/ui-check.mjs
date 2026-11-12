// Checks the playground page itself in headless Chrome: it must start without errors, draw the market, respond to its
// controls, and fit a phone's screen. web/check.mjs checks the engine's WebAssembly build; this checks the page.
//
//   npm ci --prefix web && node web/ui-check.mjs SITE_DIR [--screenshots DIR]
//
// Chrome comes from the CHROME_PATH environment variable, or else from the usual places it is installed.
import { existsSync, mkdirSync, readFileSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize, resolve, sep } from 'node:path';
import puppeteer from 'puppeteer-core';

const args = process.argv.slice(2);
const siteDir = args[0];
const screenshots = args.includes('--screenshots') ? args[args.indexOf('--screenshots') + 1] : null;
if (!siteDir || siteDir.startsWith('--')) {
  console.error('usage: node web/ui-check.mjs SITE_DIR [--screenshots DIR]');
  process.exit(2);
}

// The site, served as GitHub Pages would serve it.
const root = resolve(siteDir);
const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.tm': 'text/plain',
};
const server = createServer((request, response) => {
  const path = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
  const file = normalize(join(root, path.endsWith('/') ? `${path}index.html` : path));
  if (!file.startsWith(root + sep) || !existsSync(file) || !statSync(file).isFile()) {
    response.writeHead(404).end();
    return;
  }
  response.writeHead(200, { 'content-type': TYPES[extname(file)] ?? 'application/octet-stream' });
  response.end(readFileSync(file));
});
await new Promise((ready) => server.listen(0, '127.0.0.1', ready));
const site = `http://127.0.0.1:${server.address().port}/`;

const chrome = [
  process.env.CHROME_PATH,
  '/usr/bin/google-chrome',
  '/usr/bin/google-chrome-stable',
  '/usr/bin/chromium',
  '/usr/bin/chromium-browser',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
].find((path) => path && existsSync(path));
if (!chrome) {
  console.error('Chrome not found: set CHROME_PATH');
  process.exit(2);
}
// GitHub's Ubuntu runners don't allow Chrome's sandbox; the page under test is our own, served locally.
const browser = await puppeteer.launch({
  executablePath: chrome,
  headless: true,
  args: process.platform === 'linux' ? ['--no-sandbox'] : [],
});
const page = await browser.newPage();
const problems = [];
page.on('pageerror', (error) => problems.push(`uncaught: ${error.message}`));
page.on('console', (message) => {
  if (message.type() === 'error') {
    problems.push(`console: ${message.text()}`);
  }
});
page.on('response', (response) => {
  if (response.status() >= 400) {
    problems.push(`HTTP ${response.status()}: ${response.url()}`);
  }
});

let failures = 0;
const sleep = (milliseconds) => new Promise((done) => setTimeout(done, milliseconds));
const expect = (condition, message) => {
  if (!condition) {
    throw new Error(message);
  }
};
const read = (selector) => page.$eval(selector, (node) => node.textContent);

async function check(name, run) {
  try {
    const detail = await run();
    console.log(`[PASS] ${name}${detail ? `: ${detail}` : ''}`);
  } catch (error) {
    failures += 1;
    console.log(`[FAIL] ${name}: ${error.message}`);
  }
}

async function screenshot(name) {
  if (screenshots) {
    mkdirSync(screenshots, { recursive: true });
    await page.screenshot({ path: join(screenshots, `${name}.png`) });
  }
}

await page.setViewport({ width: 1440, height: 900 });
await page.goto(`${site}?seed=7&skip=30`, { waitUntil: 'load' });
await check('the engine starts', async () => {
  await page.waitForSelector('#market:not([hidden])', { timeout: 20_000 });
  await sleep(1_500);
});
await screenshot('market-desktop');

await check('the order book shows both sides', async () => {
  const sides = await page.evaluate(() => ['ask', 'bid'].map((side) =>
    [...document.querySelectorAll(`.level.${side} .px`)].filter((node) => node.textContent !== '').length));
  expect(sides[0] >= 5 && sides[1] >= 5, `${sides[0]} asks and ${sides[1]} bids shown`);
  return `${sides[1]} bid levels and ${sides[0]} ask levels`;
});

await check('the tape lists trades', async () => {
  const count = await page.$$eval('#tape li', (rows) => rows.length);
  expect(count >= 5, `${count} trades shown`);
  return `${count} trades`;
});

await check('the charts draw', async () => {
  const painted = await page.evaluate(() => ['candles', 'depth'].map((id) => {
    const canvas = document.getElementById(id);
    const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
    let count = 0;
    for (let index = 3; index < pixels.length; index += 4) {
      count += pixels[index] > 0 ? 1 : 0;
    }
    return count;
  }));
  expect(painted.every((count) => count > 2_000), `painted pixels: ${painted}`);
  return `${painted.join(' and ')} pixels painted`;
});

await check('the chart zooms, scrolls back, returns to live, and changes timeframe', async () => {
  const box = await (await page.$('#candles')).boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  for (let notch = 0; notch < 4; notch += 1) {
    await page.mouse.wheel({ deltaY: 200 });
  }
  await page.mouse.move(box.x + 100, box.y + 150);
  await page.mouse.down();
  await page.mouse.move(box.x + 400, box.y + 150, { steps: 6 });
  await page.mouse.up();
  await sleep(100);
  expect(await page.$eval('#live', (button) => !button.hidden), 'scrolling back should offer a way back to live');
  await page.click('#live');
  await sleep(100);
  expect(await page.$eval('#live', (button) => button.hidden), 'the live button should take the chart back');
  await page.click('#timeframes button:nth-child(3)');
  await sleep(150);
  const timeframe = await read('[data-field="timeframe"]');
  expect(timeframe === '15s', `the legend shows ${timeframe}`);
  await page.click('#timeframes button:nth-child(1)');
});

await check('the order book groups prices and reads out a sweep', async () => {
  await page.click('#grouping-button');
  await page.click('#grouping .menu-item:nth-child(3)');
  await sleep(150);
  const prices = await page.$$eval('.level .px', (cells) => cells.map((cell) => cell.textContent).filter(Boolean));
  expect(prices.length > 0 && prices.every((text) => Math.round(Number(text) * 100) % 10 === 0),
    `grouped prices: ${prices.slice(0, 4)}`);
  await page.hover('.level.bid:nth-child(3)');
  await sleep(150);
  const tooltip = await page.$eval('.book-tooltip', (node) => (node.hidden ? '' : node.textContent));
  expect(tooltip.includes('Selling down to') && tooltip.includes('Average price'), `tooltip: ${tooltip}`);
  await page.mouse.move(5, 5);
  await page.click('#grouping-button');
  await page.click('#grouping .menu-item:nth-child(1)');
  return `${prices.length} rows of 0.10`;
});

await check('a visitor trades against the agents', async () => {
  // Paused, the agents can't fill or trade around the visitor's orders between one step of the check and the next.
  await page.click('#play');
  try {
    // A bid picked from the book rests, and shows in the open orders, the book, and on the chart.
    await page.click('.level.bid:nth-child(3)');
    await page.click('#submit');
    await sleep(200);
    expect((await read('[data-count="open"]')) === '1', 'the resting bid should be an open order');
    expect(await page.$('.level.bid.mine') !== null, 'the book should mark the bid');
    // A market order takes the offers at once, and its fills build a position.
    await page.click('[data-type="MARKET"]');
    await page.click('[data-shares="1000"]');
    await page.click('#submit');
    await sleep(300);
    const fills = Number(await read('[data-count="fills"]'));
    expect(fills > 0, 'taking the offers should fill');
    await page.click('.tab[data-tab="positions"]');
    await sleep(150);
    const position = await read('.account-table');
    expect(position.includes('+1,000'), `the position: ${position.replace(/\s+/g, ' ').slice(0, 120)}`);
    // Cancel all leaves nothing working.
    await page.click('.tab[data-tab="open"]');
    await sleep(150);
    expect((await read('[data-count="open"]')) === '1', 'the bid should still be working');
    await page.click('[data-action="cancel-all"]');
    await sleep(200);
    expect((await read('[data-count="open"]')) === '0', 'cancel all should cancel the bid');
    return `${fills} fills`;
  } finally {
    await page.click('[data-type="LIMIT"]');
    await page.click('#play');
  }
});

await check('the market runs, and pausing stops it', async () => {
  const before = await read('#clock');
  await sleep(500);
  const running = await read('#clock');
  expect(running !== before, `the clock stayed at ${before}`);
  await page.click('#play');
  const paused = await read('#clock');
  await sleep(500);
  expect((await read('#clock')) === paused, 'the clock moved while paused');
  expect((await read('#engine-state')) === 'Engine paused', 'the status bar should say the engine is paused');
  await page.click('#play');
});

await check('the symbols and speeds switch', async () => {
  await page.click('.ticker:nth-child(2)');
  await page.click('#speed-button');
  await page.click('#speed .menu-item:nth-child(5)');
  await sleep(300);
  const selected = await page.$$eval('.ticker', (tickers) => tickers.map((ticker) => ticker.getAttribute('aria-selected')));
  const speed = await read('#speed-label');
  expect(selected.join() === 'false,true,false', `selected tickers: ${selected}`);
  expect(speed === '10×', `speed ${speed}`);
  expect(await page.$eval('#speed', (menu) => menu.hidden), 'the speed menu should close');
});

await check('the command line runs an example', async () => {
  await page.click('.view-tab[data-view="terminal"]');
  await page.click('#examples button:first-child');
  await page.waitForFunction(() => document.querySelector('#output').textContent.includes('The session stays open'),
    { timeout: 15_000 });
  const output = await read('#output');
  expect(output.includes('trade #1  70 @ 100.25  buy #3  sell #4'), 'the price-time priority example should trade');
  await screenshot('command-line');
  await page.click('.view-tab[data-view="market"]');
});

await page.setViewport({ width: 390, height: 844, isMobile: true, hasTouch: true });
await page.reload({ waitUntil: 'load' });
await check('a phone screen fits the page', async () => {
  await page.waitForSelector('#market:not([hidden])', { timeout: 20_000 });
  await sleep(1_000);
  const widths = await page.evaluate(() => [document.documentElement.scrollWidth, window.innerWidth]);
  expect(widths[0] <= widths[1], `the page is ${widths[0]}px wide on a ${widths[1]}px screen`);
});
await screenshot('market-phone');

await check('nothing went wrong in the console', () => {
  expect(problems.length === 0, problems.join('; '));
});

await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
