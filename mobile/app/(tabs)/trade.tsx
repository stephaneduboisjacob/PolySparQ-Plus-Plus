import { useEffect, useMemo, useRef, useState } from 'react';
import { Alert, KeyboardAvoidingView, Platform, RefreshControl, ScrollView, StyleSheet, Text, TextInput, TouchableOpacity, View } from 'react-native';
import { router, useLocalSearchParams } from 'expo-router';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Ionicons } from '@expo/vector-icons';
import { api } from '../../src/api/endpoints';
import { ApiError } from '../../src/api/client';
import { CockpitHeader } from '../../src/components/CockpitHeader';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';
import { useAuthStore } from '../../src/state/AuthContext';
import { useAppStore } from '../../src/state/store';
import { colors, fontSize, spacing } from '../../src/utils/theme';
import { fmtPx, fmtUsd } from '../../src/utils/format';
import type { ManualPredictionCard, MobileMarket, RuntimeBot } from '../../src/api/types';

type TradeAction = 'buy' | 'sell' | 'close';
type OutcomeSide = 'UP' | 'DOWN';
type SizeMode = 'usd' | 'shares';

export default function TradeTab() {
  return <ErrorBoundary><TradeContent /></ErrorBoundary>;
}

function TradeContent() {
  const params = useLocalSearchParams<{ cid?: string }>();
  const user = useAuthStore((state) => state.user);
  const isAdmin = user?.role === 'admin';
  const queryClient = useQueryClient();
  const sharedInstance = useAppStore((state) => state.selectedInstance);
  const setSharedInstance = useAppStore((state) => state.setSelectedInstance);
  const [instance, setInstance] = useState<string>(sharedInstance ?? '');
  const [conditionId, setConditionId] = useState('');
  const [outcomeIndex, setOutcomeIndex] = useState(0);
  const [action, setAction] = useState<TradeAction>('buy');
  const [sizeMode, setSizeMode] = useState<SizeMode>('usd');
  const [spend, setSpend] = useState('');
  const [limitCents, setLimitCents] = useState('');
  const [orderNotice, setOrderNotice] = useState<{ kind: 'queued' | 'uncertain'; detail: string } | null>(null);

  const board = useQuery({ queryKey: ['manual_predictions'], queryFn: api.manualPredictions, refetchInterval: 5000, enabled: isAdmin });
  const catalogMarket = useQuery({
    queryKey: ['mobile_market', conditionId],
    queryFn: () => api.mobileMarket(conditionId),
    enabled: isAdmin && Boolean(conditionId),
    staleTime: 5000,
  });
  const fleet = useQuery({ queryKey: ['fleet'], queryFn: api.fleet, refetchInterval: 8000, enabled: isAdmin });
  const runtime = useQuery({ queryKey: ['runtime_fleet'], queryFn: api.runtimeFleet, refetchInterval: 5000, enabled: isAdmin });
  const wallet = useQuery({ queryKey: ['wallet_shared'], queryFn: api.walletShared, refetchInterval: 10000, enabled: isAdmin });

  const bots = runtime.data?.fleet ?? [];
  useEffect(() => {
    if (!bots.length) return;
    const allowed = bots.some((bot) => bot.instance === instance);
    const fallback = bots.some((bot) => bot.instance === sharedInstance) ? sharedInstance : bots[0].instance;
    if (sharedInstance && bots.some((bot) => bot.instance === sharedInstance) && sharedInstance !== instance) setInstance(sharedInstance);
    else if (!allowed) {
      const next = fallback ?? bots[0].instance;
      setInstance(next);
      if (!sharedInstance) setSharedInstance(next);
    }
  }, [bots, instance, sharedInstance, setSharedInstance]);

  useEffect(() => {
    const cid = Array.isArray(params.cid) ? params.cid[0] : params.cid;
    if (cid) setConditionId(cid);
  }, [params.cid]);

  const bot: RuntimeBot | undefined = bots.find((candidate) => candidate.instance === instance);
  const fleetBot = fleet.data?.fleet.find((candidate) => candidate.instance === instance);
  const selectedMarket = useMemo(
    () => (board.data?.cards ?? []).find((card) => card.period === 'CURRENT' && card.condition_id === conditionId),
    [board.data?.cards, conditionId],
  );
  const venueMarket = catalogMarket.data?.market;
  const genericMarket = venueMarket && venueMarket.condition_id === conditionId ? venueMarket : undefined;
  const isGenericMarket = Boolean(genericMarket && !selectedMarket);
  const side: OutcomeSide = outcomeIndex === 0 ? 'UP' : 'DOWN';
  const genericOutcome = genericMarket?.outcomes[outcomeIndex];
  const book = useQuery({
    queryKey: ['mobile_book', conditionId, outcomeIndex],
    queryFn: () => api.mobileBook(conditionId, outcomeIndex),
    enabled: isAdmin && isGenericMarket && Boolean(genericOutcome),
    refetchInterval: 1500,
    staleTime: 0,
  });
  const venueQuote = action === 'sell' ? book.data?.best_bid : book.data?.best_ask;
  const ask = isGenericMarket
    ? (venueQuote ?? null)
    : selectedMarket ? (side === 'UP' ? selectedMarket.polymarket_up_price : selectedMarket.polymarket_down_price) : null;
  const configName = fleetBot?.current_config;
  const config = useQuery({
    queryKey: ['config', configName],
    queryFn: () => api.config(configName!),
    enabled: isAdmin && Boolean(configName),
    staleTime: 30000,
  });
  const risk = (config.data?.config?.risk ?? {}) as Record<string, unknown>;
  const minOrder = Number(risk.min_order_usd) || 0;
  const maxOrder = Number(risk.max_order_usd) || 0;
  const currentPositionQuery = useQuery({
    queryKey: ['live_lite', instance],
    queryFn: () => api.liveLite(instance),
    refetchInterval: 5000,
    enabled: isAdmin && Boolean(instance) && (action === 'close' || action === 'sell'),
  });
  const positions = currentPositionQuery.data?.positions.positions ?? [];

  const lastSpendSeed = useRef('');
  useEffect(() => {
    const seedKey = `${instance}:${configName ?? ''}`;
    if (minOrder > 0 && seedKey !== lastSpendSeed.current) {
      if (!spend) setSpend(minOrder.toFixed(2));
      lastSpendSeed.current = seedKey;
    }
  }, [configName, instance, minOrder, spend]);
  const lastLimitSeed = useRef('');
  useEffect(() => {
    const seedKey = `${conditionId}:${outcomeIndex}:${action}`;
    if (ask != null && Number.isFinite(ask) && seedKey !== lastLimitSeed.current) {
      setLimitCents((ask * 100).toFixed(1));
      lastLimitSeed.current = seedKey;
    }
  }, [ask, conditionId, side]);
  const boardAge = Number(board.data?.snapshot_age_s ?? Infinity);
  const boardFresh = Boolean(board.data && !board.data.stale && boardAge <= 10);
  const bookAge = isGenericMarket ? book.data?.book_age_s : selectedMarket?.book_age_s;
  const bookFresh = isGenericMarket ? Boolean(book.data?.fresh) : bookAge != null && bookAge <= 15;
  const mode = String(bot?.mode ?? '').toUpperCase();
  const live = mode === 'LIVE';
  const sharedBot = wallet.data?.bots.find((item) => item.instance === instance);
  const sharedReady = Boolean(wallet.data?.credentials_ready && sharedBot?.uses_shared_credentials);
  const walletIdentity = shortenAddress(wallet.data?.funder_address || wallet.data?.signer_address);
  const blockedRisk = Boolean(bot?.daily_kill_fired || ['ORDER_UNCERTAIN_HALT', 'LOSS_LIMIT_HALT'].includes(bot?.risk_state ?? ''));
  const runtimeReady = Boolean(bot?.status === 'running' && bot.mode_verified && (mode === 'PAPER' || mode === 'LIVE') && !blockedRisk);
  const canClosePosition = runtimeReady && (!live || sharedReady);
  const enteredSize = Number(spend);
  const cents = Number(limitCents);
  const amount = action === 'sell' ? 0 : sizeMode === 'shares' ? enteredSize * (cents / 100) : enteredSize;
  const estimatedShares = action === 'sell' ? enteredSize : sizeMode === 'shares' ? enteredSize : (cents > 0 ? amount / (cents / 100) : 0);
  const sellPosition = positions.find((position) => position.cid === conditionId && position.side === side);
  const genericBinary = Boolean(genericMarket && genericMarket.outcomes.length === 2);
  const marketSupported = isGenericMarket ? genericBinary : Boolean(selectedMarket?.condition_id) && action === 'buy';
  const orderValid = action === 'sell'
    ? Number.isFinite(enteredSize) && enteredSize > 0 && Boolean(sellPosition && enteredSize <= sellPosition.shares + 1e-8)
      && Number.isFinite(cents) && cents >= 1 && cents <= 99
    : Number.isFinite(amount) && amount > 0 && minOrder > 0 && amount >= minOrder && amount <= maxOrder
      && Number.isFinite(cents) && cents >= 1 && cents <= 99;
  const canSubmit = isAdmin && runtimeReady && marketSupported && (isGenericMarket ? bookFresh && !catalogMarket.data?.stale : boardFresh && bookFresh)
    && ask != null && orderValid && (!live || sharedReady) && orderNotice?.kind !== 'uncertain';

  const changeSizeMode = (next: SizeMode) => {
    if (next === sizeMode) return;
    if (Number.isFinite(enteredSize) && enteredSize > 0 && cents > 0) {
      const converted = next === 'shares' ? enteredSize / (cents / 100) : enteredSize * (cents / 100);
      setSpend(converted.toFixed(2));
    }
    setSizeMode(next);
  };

  const submit = useMutation({
    mutationFn: () => api.manualBuy({
      instance,
      condition_id: conditionId,
      side,
      size_usd: amount,
      ...(isGenericMarket ? { outcome_index: outcomeIndex, direction: action === 'sell' ? 'SELL' as const : 'BUY' as const } : {}),
      ...(action === 'sell' ? { size_shares: enteredSize } : {}),
      limit_price: cents / 100,
      expected_mode: live ? 'LIVE' : 'PAPER',
      ...(live ? { confirm_live: true } : {}),
    }),
    onSuccess: async (response) => {
      setOrderNotice({
        kind: 'queued',
        detail: `${response.instance.toUpperCase()} · ${response.mode} · ${response.direction ?? 'BUY'} ${response.outcome_label || response.side} · ${action === 'sell' ? `${enteredSize.toFixed(2)} shares` : fmtUsd(response.size_usd)} at ${(response.direction === 'SELL' ? 'minimum' : 'maximum')} ${(response.limit_price * 100).toFixed(1)}¢. The bot has not reported a fill yet.`,
      });
      await queryClient.invalidateQueries({ queryKey: ['activity'] });
      Alert.alert(
        response.status === 'queued' ? 'Order request queued' : 'Order request accepted',
        `${response.instance.toUpperCase()} · ${response.mode} · ${response.direction ?? 'BUY'} ${response.outcome_label || response.side}\n${response.question || response.asset || 'Market'} · ${(response.direction === 'SELL' ? 'minimum' : 'maximum')} ${(response.limit_price * 100).toFixed(1)}¢ · ${action === 'sell' ? `${enteredSize.toFixed(2)} shares` : fmtUsd(response.size_usd)}\nThe bot has not reported a fill yet. Check Portfolio → Orders for the final result.`,
      );
    },
    onError: (error: Error) => {
      const definiteReject = error instanceof ApiError && error.status < 500;
      if (definiteReject) {
        setOrderNotice(null);
        Alert.alert('Order rejected', error.message);
        return;
      }
      setOrderNotice({ kind: 'uncertain', detail: 'The connection ended before the server confirmed the result. Check Portfolio → Orders and the bot log before retrying; the request may have reached the bot.' });
      Alert.alert('Order result uncertain', 'Do not submit a duplicate yet. Check Portfolio → Orders and the selected bot log first.');
    },
  });

  const requestTrade = () => {
    if (!canSubmit) return;
    const direction = action.toUpperCase();
    const question = selectedMarket?.question || genericMarket?.question || 'Selected Polymarket market';
    const outcomeLabel = genericOutcome?.label || side;
    const sizeLine = action === 'sell' ? `Sell ${enteredSize.toFixed(2)} shares` : `Spend ${fmtUsd(amount)} · about ${estimatedShares.toFixed(2)} shares`;
    const limitLabel = action === 'sell' ? 'minimum' : 'maximum';
    const summary = `${instance.toUpperCase()} · ${mode}\n${question}\n${direction} ${outcomeLabel} · ${limitLabel} ${cents.toFixed(1)}¢\n${sizeLine}`;
    Alert.alert(live ? `Confirm LIVE ${direction}` : `Confirm PAPER ${direction}`, summary, [
      { text: 'Review', style: 'cancel' },
      { text: live ? `Submit LIVE ${direction}` : `Queue PAPER ${direction}`, style: live ? 'destructive' : 'default', onPress: () => submit.mutate() },
    ]);
  };

  const requestClose = (cid: string, positionSide: string, asset: string) => {
    if (!canClosePosition) return;
    const confirmation = live
      ? `Send a close request for ${asset} ${positionSide} on ${instance.toUpperCase()} in LIVE mode? The fill and final P&L may differ from the current mark.`
      : `Send a close request for ${asset} ${positionSide} on ${instance.toUpperCase()} in PAPER mode?`;
    Alert.alert(live ? 'Confirm LIVE close' : 'Confirm paper close', confirmation, [
      { text: 'Cancel', style: 'cancel' },
      {
        text: live ? 'Request LIVE close' : 'Request close',
        style: live ? 'destructive' : 'default',
        onPress: async () => {
          try {
            const result = await api.forceSell(cid, positionSide, instance);
            Alert.alert('Close request queued', `${result.queued ?? 'Your bot'} will attempt to close this position. The request is not a confirmed fill.`);
            await queryClient.invalidateQueries({ queryKey: ['live_lite', instance] });
            await queryClient.invalidateQueries({ queryKey: ['activity'] });
          } catch (error) {
            Alert.alert('Close was not accepted', (error as Error).message);
          }
        },
      },
    ]);
  };

  if (!isAdmin) {
    return <SafeAreaView style={styles.safe} edges={['top']}><CockpitHeader title="Trade" subtitle="ADMIN CONTROL" /><View style={styles.accessDenied}><Ionicons name="lock-closed-outline" size={30} color={colors.yellow} /><Text style={styles.accessTitle}>Trading is restricted</Text><Text style={styles.accessText}>Manual trading is available to the ADMIN account only.</Text></View></SafeAreaView>;
  }

  const refresh = async () => { await Promise.all([board.refetch(), catalogMarket.refetch(), book.refetch(), fleet.refetch(), runtime.refetch(), wallet.refetch(), currentPositionQuery.refetch()]); };
  const currentError = (!selectedMarket ? catalogMarket.error : null) || (isGenericMarket ? book.error : board.error) || runtime.error || fleet.error;

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <CockpitHeader title="Trade" subtitle="MANUAL EXECUTION · ONE BOT AT A TIME" mode={bot?.mode} />
      <KeyboardAvoidingView style={styles.fill} behavior={Platform.OS === 'ios' ? 'padding' : undefined}>
        <ScrollView
          style={styles.scroll}
          contentContainerStyle={styles.content}
          keyboardShouldPersistTaps="handled"
          refreshControl={<RefreshControl refreshing={board.isFetching || catalogMarket.isFetching || book.isFetching || runtime.isFetching} onRefresh={refresh} tintColor={colors.accent} />}
        >
          {currentError ? <View style={styles.errorPanel}><Ionicons name="alert-circle" size={18} color={colors.red} /><Text style={styles.errorText}>{(currentError as Error).message}</Text></View> : null}
          {orderNotice ? (
            <View style={[styles.orderNotice, orderNotice.kind === 'uncertain' && styles.orderNoticeUncertain]}>
              <Ionicons name={orderNotice.kind === 'queued' ? 'time-outline' : 'help-circle-outline'} size={18} color={orderNotice.kind === 'queued' ? colors.yellow : colors.red} />
              <View style={styles.orderNoticeCopy}>
                <Text style={[styles.orderNoticeTitle, { color: orderNotice.kind === 'queued' ? colors.yellow : colors.red }]}>{orderNotice.kind === 'queued' ? 'QUEUED · AWAITING BOT RESPONSE' : 'SUBMISSION RESULT UNCERTAIN'}</Text>
                <Text style={styles.orderNoticeText}>{orderNotice.detail}</Text>
                <View style={styles.orderNoticeActions}>
                  <TouchableOpacity onPress={() => router.push({ pathname: '/(tabs)/positions', params: { view: 'orders' } })}><Text style={styles.orderNoticeActionText}>REVIEW ORDERS</Text></TouchableOpacity>
                  {orderNotice.kind === 'queued' ? (
                    <TouchableOpacity onPress={() => setOrderNotice(null)}><Text style={styles.orderNoticeDismiss}>DISMISS</Text></TouchableOpacity>
                  ) : (
                    <TouchableOpacity onPress={() => Alert.alert('Clear the uncertainty warning?', 'Only continue after checking Orders and the selected bot log for this request.', [
                      { text: 'Keep warning', style: 'cancel' },
                      { text: 'I checked · allow another request', style: 'destructive', onPress: () => setOrderNotice(null) },
                    ])}><Text style={styles.orderNoticeDismiss}>I CHECKED</Text></TouchableOpacity>
                  )}
                </View>
              </View>
            </View>
          ) : null}
          <Text style={styles.sectionLabel}>SELECT A BOT</Text>
          <ScrollView horizontal showsHorizontalScrollIndicator={false} style={styles.botPicker}>
            {bots.map((candidate) => {
              const active = candidate.instance === instance;
              const ready = candidate.status === 'running' && candidate.mode_verified;
              return <TouchableOpacity key={candidate.instance} style={[styles.botChip, active && styles.botChipActive]} onPress={() => { setInstance(candidate.instance); setSharedInstance(candidate.instance); }}><View style={[styles.botDot, { backgroundColor: ready ? colors.green : colors.textMuted }]} /><Text style={[styles.botChipText, active && styles.botChipTextActive]}>{candidate.instance.toUpperCase()}</Text><Text style={[styles.botChipMode, candidate.mode === 'LIVE' && styles.liveText]}>{candidate.mode}</Text></TouchableOpacity>;
            })}
          </ScrollView>

          <View style={[styles.readiness, runtimeReady ? styles.ready : styles.notReady]}>
            <Ionicons name={runtimeReady ? 'shield-checkmark-outline' : 'warning-outline'} size={18} color={runtimeReady ? colors.green : colors.yellow} />
            <View style={styles.readinessCopy}><Text style={[styles.readinessTitle, { color: runtimeReady ? colors.green : colors.yellow }]}>{runtimeReady ? `${mode} runtime verified` : !bot ? 'Select a bot' : bot.status !== 'running' ? 'Bot is stopped' : !bot.mode_verified ? 'Runtime mode is unverified' : 'Risk halt is active'}</Text><Text style={styles.readinessDetail}>{live ? (sharedReady ? `Shared ADMIN wallet ${walletIdentity} · ready` : 'Shared ADMIN credentials are not ready for this bot') : 'Paper mode · no live funds are used'}</Text></View>
            {live ? <TouchableOpacity onPress={() => router.push('/(tabs)/wallet')}><Text style={styles.walletLink}>WALLET</Text></TouchableOpacity> : null}
          </View>

          <View style={styles.segmented}>
            <TradeSegment label="BUY" active={action === 'buy'} onPress={() => setAction('buy')} />
            <TradeSegment label="SELL" active={action === 'sell'} onPress={() => { setAction('sell'); setSizeMode('shares'); }} />
            <TradeSegment label="CLOSE POSITION" active={action === 'close'} onPress={() => setAction('close')} />
          </View>

          {action !== 'close' ? (
            <>
              <View style={styles.marketSectionHeader}><Text style={styles.sectionLabel}>SELECTED MARKET</Text><TouchableOpacity onPress={() => router.push('/(tabs)/markets')}><Text style={styles.marketLink}>Browse all markets</Text></TouchableOpacity></View>
              {selectedMarket ? <MarketSummary market={selectedMarket} side={side} ask={ask} fresh={boardFresh && bookFresh} /> : genericMarket ? <GenericMarketSummary market={genericMarket} outcome={genericOutcome?.label || 'Select outcome'} price={ask} fresh={bookFresh} age={bookAge} /> : <TouchableOpacity style={styles.chooseMarket} onPress={() => router.push('/(tabs)/markets')}><Ionicons name="search-outline" size={19} color={colors.accent} /><Text style={styles.chooseMarketText}>{catalogMarket.isLoading ? 'Loading selected market…' : 'Choose a market from Polymarket'}</Text><Ionicons name="chevron-forward" size={17} color={colors.textMuted} /></TouchableOpacity>}
              {!isGenericMarket && (board.data?.cards ?? []).filter((card) => card.period === 'CURRENT' && card.condition_id).length > 0 ? (
                <ScrollView horizontal showsHorizontalScrollIndicator={false} style={styles.marketPicker}>
                  {(board.data?.cards ?? []).filter((card) => card.period === 'CURRENT' && card.condition_id).map((card) => <TouchableOpacity key={card.condition_id} style={[styles.marketChip, card.condition_id === conditionId && styles.marketChipActive]} onPress={() => { setConditionId(card.condition_id!); setOutcomeIndex(0); }}><Text style={[styles.marketChipText, card.condition_id === conditionId && styles.marketChipTextActive]}>{card.asset}</Text></TouchableOpacity>)}
                </ScrollView>
              ) : null}

              <Text style={styles.sectionLabel}>OUTCOME</Text>
              <View style={styles.outcomePicker}>{isGenericMarket ? (genericMarket?.outcomes ?? []).map((outcome) => <OutcomeButton key={outcome.index} label={outcome.label} active={outcome.index === outcomeIndex} price={outcome.index === outcomeIndex ? ask : outcome.last_price} onPress={() => setOutcomeIndex(outcome.index)} />) : <><OutcomeButton label="UP" active={side === 'UP'} price={selectedMarket?.polymarket_up_price} onPress={() => setOutcomeIndex(0)} /><OutcomeButton label="DOWN" active={side === 'DOWN'} price={selectedMarket?.polymarket_down_price} onPress={() => setOutcomeIndex(1)} /></>}</View>
              {isGenericMarket && !genericBinary ? <Text style={styles.disabledHint}>This market has more than two outcomes. Manual execution is enabled for binary markets while the bot position model is extended.</Text> : null}

              <View style={styles.card}>
                <View style={styles.sizeHeader}><Text style={styles.sectionLabel}>{action === 'sell' ? 'SHARES TO SELL' : 'ORDER SIZE'}</Text>{action === 'buy' ? <View style={styles.sizeToggle}><TouchableOpacity style={[styles.sizeToggleOption, sizeMode === 'usd' && styles.sizeToggleActive]} onPress={() => changeSizeMode('usd')}><Text style={[styles.sizeToggleText, sizeMode === 'usd' && styles.sizeToggleTextActive]}>USD</Text></TouchableOpacity><TouchableOpacity style={[styles.sizeToggleOption, sizeMode === 'shares' && styles.sizeToggleActive]} onPress={() => changeSizeMode('shares')}><Text style={[styles.sizeToggleText, sizeMode === 'shares' && styles.sizeToggleTextActive]}>SHARES</Text></TouchableOpacity></View> : null}</View>
                <View style={styles.amountRow}><Text style={styles.currency}>{action === 'sell' ? '×' : sizeMode === 'usd' ? '$' : '×'}</Text><TextInput value={spend} onChangeText={setSpend} keyboardType="decimal-pad" placeholder="0.00" placeholderTextColor={colors.textMuted} style={styles.amountInput} /><Text style={styles.sizeSuffix}>{action === 'sell' || sizeMode === 'shares' ? 'shares' : 'USD'}</Text></View>
                <Text style={styles.helper}>{action === 'sell' ? `Available in ${instance.toUpperCase()}: ${sellPosition?.shares?.toFixed(2) ?? '0.00'} shares · cannot sell more than this position.` : minOrder > 0 && maxOrder > 0 ? `Bot spend limits ${fmtUsd(minOrder)}–${fmtUsd(maxOrder)}${sizeMode === 'shares' ? ' · converted at your limit price' : ' · capped at the entered spend'}` : config.isLoading ? 'Loading this bot’s order limits…' : 'Order limits are unavailable; refresh the bot configuration.'}</Text>

                <View style={styles.divider} />
                <Text style={styles.sectionLabel}>{action === 'sell' ? 'MINIMUM PRICE PER SHARE' : 'MAXIMUM PRICE PER SHARE'}</Text>
                <View style={styles.limitRow}><TextInput value={limitCents} onChangeText={setLimitCents} keyboardType="decimal-pad" placeholder="50" placeholderTextColor={colors.textMuted} style={styles.limitInput} /><Text style={styles.limitUnit}>¢</Text><View style={{ flex: 1 }} /><Text style={styles.helper}>Current {action === 'sell' ? 'bid' : 'ask'} {ask == null ? '—' : `${(ask * 100).toFixed(1)}¢`}</Text></View>
                <View style={styles.preview}>
                  <PreviewCell label="MODE" value={mode || '—'} valueColor={live ? colors.red : colors.yellow} />
                  <PreviewCell label={action === 'sell' ? 'EST. PROCEEDS' : 'SPEND'} value={action === 'sell' ? fmtUsd(enteredSize * ((cents || 0) / 100)) : Number.isFinite(amount) && amount > 0 ? fmtUsd(amount) : '—'} />
                  <PreviewCell label="EST. SHARES" value={Number.isFinite(estimatedShares) && estimatedShares > 0 ? estimatedShares.toFixed(2) : '—'} />
                </View>
                <View style={styles.freshRow}><Ionicons name={(isGenericMarket ? bookFresh : boardFresh && bookFresh) ? 'radio-button-on' : 'time-outline'} size={13} color={(isGenericMarket ? bookFresh : boardFresh && bookFresh) ? colors.green : colors.yellow} /><Text style={[styles.freshText, { color: (isGenericMarket ? bookFresh : boardFresh && bookFresh) ? colors.green : colors.yellow }]}>{(isGenericMarket ? bookFresh : boardFresh && bookFresh) ? `Outcome book fresh · ${bookAge?.toFixed(1)}s` : 'Market or order book is stale/unavailable'}</Text></View>
              </View>

              {live ? <View style={styles.liveWarning}><Ionicons name="warning" size={17} color={colors.red} /><Text style={styles.liveWarningText}>LIVE {action.toUpperCase()}: the final confirmation sends a real-money order for the selected bot and outcome. A queued request can still be rejected or remain unfilled.</Text></View> : null}
              {blockedRisk ? <View style={styles.errorPanel}><Ionicons name="pause-circle-outline" size={18} color={colors.red} /><Text style={styles.errorText}>Trading is blocked by {bot?.risk_state || 'the session loss limit'}.</Text></View> : null}
              {!runtimeReady ? <Text style={styles.disabledHint}>Start the bot and verify its runtime mode before placing a manual order.</Text> : null}
              {live && !sharedReady ? <Text style={styles.disabledHint}>Configure the shared wallet in More → Wallet, then activate LIVE for this bot.</Text> : null}
              <TouchableOpacity style={[styles.submitButton, (live || action === 'sell') && styles.submitLive, (!canSubmit || submit.isPending) && styles.submitDisabled]} disabled={!canSubmit || submit.isPending} onPress={requestTrade}>
                <Ionicons name={action === 'sell' ? 'arrow-down-circle-outline' : live ? 'flash' : 'arrow-up-circle-outline'} size={19} color={live || action === 'sell' ? '#fff' : '#081019'} />
                <Text style={[styles.submitText, (live || action === 'sell') && styles.submitLiveText]}>{submit.isPending ? 'Submitting…' : `Review ${live ? 'LIVE' : 'PAPER'} ${action.toUpperCase()} · ${genericOutcome?.label || side}`}</Text>
                <Ionicons name="chevron-forward" size={17} color={live ? '#fff' : '#081019'} />
              </TouchableOpacity>
            </>
          ) : (
            <View style={styles.closePanel}>
              <View style={styles.closeIntro}><Ionicons name="exit-outline" size={19} color={colors.accent} /><Text style={styles.closeIntroText}>Close requests exit positions already held by this bot. This does not open a short position.</Text></View>
              {currentPositionQuery.error ? <Text style={styles.errorText}>{(currentPositionQuery.error as Error).message}</Text> : null}
              {live && !sharedReady ? <View style={styles.liveWarning}><Ionicons name="warning" size={16} color={colors.red} /><Text style={styles.liveWarningText}>Shared ADMIN credentials are not ready. Configure Wallet before requesting a LIVE close.</Text></View> : null}
              {positions.map((position, index) => <View key={`${position.cid}-${position.side}-${index}`} style={styles.positionRow}>
                <View style={styles.positionInfo}><Text style={styles.positionTitle}>{position.outcome_label || position.asset} · {position.side}</Text><Text style={styles.positionMeta}>{position.shares.toFixed(2)} shares · cost {fmtUsd(position.cost_basis)} · mark {position.mark == null ? '—' : fmtPx(position.mark)}</Text><Text style={styles.positionMeta}>{position.instance?.toUpperCase() ?? instance.toUpperCase()} · {position.awaiting_settlement ? 'awaiting settlement' : 'open'}</Text></View>
                {position.awaiting_settlement ? <Text style={styles.settlingLabel}>SETTLING</Text> : <TouchableOpacity style={[styles.closeButton, !canClosePosition && styles.submitDisabled]} disabled={!canClosePosition} onPress={() => requestClose(position.cid, position.side, position.asset)}><Text style={styles.closeButtonText}>Close</Text></TouchableOpacity>}
              </View>)}
              {!currentPositionQuery.isLoading && positions.length === 0 ? <View style={styles.empty}><Ionicons name="layers-outline" size={28} color={colors.textMuted} /><Text style={styles.emptyTitle}>No open positions for {instance || 'this bot'}</Text><Text style={styles.emptyDetail}>Choose a different bot or use BUY to enter a market.</Text></View> : null}
            </View>
          )}

          <TouchableOpacity style={styles.portfolioLink} onPress={() => router.push({ pathname: '/(tabs)/positions', params: { view: 'orders' } })}><Ionicons name="receipt-outline" size={17} color={colors.accent} /><Text style={styles.portfolioLinkText}>Orders, fills and open positions</Text><Ionicons name="arrow-forward" size={15} color={colors.textMuted} /></TouchableOpacity>
        </ScrollView>
      </KeyboardAvoidingView>
    </SafeAreaView>
  );
}

