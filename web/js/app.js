// The playground: drives the engine's WebAssembly build and draws what it does, a simulated market trading through
// auctions and continuous sessions, and the engine's command line.
import createTradeMatch from '../tradematch.js';
import { BookView, GROUPINGS } from './book.js';
import { PriceChart, TIMEFRAMES } from './chart.js';
import { DepthChart } from './depth.js';
import { $, element, text } from './dom.js';
import { grouped, price } from './format.js';
import { Quote, Session, Tickers } from './header.js';
import { CandleHistory } from './history.js';
import { readPalette } from './palette.js';
import { TapeView } from './tape.js';
import { startTerminal } from './terminal.js';
import { toast } from './toast.js';
import { Account, Ticket } from './trading.js';

const STEP_SECONDS = 0.01;  // simulated time per simulator step
const SPEEDS = [0.5, 1, 2, 5, 10, 25];  // simulated seconds per second
const MAX_STEPS = 2_500;  // per frame, so a long pause in the browser can't stall the page

readPalette();

let tradematch;
try {
  tradematch = await createTradeMatch();
} catch (error) {
  $('#loading p').textContent = `The engine couldn't start in this browser: ${error}`;
  $('#loading').classList.add('error');
  throw error;
}
$('#loading').remove();
$('#market').hidden = false;

const terminal = startTerminal(tradematch);

for (const tab of document.querySelectorAll('.view-tab')) {
  tab.addEventListener('click', () => {
    for (const other of document.querySelectorAll('.view-tab')) {
      other.classList.toggle('active', other === tab);
    }
    for (const view of document.querySelectorAll('.view')) {
      view.hidden = view.id !== tab.dataset.view;
    }
    if (tab.dataset.view === 'terminal') {
      terminal.focus();
    }
  });
}

// The live market --------------------------------------------------------------------------------------------------

const market = {
  instance: null,
  seed: 0,
  symbol: 0,
  running: true,
  speed: 2,
  carry: 0,  // simulated steps owed to the next frame
  lastFrame: performance.now(),
  symbols: [],
  lastPrice: null,
  config: null,  // the simulated day's shape: phase lengths in nanoseconds
  histories: [],  // each symbol's candles
};

const chart = new PriceChart($('#candles'), $('#legend'), $('#live'));
const tickers = new Tickers($('#tickers'), (index) => selectSymbol(index));
const quote = new Quote($('#quote').parentElement);
const session = new Session($('#session'));
const depth = new DepthChart($('#depth'));
const book = new BookView($('#book'), (picked) => ticket.pick(picked));
const tape = new TapeView($('#tape'), $('#trade-rate'));
const ticket = new Ticket($('#ticket'), market, announceOrder);
const account = new Account($('.orders-panel'), $('#account-card'), market, {
  onCancel: cancelOrder,
  onFlatten: closePosition,
});
chart.onOrders({ cancel: cancelOrder, move: moveOrder });

// Trading against the agents ---------------------------------------------------------------------------------------

const describe = (request, symbol) =>
  `${request.side === 'BUY' ? 'buy' : 'sell'} ${grouped(request.quantity)} ${symbol}`;

// What happened to an order just sent. Fills are announced as the engine reports them.
function announceOrder(request, result) {
  const symbol = market.symbols[market.symbol] ?? '';
  if (!result.accepted) {
    toast(`Order rejected: ${result.message}`, 'error', 4000);
  } else if (result.queued) {
    toast(`Your ${describe(request, symbol)} waits for the auction's uncross`);
  } else if (result.pending) {
    toast(`Your ${describe(request, symbol)} waits for its stop at ${price(request.stop)}`);
  } else if (result.rested) {
    toast(`Your ${describe(request, symbol)} rests at ${price(request.price)}${
      result.filled > 0 ? ` after ${grouped(result.filled)} traded` : ''}`);
  }
}

function cancelOrder(id) {
  if (id === null) {
    const cancelled = market.instance.cancelAll();
    toast(`Cancelled ${cancelled} order${cancelled === 1 ? '' : 's'}`);
    return;
  }
  const result = JSON.parse(market.instance.cancel(id));
  toast(result.cancelled ? `Cancelled ${grouped(result.quantity)} shares` : `Couldn't cancel: ${result.message}`,
    result.cancelled ? 'info' : 'error');
}

function moveOrder(id, target, remaining) {
  const result = JSON.parse(market.instance.amend(id, target, remaining));
  toast(result.replaced ? `Moved your order to ${price(target)}` : `Couldn't move the order: ${result.message}`,
    result.replaced ? 'info' : 'error');
}

