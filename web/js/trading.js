// Trading against the agents: the order ticket, the visitor's open orders, fills, and positions, with live P&L.
// Orders go to the same engine the agents trade on, and stay the visitor's until they fill, expire, or are cancelled.
import { element, text } from './dom.js';
import { clock, grouped, price } from './format.js';
import { toast } from './toast.js';

const TYPES = { LIMIT: 'Limit', MARKET: 'Market', STOP: 'Stop', STOP_LIMIT: 'Stop limit' };
const TIFS = {
  GTC: 'Good till cancelled',
  DAY: 'Day: expires at the close',
  IOC: 'Immediate or cancel: what can trade now',
  FOK: 'Fill or kill: all of it now, or nothing',
};
const LIQUIDITY = { M: 'Maker', T: 'Taker', A: 'Auction' };
const HISTORY = 240;  // P&L samples kept for the account's chart, one per simulated second

// Money from ticks of one cent, signed: +$1,234.50 or −$12.00.
export function money(cents, signed = true) {
  const sign = cents > 0.5 ? '+' : cents < -0.5 ? '−' : '';
  const dollars = Math.abs(cents) / 100;
  return `${signed ? sign : cents < -0.5 ? '−' : ''}$${dollars.toLocaleString('en-US', {
    minimumFractionDigits: 2, maximumFractionDigits: 2 })}`;
}

const parseTicks = (value) => {
  const number = Number(String(value).replace(/[$,\s]/g, ''));
  return Number.isFinite(number) && number > 0 ? Math.round(number * 100) : null;
};

const parseShares = (value) => {
  const number = Number(String(value).replace(/[,\s]/g, ''));
  return Number.isInteger(number) && number > 0 ? number : null;
};

export class Ticket {
  constructor(form, market, onSent) {
    this.form = form;
    this.market = market;
    this.onSent = onSent;
    this.state = { side: 'BUY', type: 'LIMIT', tif: 'GTC' };
    this.symbol = -1;
    this.view = null;
    this.fields = Object.fromEntries([...form.querySelectorAll('[id]')].map((node) => [node.id, node]));

    for (const button of form.querySelectorAll('[data-side]')) {
      button.addEventListener('click', () => this.setSide(button.dataset.side));
    }
    for (const button of form.querySelectorAll('[data-type]')) {
      button.addEventListener('click', () => this.setType(button.dataset.type));
    }
    for (const button of form.querySelectorAll('[data-tif]')) {
      button.title = TIFS[button.dataset.tif];
      button.addEventListener('click', () => this.setTif(button.dataset.tif));
    }
    for (const button of form.querySelectorAll('[data-step]')) {
      button.addEventListener('click', () => this.step(button.dataset.field, Number(button.dataset.step)));
    }
    for (const button of form.querySelectorAll('[data-fill]')) {
      button.addEventListener('click', () => this.fillPrice(button.dataset.fill));
    }
    for (const button of form.querySelectorAll('[data-shares]')) {
      button.addEventListener('click', () => {
        this.fields.quantity.value = grouped(Number(button.dataset.shares));
        this.refresh();
      });
    }
    form.addEventListener('input', () => this.refresh());
    form.addEventListener('submit', (event) => {
      event.preventDefault();
      this.send();
    });
    this.setSide('BUY');
    this.setType('LIMIT');
    this.setTif('GTC');
  }

  setSide(side) {
    this.state.side = side;
    for (const button of this.form.querySelectorAll('[data-side]')) {
      button.setAttribute('aria-checked', String(button.dataset.side === side));
    }
    this.form.dataset.side = side;
    this.refresh();
  }

  setType(type) {
    this.state.type = type;
    for (const button of this.form.querySelectorAll('[data-type]')) {
      button.setAttribute('aria-checked', String(button.dataset.type === type));
    }
    this.form.dataset.type = type;
    this.refresh();
  }

  setTif(tif) {
    this.state.tif = tif;
    for (const button of this.form.querySelectorAll('[data-tif]')) {
      button.setAttribute('aria-checked', String(button.dataset.tif === tif));
    }
    this.refresh();
  }

  // A price picked from the order book or the chart: a limit order at that price.
  pick(ticks) {
    if (this.state.type === 'MARKET' || this.state.type === 'STOP') {
      this.setType('LIMIT');
    }
    this.fields.price.value = price(ticks);
    this.refresh();
    this.fields.price.focus({ preventScroll: true });
  }

