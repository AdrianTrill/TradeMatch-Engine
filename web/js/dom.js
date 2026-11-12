// Small DOM helpers.

export const $ = (selector, root = document) => root.querySelector(selector);

export function element(tag, className = '', html = '') {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (html) {
    node.innerHTML = html;
  }
  return node;
}

// Sets a node's text only when it changes, which spares the browser needless layout work every frame.
export function text(node, value) {
  const next = String(value);
  if (node.textContent !== next) {
    node.textContent = next;
  }
}