// Closes a position with a market order the other way.
function closePosition(symbol) {
  const shares = account.position(symbol).shares;
  if (shares === 0) {
    return;
  }
  const request = { side: shares > 0 ? 'SELL' : 'BUY', type: 'MARKET', quantity: Math.abs(shares), price: 0, stop: 0,
    tif: 'IOC', postOnly: false, display: 0 };
  const result = JSON.parse(market.instance.submit(symbol, request));
  if (!result.accepted) {
    toast(`Couldn't close the position: ${result.message}`, 'error');
  } else if (result.queued) {
    toast(`Closing ${market.symbols[symbol]} at the auction's uncross`);
  }
}

// A market for the seed, fast-forwarded by `skip` simulated seconds. The URL keeps the seed, so a market can be shared;
// a first visit starts a minute into the first day, so the chart already has candles.
function newMarket(seed, skip = 0) {
  market.instance?.delete();
  market.instance = new tradematch.LiveMarket(seed);
  market.instance.step(Math.round(skip / STEP_SECONDS));
  market.seed = seed;
  quote.reset();
  book.reset();
  tape.reset();
  account.reset();
  ticket.symbol = -1;
  market.histories = [];
  const phases = JSON.parse(market.instance.config()).phases.map(([, length]) => length);
  market.config = {
    opening: phases[0],
    continuous: phases[1],
    closing: phases[2],
    closed: phases[3],
    dayLength: phases.reduce((sum, length) => sum + length, 0),
  };
  chart.goLive();
  text($('#seed'), seed);
  const url = new URL(window.location.href);
  url.searchParams.set('seed', seed);
  url.searchParams.delete('skip');
  window.history.replaceState(null, '', url);
}

// Times a burst of requests on a market of its own: long enough for the browser's coarse clock, which a single
// frame's work is not.
function measureSpeed() {
  const button = $('#rate');
  button.textContent = 'measuring…';
  setTimeout(() => {
    const bench = new tradematch.LiveMarket(7);
    bench.step(1_000);
    const before = JSON.parse(bench.snapshot(0, 1, Infinity)).requests;
    const started = performance.now();
    bench.step(20_000);
    const elapsed = performance.now() - started;
    const requests = JSON.parse(bench.snapshot(0, 1, Infinity)).requests - before;
    bench.delete();
    const rate = requests / (elapsed / 1000);
    if (!Number.isFinite(rate)) {
      button.textContent = '–';  // a clock that didn't move, as in a headless browser's virtual time
    } else {
      button.textContent = rate >= 1e6 ? `${(rate / 1e6).toFixed(1)} million` : grouped(rate);
    }
  }, 50);
}

function selectSymbol(index) {
  market.symbol = index;
  quote.reset();
  chart.reset();
  book.reset();
  tape.reset();
}

function history(index) {
  market.histories[index] ??= new CandleHistory();
  return market.histories[index];
}

// While a call auction collects orders, what its uncross would do right now.
function renderAuction(view) {
  const status = $('#auction-status');
  const auction = view.phase.includes('AUCTION');
  status.hidden = !auction;
  $('#chart-hint').hidden = auction;
  $('#depth-note').textContent = auction && view.auction
    ? 'Buyers and sellers by price: the uncross is where they cross'
    : 'Cumulative size by price';
  if (!auction) {
    return;
  }
  const kind = view.phase === 'OPENING AUCTION' ? 'Opening' : 'Closing';
  if (!view.auction) {
    text(status, `${kind} auction · no orders cross yet`);
    return;
  }
  const { price: uncross, matched, buy_surplus: buys, sell_surplus: sells } = view.auction;
  const imbalance = buys > sells ? `${grouped(buys - sells)} more to buy` : sells > buys
    ? `${grouped(sells - buys)} more to sell` : 'balanced';
  text(status, `${kind} auction · would trade ${grouped(matched)} at ${price(uncross)} · ${imbalance}`);
}

function render(view, candles) {
  market.symbols = view.symbols;
  const daySeconds = market.config.dayLength / 1e9;
  account.update(view, view.symbols, daySeconds);
  const overlay = account.overlay(market.symbol);
  ticket.update(view, market.symbol);
  tickers.update(view.watch, market.symbol);
  quote.update(view.watch[market.symbol]);
  session.update(view, market.config);
  renderAuction(view);

  text($('#requests'), grouped(view.requests));
  text($('#trades'), grouped(view.trade_count));
  text($('#auctions'), grouped(view.auctions));
  text($('#resting'), grouped(view.orders));

  chart.draw({ view, history: candles, config: market.config, overlay });
  depth.draw(view);
  book.update(view, overlay.orders);
  tape.update(view, daySeconds);
}

