import { apiGet, apiPost } from './client';
import type {
  ActivityResponse,
  ConfigListItem,
  ConfigResponse,
  ConfigWriteResponse,
  ClosedPositionsResponse,
  FleetResponse,
  HeatmapData,
  LiveLiteResponse,
  ManualBuyRequest,
  ManualBuyResponse,
  ManualPredictionBoard,
  MobileMarketBook,
  MobileMarketPage,
  MobileMarket,
  MarketItem,
  Order,
  PerfStats,
  PnlByInstance,
  PushPreferences,
  SessionReportItem,
  SharedWalletConfigureResponse,
  SharedWalletStatus,
  Settlement,
  SignalHistoryItem,
  StrategyHistoryItem,
  RuntimeFleetResponse,
  WalletModeResponse,
  WalletStatus,
  WhoAmI,
} from './types';

type Envelope<T, K extends string> = { [P in K]: T };

function unwrapArray<T>(value: unknown, key: string): T[] {
  if (Array.isArray(value)) return value as T[];
  if (value && typeof value === 'object') {
    const nested = (value as Record<string, unknown>)[key];
    if (Array.isArray(nested)) return nested as T[];
  }
  return [];
}

export const api = {
  whoami: () => apiGet<WhoAmI>('/api/whoami'),

  liveLite: (instance?: string) =>
    apiGet<LiveLiteResponse>('/api/live_lite', instance ? { instance } : undefined),

  activity: async (instance?: string): Promise<ActivityResponse> => {
    const value = await apiGet<any>('/api/activity', instance ? { instance } : undefined);
    return {
      ...value,
      orders: unwrapArray<Order>(value.orders, 'orders'),
      settles: unwrapArray<Settlement>(value.settles, 'settles'),
      orderstats: unwrapArray(value.orderstats, 'stats'),
      source_pnl: unwrapArray(value.source_pnl, 'sources'),
    };
  },

  fleet: () => apiGet<FleetResponse>('/api/fleet'),
  fleetCreate: (instance: string, template: string, execution_mode: 'maker' | 'taker' | 'hybrid') =>
    apiPost<{ ok: boolean; instance: string; message: string }>('/api/v4/action', {
      action: 'fleet', operation: 'create', instance, template, execution_mode,
    }),
  runtimeFleet: () => apiGet<RuntimeFleetResponse>('/api/v3/fleet'),

  orders: async (limit = 60, instance?: string) => {
    const value = await apiGet<Envelope<Order[], 'orders'>>('/api/orders', {
      limit: String(limit),
      ...(instance ? { instance } : {}),
    });
    return value.orders ?? [];
  },
  cancelOpenOrders: (instance: string) =>
    apiPost<{ ok: boolean; instance: string; queued: string }>('/api/orders/cancel_open', { instance }),
  cancelOrder: (instance: string, client_order_id: string, exchange_order_id: string) =>
    apiPost<{ ok: boolean; status: string; instance: string; queued: string }>('/api/orders/cancel', {
      instance, client_order_id, exchange_order_id,
    }),

  settles: async (limit = 40, instance?: string) => {
    const value = await apiGet<Envelope<Settlement[], 'settles'>>('/api/settles', {
      limit: String(limit),
      ...(instance ? { instance } : {}),
    });
    return value.settles ?? [];
  },

  closedPositions: (days = 0.5, instance?: string, mode = '', cursor?: string, limit = 100) =>
    apiGet<ClosedPositionsResponse>('/api/v4/closed_positions', {
      days: String(days),
      limit: String(limit),
      ...(instance ? { instance } : {}),
      ...(mode ? { mode } : {}),
      ...(cursor ? { cursor } : {}),
    }),

  configs: () => apiGet<{ configs: ConfigListItem[] }>('/api/configs'),
  config: (name: string) => apiGet<ConfigResponse>('/api/config', { name }),
  configWrite: (name: string, updates: Record<string, unknown>) =>
    apiPost<ConfigWriteResponse>('/api/config/write', { name, updates }),

  markets: (limit = 40) =>
    apiGet<{ markets: MarketItem[] }>('/api/markets', { limit: String(limit) }),
  mobileMarkets: (query = '', offset = 0, limit = 30) =>
    apiGet<MobileMarketPage>('/api/mobile/markets', {
      q: query, offset: String(offset), limit: String(limit),
    }),
  mobileMarket: (conditionId: string) =>
    apiGet<{ market: MobileMarket; stale?: boolean; server_ts: number }>('/api/mobile/market', { condition_id: conditionId }),
  mobileBook: (conditionId: string, outcomeIndex: number) =>
    apiGet<MobileMarketBook>('/api/mobile/book', {
      condition_id: conditionId, outcome_index: String(outcomeIndex),
    }),
  manualPredictions: () => apiGet<ManualPredictionBoard>('/api/manual_predictions'),
  perfStats: () => apiGet<PerfStats>('/api/perf_stats'),
  heatmap: () => apiGet<HeatmapData>('/api/heatmap'),
  pnlByInstance: () => apiGet<PnlByInstance>('/api/pnl_by_instance'),
  signalHistory: (limit = 30) =>
    apiGet<{ signals: SignalHistoryItem[] }>('/api/signal_history', { limit: String(limit) }),
  logTail: (instance?: string, lines = 300) =>
    apiGet<{ lines: string[]; path: string; total: number }>('/api/log_tail', {
      lines: String(lines),
      ...(instance ? { instance } : {}),
    }),
  health: () => apiGet<import('./types').HealthInfo>('/api/health'),

  botControl: (action: string, instance: string, extra?: Record<string, string | number | boolean>) =>
    apiPost<{ ok: boolean; error?: string; stderr?: string }>('/api/bot/control', {
      action,
      instance,
      ...extra,
    }),
  supervisorControl: (action: string, instance: string) =>
    apiPost<{ ok: boolean; error?: string }>('/api/supervisor/control', { action, instance }),
  supervisorModel: (instance: string, model: string) =>
    apiPost<{ ok: boolean; restarted?: boolean }>('/api/supervisor/model', { instance, model }),
  strategyHistory: (instance: string, limit = 40) =>
    apiGet<{ instance: string; history: StrategyHistoryItem[] }>('/api/strategy_history', {
      instance,
      limit: String(limit),
    }),

  wallet: (instance: string) => apiGet<WalletStatus>('/api/wallet', { instance }),
  walletShared: () => apiGet<SharedWalletStatus>('/api/wallet/shared'),
  walletSharedConfigure: (credentials: {
    api_key: string;
    api_secret: string;
    api_passphrase: string;
    private_key: string;
    funder_address: string;
    signer_address: string;
    signature_type: number;
  }) => apiPost<SharedWalletConfigureResponse>('/api/wallet/shared', credentials),
  walletMode: (instance: string, mode: 'live' | 'paper', confirm_live = false) =>
    apiPost<WalletModeResponse>('/api/wallet/mode', { instance, mode, confirm_live }),
  walletWrite: (instance: string, updates: Record<string, unknown>) =>
    apiPost<ConfigWriteResponse>('/api/wallet/write', { instance, updates }),
  goLive: (instance: string, credentials: {
    private_key: string;
    api_key?: string;
    api_secret?: string;
    api_passphrase?: string;
  }) => apiPost<{ ok: boolean; results?: Array<{ instance: string; ok: boolean; error?: string }> }>(
    '/api/wallet/go_live',
    { ...credentials, instances: [instance] },
  ),
  switchPaper: (instance: string) =>
    apiPost<{ ok: boolean; results?: Array<{ instance: string; ok: boolean; error?: string }> }>(
      '/api/wallet/paper',
      { instances: [instance] },
    ),

  forceSell: (cid: string, side: string, instance?: string) =>
    apiPost<{ ok: boolean; queued?: string; instance?: string }>('/api/force_sell', { cid, side, ...(instance ? { instance } : {}) }),
  forceSellAll: () =>
    apiPost<{ ok: boolean; queued?: number }>('/api/force_sell_all', {}),
  manualBuy: (request: ManualBuyRequest) =>
    apiPost<ManualBuyResponse>('/api/v3/command', { op: 'manual_buy', ...request }),

  sessionReports: () => apiGet<{ reports: SessionReportItem[] }>('/api/session_reports'),
  sessionReport: (file: string) => apiGet<{ file: string; content: string }>('/api/session_report', { file }),

  pushRegister: (fcmToken: string, deviceId: string) =>
    apiPost<{ ok: boolean }>('/api/push/register', { fcm_token: fcmToken, device_id: deviceId }),
  pushPreferences: () => apiGet<PushPreferences>('/api/push/preferences'),
  pushPreferencesUpdate: (prefs: Partial<PushPreferences>) =>
    apiPost<{ ok: boolean }>('/api/push/preferences', prefs),
};
