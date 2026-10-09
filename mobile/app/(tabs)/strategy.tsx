import { useCallback, useEffect, useMemo, useState } from 'react';
import {
  Alert,
  Modal,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Ionicons } from '@expo/vector-icons';
import { api } from '../../src/api/endpoints';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';
import { CockpitHeader } from '../../src/components/CockpitHeader';
import { colors, fontSize, spacing } from '../../src/utils/theme';

type ConfigEntry = {
  path: string;
  section: string;
  key: string;
  value: unknown;
};

function titleCase(value: string): string {
  return value
    .replace(/[._-]+/g, ' ')
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function flattenConfig(value: Record<string, unknown>): ConfigEntry[] {
  const entries: ConfigEntry[] = [];
  const visit = (object: Record<string, unknown>, parents: string[]) => {
    Object.entries(object).forEach(([key, child]) => {
      if (child && typeof child === 'object' && !Array.isArray(child)) {
        visit(child as Record<string, unknown>, [...parents, key]);
        return;
      }
      entries.push({
        path: [...parents, key].join('.'),
        section: parents.join('.') || 'general',
        key,
        value: child,
      });
    });
  };
  visit(value, []);
  return entries;
}

export default function StrategyTab() {
  return <ErrorBoundary><StrategyContent /></ErrorBoundary>;
}

function StrategyContent() {
  const queryClient = useQueryClient();
  const [selectedConfig, setSelectedConfig] = useState('');
  const [changes, setChanges] = useState<Record<string, unknown>>({});
  const [expanded, setExpanded] = useState<Record<string, boolean>>({});
  const [query, setQuery] = useState('');
  const [pickerOpen, setPickerOpen] = useState(false);
  const [view, setView] = useState<'editor' | 'raw'>('editor');
  const [notice, setNotice] = useState<string | null>(null);

  const configsQuery = useQuery({ queryKey: ['configs'], queryFn: api.configs });
  const configs = configsQuery.data?.configs ?? [];
  const activeConfig = selectedConfig || configs[0]?.name || '';

  const configQuery = useQuery({
    queryKey: ['config', activeConfig],
    queryFn: () => api.config(activeConfig),
    enabled: Boolean(activeConfig),
  });

  useEffect(() => {
    if (!selectedConfig && configs[0]?.name) setSelectedConfig(configs[0].name);
  }, [configs, selectedConfig]);

  const tunableSet = useMemo(
    () => new Set(configQuery.data?.tunable_keys ?? []),
    [configQuery.data?.tunable_keys],
  );

  const sections = useMemo(() => {
    const all = flattenConfig(configQuery.data?.config ?? {}).filter((entry) => tunableSet.has(entry.key));
    const needle = query.trim().toLowerCase();
    const filtered = needle
      ? all.filter((entry) => `${entry.path} ${entry.value}`.toLowerCase().includes(needle))
      : all;
    return filtered.reduce<Record<string, ConfigEntry[]>>((result, entry) => {
      (result[entry.section] ??= []).push(entry);
      return result;
    }, {});
  }, [configQuery.data?.config, query, tunableSet]);

  useEffect(() => {
    const names = Object.keys(sections);
    if (!names.length) return;
    setExpanded((current) => {
      if (Object.keys(current).length) return current;
      const preferred = names.find((name) => name === 'strategy.hot') ?? names[0];
      return { [preferred]: true };
    });
  }, [sections]);

  const saveMutation = useMutation({
    mutationFn: () => api.configWrite(activeConfig, changes),
    onSuccess: async (result) => {
      const rejected = Object.entries(result.rejected ?? {});
      if (rejected.length) {
        Alert.alert(
          'Some changes were rejected',
          rejected.map(([key, reason]) => `${key}: ${reason}`).join('\n'),
        );
      }
      const changedCount = result.changed?.length ?? 0;
      setChanges({});
      setNotice(`${changedCount} setting${changedCount === 1 ? '' : 's'} saved. Hot reload is active.`);
      await queryClient.invalidateQueries({ queryKey: ['config', activeConfig] });
      await queryClient.invalidateQueries({ queryKey: ['fleet'] });
    },
    onError: (error: Error) => Alert.alert('Save failed', error.message),
  });

  const chooseConfig = useCallback((name: string) => {
    const apply = () => {
      setSelectedConfig(name);
      setChanges({});
      setExpanded({});
      setNotice(null);
      setPickerOpen(false);
    };
    if (!Object.keys(changes).length) return apply();
    Alert.alert('Discard unsaved changes?', 'Switching configs will discard your edits.', [
      { text: 'Cancel', style: 'cancel' },
      { text: 'Discard', style: 'destructive', onPress: apply },
    ]);
  }, [changes]);

  const refresh = useCallback(() => {
    if (Object.keys(changes).length) {
      Alert.alert('Discard unsaved changes?', 'Reloading will discard your edits.', [
        { text: 'Cancel', style: 'cancel' },
        { text: 'Reload', style: 'destructive', onPress: () => { setChanges({}); configQuery.refetch(); } },
      ]);
      return;
    }
    configQuery.refetch();
  }, [changes, configQuery]);

  const hasChanges = Object.keys(changes).length > 0;
  const sectionCount = Object.keys(sections).length;
  const settingCount = Object.values(sections).reduce((sum, items) => sum + items.length, 0);

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <CockpitHeader
        title="Strategies"
        subtitle={`${settingCount} live settings · ${sectionCount} sections`}
        action={
          <TouchableOpacity style={styles.iconButton} onPress={refresh} accessibilityLabel="Reload config">
            <Ionicons name="refresh" size={20} color={colors.textDim} />
          </TouchableOpacity>
        }
      />

      <TouchableOpacity style={styles.configSelect} onPress={() => setPickerOpen(true)}>
        <View style={styles.configIcon}>
          <Ionicons name="document-text-outline" size={18} color={colors.accent} />
        </View>
        <View style={styles.configText}>
          <Text style={styles.configLabel}>ACTIVE CONFIG</Text>
          <Text style={styles.configName} numberOfLines={1}>
            {activeConfig ? `polypulse-${activeConfig}.toml` : 'No config available'}
          </Text>
        </View>
        <Ionicons name="chevron-down" size={18} color={colors.textMuted} />
      </TouchableOpacity>

      <View style={styles.toolbar}>
        <View style={styles.segmented}>
          {(['editor', 'raw'] as const).map((item) => (
            <TouchableOpacity
              key={item}
              style={[styles.segment, view === item && styles.segmentActive]}
              onPress={() => setView(item)}
            >
              <Text style={[styles.segmentText, view === item && styles.segmentTextActive]}>
                {item === 'editor' ? 'Settings' : 'Raw TOML'}
              </Text>
            </TouchableOpacity>
          ))}
        </View>
        {view === 'editor' && (
          <View style={styles.searchBox}>
            <Ionicons name="search" size={16} color={colors.textMuted} />
            <TextInput
              value={query}
              onChangeText={setQuery}
              placeholder="Find setting"
              placeholderTextColor={colors.textMuted}
              style={styles.searchInput}
              autoCapitalize="none"
              autoCorrect={false}
            />
            {query ? (
              <TouchableOpacity onPress={() => setQuery('')} accessibilityLabel="Clear search">
                <Ionicons name="close-circle" size={16} color={colors.textMuted} />
              </TouchableOpacity>
            ) : null}
          </View>
        )}
      </View>

      {notice && (
        <View style={styles.notice}>
          <Ionicons name="checkmark-circle" size={16} color={colors.green} />
          <Text style={styles.noticeText}>{notice}</Text>
          <TouchableOpacity onPress={() => setNotice(null)}><Ionicons name="close" size={16} color={colors.textMuted} /></TouchableOpacity>
        </View>
      )}

      <ScrollView
        style={styles.scroll}
        contentContainerStyle={styles.content}
        keyboardShouldPersistTaps="handled"
        refreshControl={(
          <RefreshControl
            refreshing={configQuery.isFetching && !configQuery.isLoading}
            onRefresh={refresh}
            tintColor={colors.accent}
          />
        )}
      >
        {configsQuery.error && <InlineError message={(configsQuery.error as Error).message} />}
        {configQuery.error && <InlineError message={(configQuery.error as Error).message} />}

        {view === 'raw' ? (
          configQuery.data?.raw ? (
            <ScrollView horizontal style={styles.rawPanel}>
              <Text selectable style={styles.rawText}>{configQuery.data.raw}</Text>
            </ScrollView>
          ) : (
            <EmptyState icon="lock-closed-outline" text="Raw TOML is restricted for this account." />
          )
        ) : (
          <>
            {Object.entries(sections).map(([section, entries]) => {
              const isOpen = Boolean(expanded[section]) || Boolean(query);
              const changedInSection = entries.filter((entry) => entry.path in changes).length;
              return (
                <View key={section} style={styles.section}>
                  <TouchableOpacity
                    style={styles.sectionHeader}
                    onPress={() => setExpanded((state) => ({ ...state, [section]: !isOpen }))}
                  >
                    <View style={styles.sectionHeading}>
                      <Text style={styles.sectionTitle}>{titleCase(section)}</Text>
                      <Text style={styles.sectionMeta}>
                        {entries.length} setting{entries.length === 1 ? '' : 's'}
                        {changedInSection ? ` · ${changedInSection} changed` : ''}
                      </Text>
                    </View>
                    <Ionicons name={isOpen ? 'chevron-up' : 'chevron-down'} size={18} color={colors.textMuted} />
                  </TouchableOpacity>
                  {isOpen && entries.map((entry) => (
                    <KnobRow
                      key={`${activeConfig}:${entry.path}`}
                      name={entry.key}
                      path={entry.path}
                      value={entry.path in changes ? changes[entry.path] : entry.value}
                      changed={entry.path in changes}
                      onChange={(value) => {
                        setNotice(null);
                        setChanges((state) => ({ ...state, [entry.path]: value }));
                      }}
                    />
                  ))}
                </View>
              );
            })}
            {!configQuery.isLoading && activeConfig && !settingCount && (
              <EmptyState icon="options-outline" text={query ? 'No matching settings.' : 'This config has no editable settings.'} />
            )}
          </>
        )}
      </ScrollView>

      <View style={styles.saveBar}>
        <View style={styles.saveSummary}>
          <Text style={[styles.saveCount, hasChanges && styles.saveCountDirty]}>
            {hasChanges ? `${Object.keys(changes).length} unsaved` : 'Config is up to date'}
          </Text>
          <Text style={styles.saveHint}>Changes hot reload in about 2 seconds</Text>
        </View>
        {hasChanges && (
          <TouchableOpacity style={styles.resetButton} onPress={() => setChanges({})} accessibilityLabel="Discard changes">
            <Ionicons name="arrow-undo" size={19} color={colors.textDim} />
          </TouchableOpacity>
        )}
        <TouchableOpacity
          style={[styles.saveButton, (!hasChanges || saveMutation.isPending) && styles.saveButtonDisabled]}
          disabled={!hasChanges || saveMutation.isPending}
          onPress={() => saveMutation.mutate()}
        >
          <Ionicons name={saveMutation.isPending ? 'sync' : 'flash'} size={17} color="#fff" />
          <Text style={styles.saveButtonText}>{saveMutation.isPending ? 'Saving' : 'Save & reload'}</Text>
        </TouchableOpacity>
      </View>

      <Modal visible={pickerOpen} transparent animationType="fade" onRequestClose={() => setPickerOpen(false)}>
        <TouchableOpacity style={styles.modalBackdrop} activeOpacity={1} onPress={() => setPickerOpen(false)}>
          <View style={styles.pickerPanel} onStartShouldSetResponder={() => true}>
            <View style={styles.pickerHeader}>
              <Text style={styles.pickerTitle}>Select config</Text>
              <TouchableOpacity style={styles.iconButton} onPress={() => setPickerOpen(false)}>
                <Ionicons name="close" size={21} color={colors.textDim} />
              </TouchableOpacity>
            </View>
            <ScrollView style={styles.pickerList}>
              {configs.map((config) => {
                const active = config.name === activeConfig;
                return (
                  <TouchableOpacity key={config.name} style={styles.pickerItem} onPress={() => chooseConfig(config.name)}>
                    <Ionicons name="document-text-outline" size={18} color={active ? colors.accent : colors.textMuted} />
                    <View style={styles.pickerItemText}>
                      <Text style={[styles.pickerName, active && styles.pickerNameActive]}>{config.file}</Text>
                      {config.mtime ? <Text style={styles.pickerMeta}>Updated {new Date(config.mtime).toLocaleString()}</Text> : null}
                    </View>
                    {active && <Ionicons name="checkmark" size={20} color={colors.accent} />}
                  </TouchableOpacity>
                );
              })}
            </ScrollView>
          </View>
        </TouchableOpacity>
      </Modal>
    </SafeAreaView>
  );
}

