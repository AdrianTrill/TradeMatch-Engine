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
market.step(12_000);  // two simulated minutes: an opening auction, continuous trading, and a close
const view = JSON.parse(market.snapshot(0, 10));
market.delete();
if (view.trade_count > 0 && view.auctions > 0 && view.candles.length > 0) {
  console.log(`[PASS] live market: ${view.requests} requests, ${view.trade_count} trades, ${view.auctions} auctions`);
} else {
  failures += 1;
  console.log(`[FAIL] live market: ${JSON.stringify(view).slice(0, 200)}`);
}

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
