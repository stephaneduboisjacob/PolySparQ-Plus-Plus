import { RefreshControl, ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { useQuery } from '@tanstack/react-query';
import { router } from 'expo-router';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Ionicons } from '@expo/vector-icons';
import { api } from '../../src/api/endpoints';
import { useAppStore } from '../../src/state/store';
import { useAuthStore } from '../../src/state/AuthContext';
import { PriceStrip } from '../../src/components/PriceStrip';
import { CockpitHeader } from '../../src/components/CockpitHeader';
import { InstanceSelector } from '../../src/components/InstanceSelector';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';
import { fmtAgo, fmtPx, fmtUsd, pnlColor } from '../../src/utils/format';
import { POLL_INTERVALS } from '../../src/utils/constants';
import { colors, fontSize, spacing } from '../../src/utils/theme';

export default function DashboardTab() {
  return <ErrorBoundary><DashboardContent /></ErrorBoundary>;
}

function DashboardContent() {
  const instance = useAppStore((state) => state.selectedInstance);
  const streamFills = useAppStore((state) => state.recentFills) ?? [];
  const user = useAuthStore((state) => state.user);
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

  const status = live.data?.status;
  const health = live.data?.health;
  const positions = live.data?.positions?.positions ?? [];
  const radar = live.data?.radar ?? [];
  const instances = status?.instances_running ?? user?.instances ?? [];
  const fills = streamFills.length ? streamFills : (activity.data?.orders ?? []).filter((order) => order.status === 'filled').map((order) => ({
    ts: order.ts_created,
    asset: order.asset,
    direction: order.direction,
    side_label: order.side_label,
    price: order.price,
    size_shares: order.size_shares,
    source: order.source,
  }));

  const refresh = async () => { await Promise.all([live.refetch(), activity.refetch()]); };

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <CockpitHeader
        title="Command Center"
        subtitle={instance ? instance.toUpperCase() : 'ALL BOTS · FLEET OVERVIEW'}
        mode={status?.mode}
      />

      <ScrollView
        style={styles.scroll}
        contentContainerStyle={styles.content}
        refreshControl={<RefreshControl refreshing={live.isFetching || activity.isFetching} onRefresh={refresh} tintColor={colors.accent} />}
      >
        <InstanceSelector instances={instances} />
        <PriceStrip />

        {status?.mode === 'live' && (
          <View style={styles.liveBanner}><Ionicons name="flash" size={17} color={colors.red} /><Text style={styles.liveBannerText}>LIVE EXECUTION ACTIVE</Text><Text style={styles.liveBannerMeta}>Real funds</Text></View>
        )}

        {status && (
          <View style={styles.accountPanel}>
            <View style={styles.accountPrimary}>
              <Text style={styles.metricLabel}>NET P&L</Text>
              <Text style={[styles.netPnl, { color: pnlColor(status.net_pnl) }]}>{fmtUsd(status.net_pnl)}</Text>
              <Text style={styles.accountMeta}>{status.wins} wins · {status.losses} losses{status.win_pct != null ? ` · ${status.win_pct.toFixed(0)}%` : ''}</Text>
            </View>
            <View style={styles.metricGrid}>
              <Metric label="Equity" value={fmtUsd(status.equity_usd)} />
              <Metric label="Cash" value={fmtUsd(status.cash_usd)} />
              <Metric label="Deployed" value={fmtUsd(status.deployed_usd)} />
              <Metric label="Fees" value={fmtUsd(status.fees_usd)} />
            </View>
          </View>
        )}

        {health && (
          <View style={styles.healthStrip}>
            <HealthCell icon="radio-outline" label="Bot" value={status?.running ? 'Running' : 'Stopped'} ok={Boolean(status?.running)} />
            <HealthCell icon="layers-outline" label="Books" value={status?.health_stale ? `${status.health_stale} stale` : 'Fresh'} ok={!status?.health_stale} />
            <HealthCell icon="git-compare-outline" label="Oracle" value={health.oracle_source || 'Chainlink'} ok={!health.oracle_source || health.oracle_source === 'chainlink'} />
          </View>
        )}

        {!!radar.length && (
          <View style={styles.section}>
            <SectionHeader title="Operator radar" count={radar.length} />
            <View style={styles.sectionBody}>
              {radar.map((item, index) => (
                <View key={`${item.kind}-${index}`} style={styles.alertRow}>
                  <View style={[styles.alertMarker, { backgroundColor: item.level === 'critical' ? colors.red : item.level === 'warn' ? colors.yellow : colors.accent }]} />
                  <View style={styles.alertMain}><Text style={styles.alertTitle}>{item.title}</Text>{item.detail ? <Text style={styles.alertDetail}>{item.detail}</Text> : null}</View>
                  <Text style={styles.alertLevel}>{item.level}</Text>
                </View>
              ))}
            </View>
          </View>
        )}

        <View style={styles.section}>
          <SectionHeader title="Open positions" count={positions.length} onPress={() => router.push('/(tabs)/positions')} />
          <View style={styles.sectionBody}>
            {positions.slice(0, 4).map((position) => (
              <View key={`${position.cid}-${position.side}-${position.instance ?? ''}`} style={styles.positionRow}>
                <View style={[styles.sideMark, { backgroundColor: position.side === 'UP' ? colors.greenDim : colors.redDim }]}><Text style={[styles.sideText, { color: position.side === 'UP' ? colors.green : colors.red }]}>{position.side}</Text></View>
                <View style={styles.positionMain}><Text style={styles.positionTitle}>{position.asset} · {position.instance || 'default'}</Text><Text style={styles.positionMeta}>{position.shares.toFixed(2)} shares · avg {fmtPx(position.avg)} · mark {fmtPx(position.mark)}</Text></View>
                <Text style={[styles.positionPnl, { color: pnlColor(position.pnl_usd) }]}>{fmtUsd(position.pnl_usd)}</Text>
              </View>
            ))}
            {!positions.length && <Text style={styles.emptyRow}>No capital currently deployed</Text>}
          </View>
        </View>

        <View style={styles.section}>
          <SectionHeader title="Recent fills" count={fills.length} onPress={() => router.push('/(tabs)/positions')} />
          <View style={styles.sectionBody}>
            {fills.slice(0, 6).map((fill, index) => (
              <View key={`${fill.ts}-${index}`} style={styles.fillRow}>
                <Ionicons name={fill.direction === 'buy' ? 'arrow-down' : 'arrow-up'} size={16} color={fill.direction === 'buy' ? colors.green : colors.red} />
                <Text style={styles.fillTitle}>{fill.direction.toUpperCase()} {fill.asset || '?'} {fill.side_label}</Text>
                <Text style={styles.fillPrice}>{fmtPx(fill.price)}</Text>
                <Text style={styles.fillTime}>{fmtAgo(fill.ts)}</Text>
              </View>
            ))}
            {!fills.length && <Text style={styles.emptyRow}>No recent fills</Text>}
          </View>
        </View>
      </ScrollView>
    </SafeAreaView>
  );
}

