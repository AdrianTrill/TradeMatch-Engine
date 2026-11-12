// The price chart: one-second candles with volume, the hidden fair value the agents trade around, and the last price.
import { crisp, font, niceStep, surface, tag } from './canvas.js';
import { price } from './format.js';
import { palette } from './palette.js';

const AXIS = 66;  // width of the price axis on the right
const SLOTS = 90;  // candles across the chart

export class PriceChart {
  constructor(canvas) {
    this.canvas = canvas;
  }

  draw(view) {
    const { context, width, height } = surface(this.canvas);
    const candles = view.candles;
    if (candles.length === 0) {
      context.font = font(13);
      context.fillStyle = palette.muted;
      context.textAlign = 'center';
      context.fillText('Waiting for the opening auction to trade…', width / 2, height / 2);
      context.textAlign = 'left';
      return;
    }

    const plot = width - AXIS;
    const top = 16;
    const volumeHeight = Math.round(height * 0.16);
    const bottom = height - volumeHeight - 10;
    let low = Math.min(view.fair, ...candles.map((candle) => candle[3]));
    let high = Math.max(view.fair, ...candles.map((candle) => candle[2]));
    const padding = Math.max(2, (high - low) * 0.12);
    low -= padding;
    high += padding;
    const y = (value) => top + ((high - value) / (high - low)) * (bottom - top);

    // Grid lines at round prices, labelled where the price tags leave room.
    const last = candles.at(-1);
    const lastY = y(view.last ?? last[4]);
    const fairY = y(view.fair);
    const step = Math.max(1, niceStep(high - low, Math.max(3, Math.floor((bottom - top) / 52))));
    context.font = font(11);
    for (let value = Math.ceil(low / step) * step; value <= high; value += step) {
      const level = crisp(y(value));
      context.strokeStyle = palette.grid;
      context.beginPath();
      context.moveTo(0, level);
      context.lineTo(plot, level);
      context.stroke();
      if (Math.abs(level - lastY) > 14 && Math.abs(level - fairY) > 14) {
        context.fillStyle = palette.muted;
        context.fillText(price(value), plot + 10, level + 4);
      }
    }

    // Candles, and their volume along the bottom.
    const slot = plot / SLOTS;
    const first = SLOTS - candles.length;
    const body = Math.max(1, Math.min(slot * 0.66, 16));
    const loudest = Math.max(1, ...candles.map((candle) => candle[5]));
    candles.forEach(([, open, highest, lowest, close, volume], index) => {
      const center = (first + index + 0.5) * slot;
      const color = close >= open ? palette.buy : palette.sell;
      context.fillStyle = color;
      context.globalAlpha = 0.3;
      const bar = Math.max(1, (volume / loudest) * (volumeHeight - 6));
      context.fillRect(center - body / 2, height - bar, body, bar);
      context.globalAlpha = 1;
      context.strokeStyle = color;
      context.beginPath();
      context.moveTo(crisp(center), y(highest));
      context.lineTo(crisp(center), y(lowest));
      context.stroke();
      const upper = y(Math.max(open, close));
      context.fillRect(center - body / 2, upper, body, Math.max(1, y(Math.min(open, close)) - upper));
    });

    // The fair value and the last price, as dashed lines with tags on the axis.
    const lastColor = last[4] >= last[1] ? palette.buy : palette.sell;
    context.setLineDash([3, 4]);
    context.strokeStyle = palette.fair;
    context.globalAlpha = 0.85;
    context.beginPath();
    context.moveTo(0, crisp(fairY));
    context.lineTo(plot, crisp(fairY));
    context.stroke();
    context.strokeStyle = lastColor;
    context.globalAlpha = 0.6;
    context.beginPath();
    context.moveTo(0, crisp(lastY));
    context.lineTo(plot, crisp(lastY));
    context.stroke();
    context.globalAlpha = 1;
    context.setLineDash([]);
    // Keep the two tags apart when the prices are close.
    const fairTag = Math.abs(fairY - lastY) < 20 ? lastY + (fairY < lastY ? -20 : 20) : fairY;
    tag(context, price(view.fair), plot + 4, fairTag, palette.fair, palette.surface);
    tag(context, price(view.last ?? last[4]), plot + 4, lastY, lastColor, '#fff');
    context.font = font(10.5, 600);
    context.fillStyle = palette.fair;
    context.fillText('FAIR VALUE', 8, fairY - 6);
  }
}
