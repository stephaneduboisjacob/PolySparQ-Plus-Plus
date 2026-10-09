import { useEffect, useMemo, useState } from 'react';
import { ActivityIndicator, RefreshControl, ScrollView, StyleSheet, Text, TextInput, TouchableOpacity, View } from 'react-native';
import { useInfiniteQuery, useQuery } from '@tanstack/react-query';
import { router } from 'expo-router';
import { SafeAreaView } from 'react-native-safe-area-context';
import { Ionicons } from '@expo/vector-icons';
import { api } from '../../src/api/endpoints';
import type { MobileMarket } from '../../src/api/types';
import { CockpitHeader } from '../../src/components/CockpitHeader';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';
import { colors, fontSize, spacing } from '../../src/utils/theme';

type Category = 'ALL' | 'CRYPTO' | 'POLITICS' | 'SPORTS' | 'ECONOMY' | 'OTHER';
type SortBy = 'VOLUME' | 'LIQUIDITY';

export default function MarketsTab() {
  return <ErrorBoundary><MarketsContent /></ErrorBoundary>;
}

function MarketsContent() {
  const [search, setSearch] = useState('');
  const [query, setQuery] = useState('');
  const [category, setCategory] = useState<Category>('ALL');
  const [sortBy, setSortBy] = useState<SortBy>('VOLUME');
  useEffect(() => {
    const timer = setTimeout(() => setQuery(search.trim()), 300);
    return () => clearTimeout(timer);
  }, [search]);

  const catalog = useInfiniteQuery({
    queryKey: ['mobile_markets', query],
    queryFn: ({ pageParam }) => api.mobileMarkets(query, Number(pageParam), 30),
    initialPageParam: 0,
    getNextPageParam: (lastPage) => lastPage.has_more ? (lastPage.next_offset ?? lastPage.offset + lastPage.markets.length) : undefined,
    staleTime: 15000,
    refetchInterval: 30000,
  });
  const fastMarkets = useQuery({
    queryKey: ['manual_predictions'],
    queryFn: api.manualPredictions,
    refetchInterval: query ? false : 15000,
    enabled: !query,
  });

  const markets = useMemo(() => {
    const seen = new Set<string>();
    const rows = (catalog.data?.pages ?? []).flatMap((page) => page.markets).filter((market) => {
      if (!market.condition_id || seen.has(market.condition_id)) return false;
      seen.add(market.condition_id);
      return category === 'ALL' || categoryFor(market) === category;
    });
    return rows.sort((a, b) => {
      const av = sortBy === 'VOLUME' ? (a.volume_24h ?? a.volume ?? 0) : (a.liquidity ?? 0);
      const bv = sortBy === 'VOLUME' ? (b.volume_24h ?? b.volume ?? 0) : (b.liquidity ?? 0);
      return bv - av;
    });
  }, [catalog.data?.pages, category, sortBy]);

  const stale = catalog.data?.pages?.some((page) => page.stale) ?? false;
  const currentCards = (fastMarkets.data?.cards ?? []).filter((card) => card.period === 'CURRENT' && card.condition_id);
  const refresh = async () => { await Promise.all([catalog.refetch(), fastMarkets.refetch()]); };

  return (
    <SafeAreaView style={styles.safe} edges={['top']}>
      <CockpitHeader title="Markets" subtitle="POLYMARKET · LIVE DISCOVERY" />
      <ScrollView
        style={styles.scroll}
        contentContainerStyle={styles.content}
        keyboardShouldPersistTaps="handled"
        refreshControl={<RefreshControl refreshing={catalog.isFetching && !catalog.isFetchingNextPage} onRefresh={refresh} tintColor={colors.accent} />}
      >
        <View style={styles.hero}>
          <View style={styles.heroOrbit}><Ionicons name="sparkles-outline" size={17} color={colors.accent} /></View>
          <View style={styles.heroCopy}>
            <Text style={styles.eyebrow}>THE VENUE, AT A GLANCE</Text>
            <Text style={styles.heroTitle}>Find your next edge.</Text>
            <Text style={styles.heroSubtitle}>Search active Polymarket questions, inspect outcomes, then route a reviewed order through a selected bot.</Text>
          </View>
        </View>

        <View style={styles.searchWrap}>
          <Ionicons name="search-outline" size={19} color={colors.accent} />
          <TextInput
            value={search}
            onChangeText={setSearch}
            placeholder="Search markets, teams, topics…"
            placeholderTextColor={colors.textMuted}
            style={styles.searchInput}
            autoCapitalize="none"
            returnKeyType="search"
            accessibilityLabel="Search Polymarket markets"
          />
          {search ? <TouchableOpacity onPress={() => setSearch('')} accessibilityLabel="Clear market search"><Ionicons name="close-circle" size={19} color={colors.textMuted} /></TouchableOpacity> : null}
        </View>

        <View style={styles.filterHeader}>
          <Text style={styles.sectionLabel}>{query ? `RESULTS FOR “${query.toUpperCase()}”` : 'ACTIVE MARKETS'}</Text>
          <TouchableOpacity style={styles.sortButton} onPress={() => setSortBy((value) => value === 'VOLUME' ? 'LIQUIDITY' : 'VOLUME')} accessibilityLabel={`Sort by ${sortBy === 'VOLUME' ? 'liquidity' : 'volume'}`}>
            <Ionicons name="swap-vertical" size={14} color={colors.accent} /><Text style={styles.sortText}>{sortBy}</Text>
          </TouchableOpacity>
        </View>
        <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.categories}>
          {(['ALL', 'CRYPTO', 'POLITICS', 'SPORTS', 'ECONOMY', 'OTHER'] as Category[]).map((item) => (
            <TouchableOpacity key={item} style={[styles.categoryChip, item === category && styles.categoryChipActive]} onPress={() => setCategory(item)}>
              <Text style={[styles.categoryText, item === category && styles.categoryTextActive]}>{item}</Text>
            </TouchableOpacity>
          ))}
        </ScrollView>

        {stale ? <View style={styles.staleBanner}><Ionicons name="time-outline" size={14} color={colors.yellow} /><Text style={styles.staleText}>Showing the last cached catalog. Refresh before placing an order.</Text></View> : null}
        {catalog.error ? <View style={styles.errorPanel}><Ionicons name="alert-circle" size={18} color={colors.red} /><Text style={styles.errorText}>{(catalog.error as Error).message}</Text><TouchableOpacity onPress={() => catalog.refetch()}><Text style={styles.retry}>RETRY</Text></TouchableOpacity></View> : null}

        {!query && currentCards.length > 0 ? (
          <View style={styles.fastSection}>
            <View style={styles.filterHeader}><Text style={styles.sectionLabel}>BOT WINDOW MARKETS</Text><Text style={styles.fastTag}>FAST ENTRY</Text></View>
            <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.fastCards}>
              {currentCards.map((card) => (
                <TouchableOpacity key={card.condition_id} style={styles.fastCard} onPress={() => router.push({ pathname: '/(tabs)/trade', params: { cid: card.condition_id! } })}>
                  <Text style={styles.fastAsset}>{card.asset}</Text>
                  <Text style={styles.fastQuestion} numberOfLines={2}>{card.question || `${card.asset} current window`}</Text>
                  <Text style={styles.fastPrice}>UP {card.polymarket_up_price == null ? '—' : `${(card.polymarket_up_price * 100).toFixed(0)}¢`} <Text style={styles.fastDivider}>·</Text> DOWN {card.polymarket_down_price == null ? '—' : `${(card.polymarket_down_price * 100).toFixed(0)}¢`}</Text>
                  <View style={styles.fastAction}><Text style={styles.fastActionText}>OPEN TICKET</Text><Ionicons name="arrow-forward" size={13} color={colors.accent} /></View>
                </TouchableOpacity>
              ))}
            </ScrollView>
          </View>
        ) : null}

        <View style={styles.catalogMeta}>
          <View style={styles.liveDot} /><Text style={styles.catalogMetaText}>{markets.length} markets loaded</Text>
          <View style={{ flex: 1 }} />
          <Text style={styles.catalogMetaText}>{catalog.data?.pages?.[0]?.server_ts ? `UPDATED ${new Date(catalog.data.pages[0].server_ts).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}` : 'CONNECTING'}</Text>
        </View>

        {catalog.isLoading ? <View style={styles.loading}><ActivityIndicator color={colors.accent} /><Text style={styles.loadingText}>Connecting to the Polymarket catalog…</Text></View> : null}
        {markets.map((market) => <MarketCard key={market.condition_id} market={market} onPress={() => router.push({ pathname: '/(tabs)/trade', params: { cid: market.condition_id } })} />)}

        {!catalog.isLoading && !catalog.error && markets.length === 0 ? (
          <View style={styles.empty}><View style={styles.emptyIcon}><Ionicons name="search-outline" size={25} color={colors.accent} /></View><Text style={styles.emptyTitle}>{query ? 'No matching active markets' : 'No markets in this filter'}</Text><Text style={styles.emptyDetail}>Try a broader search or switch the category filter back to All.</Text></View>
        ) : null}

        {catalog.hasNextPage ? <TouchableOpacity style={styles.loadMore} onPress={() => catalog.fetchNextPage()} disabled={catalog.isFetchingNextPage}>
          {catalog.isFetchingNextPage ? <ActivityIndicator size="small" color={colors.accent} /> : <Ionicons name="add-circle-outline" size={17} color={colors.accent} />}
          <Text style={styles.loadMoreText}>{catalog.isFetchingNextPage ? 'LOADING MARKETS…' : 'LOAD MORE MARKETS'}</Text>
        </TouchableOpacity> : null}
        <Text style={styles.disclaimer}>Prices shown in discovery are reference prices. The trade ticket refreshes the selected outcome’s CLOB book before enabling submission.</Text>
      </ScrollView>
    </SafeAreaView>
  );
}

