// Colors the command line. Each line splits into pieces, [text, kind] pairs that join back into the line exactly, so
// coloring never changes what the interpreter said. No DOM here: the checks run it over every golden transcript.

const CELL = /^(\s*)([\d.]+)( x )([\d,]+)(\s*)(\(\d+\))/;
const ASK_COLUMN = 28;  // where a book row's ask cell starts, unless a wide bid cell pushes it along
const TRADE = /^(\s*)(trade #\d+)(\s+)([\d,]+)( @ )([\d.]+)(\s+)(buy #\d+)(\s+)(sell #\d+)(.*)$/;
const ORDER = /^(\S+)( #\d+ )(.*)$/;
const SIDE = /^((?:STOP-LIMIT|STOP) )?(BUY|SELL)(.*)$/;
const BOOK_HEADER = /^(\S+)(  )(CONTINUOUS|AUCTION)(.*)$/;
const AUCTION = /^(\S+ )(call auction started|would uncross at|uncrossed at|is trading continuously|has no crossing interest yet|back to continuous trading)(.*)$/;

// Words between the numbers: labels dim, numbers and prices bold.
function words(text) {
  return text.split(/(\s+)/).filter(Boolean).map((token) => {
    if (/^\s+$/.test(token)) {
      return [token, ''];
    }
    if (/^\(?[\d,.]+[,:)]?$|^0x[\da-f]+$|^-$/.test(token)) {
      return [token, 'strong'];
    }
    if (/^(GTC|DAY|IOC|FOK|market),?$/.test(token)) {
      return [token, 'keyword'];
    }
    if (token === '@' || /^[a-z(][a-z-]*[):,]?$/.test(token)) {
      return [token, 'dim'];
    }
    return [token, ''];
  });
}

const outcomeKind = (part) =>
  /^(rejected|killed|cancel rejected)\b/.test(part) ? 'error'
    : /^filled\b/.test(part) ? 'ok'
      : /^resting\b/.test(part) ? 'rest'
        : /^(waiting|queued|is a stop order)\b/.test(part) ? 'wait'
          : /^(cancelled|self-trade|priority reset)\b/.test(part) ? 'warn'
            : /^(amended|priority kept)\b/.test(part) ? 'ok' : 'dim';

// What became of an order: "filled 230, resting 20", "rejected: INVALID_PRICE", ... A reject's reason stays red.
function outcome(text) {
  const pieces = [];
  let kind = '';
  for (const part of text.split(/(, |: )/)) {
    if (part === ', ' || part === ': ') {
      pieces.push([part, kind === 'error' ? 'error' : 'dim']);
    } else if (part !== '') {
      kind = kind === 'error' ? kind : outcomeKind(part);
      pieces.push([part, kind]);
    }
  }
  return pieces;
}

function order([, symbol, id, rest]) {
  const pieces = [[symbol, 'symbol'], [id, 'dim']];
  const arrow = rest.indexOf(' -> ');
  const head = arrow < 0 ? rest : rest.slice(0, arrow);
  const side = head.match(SIDE);
  if (side) {
    if (side[1]) {
      pieces.push([side[1], 'wait']);
    }
    pieces.push([side[2], side[2] === 'BUY' ? 'buy side' : 'sell side'], ...words(side[3]));
  } else if (head.startsWith('replace with')) {
    pieces.push(['replace with', 'keyword'], ...words(head.slice('replace with'.length)));
  } else {
    pieces.push(...outcome(head));
  }
  if (arrow >= 0) {
    pieces.push([' -> ', 'dim'], ...outcome(rest.slice(arrow + 4)));
  }
  return pieces;
}

function cell(match, side) {
  const [, space, price, times, quantity, gap, count] = match;
  return [[space, ''], [price, side], [times, 'dim'], [quantity, 'strong'], [gap, ''], [count, 'dim']];
}

// "     100.30 x 100     (1)       99.90 x 80      (1)": the bid cell, padded out to the asks, then the ask cell.
function bookRow(line) {
  const pieces = [];
  let rest = line;
  if (line.length - line.trimStart().length < ASK_COLUMN) {
    const bid = rest.match(CELL);
    if (!bid) {
      return [[line, '']];
    }
    pieces.push(...cell(bid, 'buy'));
    rest = rest.slice(bid[0].length);
  }
  const ask = rest.match(CELL);
  if (ask) {
    pieces.push(...cell(ask, 'sell'));
    rest = rest.slice(ask[0].length);
  }
  return rest === '' ? pieces : [...pieces, [rest, '']];
}

// A line of syntax, as help and the hints show it: keywords, PLACEHOLDERS, and [optional | alternative] parts.
export function syntax(text) {
  return (text.match(/[A-Z][A-Z_]*\b|[a-z][a-z-]*:?|[[\]|@=]|\s+|[^\sA-Za-z[\]|@=]+|./g) ?? []).map((token) =>
    [token, /^[A-Z]/.test(token) ? 'arg' : /^[a-z].*:$/.test(token) ? 'dim' : /^[a-z]/.test(token) ? 'keyword'
      : /^\s+$/.test(token) ? '' : 'dim']);
}

// help prints headings, then lines of syntax, some with a description after a wide gap.
function help(line) {
  if (!line.startsWith(' ')) {
    const [, word, rest] = line.match(/^(\S+)(.*)$/) ?? [null, line, ''];
    return rest === '' ? [[word, 'heading']] : [[word, 'heading'], [rest, 'dim']];
  }
  return line.split(/(\s{3,})/).flatMap((group, index) =>
    index > 0 && /[a-z]+ [a-z]+ [a-z]+/.test(group) ? [[group, 'dim']] : syntax(group));
}

// The command as typed, after the prompt.
export function command(text) {
  let first = true;
  return text.split(/(\s+)/).filter(Boolean).map((token) => {
    if (/^\s+$/.test(token)) {
      return [token, ''];
    }
    if (first) {
      first = false;
      return [token, token === 'buy' ? 'buy' : token === 'sell' ? 'sell' : 'keyword'];
    }
    return [token, /^[\d,.]+$/.test(token) ? 'strong' : token === '@' ? 'dim' : /^[A-Z]{2,}$/.test(token) ? 'symbol'
      : /^(buy|sell)$/.test(token) ? token : ''];
  });
}

export class Highlighter {
  constructor() {
    this.book = false;  // inside a book's table of levels
  }

  // The pieces of each line of what a command printed.
  output(text, commandName = '') {
    this.book = false;
    return text.replace(/\n$/, '').split('\n').map((line) => this.line(line, commandName));
  }

  // The pieces of one line of the output of a command.
  line(text, commandName = '') {
    if (this.book && /^\s+[\d.]+ x /.test(text)) {
      return bookRow(text);
    }
    this.book = false;
    if (commandName === 'help') {
      return help(text);
    }
    let match;
    if (/^\s+BID\s+ASK$/.test(text)) {
      this.book = true;
      const [, before, gap] = text.match(/^(\s+)BID(\s+)ASK$/);
      return [[before, ''], ['BID', 'buy dim'], [gap, ''], ['ASK', 'sell dim']];
    }
    if (text.startsWith('#')) {
      return [[text, 'comment']];
    }
    if (text.startsWith('error: ')) {
      return [['error:', 'error label'], [text.slice(6), 'error']];
    }
    if ((match = text.match(TRADE))) {
      const [, indent, trade, gap1, quantity, at, price, gap2, buy, gap3, sell, rest] = match;
      const pieces = [[indent, ''], [trade, 'trade'], [gap1, ''], [quantity, 'strong'], [at, 'dim'], [price, 'strong'],
        [gap2, ''], [buy, 'buy'], [gap3, ''], [sell, 'sell']];
      return rest === '' ? pieces : [...pieces, [rest, 'wait']];
    }
    if ((match = text.match(/^(\s+)(stop #\d+ triggered)$/))) {
      return [[match[1], ''], [match[2], 'wait']];
    }
    if ((match = text.match(/^(\s+)(self-trade prevention cancelled resting #\d+|market order #\d+ expired unfilled)$/))) {
      return [[match[1], ''], [match[2], 'warn']];
    }
    if ((match = text.match(BOOK_HEADER))) {
      return [[match[1], 'symbol'], [match[2], ''], [match[3], match[3] === 'AUCTION' ? 'phase auction' : 'phase'],
        ...words(match[4])];
    }
    if ((match = text.match(AUCTION))) {
      return [[match[1], 'symbol'], [match[2], 'wait'], ...words(match[3])];
    }
    if ((match = text.match(ORDER))) {
      return order(match);
    }
    if ((match = text.match(/^([* ] )(\S+)(  resting .*)$/))) {
      return [[match[1], match[1] === '* ' ? 'keyword' : ''], [match[2], 'symbol'], ...words(match[3])];
    }
    if ((match = text.match(/^(added|selected|using) (\S+)$/))) {
      return [[`${match[1]} `, 'ok'], [match[2], 'symbol']];
    }
    if ((match = text.match(/^(expired \d+ day orders?:)(.*)$/))) {
      return [[match[1], 'warn'], ...words(match[2])];
    }
    if (/^\s*\(.*\)$/.test(text)) {
      return [[text, 'dim']];
    }
    if ((match = text.match(/^([A-Z][A-Z\d._-]*)( .*)$/))) {
      return [[match[1], 'symbol'], ...words(match[2])];
    }
    return words(text);
  }
}
