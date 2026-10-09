import { useEffect, useMemo, useState } from 'react';
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
import { router } from 'expo-router';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Ionicons } from '@expo/vector-icons';
import { api } from '../../src/api/endpoints';
import { useAuthStore } from '../../src/state/AuthContext';
import { useAppStore } from '../../src/state/store';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';
import { StatusBadge } from '../../src/components/StatusBadge';
import { CockpitHeader } from '../../src/components/CockpitHeader';
import { fmtAgo, fmtUsd, pnlColor } from '../../src/utils/format';
import { POLL_INTERVALS } from '../../src/utils/constants';
import { colors, fontSize, spacing } from '../../src/utils/theme';
import type { FleetInstance, FleetResponse, TournamentInstance } from '../../src/api/types';

export default function FleetTab() {
  return <ErrorBoundary><FleetContent /></ErrorBoundary>;
}

function FleetContent() {
  const user = useAuthStore((state) => state.user);
  const isAdmin = user?.role === 'admin';
  const readOnly = user?.role === 'demo';
  const setSelectedInstance = useAppStore((state) => state.setSelectedInstance);
  const queryClient = useQueryClient();
  const [managed, setManaged] = useState<FleetInstance | null>(null);
  const [launchOpen, setLaunchOpen] = useState(false);

  const fleetQuery = useQuery({
    queryKey: ['fleet'],
    queryFn: api.fleet,
    refetchInterval: POLL_INTERVALS.FLEET,
  });
  const pnlQuery = useQuery({ queryKey: ['pnl_by_instance'], queryFn: api.pnlByInstance, refetchInterval: POLL_INTERVALS.FLEET });
  const liveQuery = useQuery({ queryKey: ['live_lite', 'fleet'], queryFn: () => api.liveLite(), refetchInterval: POLL_INTERVALS.FLEET });

  const data = fleetQuery.data;
  const instances = data?.fleet ?? [];
  const tournament = useMemo(() => Object.fromEntries(
    (liveQuery.data?.tournament?.instances ?? []).map((item) => [item.instance, item]),
  ), [liveQuery.data?.tournament?.instances]);
  const running = instances.filter((instance) => instance.running).length;

  const refreshAll = async () => {
    await Promise.all([fleetQuery.refetch(), pnlQuery.refetch(), liveQuery.refetch()]);
  };

  useEffect(() => {
    if (!managed) return;
    const fresh = instances.find((item) => item.instance === managed.instance);
    if (fresh && fresh !== managed) setManaged(fresh);
  }, [instances, managed]);

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <CockpitHeader
        title="Fleet"
        subtitle={`${running} running · ${instances.length} configured`}
        action={(
          <View style={styles.headerActions}>
            <TouchableOpacity style={styles.ordersButton} onPress={() => router.push({ pathname: '/(tabs)/positions', params: { view: 'orders' } })} accessibilityLabel="Open order history">
              <Ionicons name="receipt-outline" size={18} color={colors.accent} />
            </TouchableOpacity>
            {isAdmin ? <TouchableOpacity style={styles.addButton} onPress={() => setLaunchOpen(true)} accessibilityLabel="Create bot"><Ionicons name="add" size={23} color="#fff" /></TouchableOpacity> : null}
          </View>
        )}
      />

      <ScrollView
        style={styles.scroll}
        contentContainerStyle={styles.content}
        refreshControl={<RefreshControl refreshing={fleetQuery.isFetching} onRefresh={refreshAll} tintColor={colors.accent} />}
      >
        {fleetQuery.error && <ErrorPanel message={(fleetQuery.error as Error).message} />}
        {instances.map((instance) => {
          const points = pnlQuery.data?.instances?.[instance.instance]?.points ?? [];
          const pnl = points.at(-1)?.pnl ?? tournament[instance.instance]?.net_pnl ?? 0;
          return (
            <FleetCard
              key={instance.instance}
              instance={instance}
              pnl={pnl}
              stats={tournament[instance.instance]}
              readOnly={readOnly}
              onManage={() => setManaged(instance)}
              onOpen={() => { setSelectedInstance(instance.instance); router.push('/(tabs)'); }}
            />
          );
        })}
        {!fleetQuery.isLoading && !instances.length && <EmptyFleet />}
      </ScrollView>

      {managed && data && (
        <ManageBotModal
          instance={managed}
          fleet={data}
          isAdmin={isAdmin}
          readOnly={readOnly}
          onClose={() => setManaged(null)}
          onChanged={() => queryClient.invalidateQueries({ queryKey: ['fleet'] })}
        />
      )}
      {data && (
        <LaunchBotModal
          visible={launchOpen}
          templates={data.fleet}
          onClose={() => setLaunchOpen(false)}
          onCreated={() => {
            setLaunchOpen(false);
            queryClient.invalidateQueries({ queryKey: ['fleet'] });
            queryClient.invalidateQueries({ queryKey: ['runtime_fleet'] });
            queryClient.invalidateQueries({ queryKey: ['wallet_shared'] });
          }}
        />
      )}
    </SafeAreaView>
  );
}

