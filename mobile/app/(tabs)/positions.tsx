import { useEffect, useRef, useState } from 'react';
import { Alert, RefreshControl, ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { useLocalSearchParams } from 'expo-router';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Ionicons } from '@expo/vector-icons';
import { api } from '../../src/api/endpoints';
import { useAppStore } from '../../src/state/store';
import { useAuthStore } from '../../src/state/AuthContext';
import type { ClosedExit } from '../../src/api/types';
import { PositionCard } from '../../src/components/PositionCard';
import { CockpitHeader } from '../../src/components/CockpitHeader';
import { InstanceSelector } from '../../src/components/InstanceSelector';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';
import { fmtAgo, fmtPx, fmtUsd, pnlColor } from '../../src/utils/format';
import { POLL_INTERVALS } from '../../src/utils/constants';
import { colors, fontSize, spacing } from '../../src/utils/theme';

type ActivityView = 'open' | 'orders' | 'results';
const RESULT_PERIODS = [
  { label: '15m', value: '0.0104166667' }, { label: '30m', value: '0.0208333333' },
  { label: '1h', value: '0.0416666667' }, { label: '3h', value: '0.125' },
  { label: '6h', value: '0.25' }, { label: '12h', value: '0.5' },
  { label: '24h', value: '1' }, { label: '7d', value: '7' },
  { label: '30d', value: '30' }, { label: 'ALL', value: '0' },
];

export default function PositionsTab() {
  return <ErrorBoundary><PositionsContent /></ErrorBoundary>;
}

