// The trade tape, newest first. Each print slides in as it happens, large prints stand out with a bar sized to the
// trade, and an auction's uncross is one amber print however many orders it filled.
import { element, text } from './dom.js';
import { clock, grouped, price } from './format.js';

const ROW = 20;  // the height of a print, --row in style.css

export class TapeView {
  constructor(list, rate) {
    this.list = list;
    this.rate = rate;
    this.newest = null;  // the ID of the newest print shown
  }

  // A new symbol or market: the next prints fill the tape without animating.
  reset() {
    this.list.replaceChildren();
    this.newest = null;
  }

  update(view, daySeconds) {
    const prints = view.trades;
    const capacity = Math.max(1, Math.floor(this.list.clientHeight / ROW));
    const continuous = prints.filter((print) => print[4] !== 'X');
    const sizes = continuous.map((print) => print[3]).sort((a, b) => a - b);
    const big = Math.max(1_000, 4 * (sizes[sizes.length >> 1] ?? 0));
    const largest = Math.max(1, ...sizes);
    const fresh = prints.filter((print) => this.newest === null || print[0] > this.newest).slice(-capacity);
    // A few new prints arrive with a tint; a burst of them, as at high speed, would only tint the whole tape.
    const animate = this.newest !== null && fresh.length <= 3;
    for (const print of fresh) {
      this.list.prepend(this.row(print, daySeconds, big, largest, animate));
    }
    if (fresh.length > 0) {
      this.newest = fresh.at(-1)[0];
    }
    while (this.list.children.length > capacity) {
      this.list.lastElementChild.remove();
    }

    // Trades in the last simulated second; the tape holds only so many, so a busier second shows as a minimum.
    const since = view.time - 1e9;
    const count = continuous.filter((print) => print[1] > since).length;
    const covered = prints.length > 0 && prints[0][1] <= since;
    text(this.rate, count === 0 ? 'Quiet' : `${count}${covered ? '' : '+'} trades/s`);
  }

  row([id, time, tradePrice, size, aggressor, fills, mine], daySeconds, big, largest, animate) {
    const row = element('li', `print ${aggressor}${animate ? ' new' : ''}${mine ? ' mine' : ''}`);
    row.dataset.id = id;
    if (aggressor === 'X') {
      row.title = `Auction uncross at ${clock(time, daySeconds, 2)}: ${grouped(fills)} fills at one price`;
      row.innerHTML = `<span class="cross-tag">Uncross</span><span class="px">${price(tradePrice)}</span>` +
        `<span class="size">${grouped(size)}</span>`;
      return row;
    }
    if (size >= big) {
      row.classList.add('big');
      row.style.setProperty('--share', String(Math.min(1, size / largest)));
    }
    row.title = `${aggressor === 'B' ? 'A buyer' : 'A seller'} took ${grouped(size)} at ${price(tradePrice)}${
      mine ? `: you ${mine === 'B' ? 'bought' : 'sold'}` : ''}`;
    row.innerHTML = `<span class="time">${clock(time, daySeconds, 2)}</span><span class="px">${price(tradePrice)}</span>` +
      `<span class="size">${grouped(size)}</span>`;
    return row;
  }
}
