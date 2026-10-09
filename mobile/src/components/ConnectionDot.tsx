import { View, Text, StyleSheet } from 'react-native';
import { useAppStore } from '../state/store';
import { colors, spacing, fontSize } from '../utils/theme';

const STATUS_COLORS = {
  connected: colors.green,
  reconnecting: colors.yellow,
  offline: colors.red,
} as const;

export function ConnectionDot() {
  const status = useAppStore((s) => s.connectionStatus);

  return (
    <View style={styles.row}>
      <View style={[styles.dot, { backgroundColor: STATUS_COLORS[status] }]} />
      <Text style={styles.label}>{status === 'connected' ? 'Live' : status}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: spacing.xs,
  },
  dot: {
    width: 8,
    height: 8,
    borderRadius: 4,
  },
  label: {
    fontSize: fontSize.xs,
    color: colors.textDim,
    textTransform: 'capitalize',
  },
});
