import { ScrollView, Text, TouchableOpacity, StyleSheet } from 'react-native';
import { useAppStore } from '../state/store';
import { colors, spacing, fontSize } from '../utils/theme';

interface Props {
  instances: string[];
}

export function InstanceSelector({ instances }: Props) {
  const selected = useAppStore((s) => s.selectedInstance);
  const setSelected = useAppStore((s) => s.setSelectedInstance);

  const all = [null, ...instances];

  return (
    <ScrollView horizontal showsHorizontalScrollIndicator={false} style={styles.row}>
      {all.map((inst) => {
        const active = inst === selected;
        return (
          <TouchableOpacity
            key={inst ?? '__all__'}
            style={[styles.chip, active && styles.chipActive]}
            onPress={() => setSelected(inst)}
          >
            <Text style={[styles.chipText, active && styles.chipTextActive]}>
              {inst ?? 'ALL BOTS'}
            </Text>
          </TouchableOpacity>
        );
      })}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  row: {
    flexGrow: 0,
    marginBottom: spacing.md,
  },
  chip: {
    backgroundColor: colors.bgCard,
    borderRadius: 16,
    paddingHorizontal: spacing.md,
    paddingVertical: spacing.xs,
    marginRight: spacing.sm,
    borderWidth: 1,
    borderColor: colors.border,
  },
  chipActive: {
    backgroundColor: colors.accent,
    borderColor: colors.accent,
  },
  chipText: {
    fontSize: fontSize.sm,
    fontWeight: '600',
    color: colors.textDim,
    textTransform: 'uppercase',
  },
  chipTextActive: {
    color: '#fff',
  },
});