function PositionsContent() {
  const [view, setView] = useState<ActivityView>('open');
  const [resultPeriod, setResultPeriod] = useState('0.5');
  const [resultMode, setResultMode] = useState('');
  const [olderResultsState, setOlderResultsState] = useState<{ scope: string; rows: ClosedExit[] }>({ scope: '', rows: [] });
  const [paginationState, setPaginationState] = useState<{ scope: string; cursor: string | null } | null>(null);
  const [loadingOlder, setLoadingOlder] = useState(false);
  const [cancellingOrders, setCancellingOrders] = useState(false);
  const [cancellingOrderId, setCancellingOrderId] = useState('');
  const resultsScopeGeneration = useRef(0);
  const params = useLocalSearchParams<{ view?: string }>();
  const instance = useAppStore((state) => state.selectedInstance);
  const user = useAuthStore((state) => state.user);
  const isAdmin = user?.role === 'admin';
  const queryClient = useQueryClient();

  const live = useQuery({
    queryKey: ['live_lite', instance],
    queryFn: () => api.liveLite(instance ?? undefined),
    refetchInterval: POLL_INTERVALS.LIVE_LITE,
  });
  const activity = useQuery({
    queryKey: ['activity', instance],
    queryFn: () => api.activity(instance ?? undefined),
    refetchInterval: POLL_INTERVALS.ACTIVITY,
  });
  const resultQuery = useQuery({
    queryKey: ['closed_positions', instance, resultPeriod, resultMode],
    queryFn: () => api.closedPositions(Number(resultPeriod), instance ?? undefined, resultMode),
    enabled: view === 'results',
    staleTime: 15_000,
  });
  const orderRuntime = useQuery({
    queryKey: ['runtime_fleet'],
    queryFn: api.runtimeFleet,
    enabled: isAdmin && view === 'orders',
    refetchInterval: POLL_INTERVALS.FLEET,
  });

  const resultsScopeKey = `${instance ?? ''}|${resultPeriod}|${resultMode}`;
  const positions = live.data?.positions?.positions ?? [];
  const orders = activity.data?.orders ?? [];
  const openOrders = orders.filter((order) => ['submitted', 'acked', 'partial', 'uncertain', 'pending_sign'].includes(order.status.toLowerCase()));
  const settles = activity.data?.settles ?? [];
  const resultTotals = resultQuery.data?.totals;
  const olderResults = olderResultsState.scope === resultsScopeKey ? olderResultsState.rows : [];
  const paginationCursor = paginationState?.scope === resultsScopeKey ? paginationState.cursor : undefined;
  const completedRows = [...(resultQuery.data?.exits ?? []), ...olderResults];
  const resultCount = resultTotals?.trades ?? settles.length;
  const periodLabel = RESULT_PERIODS.find((period) => period.value === resultPeriod)?.label ?? '12h';
  const canLoadOlder = paginationCursor === undefined
    ? Boolean(resultQuery.data?.next_cursor)
    : Boolean(paginationCursor);
  const instances = live.data?.status?.instances_running ?? user?.instances ?? [];
  const totalDeployed = positions.reduce((sum, position) => sum + (Number(position.cost_basis) || 0), 0);
  const totalPnl = positions.reduce((sum, position) => sum + (Number(position.pnl_usd) || 0), 0);

  useEffect(() => {
    if (params.view === 'orders' || params.view === 'results' || params.view === 'open') setView(params.view);
  }, [params.view]);

  useEffect(() => {
    resultsScopeGeneration.current += 1;
    setOlderResultsState({ scope: resultsScopeKey, rows: [] });
    setPaginationState(null);
    setLoadingOlder(false);
  }, [resultsScopeKey]);

  const refresh = async () => {
    await Promise.all([live.refetch(), activity.refetch(), ...(view === 'results' ? [resultQuery.refetch()] : []), ...(view === 'orders' && isAdmin ? [orderRuntime.refetch()] : [])]);
  };
  const loadOlderResults = async () => {
    const cursor = paginationCursor === undefined ? resultQuery.data?.next_cursor : paginationCursor;
    if (!cursor || loadingOlder) return;
    const generation = resultsScopeGeneration.current;
    setLoadingOlder(true);
    try {
      const page = await api.closedPositions(Number(resultPeriod), instance ?? undefined, resultMode, cursor);
      if (generation !== resultsScopeGeneration.current) return;
      setOlderResultsState((current) => ({
        scope: resultsScopeKey,
        rows: [...(current.scope === resultsScopeKey ? current.rows : []), ...page.exits],
      }));
      setPaginationState({ scope: resultsScopeKey, cursor: page.next_cursor });
    } catch (error) {
      if (generation !== resultsScopeGeneration.current) return;
      Alert.alert('History unavailable', (error as Error).message);
    } finally {
      if (generation === resultsScopeGeneration.current) setLoadingOlder(false);
    }
  };
  const requestCancelOpenOrders = () => {
    if (!instance || !isAdmin || !live.data?.status?.running || cancellingOrders) return;
    Alert.alert(
      'Cancel open orders?',
      `Queue ${instance.toUpperCase()} to cancel its currently open venue orders. The bot and venue must acknowledge the cancellations; a fill can still race before that happens.`,
      [
        { text: 'Keep orders', style: 'cancel' },
        {
          text: 'Queue cancellation',
          style: 'destructive',
          onPress: async () => {
            setCancellingOrders(true);
            try {
              const response = await api.cancelOpenOrders(instance);
              Alert.alert('Cancellation queued', `${response.instance.toUpperCase()} · the bot will report final order status in Portfolio → Orders.`);
              await queryClient.invalidateQueries({ queryKey: ['activity', instance] });
            } catch (error) {
              Alert.alert('Cancellation request failed', (error as Error).message);
            } finally {
              setCancellingOrders(false);
            }
          },
        },
      ],
    );
  };
  const requestCancelOrder = (order: (typeof orders)[number]) => {
    const target = order.instance;
    const status = order.status.toLowerCase();
    const orderId = order.exchange_order_id || order.client_order_id;
    const runtime = orderRuntime.data?.fleet.find((bot) => bot.instance === target);
    if (!isAdmin || !target || !order.client_order_id || !orderId ||
        !['submitted', 'acked', 'partial'].includes(status) ||
        runtime?.status !== 'running' || !runtime.mode_verified || cancellingOrderId) return;
    const matched = Number(order.filled_shares) || 0;
    const remaining = Math.max(0, (Number(order.size_shares) || 0) - matched);
    const title = order.question || order.asset || 'Market';
    Alert.alert(
      'Cancel this order?',
      `${target.toUpperCase()} · ${title} · ${remaining.toFixed(2)} shares remaining. Any fill racing the cancellation stays in the position ledger. Submit a replacement only after cancellation is confirmed.`,
      [
        { text: 'Keep order', style: 'cancel' },
        {
          text: 'Cancel order',
          style: 'destructive',
          onPress: async () => {
            setCancellingOrderId(order.client_order_id!);
            try {
              const response = await api.cancelOrder(target, order.client_order_id!, orderId);
              Alert.alert('Cancellation queued', `${response.instance.toUpperCase()} · the bot will confirm the venue result in Orders.`);
              await queryClient.invalidateQueries({ queryKey: ['activity', instance] });
            } catch (error) {
              Alert.alert('Cancellation request failed', (error as Error).message);
            } finally {
              setCancellingOrderId('');
            }
          },
        },
      ],
    );
  };
  const forceSellAll = () => Alert.alert('Force sell all?', 'Close every open position in the selected scope.', [
    { text: 'Cancel', style: 'cancel' },
    {
      text: 'Sell all',
      style: 'destructive',
      onPress: async () => {
        try {
          await api.forceSellAll();
          await queryClient.invalidateQueries({ queryKey: ['live_lite'] });
        } catch (error) {
          Alert.alert('Sell failed', (error as Error).message);
        }
      },
    },
  ]);

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <CockpitHeader
        title="Portfolio"
        subtitle={instance ? instance.toUpperCase() : 'ALL BOTS · OPEN + RECORDED ACTIVITY'}
        mode={live.data?.status?.mode}
        action={isAdmin && positions.length > 0 ? (
          <TouchableOpacity style={styles.sellAllButton} onPress={forceSellAll} accessibilityLabel="Force sell all positions">
            <Ionicons name="exit-outline" size={19} color={colors.red} />
          </TouchableOpacity>
        ) : undefined}
      />

      <ScrollView
        style={styles.scroll}
        contentContainerStyle={styles.content}
        refreshControl={<RefreshControl refreshing={live.isFetching || activity.isFetching || (view === 'results' && resultQuery.isFetching) || (view === 'orders' && orderRuntime.isFetching)} onRefresh={refresh} tintColor={colors.accent} />}
      >
        <InstanceSelector instances={instances} />

        <View style={styles.summary}>
          <SummaryCell label="OPEN" value={String(positions.length)} />
          <SummaryCell label="DEPLOYED" value={fmtUsd(totalDeployed)} />
          <SummaryCell label="UNREALIZED" value={fmtUsd(totalPnl)} color={pnlColor(totalPnl)} />
        </View>

        <View style={styles.segmented}>
          <Segment label="Open" count={positions.length} active={view === 'open'} onPress={() => setView('open')} />
          <Segment label="Orders" count={orders.length} active={view === 'orders'} onPress={() => setView('orders')} />
          <Segment label="Results" count={resultCount} active={view === 'results'} onPress={() => setView('results')} />
        </View>

        {(live.error || activity.error || (view === 'results' && resultQuery.error)) && (
          <View style={styles.errorPanel}><Ionicons name="alert-circle" size={18} color={colors.red} /><Text style={styles.errorText}>{((live.error || activity.error || resultQuery.error) as Error).message}</Text></View>
        )}

        {view === 'open' && (
          <>
            {positions.map((position, index) => (
              <PositionCard
                key={`${position.cid ?? index}_${position.side}_${position.instance ?? ''}`}
                position={position}
                canTrade={isAdmin}
                onSold={() => queryClient.invalidateQueries({ queryKey: ['live_lite'] })}
              />
            ))}
            {!live.isLoading && !positions.length && <Empty icon="layers-outline" title="No open positions" detail="New fills will appear here in real time." />}
          </>
        )}

        {view === 'orders' && (
          <>
            {isAdmin && <View style={styles.orderControl}>
              <View style={styles.orderControlCopy}>
                <Text style={styles.orderControlLabel}>ORDER MANAGEMENT · {instance ? instance.toUpperCase() : 'ALL BOTS'}</Text>
                <Text style={styles.orderControlDetail}>{!instance ? 'Choose one bot above to request cancellation.' : live.data?.status?.running ? `${openOrders.length} open statuses in recent activity · venue acknowledgement is asynchronous.` : 'The selected bot must be running to process cancellation requests.'}</Text>
              </View>
              <TouchableOpacity style={[styles.cancelOrdersButton, (!instance || !live.data?.status?.running || cancellingOrders) && styles.cancelOrdersDisabled]} disabled={!instance || !live.data?.status?.running || cancellingOrders} onPress={requestCancelOpenOrders}>
                <Text style={styles.cancelOrdersText}>{cancellingOrders ? 'QUEUING…' : 'CANCEL OPEN'}</Text>
              </TouchableOpacity>
            </View>}
            <View style={styles.listPanel}>
              {orders.map((order, index) => {
                const status = order.status.toLowerCase();
                const targetRuntime = orderRuntime.data?.fleet.find((bot) => bot.instance === order.instance);
                const canCancel = isAdmin && ['submitted', 'acked', 'partial'].includes(status) &&
                  Boolean(order.client_order_id) && Boolean(order.exchange_order_id || order.client_order_id) &&
                  targetRuntime?.status === 'running' && targetRuntime.mode_verified;
                const requested = Number(order.size_shares) || 0;
                const matched = Number(order.filled_shares) || 0;
                const remaining = Math.max(0, requested - matched);
                const title = order.question || [order.asset || 'Market', order.side_label].filter(Boolean).join(' · ');
                const statusColor = status === 'filled' ? colors.green : status === 'rejected' ? colors.red : status === 'cancelled' ? colors.textMuted : colors.yellow;
                return (
                  <View key={`${order.client_order_id || order.ts_created}-${index}`} style={styles.activityRow}>
                    <View style={[styles.directionIcon, { backgroundColor: order.direction === 'buy' ? colors.greenDim : colors.redDim }]}>
                      <Ionicons name={order.direction === 'buy' ? 'arrow-down' : 'arrow-up'} size={17} color={order.direction === 'buy' ? colors.green : colors.red} />
                    </View>
                    <View style={styles.activityMain}>
                      <View style={styles.activityTitleRow}>
                        <Text style={styles.activityTitle} numberOfLines={2}>{order.direction.toUpperCase()} · {title}</Text>
                        <Text style={[styles.activityStatus, { color: statusColor }]}>{order.status}</Text>
                      </View>
                      <Text style={styles.activityMeta} numberOfLines={2}>
                        {order.instance?.toUpperCase() || 'BOT'} · {order.mode?.toUpperCase() || 'MODE?'} · {matched.toFixed(2)}/{requested.toFixed(2)} sh · {remaining.toFixed(2)} left · {fmtPx(order.price)}
                      </Text>
                      {order.reject_reason ? <Text style={styles.rejectReason} numberOfLines={2}>{order.reject_reason}</Text> : null}
                      <Text style={styles.activityTime}>{fmtAgo(order.ts_created)} · {order.source}</Text>
                    </View>
                    {canCancel ? (
                      <TouchableOpacity
                        style={[styles.cancelOneButton, cancellingOrderId === order.client_order_id && styles.cancelOrdersDisabled]}
                        disabled={Boolean(cancellingOrderId)}
                        onPress={() => requestCancelOrder(order)}
                        accessibilityRole="button"
                        accessibilityLabel={`Cancel ${order.direction} order for ${title}`}
                      >
                        <Ionicons name="close-circle-outline" size={15} color={colors.red} />
                        <Text style={styles.cancelOneText}>{cancellingOrderId === order.client_order_id ? 'WAIT' : 'CANCEL'}</Text>
                      </TouchableOpacity>
                    ) : null}
                  </View>
                );
              })}
              {!activity.isLoading && !orders.length && <Empty icon="receipt-outline" title="No orders" detail="Order attempts and rejections will appear here." />}
            </View>
          </>
        )}

        {view === 'results' && (
          <View>
            <Text style={styles.scopeLabel}>{instance?.toUpperCase() ?? 'ALL BOTS'} · {periodLabel} · {resultMode ? resultMode.toUpperCase() : 'ALL MODES'}</Text>
            <ScrollView horizontal showsHorizontalScrollIndicator={false} style={styles.filterRow}>
              {RESULT_PERIODS.map((period) => <FilterChip key={period.value} label={period.label} active={period.value === resultPeriod} onPress={() => setResultPeriod(period.value)} />)}
            </ScrollView>
            <View style={styles.modeFilters}>
              {[{ label: 'ALL MODES', value: '' }, { label: 'PAPER', value: 'paper' }, { label: 'LIVE', value: 'live' }].map((mode) => (
                <FilterChip key={mode.label} label={mode.label} active={mode.value === resultMode} onPress={() => setResultMode(mode.value)} />
              ))}
            </View>
            <View style={styles.resultsSummary}>
              <ResultMetric label="WINS" value={String(resultTotals?.wins ?? '—')} color={colors.green} />
              <ResultMetric label="LOSSES" value={String(resultTotals?.losses ?? '—')} color={colors.red} />
              <ResultMetric label="BREAK EVEN" value={String(resultTotals?.breakeven ?? '—')} />
              <ResultMetric label="NET P&L" value={fmtUsd(resultTotals?.net)} color={pnlColor(resultTotals?.net)} />
              <ResultMetric label="FEES" value={fmtUsd(resultTotals?.fees)} />
              <ResultMetric label="MATCHED EXITS" value={String(resultTotals?.trades ?? '—')} />
            </View>
            <Text style={styles.resultsMeta}>{resultQuery.data?.excluded_unmatched_exits ?? 0} unmatched exit(s) excluded · Results use recorded fills and allocated fees.</Text>
            <View style={styles.listPanel}>
            {completedRows.map((result, index) => {
              const pnl = Number(result.realized_pnl) || 0;
              const markColor = pnl > 0 ? colors.green : pnl < 0 ? colors.red : colors.textMuted;
              return (
                <View key={`${result.id ?? result.ts}-${index}`} style={styles.resultRow}>
                  <View style={[styles.resultMark, { backgroundColor: pnl > 0 ? colors.greenDim : pnl < 0 ? colors.redDim : colors.bgInput }]}>
                    <Ionicons name={pnl > 0 ? 'checkmark' : pnl < 0 ? 'close' : 'remove'} size={18} color={markColor} />
                  </View>
                  <View style={styles.activityMain}>
                    <View style={styles.activityTitleRow}>
                      <Text style={styles.activityTitle}>{result.asset || '?'} {result.horizon_seconds ? `${Math.round(result.horizon_seconds / 60)}m` : ''} {result.side_label || ''}</Text>
                      <Text style={[styles.activityStatus, { color: result.mode?.toLowerCase() === 'live' ? colors.red : colors.yellow }]}>{result.mode || 'UNKNOWN'}</Text>
                    </View>
                    <Text style={styles.activityMeta}>{result.instance} · {result.source} · fees {fmtUsd(result.fees)}{result.shares != null ? ` · ${Number(result.shares).toFixed(2)} shares` : ''}</Text>
                  </View>
                  <View style={styles.resultValue}><Text style={[styles.resultPnl, { color: markColor }]}>{fmtUsd(pnl)}</Text><Text style={styles.activityTime}>{fmtAgo(result.ts)}</Text></View>
                </View>
              );
            })}
            {!resultQuery.isLoading && !completedRows.length && <Empty icon="trophy-outline" title="No matched exits" detail="Try a wider time window or another bot scope." />}
            {resultQuery.isLoading && !completedRows.length && <Empty icon="hourglass-outline" title="Loading results" detail="Reading matched exits and fees for this scope." />}
            </View>
            {canLoadOlder && <TouchableOpacity style={styles.loadMore} onPress={loadOlderResults} disabled={loadingOlder}><Text style={styles.loadMoreText}>{loadingOlder ? 'LOADING…' : 'LOAD OLDER RESULTS'}</Text><Ionicons name="chevron-down" size={15} color={colors.accent} /></TouchableOpacity>}
          </View>
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

function SummaryCell({ label, value, color }: { label: string; value: string; color?: string }) { return <View style={styles.summaryCell}><Text style={styles.summaryLabel}>{label}</Text><Text style={[styles.summaryValue, color ? { color } : null]} numberOfLines={1}>{value}</Text></View>; }
function ResultMetric({ label, value, color }: { label: string; value: string; color?: string }) { return <View style={styles.resultMetric}><Text style={styles.summaryLabel}>{label}</Text><Text style={[styles.resultMetricValue, color ? { color } : null]} numberOfLines={1}>{value}</Text></View>; }
function Segment({ label, count, active, onPress }: { label: string; count: number; active: boolean; onPress: () => void }) { return <TouchableOpacity style={[styles.segment, active && styles.segmentActive]} onPress={onPress}><Text style={[styles.segmentText, active && styles.segmentTextActive]}>{label}</Text><View style={[styles.segmentCount, active && styles.segmentCountActive]}><Text style={[styles.segmentCountText, active && styles.segmentCountTextActive]}>{count}</Text></View></TouchableOpacity>; }
function FilterChip({ label, active, onPress }: { label: string; active: boolean; onPress: () => void }) { return <TouchableOpacity style={[styles.filterChip, active && styles.filterChipActive]} onPress={onPress}><Text style={[styles.filterChipText, active && styles.filterChipTextActive]}>{label}</Text></TouchableOpacity>; }
function Empty({ icon, title, detail }: { icon: keyof typeof Ionicons.glyphMap; title: string; detail: string }) { return <View style={styles.empty}><Ionicons name={icon} size={30} color={colors.textMuted} /><Text style={styles.emptyTitle}>{title}</Text><Text style={styles.emptyDetail}>{detail}</Text></View>; }

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.bg },
  header: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: spacing.lg, paddingVertical: spacing.md },
  title: { fontSize: fontSize.xl, fontWeight: '700', color: colors.text },
  subtitle: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 2 },
  sellAllButton: { width: 40, height: 40, alignItems: 'center', justifyContent: 'center', borderWidth: 1, borderColor: colors.red, borderRadius: 6, backgroundColor: colors.redDim },
  scroll: { flex: 1 },
  content: { paddingHorizontal: spacing.lg, paddingBottom: spacing.xxl },
  summary: { minHeight: 70, flexDirection: 'row', marginBottom: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, overflow: 'hidden' },
  summaryCell: { flex: 1, minWidth: 0, justifyContent: 'center', paddingHorizontal: spacing.sm, borderRightWidth: StyleSheet.hairlineWidth, borderRightColor: colors.border },
  summaryLabel: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  summaryValue: { fontSize: fontSize.md, color: colors.text, fontWeight: '700', marginTop: 4 },
  segmented: { height: 44, flexDirection: 'row', padding: 3, marginBottom: spacing.md, borderRadius: 7, backgroundColor: colors.bgInput },
  segment: { flex: 1, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 6, borderRadius: 5 },
  segmentActive: { backgroundColor: colors.bgCard },
  segmentText: { fontSize: fontSize.xs, color: colors.textMuted, fontWeight: '600' },
  segmentTextActive: { color: colors.text },
  segmentCount: { minWidth: 19, height: 19, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 4, borderRadius: 5, backgroundColor: colors.border },
  segmentCountActive: { backgroundColor: colors.accentDim },
  segmentCountText: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  segmentCountTextActive: { color: '#fff' },
  errorPanel: { flexDirection: 'row', gap: spacing.sm, padding: spacing.md, marginBottom: spacing.md, borderWidth: 1, borderColor: colors.red, borderRadius: 6, backgroundColor: colors.redDim },
  errorText: { flex: 1, color: colors.red, fontSize: fontSize.sm },
  listPanel: { borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, overflow: 'hidden' },
  activityRow: { minHeight: 68, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  directionIcon: { width: 34, height: 34, alignItems: 'center', justifyContent: 'center', borderRadius: 6 },
  activityMain: { flex: 1, minWidth: 0 },
  activityTitleRow: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm },
  activityTitle: { flex: 1, fontSize: fontSize.xs, color: colors.text, fontWeight: '700' },
  activityStatus: { fontSize: 9, fontWeight: '700', textTransform: 'uppercase' },
  activityMeta: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 3 },
  activityTime: { fontSize: 9, color: colors.textMuted },
  cancelOneButton: { minHeight: 33, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 3, paddingHorizontal: spacing.xs, borderWidth: 1, borderColor: colors.red, borderRadius: 5, backgroundColor: colors.redDim },
  cancelOneText: { color: colors.red, fontSize: 8, fontWeight: '900' },
  rejectReason: { fontSize: 9, color: colors.red, marginTop: 3 },
  orderControl: { minHeight: 62, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, marginBottom: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard },
  orderControlCopy: { flex: 1 },
  orderControlLabel: { color: colors.text, fontSize: 9, fontWeight: '800', letterSpacing: .5 },
  orderControlDetail: { color: colors.textMuted, fontSize: 9, marginTop: 4 },
  cancelOrdersButton: { minHeight: 36, justifyContent: 'center', paddingHorizontal: spacing.md, borderRadius: 5, backgroundColor: colors.red },
  cancelOrdersDisabled: { opacity: .35 },
  cancelOrdersText: { color: '#fff', fontSize: 9, fontWeight: '900', letterSpacing: .5 },
  resultRow: { minHeight: 66, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  resultMark: { width: 34, height: 34, alignItems: 'center', justifyContent: 'center', borderRadius: 17 },
  resultValue: { alignItems: 'flex-end', gap: 3 },
  resultPnl: { fontSize: fontSize.sm, fontWeight: '700' },
  scopeLabel: { marginBottom: spacing.xs, color: colors.accent, fontSize: 9, fontWeight: '800', letterSpacing: 1 },
  filterRow: { flexGrow: 0, marginBottom: spacing.xs },
  modeFilters: { flexDirection: 'row', gap: spacing.xs, marginBottom: spacing.md },
  filterChip: { minHeight: 30, justifyContent: 'center', paddingHorizontal: spacing.md, marginRight: spacing.xs, borderWidth: 1, borderColor: colors.border, borderRadius: 15, backgroundColor: colors.bgCard },
  filterChipActive: { borderColor: colors.accent, backgroundColor: colors.accentDim },
  filterChipText: { color: colors.textMuted, fontSize: 9, fontWeight: '800' },
  filterChipTextActive: { color: colors.text },
  resultsSummary: { flexDirection: 'row', flexWrap: 'wrap', marginBottom: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, overflow: 'hidden' },
  resultMetric: { width: '33.333%', minHeight: 58, justifyContent: 'center', paddingHorizontal: spacing.sm, borderRightWidth: StyleSheet.hairlineWidth, borderBottomWidth: StyleSheet.hairlineWidth, borderColor: colors.border },
  resultMetricValue: { marginTop: 4, color: colors.text, fontSize: fontSize.md, fontWeight: '800' },
  resultsMeta: { marginBottom: spacing.sm, color: colors.textMuted, fontSize: 9 },
  loadMore: { minHeight: 42, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: spacing.xs, marginTop: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 7, backgroundColor: colors.bgCard },
  loadMoreText: { color: colors.accent, fontSize: 9, fontWeight: '800', letterSpacing: 0.6 },
  empty: { alignItems: 'center', paddingVertical: 50, paddingHorizontal: spacing.xl },
  emptyTitle: { fontSize: fontSize.md, color: colors.text, fontWeight: '700', marginTop: spacing.sm },
  emptyDetail: { fontSize: fontSize.xs, color: colors.textMuted, textAlign: 'center', marginTop: 3 },
});
