import { useEffect, useState } from 'react';
import { Alert, RefreshControl, ScrollView, StyleSheet, Switch, Text, TextInput, TouchableOpacity, View } from 'react-native';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Ionicons } from '@expo/vector-icons';
import { api } from '../../src/api/endpoints';
import { CockpitHeader } from '../../src/components/CockpitHeader';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';
import { useAuthStore } from '../../src/state/AuthContext';
import { colors, fontSize, spacing } from '../../src/utils/theme';
import type { SharedWalletBot } from '../../src/api/types';

export default function WalletTab() {
  return <ErrorBoundary><WalletContent /></ErrorBoundary>;
}

function WalletContent() {
  const user = useAuthStore((state) => state.user);
  const isAdmin = user?.role === 'admin';
  const queryClient = useQueryClient();
  const status = useQuery({ queryKey: ['wallet_shared'], queryFn: api.walletShared, enabled: isAdmin, refetchInterval: 10000 });
  const [apiKey, setApiKey] = useState('');
  const [apiSecret, setApiSecret] = useState('');
  const [passphrase, setPassphrase] = useState('');
  const [privateKey, setPrivateKey] = useState('');
  const [funder, setFunder] = useState('');
  const [signer, setSigner] = useState('');
  const [signatureType, setSignatureType] = useState('3');
  const [ackRisk, setAckRisk] = useState(false);
  const [liveCode, setLiveCode] = useState('');

  useEffect(() => {
    const value = status.data?.signature_type;
    if (value != null) setSignatureType(String(value));
  }, [status.data?.signature_type]);

  const save = useMutation({
    mutationFn: () => api.walletSharedConfigure({
      api_key: apiKey.trim(),
      api_secret: apiSecret.trim(),
      api_passphrase: passphrase.trim(),
      private_key: privateKey.trim(),
      funder_address: funder.trim(),
      signer_address: signer.trim(),
      signature_type: Number(signatureType),
    }),
    onSuccess: async (response) => {
      setApiKey(''); setApiSecret(''); setPassphrase(''); setPrivateKey(''); setFunder(''); setSigner('');
      await Promise.all([
        queryClient.invalidateQueries({ queryKey: ['wallet_shared'] }),
        queryClient.invalidateQueries({ queryKey: ['fleet'] }),
        queryClient.invalidateQueries({ queryKey: ['runtime_fleet'] }),
      ]);
      const restarted = response.restarted_instances?.length ? `Restart requested for running bots: ${response.restarted_instances.join(', ')}.` : 'No running ADMIN bots needed a restart.';
      const failures = response.restart_failures?.length ? ` Restart failures: ${response.restart_failures.join(', ')}.` : '';
      Alert.alert(failures ? 'Credentials saved · restart needs attention' : 'Shared wallet saved', `${response.updated_instances.length} ADMIN bot configs now use the shared wallet. Per-bot modes were preserved. ${restarted}${failures}`);
    },
    onError: (error: Error) => Alert.alert('Wallet save failed', error.message),
  });

  const requestSave = () => {
    const missing = [
      !status.data?.api_key_set && !apiKey.trim() && 'API key',
      !status.data?.api_secret_set && !apiSecret.trim() && 'API secret',
      !status.data?.api_passphrase_set && !passphrase.trim() && 'API passphrase',
      !status.data?.private_key_set && !privateKey.trim() && 'private key',
      !status.data?.funder_address && !funder.trim() && 'funder/proxy address',
      !status.data?.signer_address && !signer.trim() && 'signer address',
    ].filter(Boolean);
    if (missing.length) return Alert.alert('Complete the first-time setup', `Enter ${missing.join(', ')} before saving the shared wallet.`);
    Alert.alert(
      'Save shared ADMIN credentials?',
      'This writes the protected wallet bundle once, points every ADMIN bot at it, clears legacy per-bot credential fields, and restarts currently running ADMIN bots. Each bot keeps its current PAPER/LIVE mode.',
      [
        { text: 'Review', style: 'cancel' },
        { text: 'Save shared wallet', style: 'default', onPress: () => save.mutate() },
      ],
    );
  };

  const changeMode = (bot: SharedWalletBot, next: 'live' | 'paper') => {
    const enabling = next === 'live';
    if (enabling && (!status.data?.credentials_ready || !bot.uses_shared_credentials)) {
      Alert.alert('Shared wallet setup required', 'Save and attach the shared ADMIN credentials before enabling LIVE for this bot.');
      return;
    }
    if (enabling && (!ackRisk || liveCode.trim().toUpperCase() !== 'LIVE')) {
      Alert.alert('LIVE confirmation required', 'Acknowledge the real-money risk and type LIVE before enabling a bot.');
      return;
    }
    const title = enabling ? `Enable LIVE for ${bot.instance}?` : `Return ${bot.instance} to PAPER?`;
    const detail = enabling
      ? `This bot will use the shared ADMIN wallet and can place real-money orders. Other bots keep their current modes. ${bot.running ? 'The bot will restart in LIVE mode.' : 'The bot is stopped; the next start will use LIVE mode.'}`
      : `This bot alone will switch to PAPER mode. Other ADMIN bots are unchanged. ${bot.running ? 'The bot will restart.' : ''}`;
    Alert.alert(title, detail, [
      { text: 'Cancel', style: 'cancel' },
      {
        text: enabling ? 'Enable LIVE' : 'Switch to PAPER',
        style: enabling ? 'destructive' : 'default',
        onPress: async () => {
          try {
            const result = await api.walletMode(bot.instance, next, enabling);
            setAckRisk(false); setLiveCode('');
            await Promise.all([
              queryClient.invalidateQueries({ queryKey: ['wallet_shared'] }),
              queryClient.invalidateQueries({ queryKey: ['fleet'] }),
              queryClient.invalidateQueries({ queryKey: ['runtime_fleet'] }),
            ]);
            Alert.alert('Bot mode updated', `${result.instance.toUpperCase()} is configured for ${result.mode}. ${result.restarted ? 'Restart requested for the running bot.' : result.was_running ? 'Restart requested.' : 'It will take effect on the next start.'}`);
          } catch (error) {
            Alert.alert('Mode change failed', (error as Error).message);
          }
        },
      },
    ]);
  };

  if (!isAdmin) {
    return <SafeAreaView style={styles.safe} edges={['top']}><CockpitHeader title="Wallet" subtitle="ADMIN CONTROL" /><View style={styles.denied}><Ionicons name="lock-closed-outline" size={30} color={colors.yellow} /><Text style={styles.deniedTitle}>Admin access required</Text><Text style={styles.deniedText}>Shared credentials belong to the ADMIN account.</Text></View></SafeAreaView>;
  }

  const data = status.data;
  const ready = Boolean(data?.credentials_ready);
  const refresh = async () => { await status.refetch(); };

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <CockpitHeader title="Shared Wallet" subtitle="ADMIN · FLEET-WIDE CREDENTIALS" />
      <ScrollView style={styles.scroll} contentContainerStyle={styles.content} keyboardShouldPersistTaps="handled" refreshControl={<RefreshControl refreshing={status.isFetching} onRefresh={refresh} tintColor={colors.accent} />}>
        {status.error ? <View style={styles.errorPanel}><Ionicons name="alert-circle" size={18} color={colors.red} /><Text style={styles.errorText}>{(status.error as Error).message}</Text></View> : null}

        <View style={[styles.statusCard, ready ? styles.statusReady : styles.statusMissing]}>
          <View style={styles.statusIcon}><Ionicons name={ready ? 'shield-checkmark' : 'wallet-outline'} size={22} color={ready ? colors.green : colors.yellow} /></View>
          <View style={styles.statusCopy}><Text style={[styles.statusTitle, { color: ready ? colors.green : colors.yellow }]}>{ready ? 'Shared credentials ready' : 'Complete wallet setup'}</Text><Text style={styles.statusDetail}>{ready ? 'The bundle is present and passed local shape checks. Exchange connectivity is confirmed by the bot runtime.' : 'One protected bundle will serve every ADMIN bot.'}</Text></View>
        </View>
        <View style={styles.credentialChecks}>
          <CredentialCheck label="API key" ready={Boolean(data?.api_key_set)} />
          <CredentialCheck label="API secret" ready={Boolean(data?.api_secret_set)} />
          <CredentialCheck label="Passphrase" ready={Boolean(data?.api_passphrase_set)} />
          <CredentialCheck label="Private key" ready={Boolean(data?.private_key_set)} />
          <CredentialCheck label="Addresses" ready={Boolean(data?.funder_address && data?.signer_address)} />
          <CredentialCheck label="Signature type" ready={data?.signature_type != null} />
        </View>

        <View style={styles.scopeCard}>
          <View style={styles.scopeHeader}><Ionicons name="people-outline" size={17} color={colors.accent} /><Text style={styles.scopeTitle}>One ADMIN account · all bots</Text></View>
          <Text style={styles.scopeText}>{data?.shared_bot_count ?? 0} of {data?.total_bot_count ?? 0} bot configs use the shared credential files. Saving attaches every current ADMIN bot and preserves each bot’s mode.</Text>
          {data?.legacy_source_instance ? <Text style={styles.legacyNote}>Existing credential material was detected in the legacy {data.legacy_source_instance.toUpperCase()} profile. Saving migrates it to the shared wallet.</Text> : null}
        </View>

        <View style={styles.sectionHeader}><Text style={styles.sectionTitle}>CREDENTIAL BUNDLE</Text><Text style={styles.writeOnly}>WRITE ONLY</Text></View>
        <View style={styles.formCard}>
          <Text style={styles.formIntro}>Fields stay only in this screen’s memory until submission. Blank secret fields preserve their saved values after initial setup. Values are never returned after saving.</Text>
          <SecretField label="Polymarket API key" value={apiKey} onChangeText={setApiKey} autoCapitalize="none" placeholder={data?.api_key_set ? 'Saved · blank keeps current value' : 'Required for first-time setup'} />
          <SecretField label="Polymarket API secret" value={apiSecret} onChangeText={setApiSecret} placeholder={data?.api_secret_set ? 'Saved · blank keeps current value' : 'Required for first-time setup'} />
          <SecretField label="Polymarket API passphrase" value={passphrase} onChangeText={setPassphrase} placeholder={data?.api_passphrase_set ? 'Saved · blank keeps current value' : 'Required for first-time setup'} />
          <SecretField label="Wallet private key" value={privateKey} onChangeText={setPrivateKey} placeholder={data?.private_key_set ? 'Saved · blank keeps current value' : 'Required for first-time setup'} />
          <PlainField label="Funder / proxy address" value={funder} onChangeText={setFunder} placeholder={data?.funder_address ? `Saved · ${maskAddress(data.funder_address)}` : '0x…'} />
          <PlainField label="Signer address" value={signer} onChangeText={setSigner} placeholder={data?.signer_address ? `Saved · ${maskAddress(data.signer_address)}` : '0x…'} />
          <Text style={styles.fieldLabel}>SIGNATURE TYPE</Text>
          <View style={styles.signatureRow}>{[
            { value: '0', label: 'EOA' }, { value: '1', label: 'Proxy' }, { value: '2', label: 'Gnosis Safe' }, { value: '3', label: 'Deposit · EIP-1271' },
          ].map((option) => <TouchableOpacity key={option.value} style={[styles.signatureOption, signatureType === option.value && styles.signatureActive]} onPress={() => setSignatureType(option.value)}><Text style={[styles.signatureText, signatureType === option.value && styles.signatureTextActive]}>{option.label}</Text><Text style={[styles.signatureCode, signatureType === option.value && styles.signatureTextActive]}>{option.value}</Text></TouchableOpacity>)}</View>
          <TouchableOpacity style={[styles.saveButton, save.isPending && styles.disabled]} disabled={save.isPending} onPress={requestSave}><Ionicons name="lock-closed-outline" size={17} color="#081019" /><Text style={styles.saveText}>{save.isPending ? 'Saving securely…' : 'Save for all ADMIN bots'}</Text></TouchableOpacity>
        </View>

        <View style={styles.modeHeading}><View><Text style={styles.sectionTitle}>BOT MODES</Text><Text style={styles.sectionCaption}>Credentials are shared; PAPER/LIVE remains per bot.</Text></View><TouchableOpacity onPress={() => { setAckRisk(false); setLiveCode(''); }}><Text style={styles.clearText}>Reset LIVE confirm</Text></TouchableOpacity></View>
        <View style={styles.liveGuard}>
          <View style={styles.ackRow}><Switch value={ackRisk} onValueChange={setAckRisk} trackColor={{ false: colors.bgInput, true: colors.redDim }} thumbColor={ackRisk ? colors.red : colors.textMuted} /><Text style={styles.ackText}>I understand LIVE orders can use real funds.</Text></View>
          <TextInput value={liveCode} onChangeText={setLiveCode} placeholder="Type LIVE to enable a bot" placeholderTextColor={colors.textMuted} autoCapitalize="characters" style={styles.liveInput} />
        </View>
        <View style={styles.botList}>
          {(data?.bots ?? []).map((bot) => <BotModeRow key={bot.instance} bot={bot} ready={ready} onMode={changeMode} />)}
          {!status.isLoading && !(data?.bots.length) ? <Text style={styles.empty}>No ADMIN bot configurations found.</Text> : null}
        </View>
        <Text style={styles.footerNote}>LIVE credentials are stored on the cockpit server in protected files. This screen does not enable LIVE automatically when credentials are saved.</Text>
      </ScrollView>
    </SafeAreaView>
  );
}

