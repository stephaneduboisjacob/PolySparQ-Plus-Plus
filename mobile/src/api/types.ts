export type UserRole = 'admin' | 'user' | 'demo';

export interface WhoAmI {
  username: string | null;
  role: UserRole;
  plan: string;
  instance?: string | null;
  instances: string[] | null;
  demo_expires_in?: number;
  demo_total_secs?: number;
}

export interface BotDetail {
  running: boolean;
  pid?: number;
  session_start?: string;
  instances_running?: string[];
}

export interface Status {
  mode: string;
  running: boolean;
  pid?: number;
  bot: BotDetail;
  wallet_usd?: number;
  cash_usd: number;
  equity_usd: number;
  realized_pnl: number;
  unrealized_pnl: number;
  net_pnl: number;
  deployed_usd: number;
  market_value_usd: number;
  buy_value_usd: number;
  sell_value_usd: number;
  fees_usd: number;
  open_positions: number;
  wins: number;
  losses: number;
  win_pct: number | null;
  accounting_source?: string;
  health?: string;
  health_fresh?: number;
  health_stale?: number;
  health_total?: number;
  heartbeat?: string;
  server_ts: number;
  instances_running?: string[];
}

export interface Position {
  cid: string;
  token_id: string;
  side: string;
  outcome_label?: string;
  asset: string;
  shares: number;
  avg: number;
  mark: number | null;
  mark_source?: string | null;
  pnl_usd: number | null;
  pnl_pct: number | null;
  cost_basis: number;
  market_value?: number | null;
  buy_cost?: number;
  instance?: string;
  horizon_seconds?: number;
  seconds_left?: number | null;
  window_close?: number;
  awaiting_settlement: boolean;
}

export interface RadarItem {
  kind: string;
  level: 'critical' | 'warn' | 'info' | string;
  title: string;
  detail?: string;
  cid?: string;
}

export interface PositionPayload {
  positions: Position[];
  ts?: number;
  total_unrealized?: number;
  deployed_usd?: number;
  market_value?: number;
  source?: string;
}

export interface LiveLiteResponse {
  status: Status;
  positions: PositionPayload;
  markets: { markets: MarketItem[] };
  radar: RadarItem[];
  health: HealthInfo;
  tournament?: TournamentResponse;
  instance?: string;
  server_ts?: number;
}

export interface HealthInfo {
  running?: boolean;
  ws: Record<string, string>;
  events?: string;
  heartbeat?: string;
  orderbook_health?: string;
  oracle_source?: string;
  wallet_usd?: number;
  parsefail?: number;
  server_ts?: number;
}

export interface Order {
  client_order_id?: string;
  exchange_order_id?: string | null;
  ts_created: string | number;
  source: string;
  direction: string;
  side_label: string;
  price: number;
  size_shares: number;
  notional_usd: number;
  fee_usd: number;
  status: string;
  mode?: string;
  execution_mode?: string;
  reject_reason?: string;
  condition_id: string;
  question?: string;
  filled_shares?: number;
  instance?: string;
  asset?: string;
  horizon_seconds?: number;
}

export interface Settlement {
  ts_created: string | number;
  condition_id: string;
  side_label: string;
  instance?: string;
  source: string;
  settle_price: number;
  size_shares: number;
  payout_usd: number;
  asset?: string;
  horizon_seconds?: number;
  target_price?: number;
  question?: string;
  avg_entry: number;
  cost_basis: number;
  realized_pnl: number;
}

export interface OrderStats {
  source: string;
  direction: string;
  status: string;
  count: number;
  notional: number;
  fees: number;
}

export interface SourcePnl {
  source: string;
  buys: number;
  sells: number;
  cashflow: number;
  fees: number;
  net: number;
}

export interface ActivityResponse {
  orders: Order[];
  settles: Settlement[];
  orderstats: OrderStats[];
  source_pnl: SourcePnl[];
  server_ts?: number;
}

export interface StrategySummary {
  band?: string;
  per_entry_usd?: number;
  max_window_usd?: number;
  max_deployed_usd?: number;
  signals: string[];
  exits: string[];
  blocked: string[];
  entries_per_window?: number;
  stake?: number;
}

