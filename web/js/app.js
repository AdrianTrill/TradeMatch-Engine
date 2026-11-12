// The playground: drives the engine's WebAssembly build and draws what it does, a simulated market trading through
// auctions and continuous sessions, and the engine's command line.
import createTradeMatch from '../tradematch.js';
import { BookView } from './book.js';
import { PriceChart, TIMEFRAMES } from './chart.js';
import { DepthChart } from './depth.js';
import { $, element, text } from './dom.js';
import { clock, grouped, price } from './format.js';
import { CandleHistory } from './history.js';
import { readPalette } from './palette.js';
import { TapeView } from './tape.js';
import { startTerminal } from './terminal.js';
import { toast } from './toast.js';

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
const depth = new DepthChart($('#depth'));
const book = new BookView($('#book'));
const tape = new TapeView($('#tape'));

// A market for the seed, fast-forwarded by `skip` simulated seconds. The URL keeps the seed, so a market can be shared;
// a first visit starts a minute into the first day, so the chart already has candles.
function newMarket(seed, skip = 0) {
  market.instance?.delete();
  market.instance = new tradematch.LiveMarket(seed);
  market.instance.step(Math.round(skip / STEP_SECONDS));
  market.seed = seed;
  market.lastPrice = null;
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

function renderSymbols(view) {
  if (view.symbols.join() !== market.symbols.join()) {
    market.symbols = view.symbols;
    $('#tickers').replaceChildren(...view.symbols.map((name, index) => {
      const button = element('button', 'ticker', `<span class="ticker-name">${name}</span><span class="ticker-detail">–</span>`);
      button.setAttribute('role', 'tab');
      button.addEventListener('click', () => selectSymbol(index));
      return button;
    }));
  }
  [...$('#tickers').children].forEach((button, index) => {
    button.setAttribute('aria-selected', String(index === market.symbol));
    if (index === market.symbol) {
      text(button.lastElementChild, view.last === null ? '–' : price(view.last));
    }
  });
}

function selectSymbol(index) {
  market.symbol = index;
  market.lastPrice = null;
  chart.reset();
}

function history(index) {
  market.histories[index] ??= new CandleHistory();
  return market.histories[index];
}

function render(view, candles) {
  renderSymbols(view);
  const last = $('#last');
  if (view.last !== null && view.last !== market.lastPrice) {
    last.className = `quote-price${market.lastPrice === null ? '' : view.last > market.lastPrice ? ' up' : ' down'}`;
    market.lastPrice = view.last;
  }
  text(last, view.last === null ? '–' : price(view.last));
  text($('#change'), `${grouped(view.volume)} shares traded`);

  const phase = $('#phase');
  text(phase, view.phase);
  phase.className = `phase${view.phase.includes('AUCTION') ? ' auction' : ''}${view.phase === 'CLOSED' ? ' closed' : ''}`;
  text($('#clock'), `Day ${view.day} · ${clock(view.time, market.config.dayLength / 1e9)}`);

  text($('#requests'), grouped(view.requests));
  text($('#trades'), grouped(view.trade_count));
  text($('#auctions'), grouped(view.auctions));
  text($('#resting'), grouped(view.orders));

  chart.draw({ view, history: candles, config: market.config });
  depth.draw(view);
  book.update(view);
  tape.update(view, market.config.dayLength / 1e9);
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
    const view = JSON.parse(market.instance.snapshot(market.symbol, Math.max(book.levels, 24), candles.since));
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

function setSpeed(speed) {
  market.speed = speed;
  for (const button of $('#speed').children) {
    button.setAttribute('aria-checked', String(Number(button.dataset.speed) === speed));
  }
}

for (const speed of SPEEDS) {
  const button = element('button');
  button.textContent = `${speed === 0.5 ? '½' : speed}×`;
  button.dataset.speed = speed;
  button.setAttribute('role', 'radio');
  button.title = `${speed} simulated second${speed === 1 ? '' : 's'} per second`;
  button.addEventListener('click', () => setSpeed(speed));
  $('#speed').append(button);
}

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
