// The price chart: candles at a chosen timeframe on a time axis, with volume, the hidden fair value, the day's VWAP,
// each day's auctions and close shaded, the auction crosses marked, and a crosshair. The wheel zooms, dragging
// scrolls back in time, and a double click returns to the live edge.
import { crisp, font, niceStep, surface, tag } from './canvas.js';
import { text } from './dom.js';
import { clock, compact, grouped, price } from './format.js';
import { lowerBound } from './history.js';
import { palette } from './palette.js';

const AXIS = 68;  // width of the price axis
const TIME_AXIS = 24;  // height of the time axis
const TOP = 60;  // room above the candles for the legend
const RIGHT_PAD = 3;  // empty slots to the right of the live candle
const ZOOM = { min: 20, max: 480, initial: 96 };
const TIME_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1_800, 3_600, 7_200, 14_400];

export const TIMEFRAMES = [1, 5, 15, 60];

export class PriceChart {
  constructor(canvas, legend, liveButton) {
    this.canvas = canvas;
    this.legend = legend;
    this.liveButton = liveButton;
    this.timeframe = 1;
    this.visible = ZOOM.initial;
    this.anchor = null;  // the newest slot shown when scrolled back, or null to follow the live edge
    this.range = null;  // the price range drawn, eased toward the visible data's range
    this.hover = null;
    this.drag = null;
    this.show = { fair: true, vwap: true };
    this.layout = null;
    this.fit = true;  // fit the zoom to the data on the next draw
    this.listen();
    liveButton.addEventListener('click', () => this.goLive());
    for (const button of legend.querySelectorAll('[data-series]')) {
      button.addEventListener('click', () => {
        this.show[button.dataset.series] = !this.show[button.dataset.series];
        button.setAttribute('aria-pressed', String(this.show[button.dataset.series]));
      });
    }
  }

  setTimeframe(seconds) {
    this.timeframe = seconds;
    this.goLive();
  }

  goLive() {
    this.anchor = null;
    this.visible = ZOOM.initial;
    this.range = null;
    this.fit = true;
  }

  // A new symbol or market: snap the price range instead of easing toward it.
  reset() {
    this.range = null;
  }

  listen() {
    const canvas = this.canvas;
    const position = (event) => {
      const box = canvas.getBoundingClientRect();
      return { x: event.clientX - box.left, y: event.clientY - box.top };
    };
    canvas.addEventListener('pointermove', (event) => {
      this.hover = position(event);
      if (this.drag && this.layout) {
        const slots = (this.drag.x - this.hover.x) / this.layout.slotWidth;
        this.anchor = Math.min(this.layout.liveSlot, Math.round(this.drag.anchor + slots));
        if (this.anchor >= this.layout.liveSlot) {
          this.anchor = null;
        }
      }
    });
    canvas.addEventListener('pointerleave', () => {
      this.hover = null;
    });
    canvas.addEventListener('pointerdown', (event) => {
      if (!this.layout) {
        return;
      }
      canvas.setPointerCapture(event.pointerId);
      this.drag = { x: position(event).x, anchor: this.anchor ?? this.layout.liveSlot };
      canvas.classList.add('dragging');
    });
    const release = () => {
      this.drag = null;
      canvas.classList.remove('dragging');
    };
    canvas.addEventListener('pointerup', release);
    canvas.addEventListener('pointercancel', release);
    canvas.addEventListener('dblclick', () => this.goLive());
    canvas.addEventListener('wheel', (event) => {
      if (!this.layout) {
        return;
      }
      event.preventDefault();
      // Zoom around the slot under the pointer, which stays put.
      const { firstSlot, slotWidth, liveSlot } = this.layout;
      const pointer = position(event).x;
      const slot = firstSlot + pointer / slotWidth;
      const visible = Math.round(Math.min(ZOOM.max, Math.max(ZOOM.min, this.visible * Math.exp(event.deltaY * 0.0015))));
      const width = this.layout.plot / (visible + RIGHT_PAD);
      const first = slot - pointer / width;
      const anchor = Math.round(first + visible - 1);
      this.visible = visible;
      this.anchor = anchor >= liveSlot ? null : anchor;
    }, { passive: false });
  }

