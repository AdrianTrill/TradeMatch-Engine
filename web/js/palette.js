// The theme's colors for the canvases, read from the page's CSS custom properties so the charts follow the theme.

const NAMES = [
  'text', 'text-2', 'muted', 'faint', 'grid', 'border', 'border-strong', 'surface', 'surface-raised', 'surface-sunken',
  'buy', 'buy-soft', 'buy-strong', 'sell', 'sell-soft', 'sell-strong', 'auction', 'auction-soft', 'fair', 'vwap',
  'accent', 'accent-soft', 'crosshair', 'font-ui', 'font-mono',
];

// Keyed in camel case: --text-2 is palette.text2, --buy-soft is palette.buySoft.
export const palette = {};

export function readPalette() {
  const style = getComputedStyle(document.documentElement);
  for (const name of NAMES) {
    palette[name.replace(/-(\w)/g, (_, letter) => letter.toUpperCase())] = style.getPropertyValue(`--${name}`).trim();
  }
}
