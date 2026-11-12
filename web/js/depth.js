// The depth chart: each side's cumulative size outward from the best price, as filled steps around the mid price. The
// pointer reads out the size and value up to its price: one side's in continuous trading, and during a call auction
// both curves, whose crossing is the uncross.
import { crisp, font, niceStep, surface } from './canvas.js';
import { compact, grouped, price } from './format.js';
import { palette } from './palette.js';

const AXIS = 22;  // height of the price axis along the bottom

export class DepthChart {
  constructor(canvas) {
    this.canvas = canvas;
    this.hover = null;
    canvas.addEventListener('pointermove', (event) => {
      const box = canvas.getBoundingClientRect();
      this.hover = { x: event.clientX - box.left, y: event.clientY - box.top };
    });
    canvas.addEventListener('pointerleave', () => {
      this.hover = null;
    });
  }

  draw(view) {
    const { context, width, height } = surface(this.canvas);
    const { bids, asks } = view;
    if (bids.length === 0 && asks.length === 0) {
      return;
    }
    // During a call auction the book may cross: the chart then centres on the uncross price, where the buyers' curve
    // meets the sellers'.
    const bestBid = bids[0]?.[0] ?? asks[0][0];
    const bestAsk = asks[0]?.[0] ?? bids[0][0];
    const mid = view.auction ? view.auction.price : (bestBid + bestAsk) / 2;
    const reach = Math.max(1, mid - (bids.at(-1)?.[0] ?? mid), (asks.at(-1)?.[0] ?? mid) - mid);
    const total = (levels) => levels.reduce((sum, level) => sum + level[1], 0);
    const most = Math.max(1, total(bids), total(asks)) * 1.12;
    const bottom = height - AXIS;
    const x = (value) => ((value - (mid - reach)) / (2 * reach)) * width;
    const y = (size) => bottom - (size / most) * (bottom - 10);

    // Size grid lines, labelled on the right.
    context.font = font(10.5);
    const step = niceStep(most, 3);
    for (let size = step; size < most; size += step) {
      context.strokeStyle = palette.grid;
      context.beginPath();
      context.moveTo(0, crisp(y(size)));
      context.lineTo(width, crisp(y(size)));
      context.stroke();
      context.fillStyle = palette.muted;
      const label = compact(size);
      context.fillText(label, width - context.measureText(label).width - 6, y(size) - 4);
    }

    this.side(context, bids, x, y, bottom, palette.buy, mid - reach);
    this.side(context, asks, x, y, bottom, palette.sell, mid + reach);

    // The mid price, and prices along the bottom.
    context.setLineDash([3, 4]);
    context.strokeStyle = view.auction ? palette.auction : palette.faint;
    context.beginPath();
    context.moveTo(crisp(x(mid)), 16);
    context.lineTo(crisp(x(mid)), bottom);
    context.stroke();
    context.setLineDash([]);
    context.textAlign = 'center';
    context.font = font(10.5, 600);
    context.fillStyle = view.auction ? palette.auction : palette.text2;
    context.fillText(view.auction ? `UNCROSS ${price(mid)}` : `MID ${(mid / 100).toFixed(3)}`, x(mid), 12);
    context.font = font(10.5);
    context.fillStyle = palette.muted;
    for (const value of [mid - reach * 0.66, mid + reach * 0.66]) {
      context.fillText(price(Math.round(value)), x(value), height - 7);
    }
    context.textAlign = 'left';

    if (this.hover && this.hover.y < bottom) {
      this.readout(context, view, mid, reach, width, bottom, y);
    }
  }