  draw({ view, history, config }) {
    const { context, width, height } = surface(this.canvas);
    const timeframe = this.timeframe;
    const length = timeframe * 1e9;
    const bars = history.bars(timeframe);
    const plot = width - AXIS;
    const bottom = height - TIME_AXIS;
    const volumeTop = bottom - Math.max(36, Math.round((bottom - TOP) * 0.17));
    const priceBottom = volumeTop - 10;

    const liveSlot = Math.floor(view.time / length);
    if (this.fit) {
      // A short history fills the chart with wider candles rather than leaving it mostly empty.
      this.visible = Math.max(ZOOM.min, Math.min(ZOOM.initial, liveSlot + 1));
      this.fit = false;
    }
    const lastSlot = this.anchor ?? liveSlot;
    const firstSlot = lastSlot - this.visible + 1;
    const slotWidth = plot / (this.visible + RIGHT_PAD);
    const xOfSlot = (slot) => (slot - firstSlot + 0.5) * slotWidth;
    const xOfTime = (time) => (time / length - firstSlot) * slotWidth;
    this.layout = { plot, firstSlot, slotWidth, liveSlot };
    this.liveButton.hidden = this.anchor === null;

    const first = lowerBound(bars, firstSlot * length);
    const end = lowerBound(bars, (lastSlot + 1) * length);
    const shown = bars.slice(first, end);

    this.drawSessions(context, config, firstSlot, lastSlot, length, xOfTime, plot, bottom);
    if (bars.length === 0) {
      this.legendFor(view, null, null);
      context.font = font(13);
      context.fillStyle = palette.muted;
      context.textAlign = 'center';
      context.fillText('Waiting for the opening auction to trade…', plot / 2, (TOP + priceBottom) / 2);
      context.textAlign = 'left';
      this.drawTimeAxis(context, config, firstSlot, lastSlot, length, xOfTime, plot, bottom, height);
      return;
    }

    // The price range of what is shown, eased so that new highs and lows don't jolt the chart.
    let low = Infinity;
    let high = -Infinity;
    for (const bar of shown) {
      low = Math.min(low, bar[3], this.show.fair ? bar[6] : Infinity, this.show.vwap && bar[7] ? bar[7] : Infinity);
      high = Math.max(high, bar[2], this.show.fair ? bar[6] : -Infinity, this.show.vwap && bar[7] ? bar[7] : -Infinity);
    }
    if (!Number.isFinite(low)) {
      low = bars.at(-1)[3];
      high = bars.at(-1)[2];
    }
    const padding = Math.max(3, (high - low) * 0.08);
    const target = { low: low - padding, high: high + padding };
    if (!this.range || this.drag) {
      this.range = target;
    } else {
      this.range.low += (target.low - this.range.low) * 0.25;
      this.range.high += (target.high - this.range.high) * 0.25;
    }
    const range = this.range;
    const y = (value) => TOP + ((range.high - value) / (range.high - range.low)) * (priceBottom - TOP);
    const priceAt = (pixel) => range.high - ((pixel - TOP) / (priceBottom - TOP)) * (range.high - range.low);

    // Grid lines at round prices.
    const step = Math.max(1, niceStep(range.high - range.low, Math.max(2, Math.floor((priceBottom - TOP) / 48))));
    const gridPrices = [];
    context.strokeStyle = palette.grid;
    for (let value = Math.ceil(range.low / step) * step; value <= range.high; value += step) {
      gridPrices.push(value);
      context.beginPath();
      context.moveTo(0, crisp(y(value)));
      context.lineTo(plot, crisp(y(value)));
      context.stroke();
    }
    this.drawTimeAxis(context, config, firstSlot, lastSlot, length, xOfTime, plot, bottom, height);

    // Volume along the bottom, then the candles.
    const body = Math.max(1, Math.min(slotWidth * 0.7, 18));
    const loudest = Math.max(1, ...shown.map((bar) => bar[5]));
    for (const bar of shown) {
      const x = xOfSlot(bar[0] / length);
      const up = bar[4] >= bar[1];
      context.fillStyle = up ? palette.buy : palette.sell;
      context.globalAlpha = 0.28;
      const size = Math.max(1, (bar[5] / loudest) * (bottom - volumeTop - 2));
      context.fillRect(x - body / 2, bottom - size, body, size);
      context.globalAlpha = 1;
      context.strokeStyle = context.fillStyle;
      context.beginPath();
      context.moveTo(crisp(x), Math.round(y(bar[2])));
      context.lineTo(crisp(x), Math.round(y(bar[3])));
      context.stroke();
      const top = y(Math.max(bar[1], bar[4]));
      context.fillRect(Math.round(x - body / 2), Math.round(top), Math.round(body), Math.max(1, Math.round(y(Math.min(bar[1], bar[4])) - top)));
    }

    // The fair value through every bar, and the VWAP, which starts over each day.
    const dayLength = config.dayLength;
    if (this.show.vwap) {
      this.line(context, shown, 7, xOfSlot, y, length, palette.vwap, [], (bar, previous) =>
        Math.floor(bar[0] / dayLength) !== Math.floor(previous[0] / dayLength));
    }
    if (this.show.fair) {
      this.line(context, shown, 6, xOfSlot, y, length, palette.fair, [4, 3], () => false);
    }

    this.drawCrosses(context, view.crosses, config, firstSlot, lastSlot, length, xOfSlot, y, plot);

    // The last price, and the price under the pointer, tagged on the axis.
    const last = view.last ?? bars.at(-1)[4];
    const lastBar = bars.at(-1);
    const lastColor = lastBar[4] >= lastBar[1] ? palette.buy : palette.sell;
    const tags = [];
    if (this.anchor === null) {
      context.setLineDash([2, 3]);
      context.strokeStyle = lastColor;
      context.globalAlpha = 0.7;
      context.beginPath();
      context.moveTo(0, crisp(y(last)));
      context.lineTo(plot, crisp(y(last)));
      context.stroke();
      context.setLineDash([]);
      context.globalAlpha = 1;
      tags.push({ y: y(last), text: price(last), background: lastColor, color: '#fff' });
    }
    if (this.show.fair) {
      tags.push({ y: y(view.fair), text: price(view.fair), background: palette.fair, color: palette.surface });
    }
    const hover = this.hover && !this.drag && this.hover.x < plot && this.hover.y < bottom ? this.hover : null;
    if (hover) {
      tags.push({ y: hover.y, text: price(Math.round(priceAt(hover.y))), background: palette.surfaceRaised,
        color: palette.text, border: true });
    }
    context.font = font(11);
    context.fillStyle = palette.muted;
    for (const value of gridPrices) {
      if (tags.every((other) => Math.abs(other.y - y(value)) > 13)) {
        context.fillText(price(value), plot + 10, crisp(y(value)) + 4);
      }
    }
    // Spread the tags apart when they would overlap, keeping their order.
    tags.sort((a, b) => a.y - b.y);
    for (let index = 1; index < tags.length; index += 1) {
      tags[index].y = Math.max(tags[index].y, tags[index - 1].y + 19);
    }
    for (const label of tags) {
      tag(context, label.text, plot + 4, Math.min(bottom - 10, Math.max(10, label.y)), label.background, label.color);
    }

    // The crosshair, snapped to the slot under the pointer.
    let hovered = null;
    if (hover) {
      const slot = Math.floor(firstSlot + hover.x / slotWidth);
      const x = xOfSlot(slot);
      hovered = bars[lowerBound(bars, slot * length)];
      if (!hovered || hovered[0] !== slot * length) {
        hovered = null;
      }
      context.strokeStyle = palette.crosshair;
      context.setLineDash([4, 4]);
      context.beginPath();
      context.moveTo(crisp(x), TOP - 8);
      context.lineTo(crisp(x), bottom);
      context.moveTo(0, crisp(hover.y));
      context.lineTo(plot, crisp(hover.y));
      context.stroke();
      context.setLineDash([]);
      const label = timeLabel(slot * length, dayLength, timeframe);
      context.font = font(11, 600);
      const labelWidth = context.measureText(label).width + 14;
      context.fillStyle = palette.surfaceRaised;
      context.strokeStyle = palette.borderStrong;
      context.beginPath();
      context.roundRect(Math.round(x - labelWidth / 2), bottom + 3, labelWidth, 18, 4);
      context.fill();
      context.stroke();
      context.fillStyle = palette.text;
      context.textAlign = 'center';
      context.fillText(label, x, bottom + 16);
      context.textAlign = 'left';
    }
    // The legend follows the pointer, or else shows the newest bar in view, each against the bar before.
    const focus = hovered ?? (this.anchor === null ? bars.at(-1) : shown.at(-1));
    this.legendFor(view, focus ?? null, focus ? bars[lowerBound(bars, focus[0]) - 1] ?? null : null);
  }

