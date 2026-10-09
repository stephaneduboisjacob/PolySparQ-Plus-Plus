# Architecture

PolySparQ++ is a multi-component application for researching and automating prediction-market workflows. Its components can be built and operated independently during development.

## Components

1. **C++ engine (`src/`)** — connects to Polymarket market data and reference feeds, maintains in-memory market state, evaluates configured strategies, applies risk checks, and routes actions to paper or live execution.
2. **PostgreSQL (`sql/`, `src/db/`)** — stores market observations, orders, fills, and research records. The `vp` schema and historical database name are retained for compatibility.
3. **Cockpit (`scripts/cockpit.py`, `scripts/cockpit_ui.html`)** — a Python HTTP server and browser UI for observing engine and fleet state. Some fleet-management features expect a Linux systemd user session.
4. **Mobile client (`mobile/`)** — a React Native / Expo client that connects to a cockpit URL supplied by the operator.
5. **Replay and research tools (`src/engine/`, `scripts/`)** — local utilities for inspecting historical data and evaluating research workflows.

## Data flow

```text
Market feeds ──> C++ engine ──> strategy and risk checks ──> paper/live executor
                      │                                  │
                      └────────────> PostgreSQL <────────┘
                                         │
                                         v
                              Python cockpit / mobile UI
```

The engine and cockpit use local configuration files and PostgreSQL. Credentials, account state, databases, logs, and deployment-specific service settings are operator-managed and are not part of the public source tree.

## Build boundaries

- CMake builds the C++ engine, feed proxy, collector, replay utilities, and tests.
- Python dependencies for the cockpit are listed in `scripts/requirements-cockpit.txt`.
- The mobile app has its own `package.json` and lockfile under `mobile/`.
- `src/third_party/toml.hpp` is vendored toml++; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The product display name is PolySparQ++. Internal executable, database, config, service, and storage-key identifiers are kept where changing them would break existing installations.

## Contact

Stephane Jacob · [jacobstephane@outlook.com](mailto:jacobstephane@outlook.com)
