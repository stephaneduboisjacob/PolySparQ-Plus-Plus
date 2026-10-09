import { View, Text, StyleSheet } from 'react-native';
import { colors, spacing, fontSize } from '../utils/theme';

interface Props {
  label: string;
  color?: string;
  bgColor?: string;
}

export function StatusBadge({ label, color, bgColor }: Props) {
  const fg = color || colors.text;
  const bg = bgColor || colors.bgInput;

  return (
    <View style={[styles.badge, { backgroundColor: bg }]}>
      <Text style={[styles.text, { color: fg }]}>{label}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  badge: {
    paddingHorizontal: spacing.sm,
    paddingVertical: 2,
    borderRadius: 4,
    alignSelf: 'flex-start',
  },
  text: {
    fontSize: fontSize.xs,
    fontWeight: '700',
    textTransform: 'uppercase',
  },
});