  // A series through the bars' centres, broken where `breaks` says so or where a value is missing.
  line(context, bars, field, xOfSlot, y, length, color, dash, breaks) {
    context.save();
    context.strokeStyle = color;
    context.lineWidth = 1.6;
    context.lineJoin = 'round';
    context.setLineDash(dash);
    context.beginPath();
    let previous = null;
    for (const bar of bars) {
      const value = bar[field];
      if (value === null || value === undefined) {
        previous = null;
        continue;
      }
      const x = xOfSlot(bar[0] / length);
      if (previous && !breaks(bar, previous)) {
        context.lineTo(x, y(value));
      } else {
        context.moveTo(x, y(value));
      }
      previous = bar;
    }
    context.stroke();
    context.restore();
  }

  // Each day's opening and closing auctions shaded amber, and the close grey, labelled when there is room.
  drawSessions(context, config, firstSlot, lastSlot, length, xOfTime, plot, bottom) {
    const { dayLength, opening, continuous, closing } = config;
    const from = Math.max(0, firstSlot * length);
    const to = (lastSlot + 1 + RIGHT_PAD) * length;
    let labelEnd = -Infinity;  // where the last label drawn ends, so labels never overprint
    for (let day = Math.floor(from / dayLength) * dayLength; day < to; day += dayLength) {
      const bands = [
        [day, day + opening, true, 'OPENING AUCTION', 'OPEN'],
        [day + opening + continuous, day + opening + continuous + closing, true, 'CLOSING AUCTION', 'CLOSE'],
        [day + opening + continuous + closing, day + dayLength, false, 'CLOSED', ''],
      ];
      for (const [start, end, auction, long, short] of bands) {
        const left = Math.max(0, xOfTime(start));
        const right = Math.min(plot, xOfTime(end));
        if (right <= left) {
          continue;
        }
        // Narrow bands fade, so that a zoomed-out chart isn't striped with them.
        context.fillStyle = auction ? palette.auctionSoft : palette.hover;
        context.globalAlpha = (auction ? 0.55 : 1) * Math.min(1, (right - left) / 10);
        context.fillRect(left, TOP - 16, right - left, bottom - TOP + 16);
        context.globalAlpha = 1;
        // The short label may spill past a narrow band, if it overprints no other label.
        context.font = font(9.5, 700);
        const label = context.measureText(long).width + 10 <= right - left ? long : right - left >= 6 ? short : '';
        const half = context.measureText(label).width / 2;
        const center = Math.min(plot - half - 2, Math.max(half + 2, (left + right) / 2));
        if (label && center - half > labelEnd + 6) {
          context.fillStyle = auction ? palette.auction : palette.muted;
          context.textAlign = 'center';
          context.fillText(label, center, TOP - 5);
          context.textAlign = 'left';
          labelEnd = center + half;
        }
      }
    }
  }

