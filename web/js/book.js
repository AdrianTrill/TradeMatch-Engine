// The order book ladder: asks above, bids below, meeting at the spread, with as many levels as the panel has room for.
import { element, text } from './dom.js';
import { grouped, price } from './format.js';

const ROW = 20;  // the height of a level, --row in style.css

export class BookView {
  constructor(root) {
    root.replaceChildren(element('div', 'columns', '<span>Price</span><span>Size</span><span>Orders</span>'));
    this.asks = element('div', 'book-side asks');
    this.mid = element('div', 'book-mid', '<span class="mid-price">–</span><span class="mid-note"></span>');
    this.bids = element('div', 'book-side bids');
    root.append(this.asks, this.mid, this.bids);
    this.midPrice = this.mid.firstElementChild;
    this.midNote = this.mid.lastElementChild;
    this.rows = 0;
    this.lastPrice = null;
    new ResizeObserver(() => this.layout()).observe(this.asks);
    this.layout();
  }

  // The number of levels shown on each side.
  get levels() {
    return this.rows;
  }

  layout() {
    const rows = Math.max(3, Math.floor(this.asks.clientHeight / ROW));
    if (rows !== this.rows) {
      this.rows = rows;
      this.askCells = build(this.asks, 'ask', rows);
      this.bidCells = build(this.bids, 'bid', rows);
    }
  }

  update(view) {
    const count = this.rows;
    const asks = view.asks.slice(0, count);
    const bids = view.bids.slice(0, count);
    const largest = Math.max(1, ...asks.map((level) => level[1]), ...bids.map((level) => level[1]));
    // Asks run from the highest shown down to the best, so the two best prices meet at the spread.
    this.askCells.forEach((cells, index) => fill(cells, asks[count - 1 - index], largest));
    this.bidCells.forEach((cells, index) => fill(cells, bids[index], largest));

    if (view.last !== null && view.last !== this.lastPrice) {
      this.midPrice.className = `mid-price${this.lastPrice === null ? '' : view.last > this.lastPrice ? ' up' : ' down'}`;
      this.lastPrice = view.last;
    }
    text(this.midPrice, view.last === null ? '–' : price(view.last));
    const bid = view.bids[0]?.[0];
    const ask = view.asks[0]?.[0];
    if (bid === undefined || ask === undefined) {
      text(this.midNote, 'One side of the book is empty');
    } else if (bid >= ask) {
      text(this.midNote, 'Crossed until the auction uncrosses');
    } else {
      const spread = ask - bid;
      text(this.midNote, `Spread ${price(spread)} · ${((spread / ((ask + bid) / 2)) * 1e4).toFixed(1)} bps`);
    }
  }
}

function build(container, side, count) {
  const cells = [];
  for (let index = 0; index < count; index += 1) {
    const row = element('div', `level ${side}`,
      '<div class="bar"></div><span class="px"></span><span class="size"></span><span class="orders"></span>');
    cells.push({ row, bar: row.children[0], price: row.children[1], size: row.children[2], orders: row.children[3] });
  }
  container.replaceChildren(...cells.map((cell) => cell.row));
  return cells;
}

function fill(cells, level, largest) {
  text(cells.price, level ? price(level[0]) : '');
  text(cells.size, level ? grouped(level[1]) : '');
  text(cells.orders, level ? level[2] : '');
  const width = level ? `${((100 * level[1]) / largest).toFixed(1)}%` : '0%';
  if (cells.bar.style.width !== width) {
    cells.bar.style.width = width;
  }
}
