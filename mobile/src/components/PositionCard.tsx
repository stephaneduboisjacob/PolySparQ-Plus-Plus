import { View, Text, TouchableOpacity, StyleSheet, Alert } from 'react-native';
import { positionMarkKey, useAppStore } from '../state/store';
import { api } from '../api/endpoints';
import { fmtUsd, fmtPx, fmtPct, pnlColor } from '../utils/format';
import { colors, spacing, fontSize } from '../utils/theme';
import { StatusBadge } from './StatusBadge';
import type { Position } from '../api/types';

interface Props { position: Position; canTrade?: boolean; onSold?: () => void; }

export function PositionCard({ position: p, canTrade = false, onSold }: Props) {
  const marks = useAppStore((s) => s.positionMarks);
  const key = positionMarkKey(p.cid, p.side, p.instance);
  const liveMark = marks?.[key];

  const mark = liveMark?.mark ?? p.mark;
  const pnl = liveMark?.pnl_usd ?? p.pnl_usd;
  const pnlPct = liveMark?.pnl_pct ?? p.pnl_pct;

  async function handleForceSell() {
    Alert.alert(
      'Force Sell',
      `Sell ${p.shares.toFixed(2)} shares of ${p.outcome_label || p.asset} ${p.outcome_label ? `(${p.side})` : ''}?`,
      [
        { text: 'Cancel', style: 'cancel' },
        {
          text: 'Sell',
          style: 'destructive',
          onPress: async () => {
            try {
              await api.forceSell(p.cid, p.side, p.instance);
              onSold?.();
              Alert.alert('Queued', 'Force sell command queued');
            } catch (e: any) {
              Alert.alert('Error', e.message);
            }
          },
        },
      ],
    );
  }

  const sideColor = p.side === 'UP' ? colors.green : colors.red;

  return (
    <View style={styles.card}>
      <View style={styles.header}>
        <View style={styles.headerLeft}>
          <Text style={styles.asset} numberOfLines={1}>{p.outcome_label || p.asset}</Text>
          <StatusBadge label={p.side} color={sideColor} bgColor={p.side === 'UP' ? colors.greenDim : colors.redDim} />
          {p.instance && (
            <Text style={styles.instance}>{p.instance}</Text>
          )}
        </View>
        {p.awaiting_settlement && (
          <StatusBadge label="SETTLING" color={colors.yellow} bgColor="#3d3000" />
        )}
      </View>

      <View style={styles.grid}>
        <View style={styles.cell}>
          <Text style={styles.cellLabel}>Shares</Text>
          <Text style={styles.cellValue}>{p.shares.toFixed(2)}</Text>
        </View>
        <View style={styles.cell}>
          <Text style={styles.cellLabel}>Avg Entry</Text>
          <Text style={styles.cellValue}>{fmtPx(p.avg)}</Text>
        </View>
        <View style={styles.cell}>
          <Text style={styles.cellLabel}>Mark</Text>
          <Text style={[styles.cellValue, { color: colors.accent }]}>{fmtPx(mark)}</Text>
        </View>
        <View style={styles.cell}>
          <Text style={styles.cellLabel}>P&L</Text>
          <Text style={[styles.cellValue, { color: pnlColor(pnl) }]}>
            {fmtUsd(pnl)} ({fmtPct(pnlPct)})
          </Text>
        </View>
      </View>

      <View style={styles.footer}>
        <Text style={styles.deployed}>Deployed: {fmtUsd(p.cost_basis)}</Text>
        {canTrade && !p.awaiting_settlement && (
          <TouchableOpacity style={styles.sellBtn} onPress={handleForceSell} accessibilityRole="button">
            <Text style={styles.sellBtnText}>FORCE SELL</Text>
          </TouchableOpacity>
        )}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  card: {
    backgroundColor: colors.bgCard,
    borderRadius: 8,
    padding: spacing.lg,
    marginBottom: spacing.sm,
    borderWidth: 1,
    borderColor: colors.border,
  },
  header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginBottom: spacing.md,
  },
  headerLeft: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: spacing.sm,
  },
  asset: {
    fontSize: fontSize.lg,
    fontWeight: '700',
    color: colors.text,
  },
  instance: {
    fontSize: fontSize.xs,
    color: colors.textMuted,
  },
  grid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: spacing.sm,
    marginBottom: spacing.md,
  },
  cell: {
    width: '47%',
  },
  cellLabel: {
    fontSize: fontSize.xs,
    color: colors.textMuted,
  },
  cellValue: {
    fontSize: fontSize.md,
    fontWeight: '600',
    color: colors.text,
  },
  footer: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  deployed: {
    fontSize: fontSize.sm,
    color: colors.textDim,
  },
  sellBtn: {
    backgroundColor: colors.redDim,
    paddingHorizontal: spacing.md,
    paddingVertical: spacing.xs,
    borderRadius: 6,
    borderWidth: 1,
    borderColor: colors.red,
  },
  sellBtnText: {
    fontSize: fontSize.xs,
    fontWeight: '700',
    color: colors.red,
  },
});
