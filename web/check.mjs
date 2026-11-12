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

// Trading from the page: an order that takes liquidity fills at once, a passive one rests and can be amended and
// cancelled, the agents' orders are out of reach, rejects say why, and every fill is reported once.
{
  const trader = new tradematch.LiveMarket(9);
  trader.step(1_000);  // ten seconds in: continuous trading
  const book = JSON.parse(trader.snapshot(0, 5, Infinity));
  const order = (fields) => ({ side: 'BUY', type: 'LIMIT', quantity: 100, price: 0, stop: 0, tif: 'GTC', postOnly: false,
    display: 0, ...fields });
  const ask = book.asks[0][0];
  const bid = book.bids[0][0];
  const problems = [];

  const taken = JSON.parse(trader.submit(0, order({ quantity: 500, price: ask + 5, tif: 'IOC' })));
  let account = JSON.parse(trader.account());
  const takerFills = account.fills.filter((fill) => fill[3] === taken.id);
  if (!taken.accepted || taken.filled === 0 || takerFills.reduce((sum, fill) => sum + fill[6], 0) !== taken.filled ||
      !takerFills.every((fill) => fill[7] === 'T' && fill[4] === 'BUY')) {
    problems.push(`a taking order: ${JSON.stringify(taken)} ${JSON.stringify(takerFills)}`);
  }
  if (!account.done.some(([id, , , , filled]) => id === taken.id && filled === taken.filled)) {
    problems.push(`a finished IOC order should be reported: ${JSON.stringify(account.done)}`);
  }
  if (JSON.parse(trader.account()).fills.length !== 0) {
    problems.push('fills should be reported once');
  }

  const rested = JSON.parse(trader.submit(0, order({ side: 'SELL', quantity: 300, price: ask + 50 })));
  account = JSON.parse(trader.account());
  const working = account.orders.find(([id]) => id === rested.id);
  const amended = JSON.parse(trader.amend(rested.id, ask + 50, 200));
  const refused = JSON.parse(trader.cancel(1));
  const cancelled = JSON.parse(trader.cancel(rested.id));
  account = JSON.parse(trader.account());
  if (!rested.rested || !working || working[10] !== 300 || working[11] !== 'resting' || !amended.replaced ||
      !amended.kept_priority || refused.cancelled || !cancelled.cancelled || cancelled.quantity !== 200 ||
      !account.done.some(([id, , , , , reason]) => id === rested.id && reason === 'cancelled')) {
    problems.push(`a resting order: ${JSON.stringify([rested, working, amended, refused, cancelled])}`);
  }

  // An amend that makes the order marketable trades at once, and the order finishes filled at its own size.
  const parked = JSON.parse(trader.submit(0, order({ quantity: 200, price: bid - 30 })));
  const lifted = JSON.parse(trader.amend(parked.id, ask + 20, 200));
  account = JSON.parse(trader.account());
  const finished = account.done.find(([id]) => id === parked.id);
  if (!lifted.replaced || !finished || finished[3] !== 200 || finished[4] !== 200 || finished[5] !== 'filled') {
    problems.push(`a marketable amend: ${JSON.stringify([lifted, finished])}`);
  }

  const empty = JSON.parse(trader.submit(0, order({ quantity: 0, price: bid })));
  const crossing = JSON.parse(trader.submit(0, order({ price: ask + 10, postOnly: true })));
  if (empty.accepted || empty.reject !== 'INVALID_QUANTITY' || crossing.reject !== 'POST_ONLY_WOULD_CROSS') {
    problems.push(`rejects: ${JSON.stringify([empty, crossing])}`);
  }

  // A bid joining the best price is filled by the agents' selling, as the maker.
  const latest = JSON.parse(trader.snapshot(0, 1, Infinity));
  const joined = JSON.parse(trader.submit(0, order({ price: latest.bids[0][0] })));
  const makerFills = [];
  for (let second = 0; second < 30 && makerFills.length === 0; second += 1) {
    trader.step(100);
    makerFills.push(...JSON.parse(trader.account()).fills.filter((fill) => fill[3] === joined.id));
  }
  if (makerFills.length === 0 || !makerFills.every((fill) => fill[7] === 'M')) {
    problems.push(`a joined bid should fill as the maker: ${JSON.stringify(makerFills)}`);
  }
  trader.delete();
  if (problems.length === 0) {
    console.log(`[PASS] trading: took ${taken.filled} at once, amended and cancelled a resting order, filled as maker`);
  } else {
    failures += 1;
    console.log(`[FAIL] trading: ${problems.join('; ')}`);
  }
}

const terminal = new tradematch.Terminal();
const reply = terminal.run('add ACME') + terminal.run('buy ACME 10 @ 100.00');
const help = terminal.run('help');
terminal.delete();
if (reply.includes('ACME #1 BUY 10 @ 100.00')) {
  console.log('[PASS] terminal keeps its session from command to command');
} else {
  failures += 1;
  console.log(`[FAIL] terminal: ${reply}`);
}

// The command line colors what the interpreter prints. Cut into pieces, every line of every golden transcript, and
// of help, must join back into exactly itself, and the pieces must recognize everything the transcripts say.
{
  const { Highlighter, command } = await import(pathToFileURL(resolve(siteDir, 'js/highlight.js')).href);
  const highlighter = new Highlighter();
  const problems = [];
  const kinds = new Set();
  const colored = (lines, pieces) => pieces.forEach((line, index) => {
    if (line.map(([text]) => text).join('') !== lines[index]) {
      problems.push(`"${lines[index]}" came back as "${line.map(([text]) => text).join('')}"`);
    }
    line.forEach(([, kind]) => kinds.add(kind));
  });
  for (const name of readdirSync(scenarioDir).filter((file) => file.endsWith('.expected')).sort()) {
    const transcript = readFileSync(join(scenarioDir, name), 'utf8').replaceAll('\r', '');
    // Each "> command" line, then everything up to the next one.
    for (const [, typed, printed] of transcript.matchAll(/^> (.*)\n((?:(?!> ).*\n)*)/gm)) {
      colored([typed], [command(typed)]);
      const lines = printed.replace(/\n$/, '').split('\n');
      colored(lines, highlighter.output(printed, typed.split(' ')[0]));
    }
  }
  colored(help.replace(/\n$/, '').split('\n'), highlighter.output(help, 'help'));
  const missing = ['buy side', 'sell side', 'buy', 'sell', 'trade', 'ok', 'rest', 'wait', 'warn', 'error', 'phase',
    'phase auction', 'comment', 'symbol', 'heading', 'arg', 'keyword'].filter((kind) => !kinds.has(kind));
  if (missing.length > 0) {
    problems.push(`nothing was colored as ${missing.join(', ')}`);
  }
  if (problems.length === 0) {
    console.log(`[PASS] the command line's colors keep every character, in ${kinds.size} kinds`);
  } else {
    failures += 1;
    console.log(`[FAIL] the command line's colors: ${problems.slice(0, 3).join('; ')}`);
  }
}

process.exit(failures === 0 ? 0 : 1);