function BotModeRow({ bot, ready, onMode }: { bot: SharedWalletBot; ready: boolean; onMode: (bot: SharedWalletBot, mode: 'live' | 'paper') => void }) {
  const mode = bot.mode.toUpperCase();
  const live = mode === 'LIVE';
  const paper = mode === 'PAPER';
  const supported = live || paper;
  return <View style={styles.botRow}>
    <View style={[styles.botDot, { backgroundColor: bot.running ? colors.green : colors.textMuted }]} />
    <View style={styles.botInfo}><Text style={styles.botName}>{bot.instance.toUpperCase()}</Text><Text style={styles.botMeta}>{bot.running ? 'RUNNING' : 'STOPPED'} · {bot.uses_shared_credentials ? 'SHARED WALLET' : 'WALLET NOT ATTACHED'}</Text></View>
    <Text style={[styles.botMode, { color: live ? colors.red : paper ? colors.yellow : colors.textMuted }]}>{mode}</Text>
    <TouchableOpacity style={[styles.modeButton, live ? styles.paperModeButton : styles.liveModeButton, (!live && (!ready || !paper)) && styles.disabled]} disabled={!supported || (!live && !ready)} onPress={() => onMode(bot, live ? 'paper' : 'live')}><Text style={[styles.modeButtonText, live ? { color: colors.yellow } : { color: colors.red }]}>{live ? 'PAPER' : paper ? 'LIVE' : 'LOCKED'}</Text></TouchableOpacity>
  </View>;
}