  readout(context, view, mid, reach, width, bottom, y) {
    const { bids, asks } = view;
    const at = Math.round(mid - reach + (this.hover.x / width) * 2 * reach);
    const buyers = bids.filter((level) => level[0] >= at);
    const sellers = asks.filter((level) => level[0] <= at);
    const shares = (levels) => levels.reduce((sum, level) => sum + level[1], 0);
    const value = (levels) => levels.reduce((sum, level) => sum + level[0] * level[1], 0);
    const lines = [];
    const dots = [];
    if (view.auction) {
      lines.push(['Buyers at or above', grouped(shares(buyers))], ['Sellers at or below', grouped(shares(sellers))],
        ['Would trade here', grouped(Math.min(shares(buyers), shares(sellers)))]);
      dots.push([shares(buyers), palette.buy], [shares(sellers), palette.sell]);
    } else {
      const side = at <= mid ? buyers : sellers;
      if (side.length === 0) {
        return;
      }
      lines.push([at <= mid ? 'Bids at or above' : 'Asks at or below', grouped(shares(side))],
        ['Value', `$${compact(value(side) / 100)}`]);
      dots.push([shares(side), at <= mid ? palette.buy : palette.sell]);
    }

    context.strokeStyle = palette.crosshair;
    context.setLineDash([4, 4]);
    context.beginPath();
    context.moveTo(crisp(this.hover.x), 16);
    context.lineTo(crisp(this.hover.x), bottom);
    context.stroke();
    context.setLineDash([]);
    for (const [size, color] of dots) {
      context.fillStyle = color;
      context.strokeStyle = palette.surface;
      context.lineWidth = 2;
      context.beginPath();
      context.arc(this.hover.x, y(size), 4, 0, Math.PI * 2);
      context.stroke();
      context.fill();
      context.lineWidth = 1;
    }

    // The readout, beside the pointer on whichever side has room.
    context.font = font(11, 600);
    const title = price(at);
    const labelWidth = Math.max(...lines.map(([label]) => context.measureText(label).width));
    const valueWidth = Math.max(...lines.map(([, figure]) => context.measureText(figure).width));
    const boxWidth = Math.max(context.measureText(title).width, labelWidth + valueWidth + 16) + 20;
    const boxHeight = 30 + lines.length * 17;
    const left = this.hover.x + 14 + boxWidth < width ? this.hover.x + 14 : this.hover.x - 14 - boxWidth;
    const top = Math.max(18, Math.min(bottom - boxHeight - 4, this.hover.y - boxHeight / 2));
    context.fillStyle = palette.surfaceRaised;
    context.strokeStyle = palette.borderStrong;
    context.beginPath();
    context.roundRect(left, top, boxWidth, boxHeight, 8);
    context.fill();
    context.stroke();
    context.fillStyle = palette.text;
    context.fillText(title, left + 10, top + 18);
    lines.forEach(([label, figure], index) => {
      const line = top + 36 + index * 17;
      context.font = font(11, 500);
      context.fillStyle = palette.muted;
      context.fillText(label, left + 10, line);
      context.font = font(11, 600);
      context.fillStyle = palette.text;
      context.fillText(figure, left + boxWidth - 10 - context.measureText(figure).width, line);
    });
  }

  // One side as a filled staircase from the best price out to the edge of the chart.
  side(context, levels, x, y, bottom, color, edge) {
    if (levels.length === 0) {
      return;
    }
    let size = 0;
    context.beginPath();
    context.moveTo(x(levels[0][0]), bottom);
    for (const [levelPrice, quantity] of levels) {
      context.lineTo(x(levelPrice), y(size));
      size += quantity;
      context.lineTo(x(levelPrice), y(size));
    }
    context.lineTo(x(edge), y(size));
    context.save();
    context.lineWidth = 1.5;
    context.strokeStyle = color;
    context.stroke();
    context.lineTo(x(edge), bottom);
    context.closePath();
    const fill = context.createLinearGradient(0, 0, 0, bottom);
    fill.addColorStop(0, color);
    fill.addColorStop(1, 'transparent');
    context.globalAlpha = 0.32;
    context.fillStyle = fill;
    context.fill();
    context.restore();
  }
}
