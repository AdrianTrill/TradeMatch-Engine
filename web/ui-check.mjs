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
  await page.click('#speed button:nth-child(5)');
  await sleep(300);
  const selected = await page.$$eval('.ticker', (tickers) => tickers.map((ticker) => ticker.getAttribute('aria-selected')));
  const speed = await page.$eval('#speed [aria-checked="true"]', (button) => button.textContent);
  expect(selected.join() === 'false,true,false', `selected tickers: ${selected}`);
  expect(speed === '10×', `speed ${speed}`);
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
