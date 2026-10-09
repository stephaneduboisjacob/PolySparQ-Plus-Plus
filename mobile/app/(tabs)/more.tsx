import { useEffect, useState } from 'react';
import {
  View, Text, ScrollView, StyleSheet, TouchableOpacity,
  RefreshControl, Alert, Switch, TextInput,
} from 'react-native';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Ionicons } from '@expo/vector-icons';
import { router } from 'expo-router';
import Constants from 'expo-constants';
import { api } from '../../src/api/endpoints';
import { useAuthStore } from '../../src/state/AuthContext';
import { useAppStore } from '../../src/state/store';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';
import { CockpitHeader } from '../../src/components/CockpitHeader';
import { fmtAgo, fmtUsd, pnlColor } from '../../src/utils/format';
import { POLL_INTERVALS } from '../../src/utils/constants';
import { colors, spacing, fontSize } from '../../src/utils/theme';
import { registerForPushNotifications } from '../../src/notifications/fcm';
import type { PushPreferences } from '../../src/api/types';

type SubScreen = 'menu' | 'perf' | 'logs' | 'markets' | 'health' | 'notifications' | 'reports';

export default function MoreTab() {
  return (
    <ErrorBoundary>
      <MoreContent />
    </ErrorBoundary>
  );
}

function MoreContent() {
  const [screen, setScreen] = useState<SubScreen>('menu');

  if (screen === 'perf') return <PerfScreen onBack={() => setScreen('menu')} />;
  if (screen === 'logs') return <LogsScreen onBack={() => setScreen('menu')} />;
  if (screen === 'markets') return <MarketsScreen onBack={() => setScreen('menu')} />;
  if (screen === 'health') return <HealthScreen onBack={() => setScreen('menu')} />;
  if (screen === 'notifications') return <NotificationsScreen onBack={() => setScreen('menu')} />;
  if (screen === 'reports') return <ReportsScreen onBack={() => setScreen('menu')} />;

  return <MenuScreen onNavigate={setScreen} />;
}

function MenuScreen({ onNavigate }: { onNavigate: (s: SubScreen) => void }) {
  const logout = useAuthStore((s) => s.logout);
  const user = useAuthStore((s) => s.user);
  const connStatus = useAppStore((s) => s.connectionStatus);

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <CockpitHeader title="Operations" subtitle="SYSTEM · ACCOUNT · ADMIN" />
      <ScrollView style={styles.scroll} contentContainerStyle={styles.menuContent}>
        <Text style={styles.menuGroupLabel}>WORKSPACES</Text>
        <MenuItem icon="grid-outline" label="Fleet" detail="Bots, status and controls" onPress={() => router.push('/(tabs)/fleet')} />
        <MenuItem icon="options-outline" label="Strategies" detail="Configuration and decisions" onPress={() => router.push('/(tabs)/strategy')} />
        {user?.role === 'admin' ? (
          <>
            <Text style={styles.menuGroupLabel}>ADMINISTRATION</Text>
            <MenuItem icon="wallet-outline" label="Shared ADMIN Wallet" detail="One credential bundle for every ADMIN bot" onPress={() => router.push('/(tabs)/wallet')} />
          </>
        ) : null}

        <Text style={styles.menuGroupLabel}>OPERATIONS</Text>
        <MenuItem icon="stats-chart-outline" label="Performance" onPress={() => onNavigate('perf')} />
        <MenuItem icon="terminal-outline" label="Bot Logs" onPress={() => onNavigate('logs')} />
        <MenuItem icon="pulse-outline" label="System Health" onPress={() => onNavigate('health')} />
        {user?.role !== 'demo' && <MenuItem icon="notifications-outline" label="Push Notifications" onPress={() => onNavigate('notifications')} />}
        <MenuItem icon="document-text-outline" label="Session Reports" onPress={() => onNavigate('reports')} />

        <View style={styles.divider} />

        <View style={styles.infoSection}>
          <Text style={styles.infoLabel}>User</Text>
          <Text style={styles.infoValue}>{String(user?.username ?? '—')} ({String(user?.role ?? '—')})</Text>
          <Text style={styles.infoLabel}>Plan</Text>
          <Text style={styles.infoValue}>{String(user?.plan ?? '—')}</Text>
          <Text style={styles.infoLabel}>Instances</Text>
          <Text style={styles.infoValue}>{Array.isArray(user?.instances) ? user.instances.join(', ') : '—'}</Text>
          <Text style={styles.infoLabel}>Connection</Text>
          <Text style={[styles.infoValue, {
            color: connStatus === 'connected' ? colors.green : connStatus === 'reconnecting' ? colors.yellow : colors.red
          }]}>{String(connStatus)}</Text>
        </View>

        <View style={styles.divider} />

        <TouchableOpacity
          style={styles.logoutBtn}
          onPress={() =>
            Alert.alert('Logout', 'Disconnect from cockpit?', [
              { text: 'Cancel', style: 'cancel' },
              { text: 'Logout', style: 'destructive', onPress: () => { logout(); router.replace('/'); } },
            ])
          }
        >
          <Ionicons name="log-out-outline" size={20} color={colors.red} />
          <Text style={styles.logoutText}>Logout</Text>
        </TouchableOpacity>

        <Text style={styles.version}>PolySparQ++ Mobile v{Constants.expoConfig?.version ?? 'dev'}</Text>
      </ScrollView>
    </SafeAreaView>
  );
}

