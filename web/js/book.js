// The order book ladder: asks above, bids below, meeting at the spread, with as many levels as the panel has room for.
// Levels can be grouped into coarser price steps. Each row shades its share of the depth shown, flashes when its size
// changes, and on hover reads out what sweeping the book down to it would cost.
import { element, text } from './dom.js';
import { compact, grouped, price } from './format.js';

const ROW = 20;  // the height of a level, --row in style.css
const FLASH_GAP = 140;  // milliseconds a row waits between flashes

export const GROUPINGS = [1, 5, 10, 25, 50];  // ticks per row

export class BookView {
  constructor(root, onPick) {
    this.onPick = onPick;
    root.replaceChildren(element('div', 'columns', '<span>Price</span><span>Size</span><span>Orders</span>'));
    this.asks = element('div', 'book-side asks');
    this.mid = element('div', 'book-mid', `
      <span class="mid-price"><svg class="icon tick"><use href="#i-arrow"/></svg><span class="mid-value">–</span></span>
      <span class="mid-note"></span>`);
    this.bids = element('div', 'book-side bids');
    this.gauge = element('div', 'imbalance', `
      <span class="imbalance-label buy">B <b>–</b></span>
      <span class="imbalance-bar"><i></i></span>
      <span class="imbalance-label sell"><b>–</b> S</span>`);
    root.append(this.asks, this.mid, this.bids, this.gauge);
    this.midPrice = this.mid.querySelector('.mid-price');
    this.midValue = this.mid.querySelector('.mid-value');
    this.midNote = this.mid.querySelector('.mid-note');
    this.gaugeFill = this.gauge.querySelector('i');
    [this.gaugeBuy, this.gaugeSell] = this.gauge.querySelectorAll('b');
    this.tooltip = element('div', 'book-tooltip');
    this.tooltip.hidden = true;
    document.body.append(this.tooltip);
    this.rows = 0;
    this.grouping = 1;
    this.lastPrice = null;
    this.sizes = new Map();  // the size last shown at each side and price, to flash changes
    this.hovered = null;
    new ResizeObserver(() => this.layout()).observe(this.asks);
    this.layout();
  }

  // The number of levels shown on each side.
  get levels() {
    return this.rows;
  }

  // Levels the engine should send, enough to fill every row once grouped.
  get depth() {
    return Math.max(24, this.rows * this.grouping);
  }

  setGrouping(ticks) {
    this.grouping = ticks;
    this.reset();
  }

  // A new symbol or grouping: start the flashes afresh.
  reset() {
    this.sizes.clear();
    this.lastPrice = null;
  }

  layout() {
    const rows = Math.max(3, Math.floor(this.asks.clientHeight / ROW));
    if (rows !== this.rows) {
      this.rows = rows;
      this.askCells = this.build(this.asks, 'ask', rows);
      this.bidCells = this.build(this.bids, 'bid', rows);
    }
  }

  build(container, side, count) {
    const cells = [];
    for (let index = 0; index < count; index += 1) {
      const row = element('div', `level ${side}`, `
        <div class="depth-bar"></div><div class="bar"></div><div class="flash"></div>
        <span class="px"></span><span class="size"></span><span class="orders"></span>`);
      const cell = {
        row,
        side,
        index,
        depthBar: row.children[0],
        bar: row.children[1],
        flash: row.children[2],
        price: row.children[3],
        size: row.children[4],
        orders: row.children[5],
        level: null,
        flashed: 0,
      };
      row.addEventListener('mouseenter', () => this.hover(cell));
      row.addEventListener('mouseleave', () => this.hover(null));
      row.addEventListener('click', () => cell.level && this.onPick?.(cell.level[0], side));
      cells.push(cell);
    }
    container.replaceChildren(...cells.map((cell) => cell.row));
    return cells;
  }

  // `orders` are the visitor's working orders on this symbol, marked on the levels they rest at.
  update(view, orders = []) {
    const count = this.rows;
    const asks = group(view.asks, this.grouping, Math.ceil).slice(0, count);
    const bids = group(view.bids, this.grouping, Math.floor).slice(0, count);
    this.mine = new Map();
    for (const [, , side, type, , , limit, , , , remaining, status] of orders) {
      if (type === 'LIMIT' && status === 'resting') {
        const key = side === 'BUY' ? `bid${Math.floor(limit / this.grouping) * this.grouping}`
          : `ask${Math.ceil(limit / this.grouping) * this.grouping}`;
        this.mine.set(key, (this.mine.get(key) ?? 0) + remaining);
      }
    }
    this.sides = { ask: asks, bid: bids };
    const largest = Math.max(1, ...asks.map((level) => level[1]), ...bids.map((level) => level[1]));
    const deepest = Math.max(1, total(asks), total(bids));
    const now = performance.now();
    this.shown = new Map();  // this frame's sizes, which the next frame compares against
    // Asks run from the highest shown down to the best, so the two best prices meet at the spread.
    let cumulative = 0;
    for (let index = 0; index < count; index += 1) {
      const level = asks[index];
      cumulative += level ? level[1] : 0;
      this.fill(this.askCells[count - 1 - index], level, largest, cumulative / deepest, now);
    }
    cumulative = 0;
    for (let index = 0; index < count; index += 1) {
      const level = bids[index];
      cumulative += level ? level[1] : 0;
      this.fill(this.bidCells[index], level, largest, cumulative / deepest, now);
    }
    this.sizes = this.shown;
    this.updateMid(view, bids, asks);
    this.updateGauge(bids, asks);
    if (this.hovered) {
      this.hover(this.hovered);
    }
  }

