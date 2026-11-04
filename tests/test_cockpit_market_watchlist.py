"""Root market watchlist remains local, bounded, and read-only."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PAGE = ROOT / "scripts" / "cockpit_ui.html"


class RootMarketWatchlistTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = PAGE.read_text(encoding="utf-8")

    def test_watchlist_is_a_separate_accessible_discovery_view(self):
        self.assertIn('id="marketExplorerSearchView" aria-pressed="true"', self.page)
        self.assertIn('id="marketExplorerWatchView" aria-pressed="false"', self.page)
        self.assertIn("setMarketExplorerView('watchlist')", self.page)
        self.assertIn("Your watchlist is empty. Search a market and tap ☆ SAVE", self.page)
        self.assertIn("function _renderMarketExplorerWatchControls()", self.page)
        render_start = self.page.index("function renderMarketExplorer(){")
        render_end = self.page.index("function toggleMarketExplorerWatch(", render_start)
        self.assertIn("_renderMarketExplorerWatchControls();", self.page[render_start:render_end])

    def test_phone_watchlist_actions_have_readable_labels_and_large_targets(self):
        style_start = self.page.index('<style id="market-explorer-watchlist">')
        watchlist_styles = self.page[style_start:self.page.index("</style>", style_start)]
        self.assertIn(
            ".mkt-explorer-watch-actions button{width:100%;min-height:46px;padding:7px 5px;font-size:10px;line-height:1.3;overflow-wrap:anywhere}",
            watchlist_styles,
        )

    def test_auto_refresh_control_is_visible_and_stateful(self):
        self.assertIn('id="marketExplorerWatchAuto" aria-pressed="true"', self.page)
        self.assertIn('onclick="toggleMarketExplorerAutoRefresh()"', self.page)
        self.assertIn("const _marketExplorerWatchAutoKey='cockpitMarketWatchAutoRefreshV1'", self.page)
        self.assertIn("const _marketExplorerWatchAutoMs=30000", self.page)
        self.assertIn("return raw===null||raw==='1'", self.page)
        self.assertIn("auto.setAttribute('aria-pressed',String(_marketExplorerState.autoRefresh))", self.page)

    def test_auto_refresh_runs_only_for_visible_root_markets_watchlist(self):
        self.assertIn("_marketExplorerState.view!=='watchlist'", self.page)
        self.assertIn("document.visibilityState!=='visible'", self.page)
        self.assertIn("!marketsTab?.classList.contains('active')", self.page)
        self.assertIn("setInterval(()=>_maybeAutoRefreshMarketExplorerWatchlist(),_marketExplorerWatchAutoMs)", self.page)
        self.assertIn("document.addEventListener('visibilitychange',()=>_maybeAutoRefreshMarketExplorerWatchlist())", self.page)
        self.assertIn("if(S.curTab==='markets'){loadCandles();loadMarketArchiveCoverage();if(typeof _maybeAutoRefreshMarketExplorerWatchlist==='function')_maybeAutoRefreshMarketExplorerWatchlist();}", self.page)
        self.assertIn("_marketExplorerState.savedRefreshFinishedAt=Date.now()", self.page)

    def test_only_valid_condition_ids_are_persisted_and_list_is_bounded(self):
        self.assertIn("cockpitMarketWatchlistV1", self.page)
        self.assertIn("const _marketExplorerWatchLimit=12", self.page)
        self.assertIn("/^0x[0-9a-f]{64}$/", self.page)
        self.assertIn("localStorage.setItem(_marketExplorerWatchKey,JSON.stringify(_marketExplorerState.saved))", self.page)
        self.assertIn("Watchlist is full (${_marketExplorerWatchLimit} markets)", self.page)
        self.assertIn("if(raw.length>12000)return[]", self.page)
        self.assertIn("Saved for this page only; browser storage is unavailable.", self.page)

    def test_watchlist_refresh_uses_existing_authenticated_read_route(self):
        self.assertIn("apiFetch(`/api/mobile/market?${params}`)", self.page)
        self.assertIn("new URLSearchParams({condition_id:entry.id})", self.page)
        server = (ROOT / "scripts" / "cockpit.py").read_text()
        self.assertIn("def api_mobile_market_detail(", server)
        self.assertIn('"/api/mobile/market": lambda qs: api_mobile_market_detail', server)

    def test_watchlist_books_stream_automatically_and_manual_refresh_remains_available(self):
        self.assertIn('id="marketExplorerWatchBookRefresh"', self.page)
        self.assertIn("onclick=\"refreshMarketExplorerWatchlistBooks()\"", self.page)
        self.assertIn("condition_ids:ids.join(',')", self.page)
        self.assertIn("/api/mobile/books/watchlist?${params.toString()}", self.page)
        self.assertIn("One public CLOB socket streams up to 12 saved binary markets (24 outcome tokens) while Watchlist is visible.", self.page)
        server = (ROOT / "scripts" / "cockpit.py").read_text()
        self.assertIn("def api_mobile_market_books_watchlist(", server)
        self.assertIn('"/api/mobile/books/watchlist": lambda qs: api_mobile_market_books_watchlist', server)
        auto_start = self.page.index("function _maybeAutoRefreshMarketExplorerWatchlist")
        auto_end = self.page.index("setInterval(()=>_maybeAutoRefreshMarketExplorerWatchlist", auto_start)
        self.assertNotIn("refreshMarketExplorerWatchlistBooks", self.page[auto_start:auto_end])

    def test_watchlist_uses_one_bounded_multi_market_stream_with_dynamic_subscriptions(self):
        self.assertIn("activeTargets:new Map()", self.page)
        self.assertIn("function _marketExplorerDesiredBookStreamTargets()", self.page)
        self.assertIn("desired.size>=_marketExplorerWatchLimit", self.page)
        self.assertIn("const tokens=[...new Set([...desired.values()].flatMap(row=>row.tokens))]", self.page)
        self.assertIn("operation:'unsubscribe',assets_ids:removed", self.page)
        self.assertIn("operation:'subscribe',assets_ids:added", self.page)
        self.assertIn("function _marketExplorerStreamQuickBookRender(id)", self.page)
        self.assertIn('id="marketExplorerLiveStreamStatus" data-state="waiting" role="status" aria-live="polite"', self.page)
        self.assertIn("TWO-SIDED BOOKS LIVE", self.page)
        self.assertIn('#marketExplorerLiveStreamStatus[data-state="reconnecting"]', self.page)
        self.assertIn('#marketExplorerLiveStreamStatus{width:100%;min-height:40px', self.page)
        self.assertIn("stream.liveTapeTargetId!==id", self.page)
        self.assertIn("document.visibilityState!=='visible'||!$('tabMarkets')?.classList.contains('active')", self.page)

    def test_saved_clob_quote_teaser_keeps_both_outcomes_and_advances_age(self):
        self.assertIn('class="mkt-explorer-book-quick" aria-label="Live CLOB quotes and browser-sampled midpoint trends"', self.page)
        self.assertIn("data-market-quick-age-id", self.page)
        self.assertIn("data-market-quick-age-index", self.page)
        self.assertIn("data-market-quick-age-id][data-market-quick-age-index]", self.page)
        self.assertIn("VIEW LIVE BOOK DETAIL", self.page)

    def test_watchlist_print_summary_is_a_bounded_partial_browser_sample(self):
        self.assertIn("function _marketExplorerQuickPrintSummary(name,trades,windowSeconds,now=Date.now(),outcomeIndex=null)", self.page)
        self.assertIn("Number(trade?.outcomeIndex)===outcomeIndex", self.page)
        self.assertIn("String(trade?.outcome||'OUTCOME').slice(0,80).toLowerCase()===outcomeName", self.page)
        self.assertIn("OBS ${windowSeconds}S · ${rows.length}P · ${cash}", self.page)
        self.assertIn("This sample may be incomplete if the stream was disconnected and is not total market volume.", self.page)
        self.assertIn("function _marketExplorerWatchPrintsMarkup(id,outcomes)", self.page)
        self.assertIn("data-outcome-label=\"${esc(name)}\"", self.page)
        self.assertIn("@media(max-width:480px){.mkt-explorer-watch-prints{grid-template-columns:minmax(0,1fr)}", self.page)
        self.assertIn("_marketExplorerQuickPrintRender(id,index,now)", self.page)
        self.assertIn("const watchlistTarget=_marketExplorerState.view==='watchlist'&&stream.activeTargets.has(id)", self.page)
        self.assertIn("entry.watchTrades=[trade,...entry.watchTrades].slice(0,200)", self.page)
        self.assertIn("updatedAt-_marketExplorerQuickTrailMaxWindowMs", self.page)
        self.assertIn("opts.savedView?_marketExplorerWatchPrintsMarkup(id,outcomes):''", self.page)
        self.assertIn("do not inform orders", self.page)

    def test_watchlist_midpoint_sparklines_are_bounded_local_fresh_book_samples(self):
        self.assertIn("const _marketExplorerQuickTrailSampleMs=5000", self.page)
        self.assertIn("const _marketExplorerQuickTrailMaxWindowMs=90000", self.page)
        self.assertIn("const _marketExplorerQuickTrailMaxSamples=19", self.page)
        self.assertIn("function _marketExplorerQuickTrailMarkup(id,index,label,trail)", self.page)
        self.assertIn("function _marketExplorerQuickTrailSummary(name,rows,windowSeconds)", self.page)
        self.assertIn("point.at)>=now-windowMs", self.page)
        self.assertIn("${_marketExplorerState.trailWindowSeconds}S MID", self.page)
        self.assertIn("sampled over the last ${windowSeconds} seconds", self.page)
        self.assertIn("function _marketExplorerSampleWatchlistQuoteTrails(now=Date.now())", self.page)
        self.assertIn("book?.source==='clob_ws'&&book.fresh===true&&age!==null&&age<=15", self.page)
        self.assertIn("svg.setAttribute('aria-label',summary.accessible)", self.page)
        self.assertIn("NO FRESH DATA", self.page)
        self.assertIn("Gaps mark periods without a fresh sample.", self.page)
        self.assertIn("Number(point.at)-Number(segment.at(-1).at)>8000", self.page)
        self.assertIn("Δ ${delta>0?'+':''}${delta.toFixed(1)}¢ · ${rows.length}PT/${spanSeconds}S", self.page)
        self.assertIn("change ${delta>0?'+':''}${delta.toFixed(1)} cents from the first sample", self.page)
        self.assertIn("const _marketExplorerTrailWindows=[30,60,90]", self.page)
        self.assertIn("const _marketExplorerTrailWindowKey='cockpitMarketTrailWindowV1'", self.page)
        self.assertIn("function setMarketExplorerQuickTrailHorizon(value)", self.page)
        self.assertIn("now-_marketExplorerQuickTrailMaxWindowMs", self.page)
        self.assertIn("_marketExplorerSampleWatchlistQuoteTrails();", self.page)
        self.assertIn("_marketExplorerQuickTrailMarkup(id,index,outcome?.label,entry?.quoteTrail?.[index])", self.page)
        self.assertIn("_marketExplorerQuickTrailMarkup(id,index,label,_marketExplorerState.bookCache[id]?.quoteTrail?.[index])", self.page)

    def test_watchlist_has_accessible_persisted_30_60_90_second_trace_range(self):
        self.assertIn('id="marketExplorerTrailHorizon" aria-label="Midpoint trace and observed print time range"', self.page)
        self.assertIn('onchange="setMarketExplorerQuickTrailHorizon(this.value)"', self.page)
        self.assertIn('<option value="30">30 SECONDS</option>', self.page)
        self.assertIn('<option value="60" selected>60 SECONDS</option>', self.page)
        self.assertIn('<option value="90">90 SECONDS</option>', self.page)
        self.assertIn("trailWindowSeconds:_readMarketExplorerTrailWindow()", self.page)
        self.assertIn("if(trailWindow)trailWindow.value=String(_marketExplorerState.trailWindowSeconds)", self.page)
        self.assertIn(".mkt-explorer-trail-window select{width:100%;min-height:46px", self.page)

    def test_expanded_market_book_uses_one_bounded_read_only_live_stream(self):
        self.assertIn("const _marketExplorerBookStream={targetId:'',socket:null", self.page)
        self.assertIn("wss://ws-subscriptions-clob.polymarket.com/ws/market", self.page)
        self.assertIn("JSON.stringify({assets_ids:currentTokens,type:'market'})", self.page)
        self.assertIn("socket.send('PING')", self.page)
        self.assertIn("},10000);", self.page)
        self.assertIn("message.event_type==='book'", self.page)
        self.assertIn("message.event_type==='price_change'", self.page)
        self.assertIn("message.event_type==='last_trade_price'", self.page)
        self.assertIn("liveTapeTargetId", self.page)
        self.assertIn("liveTrades=[trade,...stream.liveTrades].slice(0,20)", self.page)
        self.assertIn("LIVE MARKET PRINTS", self.page)
        self.assertIn("SIDE · ${side}", self.page)
        self.assertIn("EVENT TIME UNKNOWN", self.page)
        self.assertIn("The reported BUY/SELL side is shown as received and is not assumed to identify the taker or aggressor", self.page)
        self.assertIn("Live capture follows only the currently selected market detail.", self.page)
        self.assertIn("STREAM FOLLOWS ONE DETAIL", self.page)
        self.assertIn("NO PRINT OBSERVED YET", self.page)
        self.assertIn("function _marketExplorerLiveTapeStatus(id)", self.page)
        self.assertIn(" · AGE ${ageText}", self.page)
        self.assertIn("Math.floor((Date.now()-receivedAt)/1000)", self.page)
        self.assertIn("function _marketExplorerStreamTradeStatusRender(id)", self.page)
        self.assertIn("if(count.textContent!==status)count.textContent=status", self.page)
        self.assertIn("if(id)_marketExplorerStreamTradeStatusRender(id)", self.page)
        self.assertIn("price<0||price>1", self.page)
        self.assertIn("/tx/${hash}", self.page)
        self.assertIn("size===0)levels.delete(price)", self.page)
        self.assertIn("bid_levels:bids.slice(0,8).map(([price,size])=>({price,size}))", self.page)
        self.assertIn("ask_levels:asks.slice(0,8).map(([price,size])=>({price,size}))", self.page)
        self.assertIn("function _marketExplorerDepthLadder(book)", self.page)
        self.assertIn("CUM SHARES", self.page)
        self.assertIn("CUM VALUE", self.page)
        self.assertIn("DEPTH LADDER <small>${bids.length} BID · ${asks.length} ASK</small>", self.page)
        self.assertIn("document.visibilityState!=='visible'", self.page)
        self.assertIn("!$('tabMarkets')?.classList.contains('active')", self.page)
        self.assertIn("source:'clob_ws'", self.page)
        self.assertIn("Top-level imbalance and microprice use best-level shares;", self.page)
        self.assertIn("the expandable ladder shows up to eight visible levels per side", self.page)
        self.assertIn("Quotes and public prints are descriptive, may be incomplete, and do not inform orders.", self.page)
        mobile = self.page[self.page.rindex("@media(max-width:700px){"):]
        self.assertIn(".mkt-explorer-book-depth-stats{grid-template-columns:repeat(2,minmax(0,1fr))}", mobile)
        self.assertIn(".mkt-explorer-book-depth-stats>span:first-child{grid-column:1/-1}", mobile)
        narrow = self.page[self.page.rindex("@media(max-width:360px){"):]
        self.assertIn(".mkt-explorer-book-depth-stats{grid-template-columns:repeat(2,minmax(0,1fr))}", narrow)
        self.assertIn(".mkt-explorer-book-depth-stats>span:first-child{grid-column:1/-1}", narrow)

    def test_depth_ladder_reflows_to_two_by_two_cells_on_mobile(self):
        style_start = self.page.index('<style id="modern-root-cockpit-design"')
        style_end = self.page.index("</style>", style_start)
        styles = self.page[style_start:style_end]
        mobile_start = styles.index("@media(max-width:700px)")
        narrow_styles = styles[mobile_start:]
        self.assertIn(".mkt-explorer-depth-columns{display:none}", narrow_styles)
        self.assertIn(".mkt-explorer-depth-row{grid-template-columns:repeat(2,minmax(0,1fr))", narrow_styles)
        self.assertIn(".mkt-explorer-depth-row>*::before{content:attr(data-label)", narrow_styles)
        narrow_start = narrow_styles.index("@media(max-width:360px)")
        self.assertIn(".mkt-explorer-book-depth-stats>span:not(:first-child){grid-column:1/-1", narrow_styles[narrow_start:])

    def test_market_values_are_labeled_last_trade_snapshots_not_quotes(self):
        self.assertIn("Gamma last trades are separate from CLOB quotes;", self.page)
        self.assertIn("CATALOG CHECK ${esc(checked)}", self.page)
        self.assertIn("Last-trade prices are not executable quotes.", self.page)
        self.assertIn("mkt-explorer-checked", self.page)
        self.assertNotIn("marketExplorerOrder", self.page)

    def test_sorting_is_local_and_discloses_loaded_result_scope(self):
        self.assertIn('id="marketExplorerSort"', self.page)
        self.assertIn('<option value="volume">24H VOLUME</option>', self.page)
        self.assertIn('<option value="liquidity">LIQUIDITY</option>', self.page)
        self.assertIn('<option value="end">ENDS SOONEST</option>', self.page)
        self.assertIn("Sort applies to currently loaded search results only.", self.page)
        self.assertIn("Sort applies to refreshed saved-market snapshots; unavailable markets stay last.", self.page)
        self.assertIn("if(left==null)return 1", self.page)
        self.assertIn("function setMarketExplorerSort(value)", self.page)
        self.assertIn("if(value==null||value==='')return'—'", self.page)

    def test_share_link_is_clean_root_url_and_contains_only_bounded_market_ids(self):
        self.assertIn('id="marketExplorerWatchShare"', self.page)
        self.assertIn("function marketExplorerWatchlistShareUrl()", self.page)
        self.assertIn("const url=new URL('/',location.origin)", self.page)
        self.assertIn("url.searchParams.set(_marketExplorerShareParam,ids.join(','))", self.page)
        self.assertIn(".filter(id=>/^0x[0-9a-f]{64}$/.test(id)).slice(0,_marketExplorerWatchLimit)", self.page)
        self.assertIn("navigator.share({title,text,url:url.href})", self.page)
        self.assertIn("navigator.clipboard.writeText(url.href)", self.page)
        self.assertIn("const _marketExplorerShareParam='market_watchlist'", self.page)
        self.assertIn("url.searchParams.delete(_marketExplorerShareParam)", self.page)

    def test_shared_import_preserves_device_entries_and_reports_capacity(self):
        self.assertIn("for(const id of [...old.map(row=>row.id),...ids])", self.page)
        self.assertIn("const skippedCount=ids.length-includedCount", self.page)
        self.assertIn("shared link: ${_marketExplorerSharedImport.includedCount}/${_marketExplorerSharedImport.requestedCount} available", self.page)
        self.assertIn("_marketExplorerState.storageWarning?' · saved for this page only", self.page)

    def test_shared_watchlist_opens_markets_on_the_unversioned_root_boot(self):
        self.assertIn("new URL('/',location.origin)", self.page)
        self.assertIn("}else if(_marketExplorerSharedImport?.hasList){", self.page)
        self.assertIn("document.querySelector('#mainNav .nav-tab[data-tab=\"markets\"]')?.click();\n    setMarketExplorerView('watchlist');", self.page)
        self.assertIn("if(readPredictionFocusFromUrl()){", self.page)


if __name__ == "__main__":
    unittest.main()
