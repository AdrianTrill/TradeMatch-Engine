// The command line: the engine's interpreter, one session kept from command to command, with the golden scenarios
// as examples.
import { $ } from './dom.js';

const EXAMPLES = [
  ['price_time_priority', 'Price-time priority'],
  ['order_types', 'Order types'],
  ['iceberg', 'Icebergs'],
  ['stops', 'Stop orders'],
  ['amend_priority', 'Amending'],
  ['auction', 'Call auction'],
  ['self_trade_prevention', 'Self-trade prevention'],
  ['errors', 'Validation'],
];

export function startTerminal(tradematch) {
  const output = $('#output');
  const input = $('#command');
  const history = [];
  let historyIndex = 0;
  let terminal = new tradematch.Terminal();
  let exampleRun = 0;  // a newer example, or typing, stops one still being played

  function write(text, kind = '') {
    const span = document.createElement('span');
    if (kind) {
      span.className = kind;
    }
    span.textContent = text;
    output.append(span);
    output.scrollTop = output.scrollHeight;
  }

  function execute(line) {
    write(`› ${line}\n`, 'command');
    write(terminal.run(line));
  }

  $('#prompt').addEventListener('submit', (event) => {
    event.preventDefault();
    const line = input.value;
    input.value = '';
    if (line.trim() === '') {
      return;
    }
    exampleRun += 1;
    history.push(line);
    historyIndex = history.length;
    execute(line);
  });

  input.addEventListener('keydown', (event) => {
    if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') {
      return;
    }
    event.preventDefault();
    historyIndex = Math.max(0, Math.min(history.length, historyIndex + (event.key === 'ArrowUp' ? -1 : 1)));
    input.value = history[historyIndex] ?? '';
  });

  async function runExample(name) {
    const run = (exampleRun += 1);
    const script = (await (await fetch(`scenarios/${name}.tm`)).text()).replaceAll('\r', '');
    if (run !== exampleRun) {
      return;
    }
    terminal.delete();
    terminal = new tradematch.Terminal();
    output.textContent = '';
    for (const line of script.trimEnd().split('\n')) {
      if (run !== exampleRun) {
        return;
      }
      if (line.trim() === '' || line.trim().startsWith('#')) {
        write(`${line}\n`, 'comment');
        continue;
      }
      execute(line);
      await new Promise((resolve) => setTimeout(resolve, 160));
    }
    write('\nThe session stays open: type book to see it, or send more orders.\n', 'comment');
  }

  for (const [name, title] of EXAMPLES) {
    const button = document.createElement('button');
    button.textContent = title;
    button.addEventListener('click', () => runExample(name));
    $('#examples').append(button);
  }

  write('TradeMatch command line, running in WebAssembly. Run an example above, or type commands below.\n\n', 'comment');
  execute('help');
  return { focus: () => input.focus() };
}
