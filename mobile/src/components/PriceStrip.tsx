import { View, Text, StyleSheet, ScrollView } from 'react-native';
import { useAppStore } from '../state/store';
import { ASSETS } from '../utils/constants';
import { colors, spacing, fontSize } from '../utils/theme';

export function PriceStrip() {
  const prices = useAppStore((s) => s.prices);

  return (
    <ScrollView horizontal showsHorizontalScrollIndicator={false} style={styles.strip}>
      {ASSETS.map((asset) => {
        const p = prices[asset];
        if (!p) {
          return (
            <View key={asset} style={styles.card}>
              <Text style={styles.asset}>{asset}</Text>
              <Text style={styles.price}>—</Text>
            </View>
          );
        }
        const delta = p.delta_pct ?? 0;
        const deltaColor = delta > 0 ? colors.green : delta < 0 ? colors.red : colors.textDim;
        return (
          <View key={asset} style={styles.card}>
            <Text style={styles.asset}>{asset}</Text>
            <Text style={styles.price}>${p.oracle?.toLocaleString(undefined, { maximumFractionDigits: 0 })}</Text>
            <Text style={[styles.delta, { color: deltaColor }]}>
              {delta >= 0 ? '+' : ''}{delta.toFixed(3)}%
            </Text>
          </View>
        );
      })}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  strip: {
    flexGrow: 0,
    marginBottom: spacing.sm,
  },
  card: {
    backgroundColor: colors.bgCard,
    borderRadius: 8,
    paddingHorizontal: spacing.md,
    paddingVertical: spacing.sm,
    marginRight: spacing.sm,
    minWidth: 80,
    alignItems: 'center',
  },
  asset: {
    fontSize: fontSize.xs,
    fontWeight: '700',
    color: colors.textDim,
    marginBottom: 2,
  },
  price: {
    fontSize: fontSize.md,
    fontWeight: '600',
    color: colors.text,
  },
  delta: {
    fontSize: fontSize.xs,
    fontWeight: '600',
    marginTop: 2,
  },
});