function MarketCard({ market, onPress }: { market: MobileMarket; onPress: () => void }) {
  const prices = market.outcomes.slice(0, 2);
  const binary = market.outcomes.length === 2;
  return (
    <View style={styles.marketCard}>
      <View style={styles.marketTop}>
        <View style={styles.categoryPill}><Text style={styles.categoryPillText}>{categoryFor(market)}</Text></View>
        <View style={styles.marketMetrics}><Text style={styles.metricText}>24H VOL <Text style={styles.metricValue}>{formatCompact(market.volume_24h ?? market.volume)}</Text></Text><View style={styles.metricDot} /><Text style={styles.metricText}>LIQ <Text style={styles.metricValue}>{formatCompact(market.liquidity)}</Text></Text></View>
      </View>
      {market.event_title ? <Text style={styles.eventTitle} numberOfLines={1}>{market.event_title}</Text> : null}
      <Text style={styles.question} numberOfLines={3}>{market.question}</Text>
      <View style={styles.outcomeRow}>
        {prices.map((outcome) => (
          <View key={`${market.condition_id}-${outcome.index}`} style={styles.outcomePrice}>
            <Text style={styles.outcomeLabel} numberOfLines={1}>{outcome.label.toUpperCase()}</Text>
            <Text style={styles.outcomeValue}>{outcome.last_price == null ? '—' : `${(outcome.last_price * 100).toFixed(1)}¢`}</Text>
          </View>
        ))}
        {!binary ? <View style={styles.moreOutcomes}><Text style={styles.moreOutcomesText}>+{market.outcomes.length - 2} outcomes</Text></View> : null}
      </View>
      <View style={styles.cardBottom}>
        <Text style={styles.endsText}>{market.end_date ? `ENDS ${formatEndDate(market.end_date)}` : market.accepting_orders ? 'ACTIVE · VENUE OPEN' : 'NOT ACCEPTING ORDERS'}</Text>
        <TouchableOpacity style={styles.tradeButton} onPress={onPress} accessibilityLabel={`Open trade ticket for ${market.question}`}>
          <Text style={styles.tradeButtonText}>TRADE</Text><Ionicons name="arrow-forward" size={14} color="#07111a" />
        </TouchableOpacity>
      </View>
    </View>
  );
}