function FleetCard({
  instance,
  pnl,
  stats,
  readOnly,
  onManage,
  onOpen,
}: {
  instance: FleetInstance;
  pnl: number;
  stats?: TournamentInstance;
  readOnly: boolean;
  onManage: () => void;
  onOpen: () => void;
}) {
  const queryClient = useQueryClient();
  const control = useMutation({
    mutationFn: (action: string) => api.botControl(action, instance.instance,
      instance.wallet.mode === 'live' && (action === 'start' || action === 'restart') ? { confirm_live: true } : undefined),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['fleet'] }),
    onError: (error: Error) => Alert.alert('Bot control failed', error.message),
  });
  const strategy = instance.strategy ?? { signals: [], exits: [], blocked: [] };
  const walletReady = instance.wallet.api_key_set && instance.wallet.api_secret_set &&
    instance.wallet.api_passphrase_set && instance.wallet.private_key_exists;
  const chips = [
    strategy.band ? `Band ${strategy.band}` : null,
    strategy.per_entry_usd ? `${fmtUsd(strategy.per_entry_usd)}/entry` : null,
    ...(strategy.signals ?? []),
    ...(strategy.exits ?? []),
  ].filter(Boolean) as string[];

  const requestControl = (action: string) => {
    const title = `${action[0].toUpperCase()}${action.slice(1)} ${instance.instance}`;
    const liveStart = instance.wallet.mode === 'live' && (action === 'start' || action === 'restart');
    const detail = liveStart
      ? `This ${action} will run ${instance.instance} in verified LIVE mode using real-money execution. Check its open positions and orders before continuing.`
      : `Confirm ${action} for ${instance.instance}?`;
    Alert.alert(liveStart ? `Confirm LIVE ${action}` : title, detail, [
      { text: 'Cancel', style: 'cancel' },
      { text: liveStart ? `${titleCase(action)} LIVE` : titleCase(action), style: action === 'stop' || liveStart ? 'destructive' : 'default', onPress: () => control.mutate(action) },
    ]);
  };

  return (
    <View style={[styles.card, instance.running && styles.cardRunning]}>
      <TouchableOpacity style={styles.cardHeader} onPress={onOpen}>
        <View style={[styles.statusDot, { backgroundColor: instance.running ? colors.green : colors.textMuted }]} />
        <View style={styles.cardIdentity}>
          <Text style={styles.botName}>{instance.name || instance.instance}</Text>
          <Text style={styles.botId}>{instance.instance}</Text>
        </View>
        <StatusBadge
          label={instance.wallet.mode}
          color={instance.wallet.mode === 'live' ? colors.red : colors.yellow}
          bgColor={instance.wallet.mode === 'live' ? colors.redDim : '#3d3000'}
        />
        <Ionicons name="chevron-forward" size={17} color={colors.textMuted} />
      </TouchableOpacity>

      <View style={styles.statsRow}>
        <Stat label="NET P&L" value={fmtUsd(pnl)} color={pnlColor(pnl)} />
        <Stat label="W / L" value={`${stats?.wins ?? 0} / ${stats?.losses ?? 0}`} />
        <Stat label="FILLS" value={String(stats?.fills ?? 0)} />
      </View>

      <View style={styles.metaRows}>
        <MetaRow icon="document-text-outline" label="Config" value={instance.current_config} />
        <MetaRow
          icon="wallet-outline"
          label="Wallet"
          value={walletReady ? 'Ready' : 'Needs setup'}
          valueColor={walletReady ? colors.green : colors.yellow}
        />
        <MetaRow
          icon="sparkles-outline"
          label="Supervisor"
          value={instance.supervisor.running ? (instance.supervisor.model || 'Running') : 'Stopped'}
          valueColor={instance.supervisor.running ? colors.accent : colors.textMuted}
        />
      </View>

      {!!chips.length && (
        <ScrollView horizontal showsHorizontalScrollIndicator={false} style={styles.chipScroll}>
          {chips.map((chip, index) => <View key={`${chip}-${index}`} style={styles.chip}><Text style={styles.chipText}>{chip}</Text></View>)}
        </ScrollView>
      )}

      {!readOnly && (
        <View style={styles.cardActions}>
          <IconAction
            icon={instance.running ? 'stop' : 'play'}
            label={instance.running ? 'Stop' : 'Start'}
            color={instance.running ? colors.red : colors.green}
            disabled={control.isPending}
            onPress={() => requestControl(instance.running ? 'stop' : 'start')}
          />
          <IconAction icon="refresh" label="Restart" color={colors.yellow} disabled={control.isPending} onPress={() => requestControl('restart')} />
          <TouchableOpacity style={styles.manageButton} onPress={onManage}>
            <Ionicons name="settings-outline" size={17} color={colors.text} />
            <Text style={styles.manageText}>Manage</Text>
          </TouchableOpacity>
        </View>
      )}
    </View>
  );
}