export interface WalletStatus {
  mode: 'paper' | 'live' | string;
  api_key_set: boolean;
  api_secret_set: boolean;
  api_passphrase_set: boolean;
  private_key_set: boolean;
  private_key_exists: boolean;
  private_key_file?: string;
  starting_balance_usd: number;
}

export interface StrategySnapshot {
  id: number;
  reason: string;
}

export interface SupervisorStatus {
  running: boolean;
  model?: string;
  extra?: string;
  last_decision?: {
    strategy_id?: number;
    llm_model?: string;
    reason?: string;
    created_at?: string | number;
  } | null;
  strategies_total: number;
}

export interface FleetInstance {
  instance: string;
  name?: string;
  running: boolean;
  current_config: string;
  strategy: StrategySummary;
  supervisor: SupervisorStatus;
  recent_strategies: StrategySnapshot[];
  wallet: WalletStatus;
}

export interface ConfigListItem {
  name: string;
  file: string;
  mtime?: number;
}

export interface FleetResponse {
  fleet: FleetInstance[];
  available_configs: ConfigListItem[];
  max_bots: number | null;
  plan: string;
  allowed_models: string[];
  allow_custom_model: boolean;
}

export interface TournamentInstance {
  instance: string;
  fills?: number;
  wins?: number;
  losses?: number;
  net_pnl?: number;
}

export interface TournamentResponse {
  instances: TournamentInstance[];
}

export interface MarketItem {
  condition_id: string;
  asset: string;
  horizon_seconds: number;
  target_price: number;
  question: string;
  window_open: number;
  window_close: number;
  phase: string;
  best_bid?: number;
  best_ask?: number;
  midprice?: number;
  spread?: number;
}

export interface ManualPredictionCard {
  asset: string;
  period: string;
  question?: string;
  condition_id?: string | null;
  model_status?: string;
  direction?: string;
  seconds_left?: number;
  polymarket_up_price?: number | null;
  polymarket_down_price?: number | null;
  book_age_s?: number | null;
  market_url?: string;
}

export interface ManualPredictionBoard {
  cards: ManualPredictionCard[];
  generated_at?: number;
  snapshot_age_s?: number;
  stale?: boolean;
  warming_up?: boolean;
  errors?: string[];
}

export interface RuntimeBot {
  instance: string;
  status: string;
  mode: string;
  configured_mode: string;
  mode_verified: boolean;
  risk_state: string;
  daily_kill_fired?: boolean;
  runtime_updated_ms?: number | null;
}

export interface RuntimeFleetResponse {
  fleet: RuntimeBot[];
  server_ts?: number;
}

export interface SharedWalletBot {
  instance: string;
  mode: string;
  running: boolean;
  uses_shared_credentials: boolean;
}

export interface SharedWalletStatus {
  scope: 'ADMIN' | string;
  credentials_ready: boolean;
  canonical_bundle_saved: boolean;
  canonical_material_present: boolean;
  legacy_source_instance?: string | null;
  api_key_set: boolean;
  api_secret_set: boolean;
  api_passphrase_set: boolean;
  private_key_set: boolean;
  funder_address?: string;
  signer_address?: string;
  signature_type?: number | null;
  shared_bot_count: number;
  total_bot_count: number;
  bots: SharedWalletBot[];
}

export interface SharedWalletConfigureResponse {
  ok: boolean;
  updated_instances: string[];
  restarted_instances: string[];
  restart_failures: string[];
  modes_preserved: boolean;
}

export interface WalletModeResponse {
  ok: boolean;
  instance: string;
  mode: string;
  was_running?: boolean;
  restarted?: boolean;
}

export interface ManualBuyRequest {
  instance: string;
  condition_id: string;
  side: 'UP' | 'DOWN';
  size_usd: number;
  size_shares?: number;
  limit_price: number;
  expected_mode: 'PAPER' | 'LIVE';
  outcome_index?: number;
  direction?: 'BUY' | 'SELL';
  confirm_live?: boolean;
}

export interface ManualBuyResponse {
  ok: boolean;
  status: 'queued' | string;
  queued: string;
  instance: string;
  mode: string;
  size_usd: number;
  limit_price: number;
  indicative_ask: number;
  estimated_shares: number;
  book_age_s: number;
  asset?: string;
  question?: string;
  condition_id: string;
  side: 'UP' | 'DOWN';
  direction?: 'BUY' | 'SELL';
  outcome_label?: string;
  size_shares?: number;
}

