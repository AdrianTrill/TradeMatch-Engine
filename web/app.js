// Drives the engine's WebAssembly build and draws what it does: a simulated market, and the engine's command line.
import createTradeMatch from './tradematch.js';

const EXAMPLES = [
  ['price_time_priority', 'Price-time priority'],
  ['order_types', 'Order types'],
  ['iceberg', 'Icebergs'],
  ['stops', 'Stop orders'],
  ['amend_priority', 'Amending'],
  ['auction', 'Call auction'],
  ['self_trade_prevention', 'Self-trade prevention'],
  ['errors', 'Validation'],
];
const DEPTH = 11;  // price levels shown per side
const CANDLES = 90;  // one-second candles on the chart
const STEP_SECONDS = 0.01;  // simulated time per step
const DAY_SECONDS = 100;  // the simulator's day: a 4 s opening auction, 90 s of trading, a 4 s close, 2 s closed
const COLORS = {
  text: '#d8dfe8',
  muted: '#7c8a9b',
  grid: '#1d2733',
  buy: '#22c38e',
  sell: '#f0596b',
  fair: '#b48cff',
  accent: '#4ea4ff',
};
const MONO = '11px ui-monospace, "SF Mono", Menlo, Consolas, monospace';

const $ = (selector) => document.querySelector(selector);
const price = (ticks) => (ticks / 100).toFixed(2);
const grouped = (value) => Math.round(value).toLocaleString('en-US');

let tradematch;
try {
  tradematch = await createTradeMatch();
} catch (error) {
  $('#loading').textContent = `The engine couldn't start in this browser: ${error}`;
  $('#loading').classList.add('error');
  throw error;
}
$('#loading').remove();
$('#market').hidden = false;

for (const tab of document.querySelectorAll('.tab')) {
  tab.addEventListener('click', () => {
    for (const other of document.querySelectorAll('.tab')) {
      other.classList.toggle('active', other === tab);
    }
    for (const view of document.querySelectorAll('.view')) {
      view.hidden = view.id !== tab.dataset.view;
    }
    if (tab.dataset.view === 'terminal') {
      $('#command').focus();
    }
  });
}

// The live market --------------------------------------------------------------------------------------------------

const market = {
  instance: null,
  symbol: 0,
  running: true,
  steps: Number($('#speed').value),
  frameMs: 1000 / 60,
  lastFrame: performance.now(),
  symbolsShown: '',
  tapeShown: '',
};

// A market for the seed, fast-forwarded by `skip` simulated seconds. The URL keeps the seed, so a market can be shared.
function newMarket(seed, skip = 0) {
  market.instance?.delete();
  market.instance = new tradematch.LiveMarket(seed);
  market.instance.step(Math.round(skip / STEP_SECONDS));
  $('#seed').textContent = seed;
  const url = new URL(window.location.href);
  url.searchParams.set('seed', seed);
  url.searchParams.delete('skip');
  window.history.replaceState(null, '', url);
}

// Times a burst of a quarter of a million requests on a market of its own: long enough for the browser's coarse
// clock, which a single frame's work is not.
function measureSpeed() {
  const button = $('#rate');
  button.textContent = 'measuring…';
  setTimeout(() => {
    const bench = new tradematch.LiveMarket(7);
    bench.step(1_000);
    const requestsBefore = JSON.parse(bench.snapshot(0, 1)).requests;
    const started = performance.now();
    bench.step(20_000);
    const elapsed = performance.now() - started;
    const requests = JSON.parse(bench.snapshot(0, 1)).requests - requestsBefore;
    bench.delete();
    const rate = requests / (elapsed / 1000);
    if (!Number.isFinite(rate)) {
      button.textContent = '–';  // a clock that didn't move, as in a headless browser's virtual time
    } else {
      button.textContent = rate >= 1e6 ? `${(rate / 1e6).toFixed(1)} million` : grouped(rate);
    }
  }, 50);
}

function sessionClock(nanoseconds) {
  const seconds = (nanoseconds / 1e9) % DAY_SECONDS;
  return `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${(seconds % 60).toFixed(2).padStart(5, '0')}`;
}