function CredentialCheck({ label, ready }: { label: string; ready: boolean }) {
  return <View style={styles.credentialCheck}><Ionicons name={ready ? 'checkmark-circle' : 'ellipse-outline'} size={13} color={ready ? colors.green : colors.textMuted} /><Text style={styles.credentialCheckText}>{label}</Text></View>;
}

function SecretField({ label, value, onChangeText, autoCapitalize = 'none', placeholder }: { label: string; value: string; onChangeText: (value: string) => void; autoCapitalize?: 'none' | 'sentences' | 'words' | 'characters'; placeholder: string }) {
  const [visible, setVisible] = useState(false);
  return <View style={styles.field}><Text style={styles.fieldLabel}>{label.toUpperCase()}</Text><View style={styles.inputWrap}><TextInput value={value} onChangeText={onChangeText} secureTextEntry={!visible} autoCapitalize={autoCapitalize} autoCorrect={false} spellCheck={false} textContentType="none" autoComplete="off" placeholder={placeholder} placeholderTextColor={colors.textMuted} style={styles.input} /><TouchableOpacity style={styles.eye} onPress={() => setVisible((current) => !current)} accessibilityLabel={visible ? 'Hide secret' : 'Show secret'}><Ionicons name={visible ? 'eye-off-outline' : 'eye-outline'} size={18} color={colors.textMuted} /></TouchableOpacity></View></View>;
}