  fill(cell, level, largest, depthShare, now) {
    cell.level = level ?? null;
    text(cell.price, level ? price(level[0]) : '');
    text(cell.size, level ? grouped(level[1]) : '');
    text(cell.orders, level ? level[2] : '');
    setWidth(cell.bar, level ? level[1] / largest : 0);
    setWidth(cell.depthBar, level ? depthShare : 0);
    const key = level ? `${cell.side}${level[0]}` : '';
    const mine = level ? this.mine.get(key) : undefined;
    cell.row.classList.toggle('mine', mine !== undefined);
    cell.row.title = mine === undefined ? '' : `Your order${mine > 0 ? `: ${grouped(mine)} shares` : ''}`;
    if (!level) {
      return;
    }
    // Flash a level whose size changed: in its side's color when it grew, neutral when it shrank.
    const before = this.sizes.get(key);
    this.shown.set(key, level[1]);
    if (before !== undefined && before !== level[1] && now - cell.flashed > FLASH_GAP) {
      cell.flashed = now;
      cell.flash.className = `flash ${level[1] > before ? 'grew' : 'shrank'}`;
      cell.flash.animate([{ opacity: 0.85 }, { opacity: 0 }], { duration: 650, easing: 'ease-out' });
    }
  }

  updateMid(view, bids, asks) {
    // During a call auction the middle of the book shows the price the uncross would trade at.
    if (view.auction) {
      this.midPrice.className = 'mid-price auction';
      this.lastPrice = null;
      text(this.midValue, price(view.auction.price));
      text(this.midNote, `Indicative uncross · ${grouped(view.auction.matched)} would trade`);
      return;
    }
    if (view.last !== null && view.last !== this.lastPrice) {
      this.midPrice.className = `mid-price${this.lastPrice === null ? '' : view.last > this.lastPrice ? ' up' : ' down'}`;
      this.lastPrice = view.last;
    }
    text(this.midValue, view.last === null ? '–' : price(view.last));
    const bid = view.bids[0]?.[0];
    const ask = view.asks[0]?.[0];
    if (view.phase.includes('AUCTION')) {
      text(this.midNote, 'Call auction: nothing would trade yet');
    } else if (bid === undefined || ask === undefined) {
      text(this.midNote, 'One side of the book is empty');
    } else if (bid >= ask) {
      text(this.midNote, 'Crossed until the auction uncrosses');
    } else {
      const spread = ask - bid;
      const ticks = `${spread} tick${spread === 1 ? '' : 's'}`;
      text(this.midNote, `Spread ${price(spread)} · ${ticks} · ${((spread / ((ask + bid) / 2)) * 1e4).toFixed(1)} bps`);
    }
  }

  // Buyers' share of the size on the levels shown.
  updateGauge(bids, asks) {
    const buy = total(bids);
    const sell = total(asks);
    const share = buy + sell === 0 ? 0.5 : buy / (buy + sell);
    setWidth(this.gaugeFill, share);
    text(this.gaugeBuy, `${Math.round(share * 100)}%`);
    text(this.gaugeSell, `${100 - Math.round(share * 100)}%`);
  }

  // Highlights the levels from the spread to the one under the pointer, and reads out what sweeping them would take.
  hover(cell) {
    for (const other of [...this.askCells, ...this.bidCells]) {
      other.row.classList.remove('swept');
    }
    this.hovered = cell;
    if (!cell || !cell.level) {
      this.tooltip.hidden = true;
      return;
    }
    const levels = this.sides[cell.side];
    const depth = levels.findIndex((level) => level[0] === cell.level[0]);
    const swept = levels.slice(0, depth + 1);
    const shares = total(swept);
    const cost = swept.reduce((sum, level) => sum + level[0] * level[1], 0);
    const cells = cell.side === 'ask' ? this.askCells.slice(this.rows - 1 - depth) : this.bidCells.slice(0, depth + 1);
    for (const sweptCell of cells) {
      sweptCell.row.classList.add('swept');
    }
    const verb = cell.side === 'ask' ? 'Buying' : 'Selling';
    this.tooltip.innerHTML = `
      <div class="tooltip-title">${verb} down to ${price(cell.level[0])}</div>
      <dl><dt>Shares</dt><dd>${grouped(shares)}</dd>
      <dt>Average price</dt><dd>${(cost / shares / 100).toFixed(3)}</dd>
      <dt>Value</dt><dd>$${compact(cost / 100)}</dd>
      <dt>Levels</dt><dd>${swept.length}</dd></dl>`;
    this.tooltip.hidden = false;
    const box = cell.row.getBoundingClientRect();
    const tip = this.tooltip.getBoundingClientRect();
    this.tooltip.style.left = `${Math.max(8, box.left - tip.width - 10)}px`;
    this.tooltip.style.top = `${Math.min(window.innerHeight - tip.height - 8, Math.max(8, box.top + box.height / 2 - tip.height / 2))}px`;
  }
}

// Levels merged into steps of `ticks`, rounding bids down and asks up, as exchanges group their books.
function group(levels, ticks, round) {
  if (ticks === 1) {
    return levels;
  }
  const merged = [];
  for (const [levelPrice, size, orders] of levels) {
    const step = round(levelPrice / ticks) * ticks;
    const last = merged.at(-1);
    if (last && last[0] === step) {
      last[1] += size;
      last[2] += orders;
    } else {
      merged.push([step, size, orders]);
    }
  }
  return merged;
}

function total(levels) {
  return levels.reduce((sum, level) => sum + level[1], 0);
}

function setWidth(node, share) {
  const width = `${(Math.min(1, share) * 100).toFixed(1)}%`;
  if (node.style.width !== width) {
    node.style.width = width;
  }
}
