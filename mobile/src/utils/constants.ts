export const DEFAULT_SERVER_URL = process.env.EXPO_PUBLIC_POLYSPARQ_SERVER_URL ?? '';

export const POLL_INTERVALS = {
  LIVE_LITE: 2000,
  ACTIVITY: 5000,
  FLEET: 5000,
  PERF_STATS: 30000,
  HEATMAP: 30000,
  PNL_BY_INSTANCE: 30000,
  MARKETS: 20000,
} as const;

export const ASSETS = ['BTC', 'ETH', 'SOL', 'XRP'] as const;
export type Asset = (typeof ASSETS)[number];
