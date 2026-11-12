// Canvas helpers shared by the charts.
import { palette } from './palette.js';

// A canvas sized for the display's pixel density and cleared, with its size in CSS pixels.
export function surface(canvas) {
  const ratio = window.devicePixelRatio || 1;
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
  }
  const context = canvas.getContext('2d');
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  context.font = font(11);
  context.lineWidth = 1;
  context.textBaseline = 'alphabetic';
  return { context, width, height };
}

export const font = (size, weight = 500) => `${weight} ${size}px ${palette.fontUi || 'system-ui, sans-serif'}`;

// A pixel-aligned coordinate for crisp one-pixel lines.
export const crisp = (value) => Math.round(value) + 0.5;

// Round "nice" steps for an axis covering `range` with about `count` labels: 1, 2, 2.5, or 5 times a power of ten.
export function niceStep(range, count) {
  const rough = Math.max(range, 1e-9) / Math.max(count, 1);
  const power = 10 ** Math.floor(Math.log10(rough));
  for (const factor of [1, 2, 2.5, 5, 10]) {
    if (factor * power >= rough) {
      return factor * power;
    }
  }
  return 10 * power;
}

// A rounded label box, such as the price tags on an axis.
export function tag(context, text, x, y, background, color, align = 'left') {
  context.font = font(11, 600);
  const width = Math.ceil(context.measureText(text).width) + 12;
  const left = align === 'right' ? x - width : x;
  context.fillStyle = background;
  context.beginPath();
  context.roundRect(left, Math.round(y - 9), width, 18, 4);
  context.fill();
  context.fillStyle = color;
  context.fillText(text, left + 6, Math.round(y + 4));
  return width;
}
