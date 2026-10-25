# Awesome Prediction Market Tools: item-by-item integration audit

**Checked:** 2026-10-25 UTC  ·  **Inventory:** 179 unique entries (178 categorized listings plus the featured Oddpool entry).
**Upstream snapshot:** `aarora4/Awesome-Prediction-Market-Tools` at `e900f5d576c9341b144b9f3cade5f0871be79471` (read-only clone).

## Method and limits

Every listed URL was opened once using the project-mandated headless Playwright CLI. For each, this report records the README description, HTTP/navigation result, current page title where available, and an individual integration decision. A `200` only proves that a page responded; it does not prove the advertised feature works, data is complete, or performance claims are true. A `403`/`429`/connection failure means the page could not be inspected in this browser pass—not that the project is necessarily dead. Non-code commercial pages were assessed from their accessible landing/docs surface; they were not treated as audited services.

The page-status sample was: 134 HTTP 200, 19 HTTP 403, 5 HTTP 404, 1 HTTP 429, and 20 unresolved DNS/SSL/navigation results. These totals describe this browser pass only; they are not a verdict on quality or whether a blocked service is still operating.

The seven linked source repositories were inspected read-only at these snapshots: MiroShark `2ddfbc3`, Aeon `6eddd85`, PolyClaw `1f5a8ab`, oracle3 `267fb7c`, pykalshi `e42d9f3`, TREMOR `66dc911`, and PMXT `4a367d8`. Source review was scoped to available README/docs, the relevant entrypoints or calculations where present, and licenses; this was not a full security or code audit. PolySparQ++ is PolyForm Noncommercial; no third-party code was copied. MiroShark is AGPL-3.0; the other checked repos declare MIT or Apache-2.0, but their licensing alone does not establish that their execution/data model is appropriate here.

## What is worth integrating

1. **TREMOR’s movement-radar concept, adapted locally.** Its source describes a real-time price/volume monitor with multiple horizons, but its published hand-set score thresholds are not a calibrated predictive metric. PolySparQ++ already records CLOB snapshots and public prints. I added a V5 **Window tape** panel that draws in-window UP-midpoint history in 15-second buckets, shows movement from the first stored quote, captured prints/notional, and book freshness. It is read-only descriptive telemetry: it does not add an intensity score, alter predictions, or enter any order gate. Because these are rolling 5/15-minute condition IDs, I use the actual current window rather than TREMOR’s 1h/24h ranges.
2. **Oracle3’s fee-aware, assumption-explicit analysis workflow.** Its open source calculates leg fees and net edge for named logical relationships and includes paper tools. This is a reasonable future *research-only* module after we define and verify event-rule relationships; it does not prove an edge in our crypto-window strategies. I did not import its code or add execution.
3. **Marketlens / Probalytics historical depth as a gap-filler candidate.** Their docs describe historical full-depth order books and trade replay, which could improve backtests if our own archival coverage is demonstrably missing. Marketlens documents an API key and limited free rows, then paid full-history tiers; Probalytics documents a free trial/API or ClickHouse access. We should compare a small, reproducible sample against our stored tape and quantify a specific history gap before deciding whether a provider is worth the cost. No key, paid call, SDK, or vendor dependency was added.
4. **Avoid external “win rate”, smart-money, AI edge, copy-trade, and arbitrage claims as order inputs.** The listing pages did not establish comparable, timestamped, after-fee, independently reconciled results. No third-party execution bot, trading SDK, custody service, leverage protocol, or social signal was connected.

