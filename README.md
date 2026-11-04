# PolySparQ++: Polymarket Bot and Research Toolkit

<p align="center">
  <img src="mobile/assets/polysparq-wordmark.png" alt="PolySparQ++" width="560">
</p>

**PolySparQ++ is an independent C++20 Polymarket trading bot and prediction-market research toolkit.** It combines Polymarket CLOB API market data and order execution with risk controls, a Python operations cockpit, PostgreSQL persistence, replay tools, and a React Native companion app.

> **Research software.** The project is experimental and can place real orders when explicitly configured to do so. Prediction-market trading can lose all funds committed to a position. Start in paper mode, review every setting, and do not treat this project as financial advice or a promise of returns.

PolySparQ++ is source-available under the [PolyForm Noncommercial License 1.0.0](LICENSE). Any commercial use requires a separate written commercial license. See [Commercial licensing](COMMERCIAL-LICENSING.md).

This is a source-available project, not a permissively licensed open-source release. Noncommercial use is governed by the published license; commercial rights are reserved for a separate agreement.

## Polymarket bot features

- A C++20 Polymarket bot engine for market feeds, strategy evaluation, risk checks, paper trading, and optional live order execution.
- Polymarket CLOB API, order-book, and market-data integration, with additional reference-price feeds for research and strategy evaluation.
- A Python web cockpit for inspecting markets, positions, strategy state, and fleet activity.
- PostgreSQL schemas and migrations for trading and research data.
- A React Native / Expo mobile client for connecting to a PolySparQ++ server.
- C++ and Python tests, plus configuration examples that keep credentials out of the repository.

The codebase is intended for developers exploring Polymarket bots, prediction-market data, paper trading, and execution or risk-control systems. The example configuration starts in paper mode; live execution requires deliberate local configuration and credentials.

## Market-book storage controls

Book snapshots are optional, high-volume research data. The `polypulse` engine
and `polypulse-collector` each record at most one full-book snapshot per token
per second when
`data_collection.book_snapshots_enabled = true`. Set
`data_collection.book_snapshot_levels` from `0` to `20` to choose how much of
each side's order book is retained: `0` preserves best bid/ask, midpoint, spread,
and summary fields without depth arrays; `1` to `19` stores a shallow ladder;
`20` keeps the full top-20 ladder and remains the engine default. Reducing this
value does not change the live in-memory order book or execution logic, but it
reduces the depth available to historical liquidity and replay analysis. The
example profiles that enable book capture use five levels per side; review the
storage tradeoff before enabling capture on a long-running instance. The
collector seeds each token from a full `book` event, then applies subsequent
price-change deltas before saving; it does not treat an individual changed
level as a complete order-book snapshot. Snapshot timestamps are captured
alongside the in-memory sample and preserved through the asynchronous database
queue, so queue delay does not make old market data appear newly observed.

The product branding is PolySparQ++. A few internal executable, database, and configuration identifiers retain their historic names for compatibility with existing installations.

## Build the Polymarket bot

See [INSTALL.md](INSTALL.md) for the complete native dependency list, database setup, mobile build steps, and troubleshooting.

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libboost-dev libpqxx-dev libsimdjson-dev libnotcurses++-dev \
  libcurl4-openssl-dev libsecp256k1-dev libfmt-dev libspdlog-dev \
  libsodium-dev libssl-dev postgresql postgresql-client