  step(field, direction) {
    const input = this.fields[field];
    if (field === 'quantity') {
      const shares = parseShares(input.value) ?? 0;
      input.value = grouped(Math.max(100, shares + direction * 100));
    } else {
      const ticks = parseTicks(input.value) ?? this.reference() ?? 10_000;
      input.value = price(Math.max(1, ticks + direction));
    }
    this.refresh();
  }

  fillPrice(which) {
    const view = this.view;
    if (!view) {
      return;
    }
    const bid = view.bids[0]?.[0];
    const ask = view.asks[0]?.[0];
    const ticks = which === 'bid' ? bid : which === 'ask' ? ask
      : bid !== undefined && ask !== undefined ? Math.round((bid + ask) / 2) : view.last;
    if (ticks !== undefined && ticks !== null) {
      this.fields.price.value = price(ticks);
      this.refresh();
    }
  }

  // The price to start from: the side's own best price, so that an order rests rather than trades by accident.
  reference() {
    const view = this.view;
    if (!view) {
      return null;
    }
    return (this.state.side === 'BUY' ? view.bids[0]?.[0] : view.asks[0]?.[0]) ?? view.last;
  }

  update(view, symbol) {
    this.view = view;
    if (symbol !== this.symbol) {
      this.symbol = symbol;
      text(this.fields['ticket-symbol'], view.symbol);
      const reference = this.reference();
      this.fields.price.value = reference ? price(reference) : '';
      const stop = reference ? reference + (this.state.side === 'BUY' ? 20 : -20) : null;
      this.fields.stop.value = stop ? price(stop) : '';
    }
    this.refresh();
  }

  // The order as the form describes it, and what is wrong with it if anything.
  order() {
    const { side, type, tif } = this.state;
    const quantity = parseShares(this.fields.quantity.value);
    const limit = type === 'LIMIT' || type === 'STOP_LIMIT' ? parseTicks(this.fields.price.value) : 0;
    const stop = type === 'STOP' || type === 'STOP_LIMIT' ? parseTicks(this.fields.stop.value) : 0;
    const iceberg = this.fields.iceberg.checked && (type === 'LIMIT' || type === 'STOP_LIMIT');
    const display = iceberg ? parseShares(this.fields.display.value) : 0;
    let problem = '';
    if (!quantity) {
      problem = 'Enter a whole number of shares';
    } else if (limit === null) {
      problem = 'Enter a limit price';
    } else if (stop === null) {
      problem = 'Enter a stop price';
    } else if (iceberg && !display) {
      problem = 'Enter how many shares the iceberg shows';
    }
    return {
      request: { side, type, quantity: quantity ?? 0, price: limit ?? 0, stop: stop ?? 0,
        tif: type === 'MARKET' ? 'IOC' : tif, postOnly: type === 'LIMIT' && this.fields['post-only'].checked,
        display: display ?? 0 },
      problem,
    };
  }

  // Rewrites the submit button and the estimate of what would trade at once.
  refresh() {
    const { request, problem } = this.order();
    const view = this.view;
    const symbol = view?.symbol ?? '';
    const verb = request.side === 'BUY' ? 'Buy' : 'Sell';
    const at = request.type === 'MARKET' ? 'at market' : request.type === 'STOP' ? `on a stop at ${price(request.stop)}`
      : request.type === 'STOP_LIMIT' ? `stop ${price(request.stop)}, limit ${price(request.price)}`
      : `at ${price(request.price)}`;
    text(this.fields.submit, problem || `${verb} ${grouped(request.quantity)} ${symbol} ${at}`);
    this.fields.submit.disabled = Boolean(problem);
    this.form.querySelector('.tif').classList.toggle('disabled', request.type === 'MARKET');

    // What would trade right now against the levels shown, for an order that can.
    let estimate = '–';
    let value = '–';
    if (view && !problem) {
      const levels = request.side === 'BUY' ? view.asks : view.bids;
      const limit = request.type === 'MARKET' ? null : request.type === 'LIMIT' ? request.price : undefined;
      if (view.phase.includes('AUCTION')) {
        estimate = view.auction ? `Joins the auction (${price(view.auction.price)})` : 'Joins the auction';
      } else if (limit !== undefined) {
        let shares = 0;
        let cost = 0;
        for (const [levelPrice, size] of levels) {
          if (limit !== null && (request.side === 'BUY' ? levelPrice > limit : levelPrice < limit)) {
            break;
          }
          const take = Math.min(size, request.quantity - shares);
          shares += take;
          cost += take * levelPrice;
          if (shares === request.quantity) {
            break;
          }
        }
        const rest = request.type === 'MARKET' || request.tif === 'IOC' || request.tif === 'FOK' ? 'expires' : 'rests';
        if (request.tif === 'FOK' && shares < request.quantity) {
          estimate = 'Not all of it now: nothing trades';
        } else {
          estimate = shares === 0 ? (limit === null ? 'Nothing to trade' : `Nothing now: it ${rest}`)
            : `${grouped(shares)} at ${(cost / shares / 100).toFixed(3)}${shares < request.quantity ? `, the rest ${rest}` : ''}`;
        }
        // A market order is worth what it would trade; a limit order, its size at its limit.
        value = money(request.type === 'MARKET' ? cost : request.quantity * request.price, false);
      } else {
        estimate = 'Waits for the stop';
        value = money(request.quantity * (request.price || request.stop), false);
      }
    }
    text(this.fields['estimate-fill'], estimate);
    text(this.fields['estimate-value'], value);
  }