function categoryFor(market: MobileMarket): Category {
  const text = `${market.category ?? ''} ${market.event_title} ${market.question}`.toLowerCase();
  if (/bitcoin|btc|ethereum|eth|crypto|solana|xrp|doge|token|coinbase/.test(text)) return 'CRYPTO';
  if (/election|president|congress|senate|trump|biden|government|politic|vote/.test(text)) return 'POLITICS';
  if (/nba|nfl|mlb|nhl|soccer|football|basketball|world cup|ufc|tennis|sport/.test(text)) return 'SPORTS';
  if (/fed |federal reserve|inflation|gdp|economy|interest rate|jobs report|treasury/.test(text)) return 'ECONOMY';
  return 'OTHER';
}

function formatCompact(value: number | null | undefined) {
  if (value == null || !Number.isFinite(value)) return '—';
  if (value >= 1_000_000) return `$${(value / 1_000_000).toFixed(1)}m`;
  if (value >= 1_000) return `$${(value / 1_000).toFixed(1)}k`;
  return `$${Math.round(value)}`;
}

function formatEndDate(value: string) {
  const time = Date.parse(value);
  if (!Number.isFinite(time)) return 'DATE TBD';
  return new Date(time).toLocaleDateString([], { month: 'short', day: 'numeric' }).toUpperCase();
}