// A canvas sized for the display's pixel density, cleared, with its drawing size in CSS pixels.
function surface(canvas) {
  const ratio = window.devicePixelRatio || 1;
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
  }
  const context = canvas.getContext('2d');
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  context.font = MONO;
  context.lineWidth = 1;
  return { context, width, height };
}

const ladderRows = { asks: [], bids: [] };
{
  const ladder = $('#ladder');
  const row = (side) => {
    const element = document.createElement('div');
    element.className = `row ${side}`;
    element.innerHTML = '<div class="bar"></div><span></span><span></span><span class="orders"></span>';
    ladder.append(element);
    return element;
  };
  for (let index = 0; index < DEPTH; index += 1) {
    ladderRows.asks.push(row('ask'));
  }
  ladderRows.spread = document.createElement('div');
  ladderRows.spread.className = 'spread';
  ladder.append(ladderRows.spread);
  for (let index = 0; index < DEPTH; index += 1) {
    ladderRows.bids.push(row('bid'));
  }
}

function fillRow(row, level, largest) {
  const [bar, priceCell, sizeCell, ordersCell] = row.children;
  priceCell.textContent = level ? price(level[0]) : '';
  sizeCell.textContent = level ? grouped(level[1]) : '';
  ordersCell.textContent = level ? level[2] : '';
  bar.style.width = level ? `${(100 * level[1]) / largest}%` : '0';
}

function drawLadder(view) {
  const largest = Math.max(1, ...view.bids.map((level) => level[1]), ...view.asks.map((level) => level[1]));
  // Asks run from the highest shown down to the best, so the two best prices meet at the spread.
  ladderRows.asks.forEach((row, index) => fillRow(row, view.asks[DEPTH - 1 - index], largest));
  ladderRows.bids.forEach((row, index) => fillRow(row, view.bids[index], largest));
  const bid = view.bids[0]?.[0];
  const ask = view.asks[0]?.[0];
  if (bid === undefined || ask === undefined) {
    ladderRows.spread.textContent = 'one side of the book is empty';
  } else if (bid >= ask) {
    ladderRows.spread.textContent = 'crossed until the auction uncrosses';
  } else {
    ladderRows.spread.textContent = `spread ${price(ask - bid)}`;
  }
}

function drawCandles(view) {
  const { context, width, height } = surface($('#candles'));
  const candles = view.candles;
  if (candles.length === 0) {
    context.fillStyle = COLORS.muted;
    context.fillText('Waiting for the opening auction to trade…', 8, 18);
    return;
  }
  let low = Math.min(view.fair, ...candles.map((candle) => candle[3]));
  let high = Math.max(view.fair, ...candles.map((candle) => candle[2]));
  const padding = Math.max(2, (high - low) * 0.08);
  low -= padding;
  high += padding;
  const axis = 54;
  const plot = width - axis;
  const volumeHeight = height * 0.18;
  const y = (value) => 6 + ((high - value) / (high - low)) * (height - volumeHeight - 12);

  context.strokeStyle = COLORS.grid;
  context.fillStyle = COLORS.muted;
  for (let line = 0; line <= 4; line += 1) {
    const value = low + ((high - low) * line) / 4;
    const level = Math.round(y(value)) + 0.5;
    context.beginPath();
    context.moveTo(0, level);
    context.lineTo(plot, level);
    context.stroke();
    context.fillText(price(value), plot + 6, level + 4);
  }

  const slot = plot / CANDLES;
  const first = CANDLES - candles.length;
  const loudest = Math.max(1, ...candles.map((candle) => candle[5]));
  candles.forEach(([, open, top, bottom, close, volume], index) => {
    const center = (first + index) * slot + slot / 2;
    const color = close >= open ? COLORS.buy : COLORS.sell;
    context.globalAlpha = 0.25;
    context.fillStyle = color;
    const bar = (volume / loudest) * (volumeHeight - 4);
    context.fillRect(center - slot * 0.35, height - bar, slot * 0.7, bar);
    context.globalAlpha = 1;
    context.strokeStyle = color;
    context.beginPath();
    context.moveTo(Math.round(center) + 0.5, y(top));
    context.lineTo(Math.round(center) + 0.5, y(bottom));
    context.stroke();
    const upper = y(Math.max(open, close));
    context.fillRect(center - slot * 0.35, upper, slot * 0.7, Math.max(1, y(Math.min(open, close)) - upper));
  });

  // The fair value is the simulator's hidden random walk, which the agents' quotes and trades follow.
  context.setLineDash([4, 4]);
  context.strokeStyle = COLORS.fair;
  context.beginPath();
  context.moveTo(0, Math.round(y(view.fair)) + 0.5);
  context.lineTo(plot, Math.round(y(view.fair)) + 0.5);
  context.stroke();
  context.setLineDash([]);
  context.fillStyle = COLORS.fair;
  context.fillText('fair', plot - 26, y(view.fair) - 4);
}