function KnobRow({
  name,
  path,
  value,
  changed,
  onChange,
}: {
  name: string;
  path: string;
  value: unknown;
  changed: boolean;
  onChange: (value: unknown) => void;
}) {
  const [draft, setDraft] = useState(Array.isArray(value) ? value.join(', ') : String(value ?? ''));
  useEffect(() => {
    if (!changed) setDraft(Array.isArray(value) ? value.join(', ') : String(value ?? ''));
  }, [changed, value]);

  const label = titleCase(name);
  const section = path.slice(0, Math.max(0, path.length - name.length - 1));
  if (typeof value === 'boolean') {
    return (
      <View style={[styles.knobRow, changed && styles.knobChanged]}>
        <View style={styles.knobText}>
          <Text style={styles.knobName}>{label}</Text>
          <Text style={styles.knobPath}>{section}</Text>
        </View>
        <Switch
          value={value}
          onValueChange={onChange}
          trackColor={{ false: colors.bgInput, true: colors.accentDim }}
          thumbColor={value ? '#fff' : colors.textMuted}
        />
      </View>
    );
  }

  const isArray = Array.isArray(value);
  const numeric = typeof value === 'number';
  return (
    <View style={[styles.knobRow, changed && styles.knobChanged]}>
      <View style={styles.knobText}>
        <Text style={styles.knobName}>{label}</Text>
        <Text style={styles.knobPath}>{isArray ? 'Comma-separated assets' : section}</Text>
      </View>
      <TextInput
        style={styles.knobInput}
        value={draft}
        onChangeText={(text) => {
          setDraft(text);
          if (isArray) onChange(text.split(',').map((item) => item.trim().toUpperCase()).filter(Boolean));
          else if (!numeric) onChange(text);
          else if (text.trim() !== '' && Number.isFinite(Number(text))) onChange(Number(text));
        }}
        onEndEditing={() => {
          if (numeric && (!draft.trim() || !Number.isFinite(Number(draft)))) {
            setDraft(String(value));
          }
        }}
        keyboardType={numeric ? 'decimal-pad' : 'default'}
        autoCapitalize={isArray ? 'characters' : 'none'}
        autoCorrect={false}
        selectTextOnFocus
      />
    </View>
  );
}