  send() {
    const { request, problem } = this.order();
    if (problem || !this.market.instance) {
      return;
    }
    const result = JSON.parse(this.market.instance.submit(this.market.symbol, request));
    this.onSent(request, result);
  }
}

// One symbol's position, valued by average cost.
class Position {
  constructor() {
    this.shares = 0;
    this.average = 0;  // ticks
    this.realized = 0;  // cents
  }

  apply(side, fillPrice, quantity) {
    const signed = side === 'BUY' ? quantity : -quantity;
    if (this.shares === 0 || Math.sign(this.shares) === Math.sign(signed)) {
      this.average = (this.average * Math.abs(this.shares) + fillPrice * quantity) / (Math.abs(this.shares) + quantity);
      this.shares += signed;
      return;
    }
    const closing = Math.min(quantity, Math.abs(this.shares));
    this.realized += closing * (fillPrice - this.average) * Math.sign(this.shares);
    this.shares += signed;
    if (this.shares === 0) {
      this.average = 0;
    } else if (Math.sign(this.shares) === Math.sign(signed)) {
      this.average = fillPrice;  // the fill closed the position and opened one the other way
    }
  }

  unrealized(mark) {
    return this.shares === 0 || mark === null ? 0 : this.shares * (mark - this.average);
  }
}

export class Account {
  constructor(root, card, market, { onFlatten, onCancel }) {
    this.root = root;
    this.card = Object.fromEntries([...card.querySelectorAll('[id]')].map((node) => [node.id, node]));
    this.market = market;
    this.onFlatten = onFlatten;
    this.onCancel = onCancel;
    this.tab = 'open';
    this.shown = {};
    this.reset();
    for (const tab of root.querySelectorAll('[data-tab]')) {
      tab.addEventListener('click', () => this.setTab(tab.dataset.tab));
    }
    root.addEventListener('click', (event) => {
      const button = event.target.closest('button[data-action]');
      if (!button) {
        return;
      }
      if (button.dataset.action === 'cancel') {
        this.onCancel(Number(button.dataset.order));
      } else if (button.dataset.action === 'flatten') {
        this.onFlatten(Number(button.dataset.symbol));
      } else if (button.dataset.action === 'cancel-all') {
        this.onCancel(null);
      }
    });
  }

  reset() {
    this.orders = [];
    this.fills = [];
    this.positions = [];
    this.history = [];
    this.lastSample = -1;
    this.shown = {};
  }

  setTab(tab) {
    this.tab = tab;
    for (const button of this.root.querySelectorAll('[data-tab]')) {
      button.classList.toggle('active', button.dataset.tab === tab);
      button.setAttribute('aria-selected', String(button.dataset.tab === tab));
    }
    this.shown = {};
  }

  position(symbol) {
    this.positions[symbol] ??= new Position();
    return this.positions[symbol];
  }

  // Takes the engine's news about the visitor's orders: working orders, new fills, and finished orders, which it
  // announces.
  update(view, symbols, daySeconds) {
    const news = JSON.parse(this.market.instance.account());
    this.orders = news.orders.sort((a, b) => b[0] - a[0]);
    this.announce(news, symbols, view);
    for (const fill of news.fills) {
      this.fills.unshift(fill);
      this.position(fill[2]).apply(fill[4], fill[5], fill[6]);
    }
    this.fills.length = Math.min(this.fills.length, 500);

    const marks = view.watch.map((summary) => summary.last);
    let realized = 0;
    let unrealized = 0;
    this.positions.forEach((position, symbol) => {
      realized += position.realized;
      unrealized += position.unrealized(marks[symbol]);
    });
    this.total = { realized, unrealized, pnl: realized + unrealized };
    const second = Math.floor(view.time / 1e9);
    if (second !== this.lastSample && (this.fills.length > 0 || this.history.length > 0)) {
      this.lastSample = second;
      this.history.push(this.total.pnl);
      this.history.splice(0, Math.max(0, this.history.length - HISTORY));
    }
    this.render(symbols, marks, daySeconds);
  }