function ManageBotModal({
  instance,
  fleet,
  isAdmin,
  readOnly,
  onClose,
  onChanged,
}: {
  instance: FleetInstance;
  fleet: FleetResponse;
  isAdmin: boolean;
  readOnly: boolean;
  onClose: () => void;
  onChanged: () => void;
}) {
  const queryClient = useQueryClient();
  const [configName, setConfigName] = useState(instance.current_config);
  const [model, setModel] = useState(instance.supervisor.model || fleet.allowed_models[0] || '');
  const [apiKey, setApiKey] = useState('');
  const [apiSecret, setApiSecret] = useState('');
  const [apiPassphrase, setApiPassphrase] = useState('');
  const [privateKey, setPrivateKey] = useState('');
  const [balance, setBalance] = useState(String(instance.wallet.starting_balance_usd ?? ''));
  const [liveAck, setLiveAck] = useState(false);
  const [liveText, setLiveText] = useState('');

  const history = useQuery({
    queryKey: ['strategy_history', instance.instance],
    queryFn: () => api.strategyHistory(instance.instance),
    enabled: !readOnly,
  });

  const operation = useMutation({
    mutationFn: async ({ kind, value }: { kind: string; value?: string | number }) => {
      if (kind === 'swap') return api.botControl('swap_config', instance.instance, {
        config_name: String(value), ...(instance.wallet.mode === 'live' ? { confirm_live: true } : {}),
      });
      if (kind === 'model') return api.supervisorModel(instance.instance, String(value));
      if (kind === 'supervisor') return api.supervisorControl(String(value), instance.instance);
      if (kind === 'recall') return api.botControl('recall_strategy', instance.instance, { strategy_id: Number(value) });
      if (kind === 'paper') return isAdmin ? api.walletMode(instance.instance, 'paper') : api.switchPaper(instance.instance);
      if (kind === 'live') return api.walletMode(instance.instance, 'live', true);
      if (kind === 'delete') return api.botControl('delete', instance.instance);
      throw new Error('Unknown operation');
    },
    onSuccess: () => { onChanged(); queryClient.invalidateQueries({ queryKey: ['wallet_shared'] }); queryClient.invalidateQueries({ queryKey: ['runtime_fleet'] }); Alert.alert('Updated', `${instance.instance} was updated successfully.`); },
    onError: (error: Error) => Alert.alert('Update failed', error.message),
  });

  const sharedWallet = useQuery({
    queryKey: ['wallet_shared'],
    queryFn: api.walletShared,
    enabled: isAdmin,
  });

  const walletSave = useMutation({
    mutationFn: () => {
      const updates: Record<string, unknown> = {};
      if (apiKey.trim()) updates.api_key = apiKey.trim();
      if (apiSecret.trim()) updates.api_secret = apiSecret.trim();
      if (apiPassphrase.trim()) updates.api_passphrase = apiPassphrase.trim();
      if (isAdmin && balance.trim()) updates.starting_balance_usd = Number(balance);
      if (!Object.keys(updates).length) throw new Error('Enter at least one wallet value.');
      return api.walletWrite(instance.instance, updates);
    },
    onSuccess: () => { setApiKey(''); setApiSecret(''); setApiPassphrase(''); onChanged(); Alert.alert('Wallet saved', 'Credential status will refresh shortly.'); },
    onError: (error: Error) => Alert.alert('Wallet save failed', error.message),
  });

  const goLive = useMutation({
    mutationFn: () => api.goLive(instance.instance, {
      private_key: privateKey.trim(), api_key: apiKey.trim(), api_secret: apiSecret.trim(), api_passphrase: apiPassphrase.trim(),
    }),
    onSuccess: () => {
      setPrivateKey(''); setLiveAck(false); setLiveText(''); onChanged(); Alert.alert('Live mode enabled', `${instance.instance} restarted in live mode.`);
    },
    onError: (error: Error) => Alert.alert('Go live failed', error.message),
  });

  const confirmLive = () => {
    if (!privateKey.trim()) return Alert.alert('Private key required', 'Enter the wallet private key for this bot.');
    if (!liveAck || liveText.trim().toUpperCase() !== 'LIVE') return Alert.alert('Confirmation required', 'Acknowledge the warning and type LIVE.');
    Alert.alert('Enable live trading?', 'This writes credentials and restarts the bot with real-money execution.', [
      { text: 'Cancel', style: 'cancel' },
      { text: 'Enable live', style: 'destructive', onPress: () => goLive.mutate() },
    ]);
  };

  const protectedBot = ['cb', 'scalp', 'conv'].includes(instance.instance);

  return (
    <Modal visible animationType="slide" onRequestClose={onClose}>
      <SafeAreaView style={styles.modalScreen} edges={['top', 'bottom']}>
        <View style={styles.sheetHeader}>
          <View style={styles.sheetTitleRow}>
            <View style={[styles.statusDot, { backgroundColor: instance.running ? colors.green : colors.textMuted }]} />
            <View><Text style={styles.sheetTitle}>{instance.name || instance.instance}</Text><Text style={styles.sheetSubtitle}>{instance.instance} · {instance.running ? 'running' : 'stopped'}</Text></View>
          </View>
          <TouchableOpacity style={styles.iconButton} onPress={onClose}><Ionicons name="close" size={23} color={colors.textDim} /></TouchableOpacity>
        </View>

        <ScrollView style={styles.scroll} contentContainerStyle={styles.sheetContent} keyboardShouldPersistTaps="handled">
          {isAdmin && (
            <ManageSection icon="document-text-outline" title="Strategy config" subtitle="Swapping restarts a running bot">
              <ChoiceList
                options={fleet.available_configs.map((item) => ({ value: item.name, label: item.name }))}
                value={configName}
                onChange={setConfigName}
              />
              <PrimaryButton
                label="Apply config"
                icon="swap-horizontal"
                disabled={configName === instance.current_config || operation.isPending}
                onPress={() => Alert.alert(instance.wallet.mode === 'live' ? 'Swap LIVE bot config?' : 'Swap config?', `${instance.instance} will use ${configName} and will keep its ${instance.wallet.mode.toUpperCase()} mode.${instance.wallet.mode === 'live' ? ' The running bot will restart in LIVE mode and may place real-money orders.' : ''}`, [
                  { text: 'Cancel', style: 'cancel' },
                  { text: instance.wallet.mode === 'live' ? 'Apply LIVE config' : 'Apply', style: instance.wallet.mode === 'live' ? 'destructive' : 'default', onPress: () => operation.mutate({ kind: 'swap', value: configName }) },
                ])}
              />
            </ManageSection>
          )}

          {!readOnly && (
            <ManageSection icon="sparkles-outline" title="AI supervisor" subtitle={instance.supervisor.running ? 'Running' : 'Stopped'}>
              {!!fleet.allowed_models.length && <ChoiceList options={fleet.allowed_models.map((item) => ({ value: item, label: item.replace(/:free$/, '') }))} value={model} onChange={setModel} />}
              <View style={styles.inlineActions}>
                <SecondaryButton
                  label="Set model"
                  icon="checkmark"
                  disabled={!model || model === instance.supervisor.model || operation.isPending}
                  onPress={() => operation.mutate({ kind: 'model', value: model })}
                />
                <SecondaryButton
                  label={instance.supervisor.running ? 'Stop supervisor' : 'Start supervisor'}
                  icon={instance.supervisor.running ? 'stop' : 'play'}
                  onPress={() => operation.mutate({ kind: 'supervisor', value: instance.supervisor.running ? 'stop' : 'start' })}
                />
              </View>
              {instance.supervisor.last_decision?.reason ? (
                <View style={styles.decision}><Text style={styles.decisionLabel}>LAST DECISION</Text><Text style={styles.decisionText}>{instance.supervisor.last_decision.reason}</Text></View>
              ) : null}
            </ManageSection>
          )}

          {!readOnly && !!instance.recent_strategies?.length && (
            <ManageSection icon="time-outline" title="Strategy history" subtitle={`${instance.supervisor.strategies_total} saved decisions`}>
              {(history.data?.history ?? []).slice(0, 8).map((item) => (
                <View key={`${item.strategy_id}-${item.created_at}`} style={styles.historyRow}>
                  <View style={styles.historyMain}>
                    <Text style={styles.historyTitle}>#{item.strategy_id} · {fmtAgo(item.created_at)}</Text>
                    <Text style={styles.historyReason} numberOfLines={2}>{item.reason || 'No decision note'}</Text>
                    <Text style={[styles.historyStats, { color: pnlColor(item.net_pnl) }]}>{item.fills ?? 0} fills · {item.wins ?? 0}/{item.losses ?? 0} · {fmtUsd(item.net_pnl)}</Text>
                  </View>
                  <TouchableOpacity style={styles.recallButton} onPress={() => operation.mutate({ kind: 'recall', value: item.strategy_id })}>
                    <Ionicons name="arrow-undo" size={17} color={colors.accent} />
                  </TouchableOpacity>
                </View>
              ))}
            </ManageSection>
          )}

          {!readOnly && (
            <ManageSection icon="wallet-outline" title="Wallet & execution" subtitle={`Configured balance ${fmtUsd(instance.wallet.starting_balance_usd)}`}>
              {isAdmin ? (
                <>
                  <View style={styles.sharedWalletNotice}>
                    <Ionicons name={sharedWallet.data?.credentials_ready ? 'shield-checkmark-outline' : 'alert-circle-outline'} size={17} color={sharedWallet.data?.credentials_ready ? colors.green : colors.yellow} />
                    <Text style={styles.sharedWalletNoticeText}>{sharedWallet.data?.credentials_ready ? 'Shared ADMIN wallet ready · credentials apply to every ADMIN bot.' : 'ADMIN credentials are managed once for the whole fleet.'}</Text>
                  </View>
                  <SecondaryButton label="Configure shared wallet" icon="key-outline" onPress={() => router.push('/(tabs)/wallet')} />
                  <FieldInput label="Configured balance (USD)" value={balance} onChangeText={setBalance} keyboardType="decimal-pad" />
                  <SecondaryButton label="Save bot balance" icon="save-outline" disabled={walletSave.isPending} onPress={() => walletSave.mutate()} />
                  <View style={styles.divider} />
                  {instance.wallet.mode === 'live' ? (
                    <TouchableOpacity style={styles.paperButton} onPress={() => Alert.alert('Return this bot to PAPER?', `${instance.instance} will restart without real-money execution. Other bots keep their current modes.`, [
                      { text: 'Cancel', style: 'cancel' },
                      { text: 'Switch to PAPER', onPress: () => operation.mutate({ kind: 'paper' }) },
                    ])}>
                      <Ionicons name="document-outline" size={18} color={colors.yellow} /><Text style={styles.paperText}>Switch this bot to PAPER</Text>
                    </TouchableOpacity>
                  ) : (
                    <TouchableOpacity style={styles.liveButton} disabled={!sharedWallet.data?.credentials_ready || operation.isPending} onPress={() => Alert.alert('Enable LIVE for this bot?', `Only ${instance.instance} changes mode. It will use the shared ADMIN wallet and can place real-money trades. ${instance.running ? 'The running bot will restart.' : 'The next start will use LIVE mode.'}`, [
                      { text: 'Cancel', style: 'cancel' },
                      { text: 'Enable LIVE', style: 'destructive', onPress: () => operation.mutate({ kind: 'live' }) },
                    ])}>
                      <Ionicons name="flash" size={18} color="#fff" /><Text style={styles.liveButtonText}>{sharedWallet.data?.credentials_ready ? 'Enable LIVE for this bot' : 'Set up shared wallet first'}</Text>
                    </TouchableOpacity>
                  )}
                </>
              ) : (
                <>
                  <View style={styles.walletStatusGrid}>
                    <WalletCheck label="API key" ok={instance.wallet.api_key_set} />
                    <WalletCheck label="Secret" ok={instance.wallet.api_secret_set} />
                    <WalletCheck label="Passphrase" ok={instance.wallet.api_passphrase_set} />
                    <WalletCheck label="Private key" ok={instance.wallet.private_key_exists} />
                  </View>
                  <SecretInput label="API key" value={apiKey} onChangeText={setApiKey} />
                  <SecretInput label="API secret" value={apiSecret} onChangeText={setApiSecret} />
                  <SecretInput label="API passphrase" value={apiPassphrase} onChangeText={setApiPassphrase} />
                  <SecondaryButton label="Save wallet settings" icon="save-outline" disabled={walletSave.isPending} onPress={() => walletSave.mutate()} />
                  <View style={styles.divider} />
                  {instance.wallet.mode === 'live' ? (
                    <TouchableOpacity style={styles.paperButton} onPress={() => Alert.alert('Switch to paper?', 'The bot will restart without real-money execution.', [
                      { text: 'Cancel', style: 'cancel' },
                      { text: 'Switch to paper', onPress: () => operation.mutate({ kind: 'paper' }) },
                    ])}>
                      <Ionicons name="document-outline" size={18} color={colors.yellow} /><Text style={styles.paperText}>Switch to paper mode</Text>
                    </TouchableOpacity>
                  ) : (
                    <>
                      <SecretInput label="Private key" value={privateKey} onChangeText={setPrivateKey} />
                      <View style={styles.ackRow}><Switch value={liveAck} onValueChange={setLiveAck} trackColor={{ false: colors.bgInput, true: colors.redDim }} thumbColor={liveAck ? colors.red : colors.textMuted} /><Text style={styles.ackText}>I understand this enables real-money trading.</Text></View>
                      <FieldInput label="Type LIVE to confirm" value={liveText} onChangeText={setLiveText} autoCapitalize="characters" />
                      <TouchableOpacity style={styles.liveButton} disabled={goLive.isPending} onPress={confirmLive}>
                        <Ionicons name="flash" size={18} color="#fff" /><Text style={styles.liveButtonText}>Save and enable live</Text>
                      </TouchableOpacity>
                    </>
                  )}
                </>
              )}
            </ManageSection>
          )}

          {isAdmin && !protectedBot && (
            <ManageSection icon="warning-outline" title="Danger zone">
              <TouchableOpacity style={styles.deleteButton} onPress={() => Alert.alert('Delete bot?', 'The bot and supervisor stop and the config is archived.', [
                { text: 'Cancel', style: 'cancel' },
                { text: 'Delete', style: 'destructive', onPress: () => operation.mutate({ kind: 'delete' }, { onSuccess: onClose }) },
              ])}>
                <Ionicons name="trash-outline" size={18} color={colors.red} /><Text style={styles.deleteText}>Delete {instance.instance}</Text>
              </TouchableOpacity>
            </ManageSection>
          )}
        </ScrollView>
      </SafeAreaView>
    </Modal>
  );
}