function Metric({ label, value }: { label: string; value: string }) { return <View style={styles.metric}><Text style={styles.metricLabel}>{label.toUpperCase()}</Text><Text style={styles.metricValue}>{value}</Text></View>; }
function HealthCell({ icon, label, value, ok }: { icon: keyof typeof Ionicons.glyphMap; label: string; value: string; ok: boolean }) { return <View style={styles.healthCell}><Ionicons name={icon} size={17} color={ok ? colors.green : colors.yellow} /><View><Text style={styles.healthLabel}>{label}</Text><Text style={styles.healthValue} numberOfLines={1}>{value}</Text></View></View>; }
function SectionHeader({ title, count, onPress }: { title: string; count: number; onPress?: () => void }) { return <TouchableOpacity style={styles.sectionHeader} disabled={!onPress} onPress={onPress}><Text style={styles.sectionTitle}>{title}</Text><View style={styles.sectionCount}><Text style={styles.sectionCountText}>{count}</Text></View><View style={{ flex: 1 }} />{onPress && <Ionicons name="chevron-forward" size={17} color={colors.textMuted} />}</TouchableOpacity>; }

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.bg },
  header: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: spacing.lg, paddingVertical: spacing.md },
  wordmark: { fontSize: fontSize.lg, color: colors.accent, fontWeight: '800', letterSpacing: 0 },
  headerMeta: { fontSize: 9, color: colors.textMuted, fontWeight: '700', marginTop: 2 },
  headerRight: { flexDirection: 'row', alignItems: 'center', gap: spacing.md },
  scroll: { flex: 1 },
  content: { paddingHorizontal: spacing.lg, paddingBottom: spacing.xxl },
  liveBanner: { height: 38, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, marginBottom: spacing.sm, borderWidth: 1, borderColor: colors.red, borderRadius: 6, backgroundColor: colors.redDim },
  liveBannerText: { fontSize: fontSize.xs, color: colors.red, fontWeight: '800' },
  liveBannerMeta: { flex: 1, textAlign: 'right', fontSize: 9, color: colors.red },
  accountPanel: { flexDirection: 'row', minHeight: 130, marginBottom: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, overflow: 'hidden' },
  accountPrimary: { width: '48%', justifyContent: 'center', padding: spacing.lg, borderRightWidth: 1, borderRightColor: colors.border },
  metricLabel: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  netPnl: { fontSize: 28, fontWeight: '800', marginVertical: 5 },
  accountMeta: { fontSize: 9, color: colors.textMuted, lineHeight: 14 },
  metricGrid: { flex: 1, flexDirection: 'row', flexWrap: 'wrap' },
  metric: { width: '50%', justifyContent: 'center', paddingHorizontal: spacing.sm, borderRightWidth: StyleSheet.hairlineWidth, borderBottomWidth: StyleSheet.hairlineWidth, borderColor: colors.border },
  metricValue: { fontSize: fontSize.sm, color: colors.text, fontWeight: '700', marginTop: 3 },
  healthStrip: { minHeight: 58, flexDirection: 'row', marginBottom: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: '#101419', overflow: 'hidden' },
  healthCell: { flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: spacing.sm, borderRightWidth: StyleSheet.hairlineWidth, borderRightColor: colors.border },
  healthLabel: { fontSize: 9, color: colors.textMuted },
  healthValue: { maxWidth: 70, fontSize: fontSize.xs, color: colors.text, fontWeight: '600', marginTop: 2 },
  section: { marginBottom: spacing.md },
  sectionHeader: { height: 36, flexDirection: 'row', alignItems: 'center', gap: spacing.sm },
  sectionTitle: { fontSize: fontSize.sm, color: colors.text, fontWeight: '700' },
  sectionCount: { minWidth: 20, height: 20, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 5, borderRadius: 4, backgroundColor: colors.bgInput },
  sectionCountText: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  sectionBody: { borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, overflow: 'hidden' },
  alertRow: { minHeight: 52, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  alertMarker: { width: 4, height: 28, borderRadius: 2 },
  alertMain: { flex: 1 },
  alertTitle: { fontSize: fontSize.xs, color: colors.text, fontWeight: '700' },
  alertDetail: { fontSize: 9, color: colors.textMuted, marginTop: 2 },
  alertLevel: { fontSize: 9, color: colors.textMuted, textTransform: 'uppercase' },
  positionRow: { minHeight: 62, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  sideMark: { width: 38, height: 30, alignItems: 'center', justifyContent: 'center', borderRadius: 5 },
  sideText: { fontSize: 9, fontWeight: '800' },
  positionMain: { flex: 1, minWidth: 0 },
  positionTitle: { fontSize: fontSize.xs, color: colors.text, fontWeight: '700' },
  positionMeta: { fontSize: 9, color: colors.textMuted, marginTop: 3 },
  positionPnl: { fontSize: fontSize.sm, fontWeight: '700' },
  fillRow: { minHeight: 46, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  fillTitle: { flex: 1, fontSize: fontSize.xs, color: colors.text, fontWeight: '600' },
  fillPrice: { fontSize: fontSize.xs, color: colors.textDim },
  fillTime: { width: 46, textAlign: 'right', fontSize: 9, color: colors.textMuted },
  emptyRow: { padding: spacing.lg, fontSize: fontSize.xs, color: colors.textMuted, textAlign: 'center' },
});