  // One toast per order for this frame's fills, and one for each order that finished without filling completely.
  announce(news, symbols, view) {
    const byOrder = new Map();
    for (const [, , symbol, order, side, fillPrice, quantity] of news.fills) {
      const entry = byOrder.get(order) ?? { symbol, side, shares: 0, cost: 0, fills: 0 };
      entry.shares += quantity;
      entry.cost += fillPrice * quantity;
      entry.fills += 1;
      byOrder.set(order, entry);
    }
    for (const { symbol, side, shares, cost, fills } of byOrder.values()) {
      const verb = side === 'BUY' ? 'Bought' : 'Sold';
      toast(`${verb} ${grouped(shares)} ${symbols[symbol]} at ${(cost / shares / 100).toFixed(2)}${
        fills > 1 ? ` in ${fills} fills` : ''}`, side === 'BUY' ? 'buy' : 'sell');
    }
    for (const [, symbol, side, quantity, filled, reason] of news.done) {
      if (reason === 'filled' || reason === 'cancelled') {
        continue;  // fills and the visitor's own cancels are announced already
      }
      const what = `${side === 'BUY' ? 'buy' : 'sell'} ${grouped(quantity)} ${symbols[symbol]}`;
      if (reason === 'expired') {
        const filledPart = filled > 0 ? `, ${grouped(filled)} filled` : '';
        toast(`Your ${what} expired${filledPart}${view.phase === 'CLOSED' ? ' at the close' : ''}`, 'info');
      } else if (reason === 'self-trade prevention') {
        toast(`Your ${what} was cancelled: a newer order of yours would have traded with it`, 'info');
      }
    }
  }

  render(symbols, marks, daySeconds) {
    const { pnl, realized, unrealized } = this.total;
    const tone = (cents) => (cents > 0.5 ? 'up' : cents < -0.5 ? 'down' : '');
    text(this.card.pnl, money(pnl));
    this.card.pnl.className = `pnl-value ${tone(pnl)}`;
    text(this.card['pnl-realized'], money(realized));
    this.card['pnl-realized'].className = tone(realized);
    text(this.card['pnl-unrealized'], money(unrealized));
    this.card['pnl-unrealized'].className = tone(unrealized);
    this.renderChart(this.card['pnl-chart']);
    text(this.root.querySelector('[data-count="open"]'), this.orders.length);
    text(this.root.querySelector('[data-count="fills"]'), this.fills.length);
    const open = this.positions.filter((position) => position && position.shares !== 0).length;
    text(this.root.querySelector('[data-count="positions"]'), open);
    this.root.querySelector('[data-action="cancel-all"]').hidden = this.tab !== 'open' || this.orders.length === 0;

    const day = (time) => clock(time, daySeconds, 2);
    let rows;
    let empty;
    if (this.tab === 'open') {
      empty = 'No open orders. Click a price in the order book to start one.';
      rows = this.orders.map(([id, symbol, side, type, tif, quantity, limit, stop, , filled, remaining, status]) => `
        <tr><td>${symbols[symbol]}</td><td class="${side === 'BUY' ? 'buy' : 'sell'}">${side === 'BUY' ? 'Buy' : 'Sell'}</td>
        <td>${TYPES[type]}${tif !== 'GTC' ? ` · ${tif}` : ''}</td>
        <td class="num">${type === 'MARKET' ? 'Market' : type === 'STOP' ? `Stop ${price(stop)}`
          : type === 'STOP_LIMIT' ? `${price(stop)} → ${price(limit)}` : price(limit)}</td>
        <td class="num">${grouped(filled)} / ${grouped(quantity)}</td>
        <td><span class="status ${status}">${status === 'resting' ? (filled > 0 ? 'Partly filled' : 'Resting')
          : status === 'stop' ? 'Waiting for stop' : 'Queued for uncross'}</span></td>
        <td class="end"><button class="row-action" data-action="cancel" data-order="${id}" title="Cancel order ${id}" aria-label="Cancel">×</button></td></tr>`);
    } else if (this.tab === 'fills') {
      empty = 'No fills yet. Orders that trade show up here.';
      rows = this.fills.slice(0, 200).map(([, time, symbol, , side, fillPrice, quantity, liquidity]) => `
        <tr><td class="muted">${day(time)}</td><td>${symbols[symbol]}</td>
        <td class="${side === 'BUY' ? 'buy' : 'sell'}">${side === 'BUY' ? 'Buy' : 'Sell'}</td>
        <td class="num">${price(fillPrice)}</td><td class="num">${grouped(quantity)}</td>
        <td><span class="liquidity ${liquidity}">${LIQUIDITY[liquidity]}</span></td></tr>`);
    } else {
      empty = 'No positions. Fills build them, and they are marked to the last trade.';
      rows = this.positions.map((position, symbol) => {
        if (!position || (position.shares === 0 && position.realized === 0)) {
          return '';
        }
        const unrealized = position.unrealized(marks[symbol]);
        return `<tr><td>${symbols[symbol]}</td>
          <td class="num ${position.shares > 0 ? 'buy' : position.shares < 0 ? 'sell' : ''}">${
            position.shares > 0 ? '+' : ''}${grouped(position.shares)}</td>
          <td class="num">${position.shares === 0 ? '–' : (position.average / 100).toFixed(3)}</td>
          <td class="num">${marks[symbol] === null ? '–' : price(marks[symbol])}</td>
          <td class="num ${unrealized > 0.5 ? 'up' : unrealized < -0.5 ? 'down' : ''}">${money(unrealized)}</td>
          <td class="num ${position.realized > 0.5 ? 'up' : position.realized < -0.5 ? 'down' : ''}">${money(position.realized)}</td>
          <td class="end">${position.shares === 0 ? '' : `<button class="text-button" data-action="flatten" data-symbol="${symbol}">Close</button>`}</td></tr>`;
      }).filter(Boolean);
    }
    const html = rows.length === 0 ? `<p class="empty">${empty}</p>` : `<table class="${this.tab}">${this.header()}<tbody>${rows.join('')}</tbody></table>`;
    if (html !== this.shown.table) {
      this.root.querySelector('.account-table').innerHTML = html;
      this.shown.table = html;
    }
  }