function LaunchBotModal({ visible, templates, onClose, onCreated }: { visible: boolean; templates: FleetInstance[]; onClose: () => void; onCreated: () => void }) {
  const [name, setName] = useState('');
  const [template, setTemplate] = useState('');
  const [execution, setExecution] = useState<'maker' | 'taker' | 'hybrid'>('maker');
  useEffect(() => { if (visible && !templates.some((item) => item.instance === template)) setTemplate(templates[0]?.instance || ''); }, [templates, template, visible]);
  const create = useMutation({
    mutationFn: async () => {
      if (!template) throw new Error('Select a bot profile to clone.');
      const instance = (name.trim() || `${template}-copy`).toLowerCase();
      if (!/^[a-z0-9][\w-]*$/.test(instance)) throw new Error('Use letters, numbers, dashes, or underscores for the instance name.');
      return api.fleetCreate(instance, template, execution);
    },
    onSuccess: (response) => { setName(''); onCreated(); Alert.alert('PAPER bot created', response.message || `${response.instance} was created stopped in PAPER mode with entries disabled.`); },
    onError: (error: Error) => Alert.alert('Bot creation failed', error.message),
  });

  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={onClose}>
      <View style={styles.modalBackdrop}>
        <View style={styles.launchPanel}>
          <View style={styles.pickerHeader}><Text style={styles.sheetTitle}>Create a PAPER bot</Text><TouchableOpacity style={styles.iconButton} onPress={onClose}><Ionicons name="close" size={22} color={colors.textDim} /></TouchableOpacity></View>
          <View style={styles.launchContent}>
            <Text style={styles.manageSectionSubtitle}>Creates the new bot stopped in PAPER mode with entries disabled. Review it in Fleet before starting.</Text>
            <FieldInput label="Instance name (optional)" value={name} onChangeText={setName} autoCapitalize="none" placeholder={template ? `${template}-copy` : 'Choose a profile first'} />
            <Text style={styles.fieldLabel}>CLONE BOT PROFILE</Text>
            <ChoiceList options={templates.map((item) => ({ value: item.instance, label: item.instance }))} value={template} onChange={setTemplate} />
            <Text style={styles.fieldLabel}>ENTRY EXECUTION</Text>
            <ChoiceList options={[{ value: 'maker', label: 'MAKER' }, { value: 'taker', label: 'TAKER' }, { value: 'hybrid', label: 'HYBRID' }]} value={execution} onChange={(value) => setExecution(value as 'maker' | 'taker' | 'hybrid')} />
            <Text style={styles.manageSectionSubtitle}>The shared ADMIN wallet reference is attached automatically. Credential setup and LIVE activation remain separate.</Text>
            <PrimaryButton label={create.isPending ? 'Creating…' : 'Create stopped PAPER bot'} icon="add-circle-outline" disabled={create.isPending || !template} onPress={() => create.mutate()} />
          </View>
        </View>
      </View>
    </Modal>
  );
}