function PlainField({ label, value, onChangeText, placeholder }: { label: string; value: string; onChangeText: (value: string) => void; placeholder: string }) {
  return <View style={styles.field}><Text style={styles.fieldLabel}>{label.toUpperCase()}</Text><TextInput value={value} onChangeText={onChangeText} autoCapitalize="none" autoCorrect={false} placeholder={placeholder} placeholderTextColor={colors.textMuted} style={styles.inputSolo} /></View>;
}

function maskAddress(address?: string) {
  if (!address || address.length < 12) return 'not set';
  return `${address.slice(0, 6)}…${address.slice(-4)}`;
}

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.bg },
  scroll: { flex: 1 },
  content: { paddingHorizontal: spacing.lg, paddingBottom: spacing.xxl, gap: spacing.md },
  statusCard: { minHeight: 84, flexDirection: 'row', alignItems: 'center', gap: spacing.md, padding: spacing.md, borderWidth: 1, borderRadius: 8 },
  statusReady: { borderColor: '#1f4a3e', backgroundColor: '#0c201b' },
  statusMissing: { borderColor: '#57441f', backgroundColor: '#211a0f' },
  statusIcon: { width: 42, height: 42, alignItems: 'center', justifyContent: 'center', borderRadius: 10, backgroundColor: '#101823' },
  statusCopy: { flex: 1 },
  statusTitle: { fontSize: fontSize.md, fontWeight: '800' },
  statusDetail: { color: colors.textMuted, fontSize: 10, lineHeight: 15, marginTop: 4 },
  credentialChecks: { flexDirection: 'row', flexWrap: 'wrap', rowGap: spacing.sm, columnGap: spacing.md, paddingHorizontal: spacing.xs },
  credentialCheck: { minWidth: '29%', flexDirection: 'row', alignItems: 'center', gap: 4 },
  credentialCheckText: { color: colors.textMuted, fontSize: 8, fontWeight: '700' },
  scopeCard: { padding: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, gap: spacing.sm },
  scopeHeader: { flexDirection: 'row', alignItems: 'center', gap: spacing.sm },
  scopeTitle: { color: colors.text, fontSize: fontSize.xs, fontWeight: '800' },
  scopeText: { color: colors.textDim, fontSize: 10, lineHeight: 16 },
  legacyNote: { color: colors.yellow, fontSize: 9, lineHeight: 14 },
  sectionHeader: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  sectionTitle: { color: colors.textMuted, fontSize: 9, fontWeight: '900', letterSpacing: .8 },
  writeOnly: { color: colors.green, fontSize: 8, fontWeight: '900', letterSpacing: .6 },
  formCard: { padding: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, gap: spacing.md },
  formIntro: { color: colors.textMuted, fontSize: 9, lineHeight: 15, marginBottom: spacing.xs },
  field: { gap: 5 },
  fieldLabel: { color: colors.textMuted, fontSize: 8, fontWeight: '800', letterSpacing: .55 },
  inputWrap: { minHeight: 45, flexDirection: 'row', alignItems: 'center', borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  input: { flex: 1, minHeight: 43, color: colors.text, fontSize: 11, paddingHorizontal: spacing.md },
  eye: { width: 42, height: 43, alignItems: 'center', justifyContent: 'center' },
  inputSolo: { minHeight: 45, color: colors.text, fontSize: 11, paddingHorizontal: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  signatureRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  signatureOption: { minHeight: 42, minWidth: '47%', flexGrow: 1, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 5, backgroundColor: colors.bgInput },
  signatureActive: { borderColor: colors.accent, backgroundColor: colors.accentDim },
  signatureText: { color: colors.textDim, fontSize: 9, fontWeight: '700' },
  signatureCode: { color: colors.textMuted, fontSize: 8 },
  signatureTextActive: { color: colors.text },
  saveButton: { minHeight: 48, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: spacing.sm, marginTop: spacing.xs, borderRadius: 6, backgroundColor: colors.accent },
  saveText: { color: '#081019', fontSize: fontSize.xs, fontWeight: '900' },
  disabled: { opacity: .4 },
  modeHeading: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: spacing.xs },
  sectionCaption: { color: colors.textMuted, fontSize: 9, marginTop: 4 },
  clearText: { color: colors.textMuted, fontSize: 8 },
  liveGuard: { padding: spacing.sm, borderWidth: 1, borderColor: '#56302c', borderRadius: 7, backgroundColor: '#201310', gap: spacing.sm },
  ackRow: { minHeight: 42, flexDirection: 'row', alignItems: 'center', gap: spacing.xs },
  ackText: { flex: 1, color: colors.textDim, fontSize: 9, lineHeight: 14 },
  liveInput: { minHeight: 41, color: colors.text, fontSize: 10, fontWeight: '800', paddingHorizontal: spacing.sm, borderWidth: 1, borderColor: '#5c3430', borderRadius: 5, backgroundColor: colors.bgInput },
  botList: { borderWidth: 1, borderColor: colors.border, borderRadius: 8, overflow: 'hidden', backgroundColor: colors.bgCard },
  botRow: { minHeight: 68, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  botDot: { width: 7, height: 7, borderRadius: 4 },
  botInfo: { flex: 1, minWidth: 0 },
  botName: { color: colors.text, fontSize: 10, fontWeight: '800' },
  botMeta: { color: colors.textMuted, fontSize: 7, marginTop: 4 },
  botMode: { fontSize: 8, fontWeight: '900' },
  modeButton: { minWidth: 56, minHeight: 34, alignItems: 'center', justifyContent: 'center', borderWidth: 1, borderRadius: 5 },
  paperModeButton: { borderColor: '#75621d', backgroundColor: '#211c0e' },
  liveModeButton: { borderColor: colors.red, backgroundColor: colors.redDim },
  modeButtonText: { fontSize: 8, fontWeight: '900' },
  footerNote: { color: colors.textMuted, fontSize: 9, lineHeight: 15, textAlign: 'center', paddingHorizontal: spacing.sm },
  errorPanel: { flexDirection: 'row', gap: spacing.sm, padding: spacing.md, borderWidth: 1, borderColor: colors.red, borderRadius: 6, backgroundColor: colors.redDim },
  errorText: { flex: 1, color: colors.red, fontSize: fontSize.sm },
  empty: { color: colors.textMuted, fontSize: fontSize.xs, padding: spacing.lg, textAlign: 'center' },
  denied: { flex: 1, alignItems: 'center', justifyContent: 'center', padding: spacing.xl, gap: spacing.sm },
  deniedTitle: { color: colors.text, fontSize: fontSize.lg, fontWeight: '800' },
  deniedText: { color: colors.textMuted, fontSize: fontSize.sm, textAlign: 'center' },
});