  header() {
    const columns = {
      open: ['Symbol', 'Side', 'Type', 'Price', 'Filled', 'Status', ''],
      fills: ['Time', 'Symbol', 'Side', 'Price', 'Shares', 'Liquidity'],
      positions: ['Symbol', 'Shares', 'Average', 'Last', 'Unrealized', 'Realized', ''],
    }[this.tab];
    const numeric = new Set(['Price', 'Filled', 'Shares', 'Average', 'Last', 'Unrealized', 'Realized']);
    return `<thead><tr>${columns.map((name) => `<th class="${numeric.has(name) ? 'num' : ''}">${name}</th>`).join('')}</tr></thead>`;
  }

  // The P&L's path as a filled line around zero.
  renderChart(svg) {
    const values = this.history;
    const key = values.length === 0 ? '' : `${values.length}:${values.at(-1)}`;
    if (key === this.shown.chart) {
      return;
    }
    this.shown.chart = key;
    const [line, area, zero] = ['.pnl-line', '.pnl-area', '.pnl-zero'].map((selector) => svg.querySelector(selector));
    if (values.length < 2) {
      line.setAttribute('d', '');
      area.setAttribute('d', '');
      zero.setAttribute('d', '');
      return;
    }
    const low = Math.min(0, ...values);
    const high = Math.max(0, ...values);
    const span = Math.max(1, high - low);
    const y = (value) => 3 + (1 - (value - low) / span) * 34;
    const points = values.map((value, index) => `${((index / (values.length - 1)) * 200).toFixed(1)},${y(value).toFixed(1)}`);
    line.setAttribute('d', `M${points.join('L')}`);
    area.setAttribute('d', `M0,${y(0)}L${points.join('L')}L200,${y(0)}Z`);
    zero.setAttribute('d', `M0,${y(0).toFixed(1)}H200`);
    svg.classList.toggle('down', values.at(-1) < 0);
  }

  // The visitor's working orders and fills on one symbol, and the position's average price, for the chart and book.
  overlay(symbol) {
    const position = this.positions[symbol];
    return {
      orders: this.orders.filter((order) => order[1] === symbol),
      fills: this.fills.filter((fill) => fill[2] === symbol).slice(0, 200),
      position: position && position.shares !== 0 ? { shares: position.shares, average: position.average } : null,
    };
  }
}
