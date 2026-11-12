// The market bar: a ticker for every symbol with its price, day change, and last minute as a sparkline; the selected
// symbol's quote and day figures; and the trading day as a timeline, counting down to the next phase.
import { element, text } from './dom.js';
import { compact, clock, price } from './format.js';

const PHASES = ['OPENING AUCTION', 'CONTINUOUS', 'CLOSING AUCTION', 'CLOSED'];
const NEXT = {
  'OPENING AUCTION': 'Uncross in',
  CONTINUOUS: 'Closing auction in',
  'CLOSING AUCTION': 'Uncross in',
  CLOSED: 'Opens in',
};

// The change from the previous close, or from the day's open on the first day.
export function dayChange(summary) {
  const reference = summary.previous_close ?? summary.open;
  if (summary.last === null || reference === null) {
    return null;
  }
  return { points: summary.last - reference, percent: ((summary.last - reference) / reference) * 100 };
}

export function changeText(change, withPoints = true) {
  if (!change) {
    return '–';
  }
  const sign = change.points > 0 ? '+' : change.points < 0 ? '−' : '';
  const percent = `${sign}${Math.abs(change.percent).toFixed(2)}%`;
  return withPoints ? `${sign}${price(Math.abs(change.points))} (${percent})` : percent;
}

const direction = (change) => (!change || change.points === 0 ? '' : change.points > 0 ? 'up' : 'down');

export class Tickers {
  constructor(root, select) {
    this.root = root;
    this.select = select;
    this.cells = [];
  }

  update(watch, selected) {
    if (this.cells.length !== watch.length) {
      this.cells = watch.map((summary, index) => {
        const button = element('button', 'ticker', `
          <span class="ticker-name">${summary.symbol}</span>
          <svg class="spark" viewBox="0 0 64 22" preserveAspectRatio="none" aria-hidden="true">
            <defs><linearGradient id="spark-${index}" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stop-color="currentColor" stop-opacity="0.32"/>
              <stop offset="1" stop-color="currentColor" stop-opacity="0"/>
            </linearGradient></defs>
            <path class="spark-fill" fill="url(#spark-${index})"/><path class="spark-line"/>
          </svg>
          <span class="ticker-line"><span class="ticker-price"></span><span class="ticker-change"></span></span>`);
        button.setAttribute('role', 'tab');
        button.addEventListener('click', () => this.select(index));
        this.root.append(button);
        return {
          button,
          spark: button.querySelector('.spark'),
          line: button.querySelector('.spark-line'),
          fill: button.querySelector('.spark-fill'),
          price: button.querySelector('.ticker-price'),
          change: button.querySelector('.ticker-change'),
          shape: '',
        };
      });
    }
    watch.forEach((summary, index) => {
      const cells = this.cells[index];
      const change = dayChange(summary);
      cells.button.setAttribute('aria-selected', String(index === selected));
      text(cells.price, summary.last === null ? '–' : price(summary.last));
      text(cells.change, changeText(change, false));
      cells.change.className = `ticker-change ${direction(change)}`;
      cells.spark.setAttribute('class', `spark ${direction(change)}`);
      const shape = summary.spark.join();
      if (shape !== cells.shape) {
        cells.shape = shape;
        const [line, area] = sparkline(summary.spark, 64, 22);
        cells.line.setAttribute('d', line);
        cells.fill.setAttribute('d', area);
      }
    });
  }
}

// A sparkline's line and the area under it, scaled to fill the box.
function sparkline(values, width, height) {
  if (values.length < 2) {
    return ['', ''];
  }
  const low = Math.min(...values);
  const high = Math.max(...values);
  const span = Math.max(1, high - low);
  const points = values.map((value, index) =>
    `${((index / (values.length - 1)) * width).toFixed(1)},${(2 + (1 - (value - low) / span) * (height - 4)).toFixed(1)}`);
  const line = `M${points.join('L')}`;
  return [line, `${line}L${width},${height}L0,${height}Z`];
}

export class Quote {
  constructor(root) {
    this.price = root.querySelector('#last');
    this.change = root.querySelector('#change');
    this.high = root.querySelector('#day-high');
    this.low = root.querySelector('#day-low');
    this.volume = root.querySelector('#day-volume');
    this.open = root.querySelector('#day-open');
    this.lastPrice = null;
  }

  reset() {
    this.lastPrice = null;
  }

  update(summary) {
    if (summary.last !== null && summary.last !== this.lastPrice) {
      const tick = this.lastPrice === null ? '' : summary.last > this.lastPrice ? ' up' : ' down';
      this.price.className = `quote-price${tick}`;
      this.lastPrice = summary.last;
    }
    text(this.price, summary.last === null ? '–' : price(summary.last));
    const change = dayChange(summary);
    text(this.change, changeText(change));
    this.change.className = `quote-change ${direction(change)}`;
    this.change.title = summary.previous_close === null ? 'Since the open' : 'Since the previous close';
    text(this.high, summary.high === null ? '–' : price(summary.high));
    text(this.low, summary.low === null ? '–' : price(summary.low));
    text(this.volume, compact(summary.volume));
    text(this.open, summary.open === null ? '–' : price(summary.open));
  }
}

export class Session {
  constructor(root) {
    this.phase = root.querySelector('#phase');
    this.clock = root.querySelector('#clock');
    this.countdown = root.querySelector('#countdown');
    this.segments = [...root.querySelectorAll('.segment')];
  }

  update(view, config) {
    text(this.phase, view.phase);
    const auction = view.phase.includes('AUCTION');
    this.phase.className = `phase${auction ? ' auction' : ''}${view.phase === 'CLOSED' ? ' closed' : ''}`;
    text(this.clock, `Day ${view.day} · ${clock(view.time, config.dayLength / 1e9)}`);
    text(this.countdown, `${NEXT[view.phase]} ${(view.phase_left / 1e9).toFixed(1)} s`);
    const current = PHASES.indexOf(view.phase);
    const lengths = [config.opening, config.continuous, config.closing, config.closed];
    this.segments.forEach((segment, index) => {
      const done = index < current ? 1 : index > current ? 0 : 1 - view.phase_left / lengths[index];
      segment.classList.toggle('current', index === current);
      segment.firstElementChild.style.width = `${(Math.min(1, Math.max(0, done)) * 100).toFixed(1)}%`;
    });
  }
}