function MenuItem({ icon, label, detail, onPress }: { icon: keyof typeof Ionicons.glyphMap; label: string; detail?: string; onPress: () => void }) {
  return (
    <TouchableOpacity style={styles.menuItem} onPress={onPress}>
      <Ionicons name={icon} size={22} color={colors.accent} />
      <View style={styles.menuItemCopy}><Text style={styles.menuItemText}>{label}</Text>{detail ? <Text style={styles.menuItemDetail}>{detail}</Text> : null}</View>
      <Ionicons name="chevron-forward-outline" size={18} color={colors.textMuted} />
    </TouchableOpacity>
  );
}

function SubHeader({ title, onBack }: { title: string; onBack: () => void }) {
  return (
    <CockpitHeader
      title={title}
      subtitle="OPERATIONS"
      action={<TouchableOpacity onPress={onBack} style={styles.backBtn} accessibilityLabel="Back to Operations"><Ionicons name="arrow-back-outline" size={21} color={colors.accent} /></TouchableOpacity>}
    />
  );
}

function PerfScreen({ onBack }: { onBack: () => void }) {
  const { data, isLoading, refetch, error } = useQuery({
    queryKey: ['perf_stats'],
    queryFn: api.perfStats,
    refetchInterval: POLL_INTERVALS.PERF_STATS,
  });

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <SubHeader title="Performance" onBack={onBack} />
      <ScrollView
        style={styles.scroll}
        contentContainerStyle={styles.content}
        refreshControl={<RefreshControl refreshing={isLoading} onRefresh={refetch} tintColor={colors.accent} />}
      >
        {error && <Text style={styles.errorText}>{'Failed to load: ' + String((error as Error)?.message ?? 'unknown')}</Text>}
        {data && (
          <View style={styles.perfGrid}>
            <PerfCell label="Net P&L" value={fmtUsd(Number(data.net_pnl) || 0)} color={pnlColor(Number(data.net_pnl) || 0)} />
            <PerfCell label="Win Rate" value={data.win_rate != null ? `${Number(data.win_rate).toFixed(1)}%` : '—'} />
            <PerfCell label="Wins" value={String(data.wins ?? 0)} color={colors.green} />
            <PerfCell label="Losses" value={String(data.losses ?? 0)} color={colors.red} />
            <PerfCell label="Best Win" value={fmtUsd(Number(data.best_win) || 0)} color={colors.green} />
            <PerfCell label="Worst Loss" value={fmtUsd(Number(data.worst_loss) || 0)} color={colors.red} />
            <PerfCell label="Total Entries" value={String(data.total_entries ?? 0)} />
            <PerfCell label="Total Exits" value={String(data.total_exits ?? 0)} />
            <PerfCell label="Avg Win" value={fmtUsd(Number(data.avg_win) || 0)} color={colors.green} />
            <PerfCell label="Avg Loss" value={fmtUsd(Number(data.avg_loss) || 0)} color={colors.red} />
            <PerfCell label="Total Fees" value={fmtUsd(Number(data.total_fees) || 0)} />
          </View>
        )}
        {!data && !isLoading && !error && <Text style={styles.empty}>No performance data</Text>}
      </ScrollView>
    </SafeAreaView>
  );
}