const styles = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.bg },
  scroll: { flex: 1 },
  content: { paddingHorizontal: spacing.lg, paddingTop: spacing.sm, paddingBottom: spacing.xxl, gap: spacing.md },
  hero: { flexDirection: 'row', alignItems: 'center', gap: spacing.md, padding: spacing.lg, borderRadius: 12, borderWidth: 1, borderColor: '#263b4b', backgroundColor: '#0c1720' },
  heroOrbit: { width: 42, height: 42, alignItems: 'center', justifyContent: 'center', borderRadius: 12, backgroundColor: '#142c3a', borderWidth: 1, borderColor: '#28556a' },
  heroCopy: { flex: 1 },
  eyebrow: { color: colors.accent, fontSize: 9, fontWeight: '800', letterSpacing: 1.2 },
  heroTitle: { color: colors.text, fontSize: fontSize.lg, fontWeight: '800', marginTop: 4 },
  heroSubtitle: { color: colors.textMuted, fontSize: 10, lineHeight: 15, marginTop: 4 },
  searchWrap: { minHeight: 50, flexDirection: 'row', alignItems: 'center', gap: spacing.sm, paddingHorizontal: spacing.md, borderRadius: 9, backgroundColor: colors.bgInput, borderWidth: 1, borderColor: '#263746' },
  searchInput: { flex: 1, color: colors.text, fontSize: fontSize.sm, paddingVertical: spacing.sm },
  filterHeader: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', minHeight: 28 },
  sectionLabel: { color: colors.textMuted, fontSize: 9, fontWeight: '800', letterSpacing: .9 },
  sortButton: { flexDirection: 'row', alignItems: 'center', gap: 5, paddingHorizontal: 9, paddingVertical: 5, borderRadius: 5, backgroundColor: colors.bgInput },
  sortText: { color: colors.accent, fontSize: 9, fontWeight: '800' },
  categories: { gap: 7, paddingRight: spacing.lg },
  categoryChip: { height: 30, justifyContent: 'center', paddingHorizontal: 11, borderWidth: 1, borderColor: colors.border, borderRadius: 15, backgroundColor: colors.bgCard },
  categoryChipActive: { borderColor: '#2f7790', backgroundColor: '#102633' },
  categoryText: { color: colors.textMuted, fontSize: 9, fontWeight: '800', letterSpacing: .35 },
  categoryTextActive: { color: colors.accent },
  staleBanner: { minHeight: 33, flexDirection: 'row', alignItems: 'center', gap: 7, paddingHorizontal: 10, borderWidth: 1, borderColor: '#51421e', borderRadius: 6, backgroundColor: '#221c10' },
  staleText: { flex: 1, color: colors.yellow, fontSize: 9 },
  fastSection: { gap: spacing.sm },
  fastTag: { color: colors.green, fontSize: 8, fontWeight: '800', letterSpacing: .8 },
  fastCards: { gap: spacing.sm, paddingRight: spacing.lg },
  fastCard: { width: 188, minHeight: 137, padding: spacing.md, borderWidth: 1, borderColor: '#235141', borderRadius: 9, backgroundColor: '#0d1d19', justifyContent: 'space-between' },
  fastAsset: { color: colors.green, fontSize: 9, fontWeight: '900', letterSpacing: 1 },
  fastQuestion: { color: colors.text, fontSize: 11, fontWeight: '700', lineHeight: 15, marginVertical: 7 },
  fastPrice: { color: colors.textMuted, fontSize: 8, fontWeight: '700' },
  fastDivider: { color: colors.border },
  fastAction: { flexDirection: 'row', alignItems: 'center', gap: 5, marginTop: 8 },
  fastActionText: { color: colors.accent, fontSize: 8, fontWeight: '900', letterSpacing: .6 },
  catalogMeta: { minHeight: 28, flexDirection: 'row', alignItems: 'center', gap: 6 },
  liveDot: { width: 6, height: 6, borderRadius: 3, backgroundColor: colors.green },
  catalogMetaText: { color: colors.textMuted, fontSize: 8, fontWeight: '700', letterSpacing: .25 },
  marketCard: { padding: spacing.md, borderWidth: 1, borderColor: colors.border, borderRadius: 10, backgroundColor: colors.bgCard, gap: spacing.sm },
  marketTop: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 7 },
  categoryPill: { paddingHorizontal: 7, paddingVertical: 4, borderRadius: 4, backgroundColor: '#142632' },
  categoryPillText: { color: colors.accent, fontSize: 8, fontWeight: '900', letterSpacing: .5 },
  marketMetrics: { flexDirection: 'row', alignItems: 'center', gap: 5 },
  metricText: { color: colors.textMuted, fontSize: 7, fontWeight: '700' },
  metricValue: { color: colors.text, fontWeight: '800' },
  metricDot: { width: 2, height: 2, borderRadius: 1, backgroundColor: colors.textMuted },
  eventTitle: { color: colors.textMuted, fontSize: 9, fontWeight: '600', marginTop: 2 },
  question: { color: colors.text, fontSize: fontSize.sm, fontWeight: '700', lineHeight: 20 },
  outcomeRow: { minHeight: 54, flexDirection: 'row', alignItems: 'stretch', gap: 7, marginTop: 2 },
  outcomePrice: { flex: 1, justifyContent: 'center', gap: 4, paddingHorizontal: 10, borderWidth: 1, borderColor: colors.border, borderRadius: 6, backgroundColor: '#0b1118' },
  outcomeLabel: { color: colors.textMuted, fontSize: 8, fontWeight: '800', letterSpacing: .45 },
  outcomeValue: { color: colors.text, fontSize: fontSize.md, fontWeight: '800' },
  moreOutcomes: { justifyContent: 'center', paddingHorizontal: 8 },
  moreOutcomesText: { color: colors.textMuted, fontSize: 8, fontWeight: '700' },
  cardBottom: { minHeight: 36, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: spacing.sm, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: colors.border, paddingTop: spacing.sm, marginTop: 2 },
  endsText: { flex: 1, color: colors.textMuted, fontSize: 8, fontWeight: '700' },
  tradeButton: { minHeight: 34, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 6, paddingHorizontal: 12, borderRadius: 5, backgroundColor: colors.accent },
  tradeButtonText: { color: '#07111a', fontSize: 9, fontWeight: '900', letterSpacing: .45 },
  loading: { alignItems: 'center', paddingVertical: 35, gap: 10 },
  loadingText: { color: colors.textMuted, fontSize: fontSize.xs },
  empty: { alignItems: 'center', paddingVertical: 42, paddingHorizontal: spacing.xl, gap: spacing.sm },
  emptyIcon: { width: 48, height: 48, alignItems: 'center', justifyContent: 'center', borderRadius: 15, backgroundColor: '#132430' },
  emptyTitle: { color: colors.text, fontSize: fontSize.md, fontWeight: '800', textAlign: 'center' },
  emptyDetail: { color: colors.textMuted, fontSize: fontSize.xs, textAlign: 'center', lineHeight: 17 },
  errorPanel: { flexDirection: 'row', alignItems: 'center', gap: 7, padding: spacing.md, borderWidth: 1, borderColor: colors.red, borderRadius: 6, backgroundColor: colors.redDim },
  errorText: { flex: 1, color: colors.red, fontSize: 10 },
  retry: { color: colors.accent, fontSize: 8, fontWeight: '900' },
  loadMore: { minHeight: 44, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 7, borderWidth: 1, borderColor: '#254657', borderRadius: 7, backgroundColor: '#0d1b24' },
  loadMoreText: { color: colors.accent, fontSize: 9, fontWeight: '900', letterSpacing: .5 },
  disclaimer: { color: colors.textMuted, fontSize: 8, lineHeight: 13, textAlign: 'center', marginTop: spacing.xs },
});
