import type { ReactNode } from 'react';
import { Image, StyleSheet, Text, View } from 'react-native';
import { ConnectionDot } from './ConnectionDot';
import { StatusBadge } from './StatusBadge';
import { colors, fontSize, spacing } from '../utils/theme';

interface Props {
  title: string;
  subtitle?: string;
  mode?: string;
  action?: ReactNode;
}

export function CockpitHeader({ title, subtitle, mode, action }: Props) {
  const normalizedMode = String(mode || '').toLowerCase();
  const live = normalizedMode === 'live';
  const paper = normalizedMode === 'paper';

  return (
    <View style={styles.container}>
      <View style={styles.brandRow}>
        <Image
          source={require('../../assets/polysparq-wordmark.png')}
          resizeMode="contain"
          style={styles.wordmark}
          accessibilityLabel="PolySparQ plus plus"
        />
        <View style={styles.statusGroup}>
          {mode ? (
            <StatusBadge
              label={live ? 'LIVE' : paper ? 'PAPER' : normalizedMode.toUpperCase()}
              color={live ? colors.red : paper ? colors.yellow : colors.textDim}
              bgColor={live ? colors.redDim : paper ? '#332911' : colors.bgInput}
            />
          ) : null}
          <ConnectionDot />
          {action}
        </View>
      </View>
      <View style={styles.titleBlock}>
        <Text style={styles.title}>{title}</Text>
        {subtitle ? <Text style={styles.subtitle}>{subtitle}</Text> : null}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { paddingHorizontal: spacing.lg, paddingTop: spacing.xs, paddingBottom: spacing.md, gap: spacing.md },
  brandRow: { minHeight: 34, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: spacing.sm },
  wordmark: { width: 148, height: 31 },
  statusGroup: { flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-end', gap: spacing.sm },
  titleBlock: { borderLeftWidth: 2, borderLeftColor: colors.accent, paddingLeft: spacing.md, gap: 1 },
  title: { color: colors.text, fontSize: fontSize.lg, fontWeight: '700', letterSpacing: -.2 },
  subtitle: { color: colors.textMuted, fontSize: fontSize.xs },
});