./scripts/setup_db.sh
cp config/polypulse.toml.example config/polypulse.toml
# Edit config/polypulse.toml. Keep [runtime].mode = "paper".

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bin/polypulse --config config/polypulse.toml
```

The executable and default database identifiers are retained for compatibility. They do not change the PolySparQ++ product name.

## Repository guide

| Path | Purpose |
| --- | --- |
| `src/` | C++ engine, feeds, execution, risk, persistence, replay, and TUI |
| `scripts/cockpit.py` | Python cockpit server |
| `scripts/cockpit_ui.html` | Cockpit interface |
| `sql/` | PostgreSQL schema and migrations |
| `tests/` | C++ and Python tests |
| `mobile/` | Expo / React Native client |
| `config/polypulse.toml.example` | Minimal paper-mode configuration template |

## Prediction history archive

The root cockpit's Predictions tab records per-window forecasts in PostgreSQL
(`vp.prediction_history_windows` and `vp.prediction_history_checkpoints`). Each
market window has checkpoints for 60 seconds before open, open, and 20%, 40%,
60%, and 80% elapsed, plus the eventual market result. The server captures these
independently of whether a browser has the Predictions tab open. Resolved
outcomes come from Polymarket's resolved market data; opening and closing
reference prices are recovered from the stored Chainlink oracle ticks.

On first start, the cockpit imports existing independent-model snapshots from
its old minute-bucket journal. It keeps only captures close to the requested
checkpoint and displays the actual capture time; checkpoints with no genuine
historical forecast remain marked “not recorded.” The archive has no retention
cleanup. The page shows 50 windows at a time and supports older pages.

The schema is also applied by `scripts/setup_db.sh` through
`sql/029_prediction_history.sql`.

## Scalper impulse research

When a lead scalper is enabled, the engine records a shadow-only impulse
candidate from Binance top-of-book order flow and volatility-scaled 250/500ms
price movement. It also records one feature sample per second and follows the
selected Polymarket best bid for five seconds. These observations never submit
orders. The `lead_lag_scalp_impulse_only` profile can consume the same detector
for a separate BTC 5m PAPER challenger; it does not change the production
Coinbase/model entry path. Its maker orders still use the configured risk and
exit safeguards.

Review the PAPER shadow observations with:

```bash
./.venv-cockpit/bin/python scripts/review_scalper_impulse_shadow.py --hours 24
```

The report separates bot instances, sessions, assets, and horizons. Its
chronological 70/30 check keeps every candidate from a market window in the
same split. Treat small samples as inconclusive. The signal must show enough
later, independent windows before anyone should draw a profitability conclusion.
A quote touching the target or stop does not establish a maker fill; venue queue
position remains unknown. The shadow report is not realized P&L.

Audit simulated maker fills against the public same-token trade tape with:

```bash
./.venv-cockpit/bin/python scripts/review_paper_maker_prints.py --hours 6 --current-sessions
```

The report checks for a same-direction price-crossing print around each paper
fill and, when available, its BBO-inferred aggressor. A matching print still
does not prove queue priority or a fill; no nearby print can also reflect tape
gaps. Add `--current-sessions` to exclude fills from before each bot's current
PAPER process began; this is the right view for telemetry enabled in newer
sessions. The 5m and 15m baseline configs persist that tape for new sessions.

## Article-reference paper experiment

The local `btc-5m-article-sim-test` configuration is a separate paper-only
reference run: one 5-share BTC UP maker entry at $0.49, held until a $0.70
maker target fills or the final 30 seconds. At that boundary the target is
cancelled and the engine may post a fresh-book, post-only sell one tick above
the bid; if no valid maker price exists or it does not fill, the position stays
open for paper settlement. The exit does not cross the spread or pay taker fees.
The current local risk caps allow at most $2.45 per window and $4.90 total
deployed across overlapping windows. Paper book touches do not model venue queue
priority, and this experiment is not evidence of profitability.

## Safety and responsible use

- Paper mode is the default in the example configuration. Do not switch to live mode until you understand the code, venue rules, and operational risks.
- Never commit wallet private keys, exchange credentials, API tokens, database dumps, customer data, or local `.env` files.
- The software is provided without warranties. You are responsible for legal eligibility, market access, security, tax treatment, and compliance in your jurisdiction.
- PolySparQ++ is independent software and is not affiliated with Polymarket, Binance, Coinbase, Polygon, Expo, or any market operator.

## Contributing and security

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. For a private vulnerability report, use the contact address in [SECURITY.md](SECURITY.md); do not post credentials or exploitable details in a public issue.

## Contact

Stephane Jacob · [jacobstephane@outlook.com](mailto:jacobstephane@outlook.com)

Copyright © 2026 Stephane Jacob.