function MarketSummary({ market, side, ask, fresh }: { market: ManualPredictionCard; side: OutcomeSide; ask?: number | null; fresh: boolean }) {
  const selectedAsk = ask ?? null;
  return <View style={styles.marketSummary}><View style={styles.marketSummaryHead}><Text style={styles.marketAsset}>{market.asset}</Text><Text style={[styles.marketStatus, { color: fresh ? colors.green : colors.yellow }]}>{fresh ? 'TRADEABLE SNAPSHOT' : 'STALE / CHECKING'}</Text></View><Text style={styles.marketQuestion}>{market.question || `${market.asset} current market`}</Text><View style={styles.marketPriceRow}><Text style={styles.marketPriceLabel}>{side} EXECUTABLE ASK</Text><Text style={styles.marketPrice}>{selectedAsk == null ? '—' : `${(selectedAsk * 100).toFixed(1)}¢`}</Text><Text style={styles.marketCountdown}>{formatCountdown(market.seconds_left)}</Text></View></View>;
}

function GenericMarketSummary({ market, outcome, price, fresh, age }: { market: MobileMarket; outcome: string; price?: number | null; fresh: boolean; age?: number | null }) {
  return <View style={styles.marketSummary}><View style={styles.marketSummaryHead}><Text style={styles.marketAsset}>{market.event_title || 'POLYMARKET'}</Text><Text style={[styles.marketStatus, { color: fresh ? colors.green : colors.yellow }]}>{fresh ? 'FRESH CLOB BOOK' : 'BOOK CHECKING'}</Text></View><Text style={styles.marketQuestion}>{market.question}</Text><View style={styles.marketPriceRow}><Text style={styles.marketPriceLabel}>{outcome.toUpperCase()} {price == null ? 'PRICE' : 'EXECUTABLE ASK/BID'}</Text><Text style={styles.marketPrice}>{price == null ? '—' : `${(price * 100).toFixed(1)}¢`}</Text><Text style={styles.marketCountdown}>{age == null ? '—' : `${age.toFixed(1)}s`}</Text></View></View>;
}