  drawTimeAxis(context, config, firstSlot, lastSlot, length, xOfTime, plot, bottom, height) {
    const span = (lastSlot - firstSlot + 1) * this.timeframe;
    const step = TIME_STEPS.find((seconds) => (seconds / span) * plot >= 76) ?? TIME_STEPS.at(-1);
    const stepLength = step * 1e9;
    const from = Math.max(0, firstSlot * length);
    const to = (lastSlot + 1) * length;
    // Day boundaries first, while they are far enough apart to label.
    const days = [];
    if ((config.dayLength / length) * this.layout.slotWidth >= 70) {
      for (let day = Math.ceil(from / config.dayLength) * config.dayLength; day <= to; day += config.dayLength) {
        const x = xOfTime(day);
        if (x >= 24 && x <= plot - 24) {
          days.push(x);
          context.strokeStyle = palette.borderStrong;
          context.beginPath();
          context.moveTo(crisp(x), TOP - 16);
          context.lineTo(crisp(x), bottom);
          context.stroke();
          context.font = font(10.5, 700);
          context.fillStyle = palette.text2;
          context.textAlign = 'center';
          context.fillText(`Day ${Math.floor(day / config.dayLength) + 1}`, x, height - 8);
        }
      }
    }
    context.font = font(10.5);
    context.textAlign = 'center';
    for (let time = Math.ceil(from / stepLength) * stepLength; time <= to; time += stepLength) {
      const x = xOfTime(time);
      if (x < 24 || x > plot - 24 || days.some((day) => Math.abs(day - x) < 48)) {
        continue;
      }
      context.strokeStyle = palette.grid;
      context.beginPath();
      context.moveTo(crisp(x), TOP);
      context.lineTo(crisp(x), bottom);
      context.stroke();
      context.fillStyle = palette.muted;
      context.fillText(timeLabel(time, config.dayLength, step), x, height - 8);
    }
    context.textAlign = 'left';
    context.strokeStyle = palette.border;
    context.beginPath();
    context.moveTo(0, crisp(bottom));
    context.lineTo(plot, crisp(bottom));
    context.stroke();
  }

