// The depth chart: each side's cumulative size outward from the best price, as filled steps around the mid price.
import { crisp, font, niceStep, surface } from './canvas.js';
import { compact, price } from './format.js';
import { palette } from './palette.js';

const AXIS = 22;  // height of the price axis along the bottom

export class DepthChart {
  constructor(canvas) {
    this.canvas = canvas;
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