Useful primary/project sources: [upstream list](https://github.com/aarora4/Awesome-Prediction-Market-Tools), [TREMOR source](https://github.com/sculptdotfun/tremor), [oracle3 source](https://github.com/YichengYang-Ethan/oracle3-prediction-market-agent), [Marketlens order-book docs](https://marketlens.trade/docs/orderbook), [Marketlens access/pricing docs](https://marketlens.trade/docs), [Probalytics order-book docs](https://www.probalytics.io/orderbook), [Adanos API docs](https://api.adanos.org/docs/), [Adjacent API docs](https://docs.adjacent.markets/), [Dome acquisition and support notice](https://domeapi.io/blog/dome-joins-polymarket), [PolyRouter docs](https://docs.polyrouter.io/api-reference/introduction), [Predictefy API docs](https://docs.predictefy.com/api/), [PMXT source](https://github.com/pmxt-dev/pmxt), [Polymarket API docs](https://docs.polymarket.com/). Vendor coverage/pricing statements above are vendor-published and were not independently verified.

## Every listed project

### Featured

1. **[Oddpool](https://www.oddpool.com)** — **Listed:** Compare real-time odds across Kalshi and Polymarket. Track prediction market prices, find arbitrage opportunities, and follow whale trades on every events. **Checked:** HTTP 200, page “Oddpool \| Compare Kalshi & Polymarket Odds in Real Time”. **Assessment:** Institutional cross-venue odds/depth product. The public page gives no inspectable API contract, pricing, data license, or latency SLA; request those before any integration. No runtime dependency.

### 🧠 AI Agents

2. **[Alphascope](https://www.alphascope.app/?utm_source=polymark.et)** — **Listed:** Get AI-powered predictions for Polymarket and Kalshi before the odds move. 10,000+ traders use Alphascope to find mispriced markets, track news impact, and spot edges. F… **Checked:** HTTP 200, page “Alphascope \| AI Predictions for Polymarket & Kalshi”. **Assessment:** AI forecasts/research are vendor outputs and the visible page is promotional. No reproducible, out-of-sample calibration evidence was established; keep as a manual comparison source only.

3. **[Octagon AI](https://octagonai.co/?utm_source=polymark.et)** — **Listed:** Research and analytics on prediction markets: model probability vs. live price, cited to source, updated around the clock. **Checked:** HTTP 200, page “Octagon \| Prediction Markets Research & Analytics”. **Assessment:** Market research product, not an engine component. Any model probabilities need blind historical scoring against our archived checkpoints before they can be considered.

4. **[Polyfactual](https://www.polyfactual.com/?utm_source=polymark.et)** — **Listed:** Premium interactive webapp for deep research on prediction markets **Checked:** HTTP 200, page “Polyfacts - AI-Powered Prediction Market Research”. **Assessment:** AI news/research surface; no public data contract was established. Could be a human research reference, not a trade signal or automated source.

5. **[Polytrader](https://www.polytrader.ai/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 404. **Assessment:** The listed deployment returns 404. No integration.

6. **[Sportstensor](https://www.sportstensor.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 404. **Assessment:** The listed destination currently returns 404; no integration until a maintained replacement is documented.

7. **[BillyBets](https://billybets.ai/?utm_source=polymark.et)** — **Listed:** Billy is a sharp, a trading desk, an app, an API, and a community. **Checked:** HTTP 200, page “Billy”. **Assessment:** Consumer trading desk/community product. No public data API or independently verified performance data established; do not copy trades.

8. **[Fraction AI](https://fractionai.xyz/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Browser access is blocked (403), so current product/API and terms could not be verified. No integration decision beyond defer.

9. **[Bankr](https://bankr.bot/?utm_source=polymark.et)** — **Listed:** Bankr is an AI agent that can buy, sell, swap coins and place limit orders. **Checked:** HTTP 200, page “Bankr”. **Assessment:** General crypto-agent/wallet product, not a Polymarket data library. Wallet/agent execution would expand custody and signing risk; no integration.

10. **[Pigeon](https://pigeon.trade/?utm_source=polymark.et)** — **Listed:** Make money in any market. Trade strategies across crypto, stocks, perps, predictions, and memes all in one chat. **Checked:** HTTP 200, page “Pigeon—Your AI quant for infinite markets”. **Assessment:** General agent harness advertises research, code and trading. Autonomous wallet behavior adds an unnecessary execution boundary; no integration.

11. **[Elastics](https://www.elastics.ai/?utm_source=polymark.et)** — **Listed:** All-in-one workspace for your research, execution, and portfolio management. **Checked:** HTTP 200, page “Elastics - AI Trading Assistant”. **Assessment:** Hosted AI trading workspace. No public implementation or independently validated edge evidence established; manual reference only.

12. **[Forcazt](https://forcazt.xyz/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Forcazt - Next-gen Prediction Market Platform”. **Assessment:** AI recommendations product; its own page showed no reports. No reliable data contract or score audit; no automated integration.

13. **[Polyprophet](https://polyprophet.com?utm_source=polymark.et)** — **Listed:** PolyProphet.com is for sale on Spaceship. Secure checkout and quick transfer. See all purchase options. No hidden fees. **Checked:** HTTP 200, page “PolyProphet.com for sale \| Spaceship.com”. **Assessment:** The domain currently offers itself for sale. The listing is stale; no integration.

14. **[Polybro](https://www.polybro.app?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page returned no inspectable title/content. **Assessment:** Current page did not expose inspectable technical documentation. Treat as an unverified hosted research product; no engine dependency.

15. **[Polymarket Tips](https://polymarket.tips?utm_source=polymark.et)** — **Listed:** Follow the top 50 Polymarket traders by verified PnL. Get real-time convergence signals when smart money aligns. Free. **Checked:** HTTP 200, page “Top 50 Polymarket Traders — Live PnL & Convergence Signals”. **Assessment:** Trader convergence/smart-money claims are not a substitute for a reconciled wallet-history audit. No public machine-readable API established; no trade copying.

16. **[Inside Edge](https://inside.fyi/?utm_source=polymark.et)** — **Listed:** AI-powered prediction market analysis platform that identifies market inefficiencies on Polymarket to help users find profitable trading opportunities with quantified ed… **Checked:** HTTP 200, page returned no inspectable title/content. **Assessment:** The page returned no inspectable title/content in this pass. No integration until the current service and data provenance are verified.

17. **[PolyMaster](https://polymaster.io?utm_source=polymark.et)** — **Listed:** AI-powered Polymarket analysis platform with whale tracking, predictive modeling, and real-time market intelligence. **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved during the check; this does not prove shutdown. No integration until verified.

18. **[MiroShark](https://github.com/aaronjmars/MiroShark)** — **Listed:** Simulate anything, for $1 & less than 10 min - Universal Swarm Intelligence Engine - MiroShark/MiroShark **Checked:** HTTP 200, page “GitHub - MiroShark/MiroShark: Simulate anything, for $1 & less than 10 min - Universal Swarm I…”. **Assessment:** General-purpose swarm/simulation platform, not a market-data connector. Source checked at 2ddfbc3; its AGPL-3.0 license and unrelated stack make code reuse unsuitable here.

19. **[Aeon](https://github.com/aaronjmars/aeon)** — **Listed:** The most autonomous AI agent framework: runs unattended on GitHub Actions, self-healing skills, drives Claude Code, Grok, Codex & more. No approval loops. Configure once… **Checked:** HTTP 200, page “GitHub - aeonfun/aeon: The most autonomous AI agent framework: runs unattended on GitHub Actio…”. **Assessment:** General-purpose unattended agent framework, not a prediction-market module. Source checked at 6eddd85; “no approval loops” is a poor fit for a trading control plane even though its license is MIT.

20. **[PolyTale](https://polytale.live/?utm_source=polymark.et)** — **Listed:** AI-powered Polymarket intelligence platform that operates as the first prediction market research AI agent on Twitter, providing real-time market insights and whale trac… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; status unknown. No integration.

21. **[PolyPulse](https://www.polypulse.tech/?utm_source=polymark.et)** — **Listed:** PolyPulse is a free Chrome extension that delivers AI-powered news summaries and market analysis for Polymarket traders. Install to get real-time insights while keeping… **Checked:** HTTP 200, page “PolyPulse - Chrome Extension for AI News Insights on Polymarket”. **Assessment:** Browser extension for market-linked news, not a server-side API. Could inspire evidence links in research, but no feed is available to consume.

22. **[Semantic 42](https://42.semanticlayer.io?utm_source=polymark.et)** — **Listed:** The answer to the ultimate question of Life, the Universe, and Everything. **Checked:** HTTP 200, page “42 \| Semantic Layer”. **Assessment:** The checked page exposed no verifiable market-data contract or relevant source. No integration.

23. **[Fere AI](https://fereai.xyz?utm_source=polymark.et)** — **Listed:** Put your crypto to work with a ready-made AI trading bot. Pick an agent, fund it, and it trades for you 24/7: it takes profit and cuts losses. Free to run. **Checked:** HTTP 200, page “AI Crypto Trading Bot — Fund an AI Agent, It Trades for You”. **Assessment:** Hosted funded crypto-agent product, outside the Polymarket engine. No wallet or strategy integration.

24. **[PolyRadar](https://www.polyradar.io/?utm_source=polymark.et)** — **Listed:** AI-powered prediction analysis platform leveraging multiple independent AI models to provide comprehensive insights for Polymarket events, featuring real-time analysis,… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; status unknown. No integration.

25. **[Astron](https://www.astron.markets/?utm_source=polymark.et)** — **Listed:** AI agent protocol building intelligent, autonomous agents for prediction markets with flagship product Raven 1.0 achieving up to 98% short-term forecasting accuracy, fea… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; status unknown. No integration.

26. **[Polyseer](https://www.polyseer.xyz/?utm_source=polymark.et)** — **Listed:** AI-powered deep research for prediction markets. Paste any Polymarket or Kalshi URL and get an analyst-grade research report in seconds. **Checked:** HTTP 200, page “Polyseer \| AI-Powered Prediction Market Research”. **Assessment:** Hosted AI market research. Forecasts would need provenance, timestamped predictions and blind scoring before any comparison; no bot input.

27. **[UnifAI](https://unifai.network?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “UnifAI Network”. **Assessment:** Agent/strategy marketplace surface, not a stable data connector. Shared/copy strategies create execution and provenance risks; no integration.

28. **[TatorTrader](https://tatortrader.quickintel.io/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 404. **Assessment:** The listed deployment returns 404. No integration.

29. **[PolyOracle](https://app.polyoracle.com/?utm_source=polymark.et)** — **Listed:** An AI-powered system that uses multiple LLMs (large language models) to analyze Polymarket’s most active markets. Instead of one brain making decisions, it’s a collectiv… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; status unknown. No integration.

30. **[Predly](https://predly.ai/?utm_source=polymark.et)** — **Listed:** AI-powered prediction market analytics platform. Spot profitable opportunities on Polymarket & Kalshi with real-time AI analysis. Get instant alerts when markets mispric… **Checked:** HTTP 200, page “Predly.ai - AI-Powered Prediction Market Analytics \| Predict Your Edge”. **Assessment:** Promotional AI “edge” claims; no independently scored track record or public data schema established. Manual reference only.

31. **[PolyClaw](https://github.com/chainstacklabs/polyclaw)** — **Listed:** Trading-enabled Polymarket skill for OpenClaw - browse markets, execute trades, track positions, discover hedges - chainstacklabs/polyclaw **Checked:** HTTP 200, page “GitHub - chainstacklabs/polyclaw: Trading-enabled Polymarket skill for OpenClaw - browse marke…”. **Assessment:** Source checked at 1f5a8ab. It loads a wallet private key from environment and submits Polygon/CLOB trades; the repo says it is unaudited. Do not import or connect its execution path.

32. **[Baozi MCP Server](https://www.npmjs.com/package/@baozi.bet/mcp-server)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** The NPM page is browser-blocked (403); current API, package and license could not be verified. No integration.

33. **[Simmer](https://simmer.markets)** — **Listed:** Connect your AI agent to prediction markets. Autonomous trading on Polymarket and Kalshi with installable trading skills. **Checked:** HTTP 200, page “Simmer — Prediction Markets for the Agent Economy”. **Assessment:** Agent-economy prediction-market product; no public, read-only data contract or externally validated model edge established. No execution integration.

34. **[TurbineFi](https://turbinefi.com)** — **Listed:** Build a prediction market trading bot in plain English. Backtest strategies, review risk, and automate supported Kalshi and Polymarket workflows with TurbineFi. **Checked:** HTTP 200, page “Prediction Market Trading Bot Builder \| TurbineFi”. **Assessment:** Hosted plain-language strategy builder/backtester. Backtest assumptions and order-book data must be independently audited; no remote strategy execution integration.

35. **[oracle3](https://github.com/YichengYang-Ethan/oracle3)** — **Listed:** Oracle3: open-source trading engine and MCP server for prediction markets. Finds fee-adjusted no-arbitrage violations across related event contracts and trades them live… **Checked:** HTTP 200, page “GitHub - YichengYang-Ethan/oracle3-prediction-market-agent: Oracle3: open-source trading engin…”. **Assessment:** Source checked at 267fb7c (Apache-2.0): relation checks expose leg fees, assumptions and net edge, plus an isolated local paper ledger. Worth borrowing the *analysis workflow* conceptually; its relationship labels/quotes do not prove executable arbitrage and no code was copied.

### 🧩 APIs

36. **[Adanos Market Sentiment API](https://api.adanos.org/docs/)** — **Listed:** REST API for stock and crypto sentiment from Reddit, X/Twitter, financial news and Polymarket. Buzz score, trend and sentiment per ticker. Free API key. **Checked:** HTTP 200, page “Adanos Market Sentiment API documentation”; docs include stock/crypto sentiment endpoints and keyed usage limits. **Assessment:** Could support a separate, low-frequency sentiment research panel, but its ticker-level coverage and timing do not establish useful BTC/ETH/SOL/XRP 5m/15m signal value. Treat it as exploratory only; do not add it to order gates.

37. **[Adjacent News](https://adj.news/?utm_source=polymark.et)** — **Listed:** Prediction-market indices, reference rates, and news. **Checked:** HTTP 200, page “Adjacent \| Prediction-market indices”; first-party API docs describe unified Polymarket/Kalshi data, delayed anonymous access, and real-time access requiring a bearer key with read scope. **Assessment:** Cross-venue reference rates might help later market research, but the public tier’s 15-minute delay is too slow for our 5m/15m windows. Real-time access would add a key and an external dependency; no current use justifies it.

38. **[ClickHouse](https://crypto.clickhouse.com/?utm_source=polymark.et)** — **Listed:** Crypto analytics powered by ClickHouse & GoldSky **Checked:** HTTP 200, page “CryptoHouse”. **Assessment:** Database/analytics infrastructure, not a prediction-market feed by itself. We already run PostgreSQL; adopting a second warehouse without a demonstrated query bottleneck would add ETL/ops cost.

39. **[Dome](https://domeapi.io?utm_source=polymark.et)** — **Listed:** Access real-time and historical prediction market data from Polymarket, Kalshi, and more. Free, reliable APIs with full orderbooks for developers and traders. **Checked:** HTTP 200, page “Dome - APIs for prediction markets”; its acquisition notice says Polymarket acquired Dome, subscriptions were canceled, and support/SLA after March 31, 2026 was not guaranteed. **Assessment:** The service is reachable, so it should not be called dead, but its published continuity/support terms make it a poor production dependency. Keep using direct Polymarket APIs and local capture unless a specific archive gap warrants a bounded evaluation.

40. **[Marketlens](https://marketlens.trade/)** — **Listed:** Polymarket historical data API: tick-level order book history, every trade, candles, and outcomes for every market since March 2026, with a Python SDK, Parquet exports,… **Checked:** HTTP 200, page “Marketlens: Polymarket Order Book History, Trades & Backtesting API”. **Assessment:** Docs describe a Python/REST historical L2 and trade archive, authenticated by API key; free tier is limited and full archive is paid. Strongest replay-data candidate if we first quantify gaps in our own archive; keep research-only, no vendor dependency in the order path.

41. **[PolyRouter](https://polyrouter.io?utm_source=polymark.et)** — **Listed:** Unified API for prediction market data. Standardized JSON/CSV formats, client libraries, and comprehensive documentation for developers. **Checked:** HTTP 200, landing page and first-party API reference inspected; docs describe normalized market, price-history, and order-book access across venues, an open-beta offer, a 100-requests/minute limit, and API-key authentication for endpoints. **Assessment:** A cross-venue read-only comparison is possible, but market coverage, latency, provenance, and completeness remain vendor claims. PolySparQ++ is Polymarket-focused and already has direct CLOB access/local capture, so this adds an unnecessary key and dependency today.

42. **[PMXT](https://github.com/qoery-com/pmxt)** — **Listed:** CCXT for prediction markets. PMXT is a unified API for trading on Polymarket, Kalshi, and more. - pmxt-dev/pmxt **Checked:** HTTP 200, page “GitHub - pmxt-dev/pmxt: CCXT for prediction markets. PMXT is a unified API for trading on Poly…”. **Assessment:** Source checked at 4a367d8 (MIT). Unified SDK spans venues, but hosted API is a third-party service and supported writes include Polymarket; self-hosted trading still brings more credential/adapter surface. No need to replace our direct CLOB adapter.

43. **[Predictefy](https://docs.predictefy.com/api/)** — **Listed:** Unified prediction-market data and non-custodial execution APIs across live venue lanes. **Checked:** HTTP 200, page “Overview \| Predictefy Docs”. **Assessment:** Beta docs document metered authenticated reads and a separate execution origin requiring `trade` scope. A future read-only data comparison is possible; no credentials, paid calls, or execution integration.

44. **[pykalshi](https://github.com/ArshKA/kalshi-client)** — **Listed:** Unofficial Python client for Kalshi's prediction markets API - arshka/pykalshi **Checked:** HTTP 200, page “GitHub - arshka/pykalshi: Unofficial Python client for Kalshi's prediction markets API · GitHub”. **Assessment:** Source checked at e42d9f3 (MIT), with Kalshi-specific WebSocket/orderbook and trading code. Kalshi is outside this Polymarket-only project; do not add the dependency or credentials.

### 🔹 Aggregator

45. **[Verso](https://www.verso.trading/?utm_source=polymark.et)** — **Listed:** Professional-grade prediction market terminal that provides real-time market data, analytics, and news intelligence for Polymarket and Kalshi traders in a Bloomberg-styl… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** Browser/DNS unresolved; no technical surface verified. No integration.

46. **[Matchr](https://matchr.xyz?utm_source=polymark.et)** — **Listed:** Search and compare prediction markets across Polymarket and Kalshi. Find matched markets, compare odds, and track prices in real-time. **Checked:** HTTP 200, page “Matchr - Cross-Platform Prediction Markets”. **Assessment:** Cross-platform terminal, but the page remained at “loading markets.” No inspectable API or data provenance; no integration.

47. **[Firefly](https://firefly.social?utm_source=polymark.et)** — **Listed:** Firefly is a social app for exploring what's happening onchain. **Checked:** HTTP 200, page “Firefly ✨ Everything App for Web3 Natives”. **Assessment:** Consumer Web3 social/aggregation surface, not an engine-ready prediction-market data source. No integration.

48. **[trade.fun](https://trade.fun/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Browser access blocked (403); current API/product not verified. No integration.

49. **[TradeFox](https://thetradefox.com?utm_source=polymark.et)** — **Listed:** Prediction market aggregator and prime brokerage. **Checked:** HTTP 200, page “TradeFox”. **Assessment:** Landing page exposed no inspectable docs or data contract. No integration.

50. **[OkayBet](https://www.okaybet.app/?utm_source=polymark.et)** — **Listed:** Trade prediction markets on Polymarket with seamless cross-chain USDC liquidity from Optimism. Fast, secure, and low-cost trading powered by modern blockchain technology. **Checked:** HTTP 200, page “Okay Bet”. **Assessment:** Consumer app remained in a client-side loading state; no public API or source contract established. No integration.

51. **[Kairos](https://kairos.trade/?utm_source=polymark.et)** — **Listed:** Kairos is the advanced trading terminal for prediction markets. One book across Kalshi, Polymarket, and Predict.fun, with real-time data, pro order types, and low-latenc… **Checked:** HTTP 200, page “Kairos - The Prediction Markets Trading Terminal”. **Assessment:** Hosted prediction-market terminal/API offer. No technical schema, data provenance or service-level details established; no dependency.

### 🔔 Alerts

52. **[Nevua Markets](https://nevua.markets/?utm_source=polymark.et)** — **Listed:** Nevua Markets is winding down. Thank you for using Nevua. **Checked:** HTTP 200, page “Nevua Markets - Winding down”. **Assessment:** The site explicitly says the product is winding down and its separate Telegram alerts will not continue. No integration.

53. **[PolyAlertHub](https://polyalerthub.com/?utm_source=polymark.et)** — **Listed:** Polymarket trading terminal with AI analytics. Track fresh wallets, whales, and smart money with instant alerts to Telegram or email. **Checked:** HTTP 200, page “PolyAlertHub — Polymarket Alerts, Analytics & Terminal”. **Assessment:** Hosted signals/alerts with a displayed win-rate claim. The page does not establish a reproducible independently audited sample; do not route alerts into orders.

54. **[Stand](https://www.stand.trade/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Browser access blocked (403); no current API or terms verified. No integration.

55. **[alerts chat](https://alerts.chat/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Current service and API could not be verified from this browser response; defer integration rather than treating the entry as live or dead.

56. **[BBB](https://docs.bbb.community?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Welcome to [BBB] \| BBB”. **Assessment:** Gated researcher/trader product. No public machine-readable contract or audit evidence established; no integration.

57. **[ElonTweets Live](https://elontweets.live/?utm_source=polymark.et)** — **Listed:** Track Elon Musk’s tweets in real-time with Elontweets.live. **Checked:** HTTP 200, page “ElonTweets.Live”. **Assessment:** Specialized disputed/deleted-tweet verification. Could support manual news research, but not an engine feed without a documented API and market relevance study.

58. **[PolySpy](https://t.me/PolySpy_bot?utm_source=polymark.et)** — **Listed:** You can contact @PolySpy_bot right away. **Checked:** HTTP 200, page “Telegram: Launch @PolySpy_bot”. **Assessment:** Telegram alert bot only in the inspected surface. No verifiable API/provenance; do not trade its messages.

59. **[PolyIntel](https://t.me/PolyIntel_bot?utm_source=polymark.et)** — **Listed:** You can contact @PolyIntel_bot right away. **Checked:** HTTP 200, page “Telegram: Launch @PolyIntel_bot”. **Assessment:** Telegram intelligence bot only in the inspected surface. No verifiable API/provenance; do not trade its messages.

60. **[Polytrackerbot](https://x.com/polytrackerbot?utm_source=polymark.et)** — **Listed:** Automated Twitter bot tracking high-conviction Polymarket whale and most profitable wallet activities, with focused filtering excluding sports betting and emphasizing bu… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** Telegram URL unresolved in browser. No integration.

61. **[PolyCopy](https://polycopy.app)** — **Listed:** Copy trade Polymarket with confidence. Copy Score is a free trader signal: after copy costs, was anything left? Copy manually or let Auto Copy Bots handle it. Free to st… **Checked:** HTTP 200, page “Polycopy: The Home for Copy Trading on Polymarket”. **Assessment:** Hosted copy-trading service. Copy performance is wallet-, timing-, fee- and slippage-dependent; no direct integration.

62. **[YN Signals](https://t.me/YNSignals?utm_source=polymark.et)** — **Listed:** Yes Or No Dismantle the chain of events that you thought were just luck. X https://x.com/AntCaveClub DM @AturX **Checked:** HTTP 200, page “Telegram: View @YNSignals”. **Assessment:** Telegram channel, not a structured feed; no independent performance audit. No integration.

### 📊 Analytics Tools

63. **[Polymarket Analytics](https://polymarketanalytics.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** The site returned 403; its current analytics surface could not be verified. No integration.

64. **[Polysights](https://app.polysights.xyz/?utm_source=polymark.et)** — **Listed:** Polysights is the intelligence and execution layer for prediction markets: market discovery, a trading terminal, research, and insider tracking in one place. **Checked:** HTTP 200, page “Polysights: The intelligence layer for prediction markets”. **Assessment:** Hosted market intelligence and alerts. No public machine API or reproducible edge evidence established; manual reference only.

65. **[Hashdive](https://www.hashdive.com/?utm_source=polymark.et)** — **Listed:** Hashdive is a powerful Polymarket Analytics platform. **Checked:** HTTP 200, page “Polymarket Analytics by Hashdive”. **Assessment:** Hosted analytics page did not expose inspectable data/API in this pass. No integration.

66. **[Betmoar](https://www.betmoar.fun/?utm_source=polymark.et)** — **Listed:** The fastest way to bet on your beliefs. Over $1B+ in volume across the leading prediction market trading interface. **Checked:** HTTP 200, page “Betmoar”. **Assessment:** Community/Discord analytics product; no suitable public API or source-level data contract established. No integration.

67. **[Parsec](https://parsec.fi/polymarket?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Browser access blocked (403). No current integration surface verified.

68. **[MobyScreener](https://www.mobyscreener.com/predictions-feed?utm_source=polymark.et)** — **Listed:** The Moby experience is on mobile, and Pro is one tap away. Same account, same edge — sharper. **Checked:** HTTP 200, page “Moby — The screener lives in your pocket now”. **Assessment:** Mobile-oriented screener, useful as a UX reference but not an engine dependency. Its data/API is not public in the inspected page.

69. **[PolyScope](https://discord.com/invite/polyscope?utm_source=polymark.et)** — **Listed:** Check out the Oddpool community on Discord - hang out with 687 other members and enjoy free voice and text chat. **Checked:** HTTP 200, page “Oddpool”. **Assessment:** The listed analytics link redirects to Oddpool. Treat as a duplicate route to the same opaque institutional service, not an independent source.

70. **[Synthesis](https://synthesis.trade/?utm_source=polymark.et)** — **Listed:** Trade the moment **Checked:** HTTP 200, page “Synthesis”. **Assessment:** The page had no inspectable technical content in this pass. No integration.

71. **[PredictFolio](https://predictfolio.com?utm_source=polymark.et)** — **Listed:** Polymarket trader analytics: live PnL charts, open positions, trade history and a leaderboard of the top 1,000 wallets, all from on-chain data. **Checked:** HTTP 200, page “Polymarket Analytics by PredictFolio”. **Assessment:** Wallet/activity aggregation and exports are useful for manual reconciliation. No documented public API; our own fill ledger remains authoritative for bot accounting.

72. **[PolymarketDash](https://www.polymarketdash.com/?utm_source=polymark.et)** — **Listed:** Polymarket trader analytics dashboard **Checked:** HTTP 200, page “PolymarketDash”. **Assessment:** Login-gated trader dashboard; no unauthenticated data contract. No integration.

73. **[Pricediction](https://web3-pricediction.vercel.app/?utm_source=polymark.et)** — **Listed:** Real-time crypto prices, AI-powered predictions, and market sentiment analysis with advanced aurora visuals **Checked:** HTTP 200, page “Pricediction”. **Assessment:** Hosted AI assistant advertises notifications, but no public schema or independently scored performance. No bot signal integration.

74. **[Polysimplr](https://www.polysimplr.com/?utm_source=polymark.et)** — **Listed:** A user-friendly Polymarket interface with AI-powered chat and analysis—making prediction markets more accessible for all. **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; status unknown. No integration.

75. **[Prediedge](https://prediedge.com)** — **Listed:** PrediEdge - Professional Polymarket intelligence. Track whale trades, unusual activity, and market movements before they happen. **Checked:** HTTP 200, page “PrediEdge - Professional Polymarket Intelligence”. **Assessment:** Hosted whale/unusual-activity alerts. Wallet labeling and thresholds need independent attribution; no machine feed or order input.

76. **[Wethr](https://wethr.net/?utm_source=polymark.et)** — **Listed:** Wethr.net is the industry leader in weather market charting and analytics. **Checked:** HTTP 200, page “Wethr.net - Weather Market Analytics”. **Assessment:** Weather-market specialty analytics, outside the current crypto-window strategy scope. Could be a future market-domain research reference, not an integration.

77. **[Polytrend](https://www.polytrend.xyz/?utm_source=polymark.et)** — **Listed:** Analytics platform providing whale tracking, market insights, and upcoming autonomous trading capabilities for Polymarket prediction markets. **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** Connection failed in this pass; current service could not be verified. No integration.

78. **[Predicting Top](https://predicting.top/?utm_source=polymark.et)** — **Listed:** Live leaderboard of the top Polymarket, Kalshi, Opinion, and Myriad traders — daily, weekly, monthly, and all-time P&L, wallets, and positions, updated every few minutes. **Checked:** HTTP 200, page “Prediction Leaderboard \| predicting.top”. **Assessment:** Live trader leaderboard, useful only as a research lead. Rankings/linked wallets are not strategy proof and do not replace our realized net-P&L reconciliation.

79. **[EventWaves](https://www.eventwaves.io/?utm_source=polymark.et)** — **Listed:** Research earnings mention markets with historical keyword counts, transcript evidence, speaker analysis, and AI forecasts. **Checked:** HTTP 200, page “EventWaves — Earnings Mentions Research”. **Assessment:** The page says “coming soon”; not an available feed.

80. **[Mention Markets](https://mentionmarkets.com/?utm_source=polymark.et)** — **Listed:** The #1 Transcript Research Tool for Prediction Markets. Trump events, Earnings Calls, and more. Search through transcripts to find trends and insights for mention market… **Checked:** HTTP 200, page “Mention Markets”. **Assessment:** Transcript search may help human analysis of mention markets, but no current strategy consumes those markets and no feed contract was verified.

81. **[Wincy Polymarket Bot](https://t.me/wincy_polymarket_bot?utm_source=polymark.et)** — **Listed:** You can view and join @wincy_polymarket_bot right away. **Checked:** HTTP 200, page “Telegram: View @wincy_polymarket_bot”. **Assessment:** Telegram community/bot surface, not a documented API. No integration.

82. **[Predicts.guru](https://www.predicts.guru/?utm_source=polymark.et)** — **Listed:** Free Polymarket wallet checker and analytics. Track any trader's PnL, win rate, positions, and trading history. Analyze events with whale activity, volume data, and AI i… **Checked:** HTTP 200, page “Polymarket Analytics & Wallet Checker — Free Trader Stats”. **Assessment:** Wallet-level realized P&L/history lookup could help manual attribution audits; no documented API or validated bot attribution path. Not an accounting source.

83. **[PredScan](https://predscan.io?utm_source=polymark.et)** — **Listed:** Independent Polymarket analytics platform providing comprehensive wallet performance tracking across prediction markets with daily P&L monitoring, win rate analysis, ROI… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** SSL/navigation failed in the browser pass. No integration until service is verified.

84. **[PolyWallet](https://polywallet.info/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “PolyWallet”. **Assessment:** Consumer wallet analytics and leaderboard. Our own wallet/fill data remains the proper source for account P&L; no integration.

85. **[FirePolymarket](https://firepolymarket.com?utm_source=polymark.et)** — **Listed:** Track smart money on Polymarket. Analyze top traders, whale alerts, and find high-consensus prediction markets. **Checked:** HTTP 200, page “Top Markets \| FirePolymarket”. **Assessment:** Market/whale discovery site; no public API or audited signal quality established. Manual research only.

86. **[PolyTrack](https://polytrack.cash/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page returned no inspectable title/content. **Assessment:** Page had no inspectable title/content in this pass; no integration surface verified.

87. **[PolyInsider](https://polyinsider.io?utm_source=polymark.et)** — **Listed:** Free live Polymarket tracker: whale bets over $5K, fresh-wallet (possible insider) trades over $500, trader P&L leaderboards and news matched to markets. **Checked:** HTTP 200, page “Polymarket Whale & Insider Trading Tracker \| PolyInsider”. **Assessment:** Whale/large-trade tracker, useful as a human alert concept. “Insider” labeling is inference and not verified provenance; no automated trading.

88. **[Polymarket Elon Tracker](https://t.me/polymarketbetbot?utm_source=polymark.et)** — **Listed:** Welcome to Polymarket ElonTracker Bot! https://elon-tracker.com/ **Checked:** HTTP 200, page “Telegram: Launch @polymarketbetbot”. **Assessment:** Telegram-linked, specialized tracker with no documented structured API. Not relevant to the general market engine.

89. **[MentionMetrix](https://www.mentionmetrix.com/?utm_source=polymark.et)** — **Listed:** Track mentions and analyze trends across speeches to power your mention markets predictions. Don’t gamble—leverage data. **Checked:** HTTP 200, page “MentionMetrix – Track Mentions Across Speeches”. **Assessment:** Speech/mention transcript analytics, domain-specific. No current strategy consumes these contracts; no integration.

90. **[future fun](https://www.future.fun/?utm_source=polymark.et)** — **Listed:** AION-powered prediction market terminal featuring the Scouter analytics tool with proprietary Edge Score rankings for Polymarket traders. **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; status unknown. No integration.

91. **[Markium](https://markiumpro.com/?utm_source=polymark.et)** — **Listed:** Trade on real-world events and outcomes with Markium. Bet on politics, sports, crypto, and current events with real money. Also, access data and trade on Pre IPO stocks… **Checked:** HTTP 200, page “Markium - Prediction Markets, Pre-IPO & Tokenized Assets Platform”. **Assessment:** Hosted multi-asset analytics/trading product; no API contract or independently validated edge established. No integration.

92. **[PolyVision](https://polyvisionx.com)** — **Listed:** Institutional-grade copy trading analysis for Polymarket. Stop gambling. Start systematizing. **Checked:** HTTP 200, page “Polyvision \| The Science of Copy Trading”. **Assessment:** Copy-trading analysis product. Scores/leaderboards need independent wallet- and fee-level reconciliation; no order automation.

93. **[pm.wiki](https://pm.wiki/?utm_source=polymark.et)** — **Listed:** Independent prediction market directory and comparison tool with 350+ project profiles, covering exchanges, analytics tools, and ecosystem projects across the prediction… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** Connection failed; the directory listing was not accessible. A project directory is discovery, not a runtime feed.

94. **[Polyguana](https://polyguana.com/?utm_source=polymark.et)** — **Listed:** Independent Polymarket analytics platform featuring trader leaderboards, market statistics, and performance tracking for data-driven prediction market insights. **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** Connection failed; current product and API not verified. No integration.

### 🔹 Arbitrage tools

95. **[ArbBets](https://getarbitragebets.com/?utm_source=polymark.et)** — **Listed:** Prediction market arbitrage finder for Polymarket, Kalshi, and Opinion. Scan 990+ markets, get fee-adjusted ROI, and automate with our arbitrage betting API. Plans from… **Checked:** HTTP 200, page “Polymarket Kalshi Arbitrage Finder — 100+ Daily Arbs \| ArbBets \| ArbBets”. **Assessment:** Commercial cross-venue arb scanner/API claims. Any candidate still needs exact event/rules mapping, fee schedules, depth, settlement and latency checks; no automated execution dependency.

96. **[Eventarb](https://www.eventarb.com/?utm_source=polymark.et)** — **Listed:** Find arbitrage between Kalshi, Polymarket, and Robinhood event contracts. See position size, fees, payout, and annualized return. **Checked:** HTTP 200, page “Event Contract Arbitrage Calculator for Kalshi, Polymarket & Robinhood”. **Assessment:** Calculator, not live execution or a data source. Its useful concept is explicit per-leg fees/payout; our project should independently recompute any basket before display.

97. **[Polytrage](http://t.me/polytrage?utm_source=polymark.et)** — **Listed:** offical polytrage x.com/TGpolytrage **Checked:** HTTP 200, page “Telegram: View @polytrage”. **Assessment:** Telegram alert channel. No structured feed or independent realized results; no integration.

98. **[PolyScalping](https://polyscalping.org/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 429; blocked/rate-limited. **Assessment:** Rate-limited (429) during the check. Current product/source not verified; no integration.

99. **[Polymarket JB Bot](https://t.me/polymarket_jb_bot?utm_source=polymark.et)** — **Listed:** You can view and join @polymarket_jb_bot right away. **Checked:** HTTP 200, page “Telegram: View @polymarket_jb_bot”. **Assessment:** Telegram bot with no inspectable API or audit trail. No integration.

100. **[Prediction Hunt](https://predictionhunt.com/?utm_source=polymark.et)** — **Listed:** Compare live odds and picks on prediction markets like NFL, politics and crypto. Get smart money, arbitrage and combos feeds for trades. **Checked:** HTTP 200, page “Prediction.com - The home of prediction markets”. **Assessment:** Cross-platform consumer odds/picks surface. No public data contract or independently verified prediction results; not an order source.

### 📈 Dashboards

101. **[Token Terminal](https://tokenterminal.com?utm_source=polymark.et)** — **Listed:** Standardized financial and usage metrics for blockchains, apps and tokenized assets. Compare fees, revenue and users across crypto. Start exploring for free. **Checked:** HTTP 200, page “Token Terminal \| Fundamentals for crypto”. **Assessment:** General blockchain fundamentals analytics, not Polymarket order-book telemetry. No integration needed.

102. **[DefiLlama](https://defillama.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** General DeFi dashboard; the listed page was browser-blocked. No direct role in this engine.

103. **[Pizzint Watch](https://www.pizzint.watch/?utm_source=polymark.et)** — **Listed:** The Pentagon Pizza Index tracks real-time pizza activity near the Pentagon as an OSINT indicator of military tension. Check the live doomsday clock, current DEFCON level… **Checked:** HTTP 200, page “Pentagon Pizza Index — Live DEFCON Level & Doomsday Clock \| PizzINT”. **Assessment:** Humor/OSINT composite signal. Source methodology and market correlation are not adequate for strategy inputs; no integration.

104. **[fergmolina](https://dune.com/fergmolina/polymarket-markets-data?utm_source=polymark.et)** — **Listed:** The industry standard for onchain data. Structured, institutional-grade blockchain datasets across RWAs, stablecoins, tokens, lending, and more. 130+ chains. **Checked:** HTTP 200, page “Polymarket Markets Data \| Dune”. **Assessment:** A Dune dashboard, useful for one-off chain-data research, not a maintained API. The inspected page remained in Dune’s loading shell.

105. **[Metaforecast](https://metaforecast.org?utm_source=polymark.et)** — **Listed:** QURI is a nonprofit research organization researching forecasting and epistemics to improve the long-term future of humanity. **Checked:** HTTP 200, page “The Quantified Uncertainty Research Institute Blog”. **Assessment:** The URL now leads to QURI nonprofit research, not the original aggregation product. Forecasting research may inform methodology, but no data connector.

106. **[Zapper](https://zapper.xyz/apps/polymarket?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Wallet portfolio explorer, browser-blocked in this pass. Existing accounting is based on fills and marks, not a generic DeFi dashboard.

107. **[LayerHub](https://layerhub.xyz/protocols/polymarket?utm_source=polymark.et)** — **Listed:** Your wallet explorer **Checked:** HTTP 200, page “LayerHub”. **Assessment:** Wallet explorer with a page timestamped March 2026; stale snapshot, not real-time operational telemetry. No integration.

108. **[KuCoinVentures](https://dune.com/kucoinventures/trading-bots-on-polymarket?utm_source=polymark.et)** — **Listed:** The industry standard for onchain data. Structured, institutional-grade blockchain datasets across RWAs, stablecoins, tokens, lending, and more. 130+ chains. **Checked:** HTTP 200, page “Trading Bots on Polymarket \| Dune”. **Assessment:** Dune dashboard, useful for manual on-chain research only; no current API contract in the listing.

### 📡 Data

109. **[sealaunch](https://dune.com/sealaunch/polymarket-trending-topics?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Dune query page was blocked (403). No integration.

110. **[filarm](https://dune.com/filarm/polymarket-activity?utm_source=polymark.et)** — **Listed:** The industry standard for onchain data. Structured, institutional-grade blockchain datasets across RWAs, stablecoins, tokens, lending, and more. 130+ chains. **Checked:** HTTP 200, page “Polymarket - Activity and Volume \| Dune”. **Assessment:** Dune Polymarket activity dashboard; page renders a loading shell. Manual exploration only, not an engine feed.

111. **[Artemis](https://app.artemisanalytics.com/sectors?tab=prediction_markets&utm_source=polymark.et)** — **Listed:** AI-powered investment research and dashboards for crypto tokens, equities, stablecoins, prediction markets, and DeFi sectors. Live data, financials, and a screener cover… **Checked:** HTTP 200, page “Artemis AI — Crypto & Equity Investment Research”. **Assessment:** General market-sector research app includes prediction markets; no source/API integration for our bot demonstrated.

112. **[Blockworks](https://blockworks.com/analytics/polymarket?utm_source=polymark.et)** — **Listed:** Interactive analytics dashboards for Polymarket. **Checked:** HTTP 200, page “Polymarket \| Analytics \| Blockworks Intel”. **Assessment:** Analytics dashboard, useful for human context only. No suitable public API or feed contract established.

113. **[Dunedata](https://dune.com/dunedata/prediction-markets?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Dune dashboard blocked in the browser; no integration surface verified.

114. **[alexmccullough](https://dune.com/alexmccullough/how-accurate-is-polymarket#a-few-things-immediately-stand-out?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Dune analysis article/query blocked in the browser. Its analysis cannot be treated as a validated forecast source.

115. **[TREMOR](https://github.com/sculptdotfun/tremor)** — **Listed:** A real-time seismic monitor for prediction markets - sculptdotfun/tremor **Checked:** HTTP 200, page “GitHub - sculptdotfun/tremor: A real-time seismic monitor for prediction markets · GitHub”. **Assessment:** Source checked at 66dc911 (MIT). It ingests Gamma into Convex and shows 5m/1h/24h movement. The concept is useful; its hand-set volume and movement score thresholds are not calibrated evidence. Adapted as a read-only, in-window tape panel using our existing PostgreSQL books/trades; no score enters execution.

116. **[Goldsky](https://goldsky.com/?utm_source=polymark.et)** — **Listed:** The data and automation platform for financial products built on stablecoins. Real-time blockchain data indexing, streaming, automation, and RPC infrastructure. **Checked:** HTTP 200, page “Real-time Blockchain Data for Onchain Finance - Goldsky”. **Assessment:** General indexing/streaming/RPC infrastructure. We already consume venue feeds and persist state; another collector/warehouse is unjustified without a reconciliation gap.

117. **[Probalytics](https://probalytics.io)** — **Listed:** Full-depth order book snapshots for Polymarket since Nov 2025 and Kalshi since May 2026, stored on every change. Markets back to October 2020 and fills to January 2022.… **Checked:** HTTP 200, page “Historical Order Book & Trade Data — Polymarket & Kalshi \| Probalytics”. **Assessment:** Vendor page documents a ClickHouse/REST/Parquet full-depth historical book archive and trial/key access. Candidate for a bounded comparison/replay sample if our own archive has a measured gap; vendor completeness/provenance claims remain unverified. No runtime dependency.

### 💸 DeFi

118. **[Ostium](https://app.ostium.com/strategies?utm_source=polymark.et)** — **Listed:** Access Ostium's strategy tool to automate trades across markets. Trade perpetuals with leverage and minimal fees. **Checked:** HTTP 200, page “Automated Trading Strategies \| Ostium”. **Assessment:** Automation product for a different perpetuals venue. Out of scope; do not connect its strategy/execution layer.

119. **[Aura](https://aura.money/?utm_source=polymark.et)** — **Listed:** The fastest, cheapest way to trade Hyperliquid perps, spot, HIP-3 markets and Polymarket predictions. One wallet, lowest fees — on iOS, Android, and desktop. **Checked:** HTTP 200, page “Aura — Trade Hyperliquid & Polymarket in one app”. **Assessment:** Consumer unified trading app for Hyperliquid/Polymarket. Not a reusable data/API contract; no wallet or execution integration.

120. **[SuiBets](https://suibets.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 404. **Assessment:** The listed domain returns 404. No integration.

121. **[Gondor](https://gondor.fi/?utm_source=polymark.et)** — **Listed:** The DeFi layer for prediction markets. Use your Polymarket positions as collateral and borrow up to 50% of their current value. **Checked:** HTTP 200, page “Borrow against your Polymarket positions”. **Assessment:** The page says the product is coming soon; borrowing/leverage is out of scope.

122. **[HyperOdd](https://hyperodd.com?utm_source=polymark.et)** — **Listed:** HyperOdd provides automated liquidity agents for perpetuals and binary prediction markets on Hyperliquid and Polymarket. Powering deep, efficient markets on-chain. **Checked:** HTTP 200, page “HyperOdd: Agentic Liquidity Infrastructure”. **Assessment:** “Coming soon” liquidity/autopilot offer. No available integration; leverage/liquidity automation is out of scope.

123. **[Robin](https://robin.markets?utm_source=polymark.et)** — **Listed:** Earn yield on Polymarket positions. **Checked:** HTTP 200, page “Robin Markets”. **Assessment:** Staking/yield product for positions. Adds protocol/custody risk and does not improve prediction measurement; no integration.

### 📚 Educational Resources

124. **[PolyNoob](https://polynoob.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Browser access blocked (403); no technical surface verified. Educational use only if later accessible.

125. **[The Oracle by Polymarket](https://news.polymarket.com/?utm_source=polymark.et)** — **Listed:** News & views from the world's largest prediction market. Read this to become a better forecaster. Click to read The Oracle by Polymarket, a Substack publication with ten… **Checked:** HTTP 200, page “The Oracle by Polymarket \| Substack”. **Assessment:** Editorial/news publication, not an API or forecast ledger. Human context only.

126. **[PolymarketGuide](https://polymarketguide.gitbook.io/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Overview \| PolymarketGuide”. **Assessment:** Independent docs/guide with MCP references. Useful for API orientation; official Polymarket docs remain source of truth and no code integration is needed.

### 🔹 Extensions

127. **[PMs4X](https://chromewebstore.google.com/detail/prediction-markets-for-x/cpimmfoflnoomfabkemagplkjeaemndp?utm_source=item-share-x&utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Before you continue”. **Assessment:** Chrome extension listing was gated behind Google’s consent/challenge screen. Browser-only product; no server integration.

### 🔹 Funds

128. **[PolyFund](https://www.polyfund.so/?utm_source=polymark.et)** — **Listed:** Fully decentralized fund management platform built on top of Polymarket. **Checked:** HTTP 200, page “PolyFund \| Create and Invest in Prediction Funds”. **Assessment:** Decentralized fund/copy manager. Investment custody/performance risk; not a data source and no integration.

### 🏗️ Infrastructure

129. **[Kuest](https://kuest.com)** — **Listed:** Create your own white-label prediction market in 15 minutes. Launch under your brand, set your fees, use your domain, and start with shared liquidity from day one. **Checked:** HTTP 200, page “Kuest — Create Your Own Prediction Market”. **Assessment:** White-label market creation infrastructure, not relevant to running a Polymarket strategy engine.

130. **[SEDA](https://docs.seda.xyz/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Introduction to SEDA \| SEDA Home”. **Assessment:** Programmable oracle infrastructure for on-chain applications. We already consume reference feeds; adding an oracle network would change trust/resolution boundaries.

131. **[Dimes](https://dimes.fi?utm_source=polymark.et)** — **Listed:** 10x your users' buying power with Dimes' plug-and-play prediction market leverage solution. Dimes is the #1 most integrated leverage provider across Polymarket front-end… **Checked:** HTTP 200, page “Dimes · PM leverage”. **Assessment:** Leverage integration. Deliberately out of scope: it changes capital exposure and liquidation risk.

132. **[Compose](https://compose.build?utm_source=polymark.et)** — **Listed:** Compose by Goldsky: ship onchain apps like the web. Move data and money across your stack - offchain, onchain, or both - with durability and orchestration built in. **Checked:** HTTP 200, page “Compose by Goldsky — The Offchain-to-Onchain Framework”. **Assessment:** Offchain-to-onchain workflow infrastructure, unnecessary for local read-only research and bot monitoring.

133. **[Baozi.bet](https://baozi.bet)** — **Listed:** Trade prediction markets on Solana with SOL. Pari-mutuel betting on crypto, sports, politics, culture. 68 MCP tools for AI agents. On-chain settlement. **Checked:** HTTP 200, page “Baozi - Prediction Markets on Solana”. **Assessment:** Separate Solana pari-mutuel venue/agent API. Not compatible with Polymarket CLOB positions/accounting; no integration.

134. **[OrderbookTrade](https://www.orderbook.trade)** — **Listed:** One API to trade, make markets, and provide liquidity across every prediction market. Built for AI agents, market makers, and platforms. **Checked:** HTTP 200, page “OrderbookTrade — The Execution Layer for Prediction Markets”. **Assessment:** Hosted unified execution layer. Replacing our execution path would add a critical third-party trust boundary; no integration without a separately approved adapter audit.

### 📰 News

135. **[PROPHET](https://www.prophetnotes.com/?utm_source=polymark.et)** — **Listed:** Join PROPHET NOTES to get reliable and straight to the point commentary on global events with skin in the game. Click to read PROPHET NOTES, by Mike S., a Substack publi… **Checked:** HTTP 200, page “PROPHET NOTES \| Mike S. \| Substack”. **Assessment:** Editorial newsletter, not a machine-readable market dataset. Human context only.

136. **[DeepNewz](https://deepnewz.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Browser-blocked (403); current source/terms could not be verified. News integration is not justified.

137. **[Prediction News](https://predictionnews.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Cloudflare blocked access; no API or current content verified.

138. **[Stocktwits](https://stocktwits.com?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** General social feed, browser-blocked in this pass. Social sentiment is noisy and no validated link to our strategies exists.

139. **[Boring News](https://www.notboring.co/?utm_source=polymark.et)** — **Listed:** Tech strategy, analysis, and philosophy, but not boring. Click to read Not Boring by Packy McCormick, a Substack publication with hundreds of thousands of subscribers. **Checked:** HTTP 200, page “Not Boring by Packy McCormick \| Substack”. **Assessment:** General editorial newsletter, not a Polymarket data API. Human context only.

### 🏛️ Official

140. **[Polymarket Live Ticker](https://ticker.polymarket.com?utm_source=polymark.et)** — **Listed:** Add live prediction market odds to your livestream or website. Easy OBS integration for streamers and content creators. **Checked:** HTTP 200, page “Polymarket Livestream Ticker - Add a Polymarket Ticker to your livestream or website”. **Assessment:** Official ticker page says it refreshes every five minutes; too slow for short-window execution and unnecessary for our direct CLOB snapshots.

### 🟪 Others

141. **[Kaito](https://kaito.ai/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Social-mindshare product, browser-blocked; no verifiable feed or strategy relevance. No integration.

142. **[Cookie fun](https://www.cookie.fun/tokens/polymarket?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Token/social analytics page blocked; no verified market-data API or use case.

143. **[Prediction Index](https://predictionindex.xyz/?utm_source=polymark.et)** — **Listed:** We map, rank, and help you discover the world of prediction markets. **Checked:** HTTP 200, page “Prediction Index”. **Assessment:** Directory/index of the ecosystem and market beliefs, not a data connector. Could be useful for occasional ecosystem discovery.

144. **[Dexu](https://dexu.ai/project/polymarket?id=2037&utm_source=polymark.et)** — **Listed:** Track Polymarket crypto mindshare, social sentiment, KOL mentions and market data on Dexu — updated multiple times daily across X, YouTube, TikTok, Reddit and Telegram. **Checked:** HTTP 200, page “Polymarket Mindshare, Sentiment & Social Analytics \| Dexu”. **Assessment:** Social mindshare/mentions are updated multiple times daily per its page; not suitable as real-time bot input. No integration.

145. **[Prediction Markets Directory](https://frontseat.co/prediction-markets?utm_source=polymark.et)** — **Listed:** Explore innovative projects from Colosseum hackathons. Browse by tracks, teams, and engagement metrics. **Checked:** HTTP 200, page “404: This page could not be found.”. **Assessment:** The listed URL renders a 404 page. No integration.

146. **[Polyteller](https://polyteller.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Polyteller \| Smart Tools for Polymarket Traders”. **Assessment:** Browser extension for timing/privacy/safety UX, not backend telemetry. Could be a product-UX reference, no code dependency.

147. **[UMA rocks](https://www.uma.rocks/?utm_source=polymark.et)** — **Listed:** Get 21% APR by delegating your UMA voting power to experienced voters, without us ever touching your tokens. **Checked:** HTTP 200, page “Earn from Polymarket disputes”. **Assessment:** UMA voting delegation/yield product is unrelated to trading analytics; it changes staking exposure, no integration.

148. **[PolyRewards](https://polyrewards.netlify.app?utm_source=polymark.et)** — **Listed:** Build and deploy your own site for free. **Checked:** HTTP 404. **Assessment:** The listed Netlify deployment returns 404. No integration.

149. **[PolyFakeIt](https://www.polyfakeit.com/?utm_source=polymark.et)** — **Listed:** Trade on your predictions. Make informed decisions with real-time market data. **Checked:** HTTP 200, page “Polymarket Clone”. **Assessment:** Mock Polymarket clone/demo, not a real market or data source. The page includes placeholder-like credential text; do not use it as a credential source or copy its demo data.

150. **[Fake-A-Polymarket](https://fake-a-polymarket.com?utm_source=polymark.et)** — **Listed:** Popular generator tool for creating realistic fake Polymarket prediction market charts with customizable odds, volatility, volume data, and chart patterns for marketing,… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** Synthetic chart generator for demos/education; generated series must never enter replay, calibration or production dashboards.

151. **[Liquid](https://protocol.useliquid.xyz?utm_source=polymark.et)** — **Listed:** Never lose money on your conviction **Checked:** HTTP 200, page “Liquid”. **Assessment:** Insurance/hedging protocol, not a market-data efficiency tool. Adds external smart-contract risk; no integration.

152. **[PolyHedg](https://polyhedg.com/?utm_source=polymark.et)** — **Listed:** Hedge your bets with Polyhedg **Checked:** HTTP 200, page “Polyhedg - Hedge Anything”. **Assessment:** Hosted hedge-generation product. Its displayed example does not establish executable hedges or venue-compatible settlement; no integration.

### 🔹 Parlays

153. **[Clutch](http://ape.clutch.market?utm_source=polymark.et)** — **Listed:** Decentralized Parlay Platform **Checked:** HTTP 200, page “Clutch”. **Assessment:** Parlay platform says it is migrating to V4, with details forthcoming. Out of scope and not currently a stable API.

154. **[PredictShark](https://www.predictshark.io/?utm_source=polymark.et)** — **Listed:** Real-time insights into Polymarket trading activity, market trends, and trader performance. **Checked:** HTTP 200, page “Predict Shark Analytics”. **Assessment:** Page says analytics are coming soon and markets are still loading. No integration.

155. **[BetStack](https://alpha.betstack.app?utm_source=polymark.et)** — **Listed:** Place bets on football and basketball matches with live odds. Build parlays, track your portfolio, and win on your predictions. **Checked:** HTTP 200, page “Betstack - Build parlays on Football, Basketball, and more”. **Assessment:** Sports-parlay consumer app; outside current product scope.

### 🔹 Portfolio Tracking

156. **[Polymarket Bros](http://brosonpm.trade?utm_source=polymark.et)** — **Listed:** Community-focused whale tracking and copy trading platform monitoring Polymarket trades over $4,000 in real-time, featuring one-click trade replication, trader identific… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; status unknown. Copy trading is not a substitute for verified after-fee strategy evidence.

157. **[PolyTracker](https://t.me/polytracker0_bot?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Telegram: Contact @polytracker0_bot”. **Assessment:** Telegram portfolio bot only in the inspected surface; no API/accounting contract established.

158. **[Polylerts](https://t.me/Polylerts_bot?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Telegram: Contact @Polylerts_bot”. **Assessment:** Telegram alert bot only; no machine-readable feed or audit evidence. No integration.

159. **[Polycool](https://polycool.live/?utm_source=polymark.et)** — **Listed:** The mobile app to trade Polymarket: every market, copy-trading and parlays, from your phone. Non-custodial. Certified Polymarket Partner. **Checked:** HTTP 200, page “Polycool — Trade Polymarket from your phone \| iOS & Android”. **Assessment:** Mobile consumer trading/copy-trading beta. UX can be observed as inspiration, but the app is not a server feed and no wallet/execution integration is appropriate.

### 🤖 Trading Bots

160. **[Polycule](https://www.polycule.trade/?utm_source=polymark.et)** — **Listed:** The #1 Polycule trading bot. **Checked:** HTTP 200, page “polycule”. **Assessment:** Trading/copy bot product; public source and strategy performance were not established. No bot-to-bot integration.

161. **[Polymtrade](https://polym.trade/?utm_source=polymark.et)** — **Listed:** Polym.trade is the first mobile trading terminal for Polymarket with AI-powered insights. **Checked:** HTTP 200, page “Prediction markets on Polymarket — Polymtrade”. **Assessment:** Mobile trading terminal with AI insights; user product, not a data contract. No wallet or execution integration.

162. **[okbet](https://tryokbet.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “okbet — Trade smarter across every prediction market”. **Assessment:** Cross-platform consumer terminal. No public schema or vetted signal feed; no integration.

163. **[Predictify](https://t.me/Predictify_bot?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Telegram: Contact @Predictify_bot”. **Assessment:** Telegram trading bot; no source/API, custody model or independently verified performance established. No integration.

164. **[Fireplace](https://fireplace.gg/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 403; blocked/rate-limited. **Assessment:** Browser access blocked (403); service details unverified. No integration.

165. **[Based](https://app.based.one/predict?utm_source=polymark.et)** — **Listed:** Explore and trade on prediction markets across various categories **Checked:** HTTP 200, page “Prediction Markets \| Based Trading \| Based”. **Assessment:** Hosted multi-category prediction-market frontend, not an engine API. No integration.

166. **[Berry](https://berryinvesting.com/?utm_source=polymark.et)** — **Listed:** Trade real stocks, crypto, and prediction markets, all from $1 with zero commission. Fund instantly with stablecoins or local currency. **Checked:** HTTP 200, page “Berry: Invest in Stocks From $1, Zero Commission”. **Assessment:** The checked domain is now a general stock-investing landing page; it does not expose the listed prediction-market connector. No integration.

167. **[Rainmaker](https://rainmaker.fun/?utm_source=polymark.et)** — **Listed:** Rainmaker's Backspin+ AI trades live tennis on Polymarket from your wallet — public settled track record at the US Open, wins and losses included. **Checked:** HTTP 200, page “AI Tennis Trading on Polymarket \| Rainmaker”. **Assessment:** Sports-specific AI tennis trading service. Its public-track-record statement is vendor-provided and strategy-specific; unrelated to current assets, no code or wallet integration.

168. **[PolyBot](https://polybot.trading/?utm_source=polymark.et)** — **Listed:** PolyBot is a Polymarket trading bot for Telegram. Analyze smart wallets, copy top traders, automate orders, set risk controls, and trade from a Safe wallet. **Checked:** HTTP 200, page “PolyBot \| Polymarket Trading Bot, Wallet Analyzer & Smart Wallets”. **Assessment:** Telegram trading/copy bot and wallet analyzer. No verified public feed or reason to entrust execution; no integration.

169. **[Polyburg](https://polyburg.com/?utm_source=polymark.et)** — **Listed:** Automatic copy-trading for Polymarket, inside Telegram. Follow proven traders, invest in managed Presets, or trade every market yourself — built on the smart-wallet inte… **Checked:** HTTP 200, page “Polyburg — Copy Polymarket's Best Traders, Automatically”. **Assessment:** Telegram copy-trading product. Trader-selection and copying have timing/fees/capacity dependencies; do not connect to bot entries.

170. **[Rocket](https://userocket.app/?utm_source=polymark.et)** — **Listed:** cutting-edge prediction market aggregator designed to give you the edge in forecasting and market analysis. **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** Browser navigation hit an authentication/error page, not an inspectable API. No integration.

171. **[Datalayer](https://datalayer.xyz/?utm_source=polymark.et)** — **Listed:** AI companion platform for onchain trading across memes, perps, yield farming, and prediction markets. Powered by Hyperliquid, Polymarket, Solana, BSC and Base. **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; broad on-chain agent product, not an established market data connector.

172. **[Predicton](https://predicton-guide.gitbook.io?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Links \| Predicton Docs”. **Assessment:** Documentation is a link directory for Telegram product/community; no stable machine data API established.

173. **[PolyScope Bot](https://polyscope.gitbook.io?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Polymarket Wallet Tracker Bot \| PolyScope Docs”. **Assessment:** Docs describe a wallet-tracker bot; no data/API contract for engine use. Human monitoring only.

174. **[PolyFocus](https://t.me/polyfocusbot?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Telegram: View @polyfocusbot”. **Assessment:** Telegram channel/bot surface, no public structured API or independently verified signals.

175. **[PolyXBot](https://www.polyxbot.org/?utm_source=polymark.et)** — **Listed:** Advanced Solana-native Telegram bot that enables seamless Polymarket trading with AI-driven market analysis, cross-chain bridging, and comprehensive portfolio management… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/connection unresolved; status unknown. Its described Solana-native bridge/trading function is outside project scope.

176. **[AIXBET](https://www.aixbet.ai/?utm_source=polymark.et)** — **Listed:** Turn prediction markets into profit with the AIxBET Smart Money Terminal **Checked:** HTTP 200, page “AIXBET - YOUR SMART MONEY COPILOT”. **Assessment:** Hosted smart-money/AI terminal. No independent validation or machine contract established; no bot inputs.

177. **[Converge](https://converge.market?utm_source=polymark.et)** — **Listed:** First prediction market aggregator and professional trading terminal unifying fragmented markets across Polymarket, Kalshi, and Limitless into one seamless platform feat… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; no service/API could be inspected. Do not infer availability from listing claims.

178. **[Sharpe Terminal](https://beta.sharpeterminal.com/?utm_source=polymark.et)** — **Listed:** The list provides no description. **Checked:** HTTP 200, page “Sharpe Terminal”. **Assessment:** Hosted general trading terminal with order/TWAP/iceberg UI. It is an alternate execution product, not a proven data enhancement; no integration.

179. **[Polylayer](https://polylayer.xyz?utm_source=polymark.et)** — **Listed:** Comprehensive Layer 2 prediction finance ecosystem built on Polymarket featuring advanced trading terminals, leveraged trading, yield generation, automated strategies, a… **Checked:** Navigation/DNS/SSL/connection unresolved. **Assessment:** DNS/navigation unresolved; leveraged DeFi layer is outside scope and unverified.

## Implementation and verification notes

This audit adds the authenticated, read-only `api_v5_market_motion` endpoint and a responsive “Window tape” panel to the actual `/v5` Markets page in `scripts/cockpit_v5.html`. The API accepts at most 16 validated condition IDs, queries existing observations inside each current market window, and leaves missing book history unavailable. The panel plots UP-midpoint samples in 15-second buckets, leaves visible gaps where buckets are missing, and shows first/latest midpoint, movement, book age, and stored public-trade count/notional across both outcomes. This is descriptive visibility only; it does not affect forecasts or bot decisions. No database table, external API, credential, strategy gate, or live-mode behavior was added.

**Validation performed:** the new focused API unit tests pass (4/4) with `./.venv-cockpit/bin/python tests/test_cockpit_market_motion.py`; `python3 -m py_compile scripts/cockpit.py`; the actual inline JavaScript in `scripts/cockpit_v5.html` passes `node --check`; and `git diff --check` passes. Playwright rendered the actual `/v5` page against mock prediction/tape responses at desktop and 390px width; the tape became one column and document width stayed at 390px. A bounded, read-only database-backed call for one active market returned the expected schema but no stored book/trade observations for that window. A planner-only `EXPLAIN` selected indexes for the market, book, and trade lookups; no `EXPLAIN ANALYZE` latency claim is made. This validates one database sample, not fleet-wide capture coverage. The authenticated HTTP route, full Android/C++ build, and running cockpit service were not exercised. The running service was left untouched. The browser status sample is dated 2026-10-25 and can change.