  // Amber diamonds at each auction's single price, labelled with its volume when an evening's close and the next
  // morning's open are far enough apart to read, and the label overprints no other.
  drawCrosses(context, crosses, config, firstSlot, lastSlot, length, xOfSlot, y, plot) {
    const placed = [];
    for (const [time, crossPrice, volume] of crosses) {
      const slot = Math.floor(time / length);
      if (slot < firstSlot || slot > lastSlot) {
        continue;
      }
      const x = xOfSlot(slot);
      const top = y(crossPrice);
      context.fillStyle = palette.auction;
      context.strokeStyle = palette.surface;
      context.lineWidth = 2;
      context.beginPath();
      context.moveTo(x, top - 6);
      context.lineTo(x + 6, top);
      context.lineTo(x, top + 6);
      context.lineTo(x - 6, top);
      context.closePath();
      context.stroke();
      context.fill();
      context.lineWidth = 1;
      if (((config.closed + config.opening) / length) * this.layout.slotWidth >= 40) {
        const opening = time % config.dayLength === config.opening;
        const label = `${opening ? 'Open' : 'Close'} ${compact(volume)}`;
        context.font = font(10, 650);
        const width = context.measureText(label).width + 10;
        const left = Math.min(plot - width - 2, Math.max(2, x - width / 2));
        const box = { left, right: left + width, top: top - 25, bottom: top - 10 };
        if (placed.every((other) => box.right + 4 < other.left || box.left - 4 > other.right ||
          box.bottom + 2 < other.top || box.top - 2 > other.bottom)) {
          placed.push(box);
          context.fillStyle = palette.auctionSoft;
          context.beginPath();
          context.roundRect(left, box.top, width, 15, 4);
          context.fill();
          context.fillStyle = palette.auction;
          context.fillText(label, left + 5, top - 14);
        }
      }
    }
  }

  legendFor(view, bar, previous) {
    const fields = this.legendFields ??= Object.fromEntries(
      [...this.legend.querySelectorAll('[data-field]')].map((node) => [node.dataset.field, node]));
    text(fields.symbol, view.symbol);
    text(fields.timeframe, this.timeframe === 60 ? '1m' : `${this.timeframe}s`);
    text(fields.open, bar ? price(bar[1]) : '–');
    text(fields.high, bar ? price(bar[2]) : '–');
    text(fields.low, bar ? price(bar[3]) : '–');
    text(fields.close, bar ? price(bar[4]) : '–');
    text(fields.volume, bar ? grouped(bar[5]) : '–');
    const reference = previous ? previous[4] : bar?.[1];
    const change = bar ? bar[4] - reference : 0;
    text(fields.change, bar ? `${change >= 0 ? '+' : '−'}${price(Math.abs(change))} (${change >= 0 ? '+' : '−'}${Math.abs((100 * change) / reference).toFixed(2)}%)` : '');
    fields.change.className = change > 0 ? 'up' : change < 0 ? 'down' : '';
    text(fields.fair, price(bar?.[6] ?? view.fair));
    const vwap = bar ? bar[7] : view.vwap;
    text(fields.vwap, vwap === null || vwap === undefined ? '–' : (vwap / 100).toFixed(2));
  }
}

// A time on the axis: the day where a day starts, and otherwise the time of day, to the precision the step needs.
function timeLabel(time, dayLength, step) {
  const day = Math.floor(time / dayLength) + 1;
  if (time % dayLength === 0) {
    return `Day ${day}`;
  }
  return step >= dayLength / 1e9 ? `Day ${day} ${clock(time, dayLength / 1e9, 0)}` : clock(time, dayLength / 1e9, 0);
}
