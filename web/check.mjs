// Checks the WebAssembly build under Node: every golden scenario must produce exactly the transcript the native
// command line produces, and the live market must run.
//
//   node web/check.mjs SITE_DIR SCENARIO_DIR
import { readFileSync, readdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [siteDir, scenarioDir] = process.argv.slice(2);
if (!siteDir || !scenarioDir) {
  console.error('usage: node web/check.mjs SITE_DIR SCENARIO_DIR');
  process.exit(2);
}
const { default: createTradeMatch } = await import(pathToFileURL(resolve(siteDir, 'tradematch.js')).href);
const tradematch = await createTradeMatch();

let failures = 0;
for (const name of readdirSync(scenarioDir).filter((file) => file.endsWith('.tm')).sort()) {
  const script = readFileSync(join(scenarioDir, name), 'utf8').replaceAll('\r', '');
  const expected = readFileSync(join(scenarioDir, name.replace(/\.tm$/, '.expected')), 'utf8').replaceAll('\r', '');
  const actual = tradematch.runScript(script);
  if (actual === expected) {
    console.log(`[PASS] ${name}`);
    continue;
  }
  failures += 1;
  const want = expected.split('\n');
  const got = actual.split('\n');
  const line = want.findIndex((text, index) => text !== got[index]);
  console.log(`[FAIL] ${name}: line ${line + 1} differs\n  expected: ${want[line]}\n  actual:   ${got[line]}`);
}

const market = new tradematch.LiveMarket(42);
market.step(12_000);  // two simulated minutes: two opening auctions, continuous trading, and a close
const view = JSON.parse(market.snapshot(0, 10, -1));
const config = JSON.parse(market.config());
if (view.trade_count > 0 && view.auctions > 0 && view.candles.length > 0) {
  console.log(`[PASS] live market: ${view.requests} requests, ${view.trade_count} trades, ${view.auctions} auctions`);
} else {
  failures += 1;
  console.log(`[FAIL] live market: ${JSON.stringify(view).slice(0, 200)}`);
}

// The chart's data: candles sent from a given start on, each with the fair value and the day's VWAP as of its end, and
// the auction crosses at the scheduled times.
const dayLength = config.phases.reduce((sum, [, length]) => sum + length, 0);
const [opening, continuous, closing] = config.phases.map(([, length]) => length);
const newest = view.candles.at(-1)[0];
const since = JSON.parse(market.snapshot(0, 1, newest)).candles;
const crossTimes = view.crosses.map(([time]) => time % dayLength);
const sampled = view.candles.slice(0, -1);
if (dayLength === 100e9 && since.length === 1 && since[0][0] === newest &&
    sampled.every((candle) => candle.length === 8 && candle[6] > 0) &&
    sampled.filter((candle) => candle[0] % dayLength > opening).every((candle) => candle[7] > 0) &&
    view.crosses.length >= 3 && view.crosses.every(([, price, volume]) => price > 0 && volume > 0) &&
    crossTimes.every((time) => time === opening || time === opening + continuous + closing)) {
  console.log(`[PASS] chart data: ${view.candles.length} candles, crosses at ${view.crosses.map(([time]) => time / 1e9)} s`);
} else {
  failures += 1;
  console.log(`[FAIL] chart data: since=${JSON.stringify(since)} crosses=${JSON.stringify(view.crosses)}`);
}

// The market watch has every symbol, and an auction shows what its uncross would do.
const watched = view.watch.map((summary) => summary.symbol).join();
let auction = null;
for (let step = 0; step < 10_000 && !auction; step += 1) {  // until the next call auction, within 100 s
  market.step(1);
  auction = JSON.parse(market.snapshot(0, 1, Infinity)).auction;
}

// The tape: prints in order of their IDs, one per continuous trade, and that auction's uncross as a single print of
// all its fills, as large as the cross.
let after = JSON.parse(market.snapshot(0, 1, Infinity));
const crossesBefore = after.crosses.length;
for (let step = 0; step < 1_000 && after.crosses.length === crossesBefore; step += 1) {
  market.step(1);
  after = JSON.parse(market.snapshot(0, 1, Infinity));
}
const prints = after.trades;
const cross = after.crosses.at(-1);
const crossPrint = prints.find((print) => print[4] === 'X' && print[1] === cross[0]);
const tapeOk = prints.every((print, index) => index === 0 || print[0] > prints[index - 1][0]) &&
  prints.filter((print) => print[4] !== 'X').every((print) => print[5] === 1) &&
  crossPrint !== undefined && crossPrint[3] === cross[2] && crossPrint[2] === cross[1] && crossPrint[5] > 1;
if (tapeOk && watched === view.symbols.join() && view.watch.every((summary) => summary.spark.length > 0 &&
    summary.volume > 0 && summary.high >= summary.low) && auction && auction.price > 0 && auction.matched > 0) {
  console.log(`[PASS] tape, market watch, and auction: ${watched}; indicative ${auction.matched} @ ${auction.price}; ` +
    `uncross printed as one trade of ${crossPrint[3]} from ${crossPrint[5]} fills`);
} else {
  failures += 1;
  console.log(`[FAIL] tape, market watch, and auction: ${JSON.stringify(auction)} ${JSON.stringify(prints.slice(-3))}`);
}
market.delete();

const terminal = new tradematch.Terminal();
const reply = terminal.run('add ACME') + terminal.run('buy ACME 10 @ 100.00');
terminal.delete();
if (reply.includes('ACME #1 BUY 10 @ 100.00')) {
  console.log('[PASS] terminal keeps its session from command to command');
} else {
  failures += 1;
  console.log(`[FAIL] terminal: ${reply}`);
}

process.exit(failures === 0 ? 0 : 1);
