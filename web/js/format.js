// Formatting for the playground. Prices are integer ticks of one cent; times are simulated nanoseconds.

export const price = (ticks) => (ticks / 100).toFixed(2);

export const grouped = (value) => Math.round(value).toLocaleString('en-US');

// 1,234 below ten thousand, then 12.3K, 123K, 1.2M, and so on.
export function compact(value) {
  const magnitude = Math.abs(value);
  if (magnitude >= 1e9) {
    return `${(value / 1e9).toFixed(magnitude >= 1e10 ? 0 : 1)}B`;
  }
  if (magnitude >= 1e6) {
    return `${(value / 1e6).toFixed(magnitude >= 1e7 ? 0 : 1)}M`;
  }
  if (magnitude >= 1e4) {
    return `${(value / 1e3).toFixed(magnitude >= 1e5 ? 0 : 1)}K`;
  }
  return grouped(value);
}

// The time of day in a simulated day of `daySeconds`, as mm:ss with `decimals` decimal places.
export function clock(nanoseconds, daySeconds, decimals = 2) {
  const seconds = (nanoseconds / 1e9) % daySeconds;
  const whole = decimals === 0 ? Math.floor(seconds % 60) : seconds % 60;
  const text = decimals === 0 ? String(whole) : whole.toFixed(decimals);
  return `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${text.padStart(decimals === 0 ? 2 : 3 + decimals, '0')}`;
}
