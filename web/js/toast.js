// Brief notifications in the corner of the page.
import { element } from './dom.js';

let stack;

export function toast(message, kind = 'info', duration = 2800) {
  if (!stack) {
    stack = element('div', 'toasts');
    stack.setAttribute('role', 'status');
    stack.setAttribute('aria-live', 'polite');
    document.body.append(stack);
  }
  const item = element('div', `toast ${kind}`);
  item.textContent = message;
  stack.append(item);
  requestAnimationFrame(() => item.classList.add('shown'));
  setTimeout(() => {
    item.classList.remove('shown');
    setTimeout(() => item.remove(), 300);
  }, duration);
}