function OutcomeButton({ label, active, price, onPress }: { label: string; active: boolean; price?: number | null; onPress: () => void }) {
  return <TouchableOpacity style={[styles.outcomeButton, active && styles.outcomeButtonActive]} onPress={onPress} accessibilityRole="button" accessibilityState={{ selected: active }}><Text style={[styles.outcomeText, active && styles.outcomeTextActive]} numberOfLines={1}>{label.toUpperCase()}</Text><Text style={[styles.outcomePrice, active && styles.outcomeTextActive]}>{price == null ? 'No quote' : `${(price * 100).toFixed(1)}¢`}</Text></TouchableOpacity>;
}

function TradeSegment({ label, active, onPress }: { label: string; active: boolean; onPress: () => void }) {
  return <TouchableOpacity style={[styles.segment, active && styles.segmentActive]} onPress={onPress}><Text style={[styles.segmentText, active && styles.segmentTextActive]}>{label}</Text></TouchableOpacity>;
}

function PreviewCell({ label, value, valueColor }: { label: string; value: string; valueColor?: string }) {
  return <View style={styles.previewCell}><Text style={styles.previewLabel}>{label}</Text><Text style={[styles.previewValue, valueColor ? { color: valueColor } : null]}>{value}</Text></View>;
}

function formatCountdown(seconds?: number) {
  if (seconds == null) return '—';
  const value = Math.max(0, Math.floor(seconds));
  return `${Math.floor(value / 60)}:${String(value % 60).padStart(2, '0')} left`;
}