export interface MobileMarketOutcome {
  index: number;
  label: string;
  last_price: number | null;
  token_id?: string | null;
}

export interface MobileMarket {
  id: string;
  condition_id: string;
  slug: string;
  question: string;
  event_title: string;
  description?: string;
  category?: string;
  end_date?: string | null;
  active: boolean;
  closed: boolean;
  accepting_orders: boolean;
  volume_24h: number | null;
  volume: number | null;
  liquidity: number | null;
  outcomes: MobileMarketOutcome[];
}

export interface MobileMarketPage {
  markets: MobileMarket[];
  query: string;
  offset: number;
  next_offset?: number;
  has_more: boolean;
  stale: boolean;
  server_ts: number;
}

export interface MobileMarketBook {
  condition_id: string;
  outcome_index: number;
  best_bid: number | null;
  best_ask: number | null;
  book_age_s: number | null;
  fresh: boolean;
  timestamp: number | null;
  tick_size: number | null;
  min_order_size: number | null;
  server_ts: number;
}

export interface ConfigResponse {
  config: Record<string, unknown>;
  name: string;
  raw?: string;
  tunable_keys: string[];
  error?: string;
}

export interface ConfigWriteResponse {
  ok: boolean;
  changed: string[];
  rejected: Record<string, string>;
}

export interface PerfStats {
  total_entries: number;
  total_exits: number;
  wins: number;
  losses: number;
  win_rate: number | null;
  best_win: number;
  worst_loss: number;
  total_fees: number;
  net_pnl: number;
  avg_win: number;
  avg_loss: number;
}

export interface ClosedExit {
  id?: number;
  ts: number;
  instance: string;
  mode: string;
  asset?: string;
  horizon_seconds?: number;
  side_label?: string;
  source: string;
  realized_pnl: number;
  fees: number;
  shares?: number;
  avg_entry?: number | null;
  exit_price?: number | null;
}

export interface ClosedPositionsSummary {
  net: number;
  fees: number;
  trades: number;
  settled: number;
  wins: number;
  losses: number;
  breakeven: number;
  wr: number | null;
}

export interface ClosedPositionsResponse {
  exits: ClosedExit[];
  totals: ClosedPositionsSummary;
  next_cursor: string | null;
  excluded_unmatched_exits: number;
  server_ts: number;
  method: string;
}

export interface HeatmapData {
  hours: number[];
  wins: number[];
  losses: number[];
}

export interface PnlPoint {
  ts: string | number;
  pnl: number;
}

export interface PnlSeries {
  name: string;
  points: PnlPoint[];
}

export interface PnlByInstance {
  instances: Record<string, PnlSeries>;
}

export interface PriceUpdate {
  oracle: number | null;
  coinbase: number | null;
  delta_pct: number | null;
}

export interface PositionMark {
  cid: string;
  side: string;
  instance?: string;
  mark: number | null;
  pnl_usd: number | null;
  pnl_pct: number | null;
  mark_source?: string | null;
  awaiting_settlement?: boolean;
}

export interface SSEFrame {
  prices?: Record<string, PriceUpdate>;
  recent_fills?: Array<{
    ts: string | number;
    asset?: string;
    direction: string;
    side_label: string;
    price: number;
    size_shares: number;
    source: string;
    instance?: string;
  }>;
  positions_marks?: PositionMark[];
  ts?: number;
}

export interface SignalHistoryItem {
  ts: string | number;
  asset: string;
  side: string;
  direction: string;
  source: string;
  price: number;
  outcome?: string;
  settle_price?: number;
  settle_pnl?: number;
}

export interface StrategyHistoryItem {
  strategy_id: number;
  created_at: string | number;
  llm_model?: string;
  reason?: string;
  fills?: number;
  wins?: number;
  losses?: number;
  net_pnl?: number;
}

export interface SessionReportItem {
  file: string;
  mtime: number;
  size: number;
}

export interface PushPreferences {
  fills: boolean;
  results: boolean;
  bot_crash: boolean;
  pnl_swing: boolean;
  pnl_swing_usd: number;
  daily_summary: boolean;
}
