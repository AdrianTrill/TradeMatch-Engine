// The trade tape, newest first, with as many trades as the panel has room for.
import { clock, grouped, price } from './format.js';

const ROW = 20;

export class TapeView {
  constructor(list) {
    this.list = list;
    this.shown = '';
  }

  update(view, daySeconds) {
    const capacity = Math.max(1, Math.floor(this.list.clientHeight / ROW));
    const trades = view.trades.slice(-capacity).reverse();
    const html = trades
      .map(([time, tradePrice, size, aggressor]) =>
        `<li><span class="time">${clock(time, daySeconds, 2)}</span>` +
        `<span class="${aggressor}">${price(tradePrice)}</span><span>${grouped(size)}</span></li>`)
      .join('');
    if (html !== this.shown) {
      this.list.innerHTML = html;
      this.shown = html;
    }
  }
}
