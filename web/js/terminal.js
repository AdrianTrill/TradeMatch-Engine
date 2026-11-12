// The command line: the engine's own interpreter, one session kept from command to command. The examples are the
// golden scenarios CI checks byte for byte, replayed as if typed. js/highlight.js colors what the interpreter prints.
import { $, element } from './dom.js';
import { Highlighter, command, syntax } from './highlight.js';
import { toast } from './toast.js';

const EXAMPLES = [
  ['price_time_priority', 'Price-time priority', 'Best price first, then first in line.'],
  ['order_types', 'Order types', 'Market, IOC, FOK, and post-only orders.'],
  ['iceberg', 'Icebergs', 'One tranche shows; the rest stays hidden.'],
  ['stops', 'Stop orders', 'Stops fire once a trade reaches them.'],
  ['amend_priority', 'Amending', 'A smaller size keeps its place in line.'],
  ['auction', 'Call auction', 'Orders collect, then cross at one price.'],
  ['self_trade_prevention', 'Self-trade prevention', "An owner's orders never meet each other."],
  ['errors', 'Validation', 'Bad requests are rejected, with reasons.'],
];

// Every command, its syntax, and what it does, for the hints and Tab completion.
const COMMANDS = {
  buy: ['buy [SYMBOL] QTY [@ PRICE | market] [options]', 'trades what it can, then rests the rest'],
  sell: ['sell [SYMBOL] QTY [@ PRICE | market] [options]', 'trades what it can, then rests the rest'],
  stop: ['stop buy|sell [SYMBOL] QTY @ STOP_PRICE [limit PRICE] [options]', 'waits for a trade at its stop price'],
  cancel: ['cancel [SYMBOL] ID', 'takes an order out of the book'],
  replace: ['replace [SYMBOL] ID QTY @ PRICE [options]', 'a smaller size at the same price keeps its place'],
  add: ['add SYMBOL', 'lists a symbol and selects it'],
  use: ['use SYMBOL', 'selects a symbol, so orders can leave it out'],
  symbols: ['symbols', 'the symbols listed so far'],
  book: ['book [SYMBOL] [LEVELS]', 'the order book, level by level'],
  order: ['order [SYMBOL] ID', 'one order, as the book holds it'],
  trades: ['trades [SYMBOL] [N]', 'the latest trades, newest first'],
  auction: ['auction [SYMBOL] begin|show|uncross', 'runs a call auction'],
  expire: ['expire', 'ends the day: DAY orders expire'],
  hash: ['hash', 'a hash of every book, to compare runs'],
  save: ['save FILE', 'snapshots every book, in this page’s memory'],
  load: ['load FILE', 'restores the books from a snapshot'],
  journal: ['journal FILE', 'records every command that follows'],
  help: ['help', 'every command'],
  quit: ['quit', 'starts a fresh session'],
};
const ORDER_COMMANDS = new Set(['buy', 'sell', 'stop', 'replace']);
const OPTIONS = ['options: gtc day ioc fok  post-only  display=N  owner=N  stp=none|newest|oldest|both  id=N',
  'in any order'];
const OPTION_WORDS = ['market', 'gtc', 'day', 'ioc', 'fok', 'post-only', 'display=', 'owner=', 'stp=', 'id='];

const MAX_LINES = 4000;
const KEYSTROKE = 16;  // milliseconds a character takes to type, up to
const MAX_TYPING = 320;  // milliseconds for a whole command
const PAUSE = 220;  // after each command's output
const READ = 380;  // after a comment, before the next command

const wait = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const nextFrame = () => new Promise((resolve) => requestAnimationFrame(resolve));
const isCommand = (line) => line.trim() !== '' && !line.trim().startsWith('#');

// One line of pieces, as spans; a kind such as 'buy side' becomes the classes t-buy t-side.
function paint(pieces, className = 't-line', tag = 'span') {
  const line = element(tag, className);
  for (const [text, kind] of pieces) {
    if (kind) {
      const node = element('span', kind.split(' ').map((name) => `t-${name}`).join(' '));
      node.textContent = text;
      line.append(node);
    } else {
      line.append(text);
    }
  }
  return line;
}

function commonPrefix(words) {
  let prefix = words[0];
  for (const word of words) {
    while (!word.toLowerCase().startsWith(prefix.toLowerCase())) {
      prefix = prefix.slice(0, -1);
    }
  }
  return prefix;
}