function shortenAddress(address?: string) {
  if (!address || address.length < 12) return 'not set';
  return `${address.slice(0, 6)}…${address.slice(-4)}`;
}

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.bg },
  fill: { flex: 1 },
  scroll: { flex: 1 },
  content: { paddingHorizontal: spacing.lg, paddingBottom: spacing.xxl, gap: spacing.md },
  sectionLabel: { color: colors.textMuted, fontSize: 9, fontWeight: '800', letterSpacing: .8, marginBottom: 2 },
  botPicker: { flexGrow: 0, marginTop: -spacing.xs },
  botChip: { minHeight: 43, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: spacing.md, marginRight: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 7, backgroundColor: colors.bgCard },
  botChipActive: { borderColor: colors.accent, backgroundColor: colors.accentDim },
  botDot: { width: 6, height: 6, borderRadius: 3 },
  botChipText: { color: colors.textDim, fontSize: 10, fontWeight: '800' },
  botChipTextActive: { color: colors.text },
  botChipMode: { color: colors.yellow, fontSize: 8, fontWeight: '800' },
  liveText: { color: colors.red },
  readiness: { minHeight: 66, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, paddingVertical: spacing.sm, borderWidth: 1, borderRadius: 7 },
  ready: { borderColor: '#1e493b', backgroundColor: '#0c211d' },
  notReady: { borderColor: '#57451e', backgroundColor: '#211b10' },
  readinessCopy: { flex: 1 },
  readinessTitle: { fontSize: fontSize.xs, fontWeight: '800' },
  readinessDetail: { color: colors.textMuted, fontSize: 9, marginTop: 3 },
  walletLink: { color: colors.accent, fontSize: 9, fontWeight: '800', padding: spacing.xs },
  segmented: { height: 44, flexDirection: 'row', padding: 3, borderRadius: 7, backgroundColor: colors.bgInput },
  segment: { flex: 1, justifyContent: 'center', alignItems: 'center', borderRadius: 5 },
  segmentActive: { backgroundColor: colors.bgCard, borderWidth: 1, borderColor: colors.border },
  segmentText: { color: colors.textMuted, fontSize: 9, fontWeight: '800', letterSpacing: .4 },
  segmentTextActive: { color: colors.text },
  marketSectionHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  marketLink: { color: colors.accent, fontSize: 10, fontWeight: '700' },
  chooseMarket: { minHeight: 58, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 7, backgroundColor: colors.bgCard },
  chooseMarketText: { color: colors.text, fontSize: fontSize.sm, flex: 1, fontWeight: '600' },
  marketSummary: { padding: spacing.md, gap: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard },
  marketSummaryHead: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  marketAsset: { color: colors.accent, fontSize: fontSize.sm, fontWeight: '800', letterSpacing: .7 },
  marketStatus: { fontSize: 8, fontWeight: '800' },
  marketQuestion: { color: colors.text, fontSize: fontSize.sm, fontWeight: '600', lineHeight: 20 },
  marketPriceRow: { minHeight: 46, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, borderTopWidth: 1, borderTopColor: colors.border },
  marketPriceLabel: { flex: 1, color: colors.textMuted, fontSize: 9, fontWeight: '700' },
  marketPrice: { color: colors.text, fontSize: fontSize.lg, fontWeight: '800' },
  marketCountdown: { color: colors.textMuted, fontSize: 9, minWidth: 48, textAlign: 'right' },
  marketPicker: { flexGrow: 0, marginTop: -spacing.xs },
  marketChip: { minWidth: 42, minHeight: 34, alignItems: 'center', justifyContent: 'center', marginRight: spacing.xs, borderWidth: 1, borderColor: colors.border, borderRadius: 5, backgroundColor: colors.bgCard },
  marketChipActive: { borderColor: colors.accent, backgroundColor: colors.accentDim },
  marketChipText: { color: colors.textMuted, fontSize: 9, fontWeight: '800' },
  marketChipTextActive: { color: colors.text },
  outcomePicker: { flexDirection: 'row', gap: spacing.sm },
  outcomeButton: { flex: 1, minHeight: 57, justifyContent: 'center', paddingHorizontal: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 7, backgroundColor: colors.bgCard },
  outcomeButtonActive: { borderColor: colors.accent, backgroundColor: colors.accentDim },
  outcomeText: { color: colors.textMuted, fontSize: fontSize.xs, fontWeight: '800' },
  outcomeTextActive: { color: colors.text },
  outcomePrice: { color: colors.textDim, fontSize: 11, fontWeight: '700', marginTop: 4 },
  card: { padding: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, gap: spacing.sm },
  sizeHeader: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  sizeToggle: { flexDirection: 'row', padding: 2, borderRadius: 5, backgroundColor: colors.bgInput },
  sizeToggleOption: { minHeight: 27, justifyContent: 'center', paddingHorizontal: spacing.sm, borderRadius: 4 },
  sizeToggleActive: { backgroundColor: colors.accentDim },
  sizeToggleText: { color: colors.textMuted, fontSize: 8, fontWeight: '800' },
  sizeToggleTextActive: { color: colors.text },
  amountRow: { height: 54, flexDirection: 'row', alignItems: 'center', gap: 7, paddingHorizontal: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgInput },
  currency: { color: colors.textMuted, fontSize: fontSize.lg, fontWeight: '700' },
  amountInput: { flex: 1, color: colors.text, fontSize: fontSize.xl, fontWeight: '800', paddingVertical: spacing.xs },
  sizeSuffix: { color: colors.textMuted, fontSize: 8, fontWeight: '700' },
  helper: { color: colors.textMuted, fontSize: 9, lineHeight: 14 },
  divider: { height: 1, backgroundColor: colors.border, marginVertical: spacing.xs },
  limitRow: { minHeight: 46, flexDirection: 'row', alignItems: 'center', gap: 6 },
  limitInput: { width: 84, height: 42, color: colors.text, fontSize: fontSize.md, fontWeight: '800', textAlign: 'right', paddingHorizontal: spacing.sm, borderWidth: 1, borderColor: colors.border, borderRadius: 5, backgroundColor: colors.bgInput },
  limitUnit: { color: colors.textMuted, fontSize: fontSize.md, fontWeight: '700' },
  preview: { minHeight: 58, flexDirection: 'row', marginTop: spacing.xs, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: '#0c121a' },
  previewCell: { flex: 1, justifyContent: 'center', paddingHorizontal: spacing.sm, borderRightWidth: StyleSheet.hairlineWidth, borderRightColor: colors.border },
  previewLabel: { color: colors.textMuted, fontSize: 8, fontWeight: '700' },
  previewValue: { color: colors.text, fontSize: fontSize.xs, fontWeight: '800', marginTop: 4 },
  freshRow: { flexDirection: 'row', alignItems: 'center', gap: 5 },
  freshText: { fontSize: 9, fontWeight: '700' },
  liveWarning: { flexDirection: 'row', gap: spacing.sm, padding: spacing.md, borderWidth: 1, borderColor: colors.red, borderRadius: 7, backgroundColor: colors.redDim },
  liveWarningText: { flex: 1, color: '#ffb1a8', fontSize: 10, lineHeight: 15 },
  errorPanel: { flexDirection: 'row', gap: spacing.sm, padding: spacing.md, borderWidth: 1, borderColor: colors.red, borderRadius: 6, backgroundColor: colors.redDim },
  errorText: { flex: 1, color: colors.red, fontSize: fontSize.sm },
  orderNotice: { flexDirection: 'row', alignItems: 'flex-start', gap: spacing.sm, padding: spacing.md, borderWidth: 1, borderColor: '#63531e', borderRadius: 7, backgroundColor: '#201a0d' },
  orderNoticeUncertain: { borderColor: colors.red, backgroundColor: colors.redDim },
  orderNoticeCopy: { flex: 1, gap: 4 },
  orderNoticeTitle: { fontSize: 8, fontWeight: '900', letterSpacing: .5 },
  orderNoticeText: { color: colors.textDim, fontSize: 9, lineHeight: 14 },
  orderNoticeActions: { flexDirection: 'row', flexWrap: 'wrap', gap: spacing.md, marginTop: spacing.xs },
  orderNoticeActionText: { color: colors.accent, fontSize: 8, fontWeight: '900' },
  orderNoticeDismiss: { color: colors.textMuted, fontSize: 8, fontWeight: '800' },
  disabledHint: { color: colors.yellow, fontSize: 10, lineHeight: 15 },
  submitButton: { minHeight: 52, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderRadius: 7, backgroundColor: colors.accent },
  submitLive: { backgroundColor: colors.red },
  submitDisabled: { opacity: .42 },
  submitText: { color: '#081019', fontSize: fontSize.xs, fontWeight: '900', flex: 1 },
  submitLiveText: { color: '#fff' },
  closePanel: { borderWidth: 1, borderColor: colors.border, borderRadius: 8, backgroundColor: colors.bgCard, overflow: 'hidden' },
  closeIntro: { flexDirection: 'row', gap: spacing.sm, padding: spacing.md, borderBottomWidth: 1, borderBottomColor: colors.border },
  closeIntroText: { flex: 1, color: colors.textDim, fontSize: 10, lineHeight: 15 },
  positionRow: { minHeight: 84, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, padding: spacing.md, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: colors.border },
  positionInfo: { flex: 1 },
  positionTitle: { color: colors.text, fontSize: fontSize.sm, fontWeight: '800' },
  positionMeta: { color: colors.textMuted, fontSize: 9, marginTop: 4 },
  closeButton: { minWidth: 66, minHeight: 38, alignItems: 'center', justifyContent: 'center', borderWidth: 1, borderColor: colors.red, borderRadius: 5, backgroundColor: colors.redDim },
  closeButtonText: { color: colors.red, fontSize: 10, fontWeight: '800' },
  settlingLabel: { color: colors.yellow, fontSize: 8, fontWeight: '800' },
  empty: { alignItems: 'center', paddingVertical: 40, paddingHorizontal: spacing.xl, gap: spacing.sm },
  emptyTitle: { color: colors.text, fontSize: fontSize.md, fontWeight: '700', textAlign: 'center' },
  emptyDetail: { color: colors.textMuted, fontSize: fontSize.xs, textAlign: 'center', lineHeight: 17 },
  portfolioLink: { minHeight: 44, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: colors.bgCard },
  portfolioLinkText: { flex: 1, color: colors.text, fontSize: fontSize.xs, fontWeight: '600' },
  accessDenied: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: spacing.sm, padding: spacing.xl },
  accessTitle: { color: colors.text, fontSize: fontSize.lg, fontWeight: '800' },
  accessText: { color: colors.textMuted, fontSize: fontSize.sm, textAlign: 'center' },
});