// One side's cumulative size as a staircase, from the best price outward.
function staircase(context, levels, x, y, color) {
  if (levels.length === 0) {
    return;
  }
  let total = 0;
  context.beginPath();
  context.moveTo(x(levels[0][0]), y(0));
  for (const [levelPrice, size] of levels) {
    context.lineTo(x(levelPrice), y(total));
    total += size;
    context.lineTo(x(levelPrice), y(total));
  }
  const outer = x(levels.at(-1)[0]);
  context.lineTo(outer, y(0));
  context.closePath();
  context.globalAlpha = 0.18;
  context.fillStyle = color;
  context.fill();
  context.globalAlpha = 1;
  context.strokeStyle = color;
  context.stroke();
}

function drawDepth(view) {
  const { context, width, height } = surface($('#depth'));
  const prices = [...view.bids, ...view.asks].map((level) => level[0]);
  if (prices.length === 0) {
    return;
  }
  const lowest = Math.min(...prices);
  const highest = Math.max(...prices);
  const sum = (levels) => levels.reduce((total, level) => total + level[1], 0);
  const most = Math.max(1, sum(view.bids), sum(view.asks));
  const x = (value) => 10 + ((value - lowest) / Math.max(1, highest - lowest)) * (width - 20);
  const y = (size) => height - 18 - (size / most) * (height - 30);

  staircase(context, view.bids, x, y, COLORS.buy);
  staircase(context, view.asks, x, y, COLORS.sell);
  context.fillStyle = COLORS.muted;
  context.fillText(price(lowest), 4, height - 4);
  const label = price(highest);
  context.fillText(label, width - context.measureText(label).width - 4, height - 4);
  context.fillText(`${grouped(most)} shares`, 4, 12);
}

function drawTape(view) {
  const rows = view.trades
    .slice(-40)
    .reverse()
    .map(
      ([time, tradePrice, size, aggressor]) =>
        `<li><span class="time">${sessionClock(time)}</span><span class="${aggressor}">${price(tradePrice)}</span>` +
        `<span>${grouped(size)}</span></li>`,
    )
    .join('');
  if (rows !== market.tapeShown) {
    $('#tape').innerHTML = rows;
    market.tapeShown = rows;
  }
}

function drawSymbols(view) {
  const names = view.symbols.join(',') + market.symbol;
  if (names === market.symbolsShown) {
    return;
  }
  market.symbolsShown = names;
  const symbols = $('#symbols');
  symbols.replaceChildren(
    ...view.symbols.map((name, index) => {
      const button = document.createElement('button');
      button.textContent = name;
      button.classList.toggle('active', index === market.symbol);
      button.addEventListener('click', () => {
        market.symbol = index;
      });
      return button;
    }),
  );
}

function render(view) {
  drawSymbols(view);
  const phase = $('#phase');
  phase.textContent = view.phase;
  phase.className = `phase${view.phase.includes('AUCTION') ? ' auction' : ''}${view.phase === 'CLOSED' ? ' closed' : ''}`;
  $('#clock').textContent = `day ${view.day} · ${sessionClock(view.time)}`;
  $('#last').textContent = view.last === null ? '–' : price(view.last);
  $('#requests').textContent = grouped(view.requests);
  $('#trades').textContent = grouped(view.trade_count);
  $('#auctions').textContent = grouped(view.auctions);
  $('#resting').textContent = grouped(view.orders);
  drawLadder(view);
  drawCandles(view);
  drawDepth(view);
  drawTape(view);
}

