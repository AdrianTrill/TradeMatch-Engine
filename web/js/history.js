// Each symbol's one-second candles, kept as they arrive and aggregated into longer timeframes on demand. A candle is
// [start, open, high, low, close, volume, fair value, day VWAP], the fair value and VWAP as of its end.

const LIMIT = 30_000;  // one-second candles kept per symbol: over eight simulated hours

export class CandleHistory {
  constructor() {
    this.candles = [];
    this.cache = new Map();  // timeframe in seconds -> { bars, valid }, where bars before `valid` are final
  }

  // The start of the newest candle, which the engine sends again with any that follow, or -1 to ask for everything.
  get since() {
    return this.candles.at(-1)?.[0] ?? -1;
  }

  merge(candles) {
    if (candles.length === 0) {
      return;
    }
    const from = candles[0][0];
    let keep = this.candles.length;
    while (keep > 0 && this.candles[keep - 1][0] >= from) {
      keep -= 1;
    }
    this.candles.length = keep;
    this.candles.push(...candles);
    if (this.candles.length > LIMIT) {
      this.candles.splice(0, this.candles.length - LIMIT);
      this.cache.clear();
      return;
    }
    for (const entry of this.cache.values()) {
      entry.valid = Math.min(entry.valid, from);
    }
  }

  // Candles of `seconds` each, aligned to multiples of their length. Only buckets from the earliest change on are
  // rebuilt.
  bars(seconds) {
    if (seconds === 1) {
      return this.candles;
    }
    const length = seconds * 1e9;
    if (!this.cache.has(seconds)) {
      this.cache.set(seconds, { bars: [], valid: -Infinity });
    }
    const entry = this.cache.get(seconds);
    if (entry.valid === Infinity) {
      return entry.bars;
    }
    const rebuildFrom = Number.isFinite(entry.valid) ? Math.floor(entry.valid / length) * length : -Infinity;
    const bars = entry.bars;
    while (bars.length > 0 && bars.at(-1)[0] >= rebuildFrom) {
      bars.pop();
    }
    let index = lowerBound(this.candles, bars.length > 0 ? bars.at(-1)[0] + length : rebuildFrom);
    for (; index < this.candles.length; index += 1) {
      const candle = this.candles[index];
      const start = Math.floor(candle[0] / length) * length;
      const bar = bars.at(-1);
      if (bar && bar[0] === start) {
        bar[2] = Math.max(bar[2], candle[2]);
        bar[3] = Math.min(bar[3], candle[3]);
        bar[4] = candle[4];
        bar[5] += candle[5];
        bar[6] = candle[6];
        bar[7] = candle[7];
      } else {
        bars.push([start, candle[1], candle[2], candle[3], candle[4], candle[5], candle[6], candle[7]]);
      }
    }
    entry.valid = Infinity;
    return bars;
  }
}

// The index of the first candle or bar starting at or after `start`.
export function lowerBound(rows, start) {
  let low = 0;
  let high = rows.length;
  while (low < high) {
    const middle = (low + high) >> 1;
    if (rows[middle][0] < start) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}