function PerfCell({ label, value, color }: { label: string; value: string; color?: string }) {
  return (
    <View style={styles.perfCell}>
      <Text style={styles.perfLabel}>{label}</Text>
      <Text style={[styles.perfValue, color ? { color } : {}]}>{value}</Text>
    </View>
  );
}

function LogsScreen({ onBack }: { onBack: () => void }) {
  const instance = useAppStore((s) => s.selectedInstance);

  const { data, isLoading, refetch, error } = useQuery({
    queryKey: ['log_tail', instance],
    queryFn: () => api.logTail(instance ?? undefined, 200),
    refetchInterval: 10000,
  });

  const lines = Array.isArray(data?.lines) ? data.lines : (typeof data?.lines === 'string' ? [data.lines] : []);

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <SubHeader title={`Logs${instance ? ` (${instance})` : ''}`} onBack={onBack} />
      <ScrollView
        style={styles.scroll}
        contentContainerStyle={styles.logsContent}
        refreshControl={<RefreshControl refreshing={isLoading} onRefresh={refetch} tintColor={colors.accent} />}
      >
        {error && <Text style={styles.errorText}>{'Failed to load: ' + String((error as Error)?.message ?? 'unknown')}</Text>}
        {lines.map((line, i) => (
          <Text key={i} style={styles.logLine}>{String(line)}</Text>
        ))}
        {lines.length === 0 && !isLoading && (
          <Text style={styles.empty}>No log lines</Text>
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

function MarketsScreen({ onBack }: { onBack: () => void }) {
  const { data, isLoading, refetch, error } = useQuery({
    queryKey: ['markets'],
    queryFn: () => api.markets(40),
    refetchInterval: POLL_INTERVALS.MARKETS,
  });

  const mkts = data?.markets as any;
  const markets: any[] = Array.isArray(mkts) ? mkts : Array.isArray(mkts?.markets) ? mkts.markets : [];

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <SubHeader title="Markets" onBack={onBack} />
      <ScrollView
        style={styles.scroll}
        contentContainerStyle={styles.content}
        refreshControl={<RefreshControl refreshing={isLoading} onRefresh={refetch} tintColor={colors.accent} />}
      >
        {error && <Text style={styles.errorText}>{'Failed to load: ' + String((error as Error)?.message ?? 'unknown')}</Text>}
        {markets.map((m, i) => (
          <View key={i} style={styles.marketCard}>
            <View style={styles.marketHeader}>
              <Text style={styles.marketAsset}>{String(m?.asset ?? '—')}</Text>
              <Text style={[styles.marketPhase, {
                color: m?.phase === 'live' ? colors.green : m?.phase === 'closing' ? colors.yellow : colors.textDim
              }]}>{String(m?.phase ?? '—')}</Text>
            </View>
            <Text style={styles.marketQuestion} numberOfLines={2}>{String(m?.question ?? '')}</Text>
            {m?.midprice != null && (
              <Text style={styles.marketMid}>
                {'Mid: '}{Number(m.midprice).toFixed(4)}{' | Spread: '}{m?.spread != null ? Number(m.spread).toFixed(4) : '—'}
              </Text>
            )}
          </View>
        ))}
        {markets.length === 0 && !isLoading && (
          <Text style={styles.empty}>No active markets</Text>
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

function HealthScreen({ onBack }: { onBack: () => void }) {
  const query = useQuery({ queryKey: ['health'], queryFn: api.health, refetchInterval: 5000 });
  const health = query.data;
  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <SubHeader title="System Health" onBack={onBack} />
      <ScrollView style={styles.scroll} contentContainerStyle={styles.content} refreshControl={<RefreshControl refreshing={query.isFetching} onRefresh={query.refetch} tintColor={colors.accent} />}>
        {query.error && <Text style={styles.errorText}>{(query.error as Error).message}</Text>}
        {health && (
          <>
            <View style={styles.healthHero}>
              <View style={[styles.healthDot, { backgroundColor: health.running ? colors.green : colors.red }]} />
              <View><Text style={styles.healthTitle}>{health.running ? 'Cockpit operational' : 'Bot process unavailable'}</Text><Text style={styles.healthMeta}>Updated {fmtAgo(health.server_ts)}</Text></View>
            </View>
            <View style={styles.settingsGroup}>
              <InfoRow label="Oracle" value={health.oracle_source || 'chainlink'} />
              <InfoRow label="Order books" value={health.orderbook_health || 'No status'} />
              <InfoRow label="Feed events" value={health.events || 'No status'} />
              <InfoRow label="Parse failures" value={String(health.parsefail ?? 0)} valueColor={health.parsefail ? colors.red : colors.green} />
            </View>
            <Text style={styles.groupLabel}>CONNECTIONS</Text>
            <View style={styles.settingsGroup}>
              {Object.entries(health.ws ?? {}).map(([name, status]) => (
                <InfoRow key={name} label={name.toUpperCase()} value={status} valueColor={status === 'connected' ? colors.green : colors.yellow} />
              ))}
            </View>
            {health.heartbeat ? <View style={styles.heartbeat}><Text style={styles.heartbeatLabel}>LATEST HEARTBEAT</Text><Text style={styles.heartbeatText}>{health.heartbeat}</Text></View> : null}
          </>
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

function NotificationsScreen({ onBack }: { onBack: () => void }) {
  const queryClient = useQueryClient();
  const prefs = useQuery({ queryKey: ['push_preferences'], queryFn: api.pushPreferences });
  const [threshold, setThreshold] = useState('');
  const update = useMutation({
    mutationFn: (values: Partial<PushPreferences>) => api.pushPreferencesUpdate(values),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['push_preferences'] }),
    onError: (error: Error) => Alert.alert('Notification update failed', error.message),
  });
  const values = prefs.data;

  useEffect(() => {
    if (values && !threshold) setThreshold(String(values.pnl_swing_usd));
  }, [threshold, values]);

  const enableDevice = async () => {
    const ok = await registerForPushNotifications();
    Alert.alert(ok ? 'Notifications enabled' : 'Notifications unavailable', ok ? 'This device is registered for fill and result alerts.' : 'Permission was denied or Firebase could not register this device.');
  };

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <SubHeader title="Push Notifications" onBack={onBack} />
      <ScrollView style={styles.scroll} contentContainerStyle={styles.content}>
        <TouchableOpacity style={styles.enablePush} onPress={enableDevice}>
          <Ionicons name="notifications" size={20} color="#fff" />
          <View style={{ flex: 1 }}><Text style={styles.enablePushTitle}>Register this device</Text><Text style={styles.enablePushText}>Requests permission and refreshes the Firebase token.</Text></View>
        </TouchableOpacity>
        {prefs.error && <Text style={styles.errorText}>{(prefs.error as Error).message}</Text>}
        {values && (
          <View style={styles.settingsGroup}>
            <SettingToggle icon="flash-outline" label="Order fills" detail="Every new buy or sell fill" value={values.fills} onChange={(fills) => update.mutate({ fills })} />
            <SettingToggle icon="trophy-outline" label="Wins and losses" detail="Result and realized P&L when a position exits" value={values.results} onChange={(results) => update.mutate({ results })} />
            <SettingToggle icon="warning-outline" label="Bot stopped" detail="Unexpected instance shutdowns" value={values.bot_crash} onChange={(bot_crash) => update.mutate({ bot_crash })} />
            <SettingToggle icon="trending-up-outline" label="P&L movement" detail="Alert when net P&L moves by the threshold" value={values.pnl_swing} onChange={(pnl_swing) => update.mutate({ pnl_swing })} />
            <View style={styles.thresholdRow}>
              <View><Text style={styles.settingLabel}>P&L threshold</Text><Text style={styles.settingDetail}>USD movement required</Text></View>
              <TextInput
                style={styles.thresholdInput}
                value={threshold || String(values.pnl_swing_usd)}
                onChangeText={setThreshold}
                onEndEditing={() => {
                  const amount = Number(threshold);
                  if (Number.isFinite(amount) && amount > 0) update.mutate({ pnl_swing_usd: amount });
                  else setThreshold(String(values.pnl_swing_usd));
                }}
                keyboardType="decimal-pad"
              />
            </View>
          </View>
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

function ReportsScreen({ onBack }: { onBack: () => void }) {
  const [selected, setSelected] = useState<string | null>(null);
  const reports = useQuery({ queryKey: ['session_reports'], queryFn: api.sessionReports });
  const report = useQuery({ queryKey: ['session_report', selected], queryFn: () => api.sessionReport(selected!), enabled: Boolean(selected) });
  if (selected) {
    return (
      <SafeAreaView style={styles.safe} edges={['top']}>
        <SubHeader title="Session Report" onBack={() => setSelected(null)} />
        <ScrollView style={styles.scroll} contentContainerStyle={styles.reportContent}>
          <Text style={styles.reportFile}>{selected}</Text>
          {report.isLoading ? <Text style={styles.empty}>Loading report...</Text> : <Text selectable style={styles.reportText}>{report.data?.content || (report.error as Error)?.message}</Text>}
        </ScrollView>
      </SafeAreaView>
    );
  }
  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <SubHeader title="Session Reports" onBack={onBack} />
      <ScrollView style={styles.scroll} contentContainerStyle={styles.content} refreshControl={<RefreshControl refreshing={reports.isFetching} onRefresh={reports.refetch} tintColor={colors.accent} />}>
        {(reports.data?.reports ?? []).map((item) => (
          <TouchableOpacity key={item.file} style={styles.reportRow} onPress={() => setSelected(item.file)}>
            <Ionicons name="document-text-outline" size={19} color={colors.accent} />
            <View style={{ flex: 1 }}><Text style={styles.reportName} numberOfLines={1}>{item.file}</Text><Text style={styles.reportMeta}>{new Date(item.mtime).toLocaleString()} · {Math.ceil(item.size / 1024)} KB</Text></View>
            <Ionicons name="chevron-forward" size={17} color={colors.textMuted} />
          </TouchableOpacity>
        ))}
      </ScrollView>
    </SafeAreaView>
  );
}

function SettingToggle({ icon, label, detail, value, onChange }: { icon: keyof typeof Ionicons.glyphMap; label: string; detail: string; value: boolean; onChange: (value: boolean) => void }) {
  return <View style={styles.settingRow}><View style={styles.settingIcon}><Ionicons name={icon} size={18} color={colors.accent} /></View><View style={{ flex: 1 }}><Text style={styles.settingLabel}>{label}</Text><Text style={styles.settingDetail}>{detail}</Text></View><Switch value={value} onValueChange={onChange} trackColor={{ false: colors.bgInput, true: colors.accentDim }} thumbColor={value ? '#fff' : colors.textMuted} /></View>;
}

function InfoRow({ label, value, valueColor }: { label: string; value: string; valueColor?: string }) {
  return <View style={styles.infoRow}><Text style={styles.infoRowLabel}>{label}</Text><Text style={[styles.infoRowValue, valueColor ? { color: valueColor } : null]} numberOfLines={2}>{value}</Text></View>;
}

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.bg },
  title: {
    fontSize: fontSize.xl, fontWeight: '700', color: colors.text,
    paddingHorizontal: spacing.lg, paddingVertical: spacing.md,
  },
  scroll: { flex: 1 },
  content: { paddingHorizontal: spacing.lg, paddingBottom: spacing.xxl },
  menuContent: { paddingHorizontal: spacing.lg, paddingBottom: spacing.xxl },
  menuGroupLabel: { color: colors.textMuted, fontSize: 9, fontWeight: '900', letterSpacing: .8, marginTop: spacing.sm, marginBottom: spacing.xs },
  menuItem: {
    flexDirection: 'row', alignItems: 'center', gap: spacing.md,
    backgroundColor: colors.bgCard, borderRadius: 8, padding: spacing.lg,
    marginBottom: spacing.sm, borderWidth: 1, borderColor: colors.border,
  },
  menuItemCopy: { flex: 1 },
  menuItemText: { fontSize: fontSize.sm, color: colors.text, fontWeight: '700' },
  menuItemDetail: { color: colors.textMuted, fontSize: 9, marginTop: 4 },
  divider: { height: 1, backgroundColor: colors.border, marginVertical: spacing.lg },
  infoSection: { gap: spacing.xs },
  infoLabel: { fontSize: fontSize.xs, color: colors.textMuted },
  infoValue: { fontSize: fontSize.md, color: colors.text, marginBottom: spacing.sm },
  logoutBtn: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingVertical: spacing.md },
  logoutText: { fontSize: fontSize.md, color: colors.red },
  version: { fontSize: fontSize.xs, color: colors.textMuted, textAlign: 'center', marginTop: spacing.xxl },
  subHeader: {
    flexDirection: 'row', alignItems: 'center', gap: spacing.md,
    paddingHorizontal: spacing.lg, paddingVertical: spacing.md,
  },
  backBtn: { padding: spacing.xs },
  subTitle: { fontSize: fontSize.xl, fontWeight: '700', color: colors.text },
  perfGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: spacing.sm },
  perfCell: {
    backgroundColor: colors.bgCard, borderRadius: 8, padding: spacing.lg,
    width: '48%', borderWidth: 1, borderColor: colors.border,
  },
  perfLabel: { fontSize: fontSize.xs, color: colors.textMuted },
  perfValue: { fontSize: fontSize.lg, fontWeight: '700', color: colors.text, marginTop: 4 },
  logsContent: { paddingHorizontal: spacing.md, paddingBottom: spacing.xxl },
  logLine: { fontSize: 10, fontFamily: 'monospace', color: colors.textDim, lineHeight: 14 },
  empty: { color: colors.textMuted, textAlign: 'center', marginTop: spacing.xxl, fontSize: fontSize.md },
  errorText: { color: colors.red, textAlign: 'center', marginTop: spacing.lg, fontSize: fontSize.sm },
  marketCard: {
    backgroundColor: colors.bgCard, borderRadius: 8, padding: spacing.lg,
    marginBottom: spacing.sm, borderWidth: 1, borderColor: colors.border,
  },
  marketHeader: {
    flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
    marginBottom: spacing.xs,
  },
  marketAsset: { fontSize: fontSize.lg, fontWeight: '700', color: colors.text },
  marketPhase: { fontSize: fontSize.xs, fontWeight: '700', textTransform: 'uppercase' },
  marketQuestion: { fontSize: fontSize.sm, color: colors.textDim, marginBottom: spacing.xs },
  marketMid: { fontSize: fontSize.xs, color: colors.textMuted },
  healthHero: { minHeight: 68, flexDirection: 'row', alignItems: 'center', gap: spacing.md, padding: spacing.lg, marginBottom: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard },
  healthDot: { width: 11, height: 11, borderRadius: 6 },
  healthTitle: { fontSize: fontSize.md, color: colors.text, fontWeight: '700' },
  healthMeta: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 2 },
  groupLabel: { fontSize: 9, color: colors.textMuted, fontWeight: '700', marginTop: spacing.lg, marginBottom: spacing.xs },
  settingsGroup: { borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, overflow: 'hidden' },
  infoRow: { minHeight: 48, flexDirection: 'row', alignItems: 'center', gap: spacing.md, paddingHorizontal: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  infoRowLabel: { width: 100, fontSize: fontSize.xs, color: colors.textMuted },
  infoRowValue: { flex: 1, fontSize: fontSize.xs, color: colors.text, textAlign: 'right' },
  heartbeat: { marginTop: spacing.md, padding: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: '#0e1420' },
  heartbeatLabel: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  heartbeatText: { fontSize: fontSize.xs, color: colors.textDim, lineHeight: 17, marginTop: spacing.xs },
  enablePush: { minHeight: 64, flexDirection: 'row', alignItems: 'center', gap: spacing.md, padding: spacing.md, marginBottom: spacing.md, borderRadius: 8, backgroundColor: colors.accent },
  enablePushTitle: { color: '#fff', fontSize: fontSize.sm, fontWeight: '700' },
  enablePushText: { color: '#dbeafe', fontSize: fontSize.xs, marginTop: 2 },
  settingRow: { minHeight: 66, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  settingIcon: { width: 32, height: 32, alignItems: 'center', justifyContent: 'center', borderRadius: 6, backgroundColor: colors.bgInput },
  settingLabel: { fontSize: fontSize.sm, color: colors.text, fontWeight: '600' },
  settingDetail: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 2 },
  thresholdRow: { minHeight: 66, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: spacing.md },
  thresholdInput: { width: 88, height: 38, textAlign: 'right', paddingHorizontal: spacing.sm, color: colors.text, fontSize: fontSize.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  reportRow: { minHeight: 62, flexDirection: 'row', alignItems: 'center', gap: spacing.md, paddingHorizontal: spacing.md, marginBottom: spacing.xs, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard },
  reportName: { color: colors.text, fontSize: fontSize.xs, fontWeight: '600' },
  reportMeta: { color: colors.textMuted, fontSize: 9, marginTop: 3 },
  reportContent: { paddingHorizontal: spacing.md, paddingBottom: spacing.xxl },
  reportFile: { color: colors.textMuted, fontSize: fontSize.xs, marginBottom: spacing.md },
  reportText: { color: colors.textDim, fontFamily: 'monospace', fontSize: 11, lineHeight: 17 },
});