function frame(now) {
  market.frameMs = 0.9 * market.frameMs + 0.1 * Math.min(100, now - market.lastFrame);
  market.lastFrame = now;
  $('#speed-label').textContent = `${((market.steps * STEP_SECONDS * 1000) / market.frameMs).toFixed(1)}×`;

  if (!$('#market').hidden) {
    if (market.running) {
      market.instance.step(market.steps);
    }
    render(JSON.parse(market.instance.snapshot(market.symbol, DEPTH)));
  }
  requestAnimationFrame(frame);
}

$('#play').addEventListener('click', () => {
  market.running = !market.running;
  $('#play').textContent = market.running ? 'Pause' : 'Resume';
});
$('#speed').addEventListener('input', (event) => {
  market.steps = Number(event.target.value);
});
$('#reseed').addEventListener('click', () => newMarket(Math.floor(Math.random() * 1_000_000)));
$('#rate').addEventListener('click', measureSpeed);
document.addEventListener('keydown', (event) => {
  if (event.code === 'Space' && event.target === document.body && !$('#market').hidden) {
    event.preventDefault();
    $('#play').click();
  }
});

{
  const parameters = new URLSearchParams(window.location.search);
  const seed = Number.parseInt(parameters.get('seed') ?? '', 10);
  const skip = Number(parameters.get('skip') ?? 0);
  newMarket(Number.isSafeInteger(seed) && seed >= 0 ? seed : 42, Number.isFinite(skip) ? Math.min(Math.max(skip, 0), 3600) : 0);
}
requestAnimationFrame(frame);
setTimeout(measureSpeed, 500);

// The command line -------------------------------------------------------------------------------------------------

const output = $('#output');
const input = $('#command');
const history = [];
let historyIndex = 0;
let terminal = new tradematch.Terminal();
let exampleRun = 0;  // a newer example, or typing, stops one still being played

function write(text, kind = '') {
  const span = document.createElement('span');
  if (kind) {
    span.className = kind;
  }
  span.textContent = text;
  output.append(span);
  output.scrollTop = output.scrollHeight;
}

function execute(line) {
  write(`> ${line}\n`, 'command');
  write(terminal.run(line));
}

$('#prompt').addEventListener('submit', (event) => {
  event.preventDefault();
  const line = input.value;
  input.value = '';
  if (line.trim() === '') {
    return;
  }
  exampleRun += 1;
  history.push(line);
  historyIndex = history.length;
  execute(line);
});

input.addEventListener('keydown', (event) => {
  if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') {
    return;
  }
  event.preventDefault();
  historyIndex = Math.max(0, Math.min(history.length, historyIndex + (event.key === 'ArrowUp' ? -1 : 1)));
  input.value = history[historyIndex] ?? '';
});

async function runExample(name) {
  const run = (exampleRun += 1);
  const script = (await (await fetch(`scenarios/${name}.tm`)).text()).replaceAll('\r', '');
  if (run !== exampleRun) {
    return;
  }
  terminal.delete();
  terminal = new tradematch.Terminal();
  output.textContent = '';
  for (const line of script.trimEnd().split('\n')) {
    if (run !== exampleRun) {
      return;
    }
    if (line.trim() === '' || line.trim().startsWith('#')) {
      write(`${line}\n`, 'comment');
      continue;
    }
    execute(line);
    await new Promise((resolve) => setTimeout(resolve, 160));
  }
  write('\nThe session stays open: type book to see it, or send more orders.\n', 'comment');
}

for (const [name, title] of EXAMPLES) {
  const button = document.createElement('button');
  button.textContent = title;
  button.addEventListener('click', () => runExample(name));
  $('#examples').append(button);
}

write('TradeMatch command line, running in WebAssembly. Run an example above, or type commands below.\n\n', 'comment');
execute('help');