function ManageSection({ icon, title, subtitle, children }: { icon: keyof typeof Ionicons.glyphMap; title: string; subtitle?: string; children: React.ReactNode }) {
  return <View style={styles.manageSection}><View style={styles.manageSectionHeader}><Ionicons name={icon} size={19} color={colors.accent} /><View><Text style={styles.manageSectionTitle}>{title}</Text>{subtitle ? <Text style={styles.manageSectionSubtitle}>{subtitle}</Text> : null}</View></View>{children}</View>;
}

function ChoiceList({ options, value, onChange }: { options: Array<{ value: string; label: string }>; value: string; onChange: (value: string) => void }) {
  return <ScrollView horizontal showsHorizontalScrollIndicator={false} style={styles.choiceScroll}>{options.map((option) => <TouchableOpacity key={option.value} style={[styles.choice, value === option.value && styles.choiceActive]} onPress={() => onChange(option.value)}><Text style={[styles.choiceText, value === option.value && styles.choiceTextActive]} numberOfLines={1}>{option.label}</Text></TouchableOpacity>)}</ScrollView>;
}

function Stat({ label, value, color }: { label: string; value: string; color?: string }) { return <View style={styles.stat}><Text style={styles.statLabel}>{label}</Text><Text style={[styles.statValue, color ? { color } : null]} numberOfLines={1}>{value}</Text></View>; }
function MetaRow({ icon, label, value, valueColor }: { icon: keyof typeof Ionicons.glyphMap; label: string; value: string; valueColor?: string }) { return <View style={styles.metaRow}><Ionicons name={icon} size={14} color={colors.textMuted} /><Text style={styles.metaLabel}>{label}</Text><Text style={[styles.metaValue, valueColor ? { color: valueColor } : null]} numberOfLines={1}>{value}</Text></View>; }
function IconAction({ icon, label, color, disabled, onPress }: { icon: keyof typeof Ionicons.glyphMap; label: string; color: string; disabled?: boolean; onPress: () => void }) { return <TouchableOpacity style={[styles.iconAction, disabled && styles.disabled]} disabled={disabled} onPress={onPress}><Ionicons name={icon} size={17} color={color} /><Text style={[styles.iconActionText, { color }]}>{label}</Text></TouchableOpacity>; }
function PrimaryButton({ label, icon, disabled, onPress }: { label: string; icon: keyof typeof Ionicons.glyphMap; disabled?: boolean; onPress: () => void }) { return <TouchableOpacity style={[styles.primaryButton, disabled && styles.disabled]} disabled={disabled} onPress={onPress}><Ionicons name={icon} size={18} color="#fff" /><Text style={styles.primaryButtonText}>{label}</Text></TouchableOpacity>; }
function SecondaryButton({ label, icon, disabled, onPress }: { label: string; icon: keyof typeof Ionicons.glyphMap; disabled?: boolean; onPress: () => void }) { return <TouchableOpacity style={[styles.secondaryButton, disabled && styles.disabled]} disabled={disabled} onPress={onPress}><Ionicons name={icon} size={17} color={colors.text} /><Text style={styles.secondaryButtonText}>{label}</Text></TouchableOpacity>; }
function WalletCheck({ label, ok }: { label: string; ok: boolean }) { return <View style={styles.walletCheck}><Ionicons name={ok ? 'checkmark-circle' : 'alert-circle'} size={16} color={ok ? colors.green : colors.yellow} /><Text style={styles.walletCheckText}>{label}</Text></View>; }
function FieldInput(props: React.ComponentProps<typeof TextInput> & { label: string }) { const { label, ...inputProps } = props; return <View style={styles.field}><Text style={styles.fieldLabel}>{label.toUpperCase()}</Text><TextInput {...inputProps} style={styles.fieldInput} placeholderTextColor={colors.textMuted} /></View>; }
function SecretInput(props: { label: string; value: string; onChangeText: (value: string) => void }) { const [visible, setVisible] = useState(false); return <View style={styles.field}><Text style={styles.fieldLabel}>{props.label.toUpperCase()}</Text><View style={styles.secretWrap}><TextInput value={props.value} onChangeText={props.onChangeText} style={styles.secretInput} secureTextEntry={!visible} autoCapitalize="none" autoCorrect={false} placeholder="Leave blank to keep current" placeholderTextColor={colors.textMuted} /><TouchableOpacity style={styles.secretToggle} onPress={() => setVisible((value) => !value)}><Ionicons name={visible ? 'eye-off-outline' : 'eye-outline'} size={18} color={colors.textMuted} /></TouchableOpacity></View></View>; }
function ErrorPanel({ message }: { message: string }) { return <View style={styles.errorPanel}><Ionicons name="alert-circle" size={18} color={colors.red} /><Text style={styles.errorText}>{message}</Text></View>; }
function EmptyFleet() { return <View style={styles.empty}><Ionicons name="grid-outline" size={32} color={colors.textMuted} /><Text style={styles.emptyTitle}>No configured bots</Text><Text style={styles.emptyText}>Launch a bot from an existing config.</Text></View>; }
function titleCase(value: string) { return value.charAt(0).toUpperCase() + value.slice(1); }

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.bg },
  headerActions: { flexDirection: 'row', alignItems: 'center', gap: spacing.xs },
  ordersButton: { width: 40, height: 40, alignItems: 'center', justifyContent: 'center', borderRadius: 8, borderWidth: 1, borderColor: colors.border, backgroundColor: colors.bgCard },
  header: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: spacing.lg, paddingVertical: spacing.md },
  title: { fontSize: fontSize.xl, fontWeight: '700', color: colors.text },
  subtitle: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 2 },
  addButton: { width: 40, height: 40, alignItems: 'center', justifyContent: 'center', borderRadius: 8, backgroundColor: colors.accent },
  scroll: { flex: 1 },
  content: { paddingHorizontal: spacing.lg, paddingBottom: spacing.xxl },
  card: { backgroundColor: colors.bgCard, borderWidth: 1, borderColor: colors.border, borderRadius: 8, marginBottom: spacing.md, overflow: 'hidden' },
  cardRunning: { borderLeftWidth: 3, borderLeftColor: colors.green },
  cardHeader: { minHeight: 58, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md },
  statusDot: { width: 8, height: 8, borderRadius: 4 },
  cardIdentity: { flex: 1 },
  botName: { fontSize: fontSize.md, fontWeight: '700', color: colors.text },
  botId: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 1 },
  statsRow: { flexDirection: 'row', borderTopWidth: 1, borderBottomWidth: 1, borderColor: colors.border, backgroundColor: '#0e1420' },
  stat: { flex: 1, minWidth: 0, paddingHorizontal: spacing.sm, paddingVertical: spacing.md, borderRightWidth: StyleSheet.hairlineWidth, borderRightColor: colors.border },
  statLabel: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  statValue: { fontSize: fontSize.md, color: colors.text, fontWeight: '700', marginTop: 3 },
  metaRows: { paddingHorizontal: spacing.md, paddingTop: spacing.sm },
  metaRow: { minHeight: 27, flexDirection: 'row', alignItems: 'center', gap: spacing.sm },
  metaLabel: { width: 65, fontSize: fontSize.xs, color: colors.textMuted },
  metaValue: { flex: 1, fontSize: fontSize.xs, color: colors.text, textAlign: 'right' },
  chipScroll: { flexGrow: 0, marginHorizontal: spacing.md, marginVertical: spacing.sm },
  chip: { maxWidth: 160, marginRight: spacing.xs, paddingHorizontal: spacing.sm, paddingVertical: 4, borderRadius: 4, backgroundColor: colors.bgInput, borderWidth: 1, borderColor: colors.border },
  chipText: { fontSize: 9, color: colors.textDim, textTransform: 'uppercase' },
  cardActions: { flexDirection: 'row', gap: spacing.sm, padding: spacing.md, borderTopWidth: 1, borderTopColor: colors.border },
  iconAction: { height: 38, flexDirection: 'row', alignItems: 'center', gap: 5, paddingHorizontal: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  iconActionText: { fontSize: fontSize.xs, fontWeight: '700' },
  manageButton: { height: 38, flex: 1, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 6, borderRadius: 6, backgroundColor: colors.accentDim },
  manageText: { color: '#fff', fontSize: fontSize.xs, fontWeight: '700' },
  disabled: { opacity: 0.38 },
  modalScreen: { flex: 1, backgroundColor: colors.bg },
  sheetHeader: { minHeight: 62, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingLeft: spacing.lg, borderBottomWidth: 1, borderBottomColor: colors.border },
  sheetTitleRow: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm },
  sheetTitle: { fontSize: fontSize.lg, color: colors.text, fontWeight: '700' },
  sheetSubtitle: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 2 },
  iconButton: { width: 46, height: 46, alignItems: 'center', justifyContent: 'center' },
  sheetContent: { padding: spacing.lg, paddingBottom: 50 },
  manageSection: { borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, padding: spacing.md, marginBottom: spacing.md, gap: spacing.md },
  manageSectionHeader: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm },
  manageSectionTitle: { fontSize: fontSize.sm, fontWeight: '700', color: colors.text },
  manageSectionSubtitle: { fontSize: fontSize.xs, color: colors.textMuted, marginTop: 2 },
  choiceScroll: { flexGrow: 0 },
  choice: { maxWidth: 220, height: 36, justifyContent: 'center', paddingHorizontal: spacing.md, marginRight: spacing.xs, borderWidth: 1, borderColor: colors.border, borderRadius: 5, backgroundColor: colors.bgInput },
  choiceActive: { borderColor: colors.accent, backgroundColor: '#102840' },
  choiceText: { fontSize: fontSize.xs, color: colors.textDim },
  choiceTextActive: { color: colors.text, fontWeight: '600' },
  primaryButton: { height: 44, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: spacing.sm, backgroundColor: colors.accent, borderRadius: 6 },
  primaryButtonText: { color: '#fff', fontSize: fontSize.sm, fontWeight: '700' },
  secondaryButton: { minHeight: 40, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  secondaryButtonText: { color: colors.text, fontSize: fontSize.xs, fontWeight: '600' },
  inlineActions: { flexDirection: 'row', flexWrap: 'wrap', gap: spacing.sm },
  decision: { padding: spacing.sm, backgroundColor: '#0e1420', borderRadius: 6 },
  decisionLabel: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  decisionText: { fontSize: fontSize.xs, color: colors.textDim, lineHeight: 17, marginTop: 3 },
  historyRow: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingTop: spacing.sm, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: colors.border },
  historyMain: { flex: 1 },
  historyTitle: { fontSize: fontSize.xs, color: colors.text, fontWeight: '700' },
  historyReason: { fontSize: fontSize.xs, color: colors.textDim, lineHeight: 16, marginVertical: 3 },
  historyStats: { fontSize: 9 },
  recallButton: { width: 38, height: 38, alignItems: 'center', justifyContent: 'center', borderWidth: 1, borderColor: colors.border, borderRadius: 6 },
  walletStatusGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: spacing.sm },
  sharedWalletNotice: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm, padding: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  sharedWalletNoticeText: { flex: 1, color: colors.textDim, fontSize: 9, lineHeight: 14 },
  walletCheck: { width: '47%', flexDirection: 'row', alignItems: 'center', gap: spacing.xs },
  walletCheckText: { fontSize: fontSize.xs, color: colors.textDim },
  field: { gap: 5 },
  fieldLabel: { fontSize: 9, color: colors.textMuted, fontWeight: '700' },
  fieldInput: { minHeight: 42, paddingHorizontal: spacing.md, color: colors.text, fontSize: fontSize.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  secretWrap: { minHeight: 42, flexDirection: 'row', alignItems: 'center', borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  secretInput: { flex: 1, color: colors.text, fontSize: fontSize.sm, paddingHorizontal: spacing.md },
  secretToggle: { width: 42, height: 40, alignItems: 'center', justifyContent: 'center' },
  divider: { height: 1, backgroundColor: colors.border },
  ackRow: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm },
  ackText: { flex: 1, fontSize: fontSize.xs, color: colors.textDim, lineHeight: 17 },
  liveButton: { height: 44, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: spacing.sm, backgroundColor: colors.red, borderRadius: 6 },
  liveButtonText: { color: '#fff', fontSize: fontSize.sm, fontWeight: '700' },
  paperButton: { height: 42, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: spacing.sm, borderWidth: 1, borderColor: colors.yellow, borderRadius: 6 },
  paperText: { color: colors.yellow, fontSize: fontSize.sm, fontWeight: '600' },
  deleteButton: { height: 42, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: spacing.sm, borderWidth: 1, borderColor: colors.red, borderRadius: 6 },
  deleteText: { color: colors.red, fontSize: fontSize.sm, fontWeight: '600' },
  modalBackdrop: { flex: 1, justifyContent: 'center', paddingHorizontal: spacing.lg, backgroundColor: 'rgba(0,0,0,0.75)' },
  launchPanel: { maxHeight: '80%', backgroundColor: colors.bgCard, borderWidth: 1, borderColor: colors.border, borderRadius: 8 },
  pickerHeader: { height: 58, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingLeft: spacing.lg, borderBottomWidth: 1, borderBottomColor: colors.border },
  launchContent: { gap: spacing.md, padding: spacing.lg },
  errorPanel: { flexDirection: 'row', gap: spacing.sm, padding: spacing.md, borderWidth: 1, borderColor: colors.red, backgroundColor: colors.redDim, borderRadius: 6, marginBottom: spacing.md },
  errorText: { flex: 1, color: colors.red, fontSize: fontSize.sm },
  empty: { alignItems: 'center', paddingVertical: 60, gap: spacing.sm },
  emptyTitle: { fontSize: fontSize.md, fontWeight: '700', color: colors.text },
  emptyText: { fontSize: fontSize.sm, color: colors.textMuted },
});
