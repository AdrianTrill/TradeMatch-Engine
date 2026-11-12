// The dark and light themes. The choice is remembered (index.html applies it before the page first paints), and the
// canvases re-read their colors when it changes.
import { $ } from './dom.js';
import { readPalette } from './palette.js';

const KEY = 'tradematch-theme';
const PAGE_COLOR = { dark: '#06080d', light: '#eef1f6' };

let redrawCanvases = () => {};
let target;  // the theme being switched to, ahead of the fade that applies it

export const currentTheme = () => (document.documentElement.dataset.theme === 'light' ? 'light' : 'dark');

function apply(name) {
  document.documentElement.dataset.theme = name;
  $('meta[name="theme-color"]').content = PAGE_COLOR[name];
  readPalette();
  const next = name === 'dark' ? 'light' : 'dark';
  const button = $('#theme');
  button.innerHTML = `<svg class="icon"><use href="#i-${next === 'light' ? 'sun' : 'moon'}"/></svg>`;
  button.title = `${next === 'light' ? 'Light' : 'Dark'} theme (T)`;
  button.setAttribute('aria-label', `Switch to the ${next} theme`);
}

export function toggleTheme() {
  const next = target === 'dark' ? 'light' : 'dark';
  target = next;
  try {
    localStorage.setItem(KEY, next);
  } catch {
    // Storage can be off, in a private window for one: then the theme lasts for this visit.
  }
  const change = () => {
    apply(next);
    redrawCanvases();
  };
  // The page cross-fades to the new theme, with the canvases already redrawn in its colors.
  if (document.startViewTransition && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
    // A switch made while one is still fading skips that fade, which rejects its promise: that's expected.
    document.startViewTransition(change).ready.catch(() => {});
  } else {
    change();
  }
}

export function startTheme(redraw) {
  redrawCanvases = redraw;
  target = currentTheme();
  apply(target);
  $('#theme').addEventListener('click', toggleTheme);
}
