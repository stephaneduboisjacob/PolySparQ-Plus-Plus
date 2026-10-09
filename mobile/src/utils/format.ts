export function fmtUsd(v: number | null | undefined, decimals = 2): string {
  if (v == null) return '—';
  const sign = v < 0 ? '-' : '';
  return `${sign}$${Math.abs(v).toFixed(decimals)}`;
}

export function fmtPct(v: number | null | undefined, decimals = 2): string {
  if (v == null) return '—';
  return `${v >= 0 ? '+' : ''}${(v * 100).toFixed(decimals)}%`;
}

export function fmtPx(v: number | null | undefined, decimals = 4): string {
  if (v == null) return '—';
  return v.toFixed(decimals);
}

export function fmtAgo(ts: string | number | null | undefined): string {
  if (ts == null) return '—';
  const ms = typeof ts === 'string' ? Date.parse(ts) : ts;
  const delta = (Date.now() - ms) / 1000;
  if (delta < 60) return `${Math.floor(delta)}s ago`;
  if (delta < 3600) return `${Math.floor(delta / 60)}m ago`;
  if (delta < 86400) return `${Math.floor(delta / 3600)}h ago`;
  return `${Math.floor(delta / 86400)}d ago`;
}

export function pnlColor(v: number | null | undefined): string {
  if (v == null || v === 0) return '#8892a4';
  return v > 0 ? '#00e676' : '#ff5252';
}