function frame(now) {
  const elapsed = Math.min(0.25, (now - market.lastFrame) / 1000);
  market.lastFrame = now;
  if (!$('#market').hidden) {
    if (market.running) {
      market.carry += (elapsed * market.speed) / STEP_SECONDS;
      const steps = Math.min(MAX_STEPS, Math.floor(market.carry));
      market.carry -= steps;
      if (steps > 0) {
        market.instance.step(steps);
      }
    }
    const candles = history(market.symbol);
    const view = JSON.parse(market.instance.snapshot(market.symbol, book.depth, candles.since));
    candles.merge(view.candles);
    render(view, candles);
  }
  requestAnimationFrame(frame);
}

function setRunning(running) {
  market.running = running;
  const play = $('#play');
  play.innerHTML = `<svg class="icon"><use href="#i-${running ? 'pause' : 'play'}"/></svg>`;
  play.title = running ? 'Pause (space)' : 'Resume (space)';
  play.setAttribute('aria-label', running ? 'Pause' : 'Resume');
  $('#engine-dot').className = `dot ${running ? 'running' : 'paused'}`;
  text($('#engine-state'), running ? 'Engine running' : 'Engine paused');
}

const speedName = (speed) => `${speed === 0.5 ? '½' : speed}×`;

function setSpeed(speed) {
  market.speed = speed;
  text($('#speed-label'), speedName(speed));
  check($('#speed'), speed);
}

// A drop-down menu of choices under a button, closed by a choice, a click elsewhere, or Escape.
function dropdown(button, menu, choices, choose) {
  const toggle = (open) => {
    menu.hidden = !open;
    button.setAttribute('aria-expanded', String(open));
  };
  for (const [value, label, note] of choices) {
    const item = element('button', 'menu-item', `<span>${label}</span><span class="menu-note">${note}</span>`);
    item.dataset.value = value;
    item.setAttribute('role', 'menuitemradio');
    item.addEventListener('click', () => {
      choose(value);
      toggle(false);
    });
    menu.append(item);
  }
  button.addEventListener('click', (event) => {
    event.stopPropagation();
    toggle(menu.hidden);
  });
  document.addEventListener('click', (event) => {
    if (!menu.hidden && !menu.contains(event.target)) {
      toggle(false);
    }
  });
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') {
      toggle(false);
    }
  });
}

function check(menu, value) {
  for (const item of menu.children) {
    item.setAttribute('aria-checked', String(Number(item.dataset.value) === value));
  }
}

dropdown($('#speed-button'), $('#speed'),
  SPEEDS.map((speed) => [speed, speedName(speed), speed === 1 ? 'real time' : `${speed} s a second`]), setSpeed);
dropdown($('#grouping-button'), $('#grouping'),
  GROUPINGS.map((ticks) => [ticks, price(ticks), ticks === 1 ? 'every price' : `${ticks} ticks a row`]), (ticks) => {
    book.setGrouping(ticks);
    text($('#grouping-label'), price(ticks));
    check($('#grouping'), ticks);
  });
check($('#grouping'), 1);

for (const seconds of TIMEFRAMES) {
  const button = element('button');
  button.textContent = seconds === 60 ? '1m' : `${seconds}s`;
  button.setAttribute('role', 'radio');
  button.setAttribute('aria-checked', String(seconds === 1));
  button.title = `${seconds === 60 ? 'One-minute' : `${seconds}-second`} candles`;
  button.addEventListener('click', () => {
    chart.setTimeframe(seconds);
    for (const other of $('#timeframes').children) {
      other.setAttribute('aria-checked', String(other === button));
    }
  });
  $('#timeframes').append(button);
}

$('#play').addEventListener('click', () => setRunning(!market.running));
$('#reseed').addEventListener('click', () => newMarket(Math.floor(Math.random() * 1_000_000)));
$('#share').addEventListener('click', async () => {
  const url = new URL(window.location.href);
  const view = JSON.parse(market.instance.snapshot(0, 1, Infinity));
  url.searchParams.set('seed', market.seed);
  url.searchParams.set('skip', Math.floor(view.time / 1e9));
  try {
    await navigator.clipboard.writeText(url.href);
    toast('Link copied: it opens this market at this moment');
  } catch {
    toast(url.href);
  }
});
$('#rate').addEventListener('click', measureSpeed);
document.addEventListener('keydown', (event) => {
  if (event.code === 'Space' && event.target === document.body && !$('#market').hidden) {
    event.preventDefault();
    setRunning(!market.running);
  }
});

{
  const parameters = new URLSearchParams(window.location.search);
  const seed = Number.parseInt(parameters.get('seed') ?? '', 10);
  const skip = Number(parameters.get('skip') ?? 60);
  newMarket(Number.isSafeInteger(seed) && seed >= 0 ? seed : 42, Number.isFinite(skip) ? Math.min(Math.max(skip, 0), 3600) : 0);
}
setSpeed(2);
setRunning(true);
requestAnimationFrame(frame);
setTimeout(measureSpeed, 600);