function InlineError({ message }: { message: string }) {
  return <View style={styles.error}><Ionicons name="alert-circle" size={17} color={colors.red} /><Text style={styles.errorText}>{message}</Text></View>;
}

function EmptyState({ icon, text }: { icon: keyof typeof Ionicons.glyphMap; text: string }) {
  return <View style={styles.empty}><Ionicons name={icon} size={28} color={colors.textMuted} /><Text style={styles.emptyText}>{text}</Text></View>;
}

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.bg },
  header: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: spacing.lg, paddingTop: spacing.md, paddingBottom: spacing.sm },
  title: { fontSize: fontSize.xl, fontWeight: '700', color: colors.text },
  subtitle: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 2 },
  iconButton: { width: 40, height: 40, alignItems: 'center', justifyContent: 'center' },
  configSelect: { minHeight: 58, marginHorizontal: spacing.lg, marginBottom: spacing.sm, paddingHorizontal: spacing.md, flexDirection: 'row', alignItems: 'center', borderWidth: 1, borderColor: colors.border, backgroundColor: colors.bgCard, borderRadius: 8 },
  configIcon: { width: 34, height: 34, alignItems: 'center', justifyContent: 'center', backgroundColor: colors.bgInput, borderRadius: 6, marginRight: spacing.sm },
  configText: { flex: 1 },
  configLabel: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  configName: { fontSize: fontSize.sm, color: colors.text, fontWeight: '600', marginTop: 2 },
  toolbar: { flexDirection: 'row', gap: spacing.sm, paddingHorizontal: spacing.lg, paddingBottom: spacing.sm },
  segmented: { flexDirection: 'row', padding: 2, backgroundColor: colors.bgInput, borderRadius: 6 },
  segment: { paddingHorizontal: spacing.md, height: 34, justifyContent: 'center', borderRadius: 4 },
  segmentActive: { backgroundColor: colors.bgCard },
  segmentText: { fontSize: fontSize.xs, color: colors.textMuted, fontWeight: '600' },
  segmentTextActive: { color: colors.text },
  searchBox: { flex: 1, height: 38, flexDirection: 'row', alignItems: 'center', gap: spacing.xs, paddingHorizontal: spacing.sm, borderWidth: 1, borderColor: colors.border, backgroundColor: colors.bgInput, borderRadius: 6 },
  searchInput: { flex: 1, color: colors.text, fontSize: fontSize.sm, paddingVertical: 0 },
  notice: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.lg, paddingVertical: spacing.sm, backgroundColor: '#082c24', borderTopWidth: 1, borderBottomWidth: 1, borderColor: '#135b48' },
  noticeText: { flex: 1, fontSize: fontSize.xs, color: colors.green },
  scroll: { flex: 1 },
  content: { paddingHorizontal: spacing.lg, paddingBottom: spacing.lg },
  section: { backgroundColor: colors.bgCard, borderWidth: 1, borderColor: colors.border, borderRadius: 8, marginBottom: spacing.sm, overflow: 'hidden' },
  sectionHeader: { minHeight: 58, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: spacing.md },
  sectionHeading: { flex: 1 },
  sectionTitle: { fontSize: fontSize.sm, color: colors.text, fontWeight: '700' },
  sectionMeta: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 2 },
  knobRow: { minHeight: 62, flexDirection: 'row', alignItems: 'center', gap: spacing.md, paddingHorizontal: spacing.md, paddingVertical: spacing.sm, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: colors.border },
  knobChanged: { backgroundColor: '#102840', borderLeftWidth: 3, borderLeftColor: colors.accent },
  knobText: { flex: 1, minWidth: 0 },
  knobName: { fontSize: fontSize.sm, color: colors.text },
  knobPath: { fontSize: 9, color: colors.textMuted, marginTop: 2 },
  knobInput: { width: 116, minHeight: 38, paddingHorizontal: spacing.sm, paddingVertical: spacing.xs, textAlign: 'right', fontSize: fontSize.sm, color: colors.text, backgroundColor: colors.bgInput, borderWidth: 1, borderColor: colors.border, borderRadius: 6 },
  rawPanel: { backgroundColor: '#070a10', borderWidth: 1, borderColor: colors.border, borderRadius: 6, padding: spacing.md },
  rawText: { color: '#aab6c8', fontFamily: 'monospace', fontSize: 11, lineHeight: 17 },
  saveBar: { minHeight: 68, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.lg, paddingVertical: spacing.sm, borderTopWidth: 1, borderTopColor: colors.border, backgroundColor: colors.bgCard },
  saveSummary: { flex: 1 },
  saveCount: { fontSize: fontSize.sm, color: colors.textDim, fontWeight: '600' },
  saveCountDirty: { color: colors.yellow },
  saveHint: { fontSize: 9, color: colors.textMuted, marginTop: 2 },
  resetButton: { width: 40, height: 40, alignItems: 'center', justifyContent: 'center', borderWidth: 1, borderColor: colors.border, borderRadius: 6 },
  saveButton: { height: 42, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: spacing.md, backgroundColor: colors.accent, borderRadius: 6 },
  saveButtonDisabled: { opacity: 0.38 },
  saveButtonText: { color: '#fff', fontSize: fontSize.xs, fontWeight: '700' },
  error: { flexDirection: 'row', gap: spacing.sm, padding: spacing.md, marginBottom: spacing.sm, borderWidth: 1, borderColor: colors.red, backgroundColor: colors.redDim, borderRadius: 6 },
  errorText: { flex: 1, color: colors.red, fontSize: fontSize.sm },
  empty: { alignItems: 'center', gap: spacing.sm, paddingVertical: 48 },
  emptyText: { color: colors.textMuted, fontSize: fontSize.sm, textAlign: 'center' },
  modalBackdrop: { flex: 1, justifyContent: 'flex-end', backgroundColor: 'rgba(0,0,0,0.72)' },
  pickerPanel: { maxHeight: '72%', backgroundColor: colors.bgCard, borderTopWidth: 1, borderColor: colors.border, paddingBottom: spacing.xxl },
  pickerHeader: { height: 58, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingLeft: spacing.lg, borderBottomWidth: 1, borderBottomColor: colors.border },
  pickerTitle: { fontSize: fontSize.lg, fontWeight: '700', color: colors.text },
  pickerList: { flexGrow: 0 },
  pickerItem: { minHeight: 58, flexDirection: 'row', alignItems: 'center', gap: spacing.md, paddingHorizontal: spacing.lg, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  pickerItemText: { flex: 1 },
  pickerName: { fontSize: fontSize.sm, color: colors.textDim },
  pickerNameActive: { color: colors.text, fontWeight: '700' },
  pickerMeta: { fontSize: 9, color: colors.textMuted, marginTop: 2 },
});