export function startTerminal(tradematch) {
  const output = $('#output');
  const input = $('#command');
  const hint = $('#command-hint');
  const highlighter = new Highlighter();
  const symbols = new Set();
  const history = [];
  let historyIndex = 0;
  let session = new tradematch.Terminal();
  let run = 0;  // each example, and each command typed, starts a new run, which stops an example still playing
  let playing = null;  // { card, fast } while an example plays

  // Appends lines, and follows them down unless the reader has scrolled back.
  function append(lines) {
    const following = output.scrollHeight - output.scrollTop - output.clientHeight < 48;
    for (const line of lines) {
      line.append('\n');
    }
    output.append(...lines);
    while (output.childElementCount > MAX_LINES) {
      output.firstElementChild.remove();
    }
    if (following) {
      output.scrollTop = output.scrollHeight;
    }
  }

  const comment = (text) => paint(text === '' ? [] : [[text, 'comment']]);

  function execute(text) {
    const name = text.trim().split(/\s+/)[0].toLowerCase();
    const lines = [paint([['› ', 'prompt'], ...command(text)], 't-line t-echo')];
    const result = session.run(text);
    if (result !== '') {
      for (const pieces of highlighter.output(result, name)) {
        lines.push(paint(pieces, pieces.some(([, kind]) => kind.startsWith('error')) ? 't-line t-fail' : 't-line'));
      }
    }
    append(lines);
    for (const [, symbol] of result.matchAll(/^(?:added|selected) (\S+)$/gm)) {
      symbols.add(symbol);
    }
    if (result.startsWith('(a new session starts)')) {
      symbols.clear();
    }
  }

  // Hints ----------------------------------------------------------------------------------------------------------

  function showUsage([usage, description]) {
    hint.replaceChildren(paint(syntax(usage), 'hint-usage'), paint([[description, 'dim']], 'hint-text'));
  }

  function showHint(matches = null) {
    const tokens = input.value.trimStart().split(/\s+/);
    const name = tokens[0].toLowerCase();
    if (matches) {
      hint.replaceChildren(paint(matches.flatMap((match, index) =>
        index === 0 ? [[match, 'keyword']] : [['  ·  ', 'dim'], [match, 'keyword']]), 'hint-usage'));
    } else if (input.value.trim() === '') {
      hint.innerHTML = '<span class="hint-text hint-tips"><kbd>Tab</kbd> completes commands and symbols <span class="sep">·</span>'
        + ' <kbd>↑</kbd> <kbd>↓</kbd> recall earlier ones <span class="sep">·</span> <kbd>Ctrl</kbd> <kbd>L</kbd> clears'
        + ' the screen</span>';
    } else if (tokens.length === 1 && !COMMANDS[name]) {
      const found = Object.keys(COMMANDS).filter((option) => option.startsWith(name));
      if (found.length === 1) {
        showUsage(COMMANDS[found[0]]);
      } else if (found.length > 1) {
        showHint(found);
      } else {
        hint.replaceChildren(paint([[`${tokens[0]} isn't a command: `, 'warn'], ['help', 'keyword'],
          [' lists them all', 'dim']], 'hint-text'));
      }
    } else if (COMMANDS[name]) {
      showUsage(ORDER_COMMANDS.has(name) && tokens.length >= 4 ? OPTIONS : COMMANDS[name]);
    } else {
      hint.replaceChildren(paint([[`${tokens[0]} isn't a command: `, 'warn'], ['help', 'keyword'],
        [' lists them all', 'dim']], 'hint-text'));
    }
  }

  // What the word being typed could become.
  function candidates(tokens) {
    const word = tokens.at(-1);
    const name = tokens[0].toLowerCase();
    const known = [...symbols];
    if (tokens.length === 1) {
      return Object.keys(COMMANDS);
    }
    if (word.startsWith('stp=')) {
      return ['stp=none', 'stp=newest', 'stp=oldest', 'stp=both'];
    }
    switch (name) {
      case 'buy':
      case 'sell':
      case 'replace':
        return [...known, ...OPTION_WORDS];
      case 'stop':
        return tokens.length === 2 ? ['buy', 'sell'] : [...known, 'limit', ...OPTION_WORDS];
      case 'auction':
        return [...known, 'begin', 'show', 'uncross'];
      case 'cancel':
      case 'book':
      case 'order':
      case 'trades':
      case 'use':
        return known;
      default:
        return [];
    }
  }

  // Tab completes the word at the end of the line as far as it is unambiguous, and lists the choices when it isn't.
  function complete() {
    const value = input.value;
    if (value.trim() === '' || input.selectionStart !== value.length) {
      return false;
    }
    const tokens = value.trimStart().split(/\s+/);
    const word = tokens.at(-1);
    const matches = candidates(tokens).filter((option) => option.toLowerCase().startsWith(word.toLowerCase()));
    if (matches.length === 0) {
      return false;
    }
    const completed = matches.length === 1 ? matches[0] + (matches[0].endsWith('=') ? '' : ' ') : commonPrefix(matches);
    input.value = value.slice(0, value.length - word.length) + completed;
    showHint(matches.length > 1 ? matches : null);
    return true;
  }

  // Typing -----------------------------------------------------------------------------------------------------------

  $('#prompt').addEventListener('submit', (event) => {
    event.preventDefault();
    const text = input.value;
    input.value = '';
    showHint();
    if (text.trim() === '') {
      return;
    }
    run += 1;
    stopPlaying();
    history.push(text);
    historyIndex = history.length;
    execute(text);
  });

  input.addEventListener('input', () => showHint());
  input.addEventListener('keydown', (event) => {
    if (event.key === 'Tab' && !event.shiftKey && complete()) {
      event.preventDefault();
    } else if (event.key === 'l' && event.ctrlKey) {
      event.preventDefault();
      output.replaceChildren();
    } else if (event.key === 'Escape') {
      // Esc clears the line, and on an empty line leaves it, so the page's shortcuts work again.
      if (input.value === '') {
        input.blur();
      }
      input.value = '';
      showHint();
    } else if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
      event.preventDefault();
      historyIndex = Math.max(0, Math.min(history.length, historyIndex + (event.key === 'ArrowUp' ? -1 : 1)));
      input.value = history[historyIndex] ?? '';
      showHint();
    }
  });

  output.addEventListener('click', () => {
    if (String(window.getSelection()) === '') {
      input.focus();
    }
  });

  $('#console-clear').addEventListener('click', () => {
    output.replaceChildren();
    input.focus();
  });
  $('#console-copy').addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(output.textContent);
      toast('Copied the session', 'success');
    } catch {
      toast("Couldn't reach the clipboard", 'error');
    }
  });

  // Examples ---------------------------------------------------------------------------------------------------------

  const cards = new Map();
  const scripts = new Map(EXAMPLES.map(([name]) => [name, fetch(`scenarios/${name}.tm`)
    .then((response) => (response.ok ? response.text() : null))
    .then((text) => text?.replaceAll('\r', '') ?? null)
    .catch(() => null)]));

  function setMeta(card, text) {
    card.querySelector('.example-meta').textContent = text;
  }

  function stopPlaying() {
    if (playing) {
      playing.card.classList.remove('running');
      setMeta(playing.card, playing.card.dataset.idle ?? '');
      playing = null;
    }
  }

  // Shows a command being typed, then takes the line away for execute() to print in color. False if stopped.
  async function type(text, current) {
    const typed = document.createTextNode('');
    const line = element('span', 't-line t-echo');
    line.append(paint([['› ', 'prompt']], ''), typed, element('span', 't-caret'));
    append([line]);
    const duration = Math.min(MAX_TYPING, KEYSTROKE * text.length);
    const start = performance.now();
    while (typed.data.length < text.length) {
      await nextFrame();
      if (current !== run) {
        line.remove();
        return false;
      }
      if (playing?.fast) {
        break;
      }
      typed.data = text.slice(0, Math.ceil(((performance.now() - start) / duration) * text.length));
    }
    line.remove();
    return true;
  }

  async function play(name) {
    const card = cards.get(name);
    if (playing?.card === card) {
      playing.fast = true;  // a second click finishes the example at once
      return;
    }
    const current = (run += 1);
    stopPlaying();
    const script = await scripts.get(name);
    if (current !== run) {
      return;
    }
    if (script === null) {
      toast("Couldn't load the example", 'error');
      return;
    }
    session.delete();
    session = new tradematch.Terminal();
    symbols.clear();
    output.replaceChildren();
    const lines = script.trimEnd().split('\n');
    const total = lines.filter(isCommand).length;
    playing = { card, fast: false };
    card.classList.remove('played');
    card.classList.add('running');
    card.style.setProperty('--progress', '0');
    let done = 0;
    let read = false;
    for (const text of lines) {
      if (current !== run) {
        return;
      }
      if (!isCommand(text)) {
        append([comment(text)]);
        read = read || text.trim() !== '';
        continue;
      }
      if (read && !playing.fast) {
        await wait(READ);
      }
      read = false;
      if (current !== run || !(await type(text, current))) {
        return;
      }
      execute(text);
      done += 1;
      card.style.setProperty('--progress', String(done / total));
      setMeta(card, `Playing · ${done} of ${total} · click to finish`);
      if (!playing.fast) {
        await wait(PAUSE);
      }
    }
    if (current !== run) {
      return;
    }
    append([comment(''), comment('# The session stays open: type book to see it, or send more orders.')]);
    card.classList.add('played');
    card.dataset.idle = `Played · ${total} commands · run it again`;
    stopPlaying();
    showHint();
  }

  for (const [name, title, description] of EXAMPLES) {
    const card = element('button', 'example', `
      <span class="example-head"><span class="example-title"></span>
        <span class="example-state"><svg class="icon play"><use href="#i-play"/></svg><svg class="icon done"><use href="#i-check"/></svg><span class="bars"><i></i><i></i><i></i></span></span></span>
      <span class="example-text"></span>
      <span class="example-meta">&nbsp;</span>
      <span class="example-progress"></span>`);
    card.type = 'button';
    card.querySelector('.example-title').textContent = title;
    card.querySelector('.example-text').textContent = description;
    card.addEventListener('click', () => play(name));
    cards.set(name, card);
    $('#examples').append(card);
    scripts.get(name).then((script) => {
      card.dataset.idle = script === null ? 'Unavailable' : `${script.split('\n').filter(isCommand).length} commands`;
      if (playing?.card !== card) {
        setMeta(card, card.dataset.idle);
      }
    });
  }

  append([
    comment("# The engine's own command interpreter, tradematch_cli, compiled to WebAssembly and running in this page."),
    comment('# Run an example, or type an order: add ACME, then buy 100 @ 100.00, sell 50 @ 99.50, and book.'),
    comment(''),
  ]);
  execute('help');
  showHint();
  return { focus: () => input.focus() };
}
