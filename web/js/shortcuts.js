// Keyboard shortcuts, and the list of them that ? opens. Typing in a field, or holding Ctrl, Cmd, or Alt, leaves the
// keys alone.
import { $, element } from './dom.js';

const LIST = [
  ['Space', 'Pause or resume the market'],
  ['1 2 3', 'Show a stock'],
  ['− +', 'Slow down or speed up'],
  ['B S', 'Buy or sell on the ticket'],
  ['L', 'Back to the live chart'],
  ['T', 'Light or dark theme'],
  ['M C', 'The market or the command line'],
  ['?', 'This list'],
  ['Esc', 'Close a menu or this list'],
];

const typing = (target) => target instanceof HTMLElement
  && (target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName));

// market: keys that act on the market, while it's shown; anywhere: keys that work in either view.
export function startShortcuts({ market, anywhere, marketShown }) {
  const dialog = $('#shortcuts');
  for (const [keys, description] of LIST) {
    const term = element('dt');
    for (const key of keys.split(' ')) {
      term.append(element('kbd', '', key));
    }
    const definition = element('dd');
    definition.textContent = description;
    $('#shortcut-list').append(term, definition);
  }
  const toggle = () => {
    if (dialog.open) {
      dialog.close();
    } else {
      dialog.showModal();
    }
  };
  $('#open-shortcuts').addEventListener('click', toggle);
  dialog.addEventListener('click', (event) => {
    if (event.target === dialog || event.target.closest('[data-close]')) {
      dialog.close();
    }
  });

  document.addEventListener('keydown', (event) => {
    if (event.defaultPrevented || event.ctrlKey || event.metaKey || event.altKey || typing(event.target)) {
      return;
    }
    if (event.key === '?') {
      event.preventDefault();
      toggle();
      return;
    }
    // Space on a focused control presses it; elsewhere it pauses the market.
    if (dialog.open || (event.key === ' ' && event.target instanceof Element
      && event.target.closest('button, a, [tabindex]'))) {
      return;
    }
    const key = event.key.length === 1 ? event.key.toLowerCase() : event.key;
    const action = anywhere[key] ?? (marketShown() ? market[key] : undefined);
    if (action) {
      event.preventDefault();
      action();
    }
  });
}
