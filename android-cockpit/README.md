# PolySparQ Observatory for Android

A new, standalone native Android cockpit for the root PolySparQ++ cockpit. The app is built with Kotlin and Jetpack Compose and connects to the existing cockpit API; it does not change or replace the repository's existing mobile app.

## Cockpit views

The phone-first navigation keeps Overview, Predictions, Markets, and Bot fleet one tap away. **More** opens the remaining views without squeezing eleven tabs into a horizontal strip:

- Overview
- Positions
- Trade history
- Execution
- Markets
- Predictions
- Bot fleet
- Window bots
- Strategies
- Engine lab
- Activity logs

The overview combines account mode and health, position and market snapshots, recent fills, a P&L chart, and the operator radar. Other views provide the corresponding API-backed tables, cards, summaries, and charts. Cards, chart labels, and controls use larger touch targets and type for phone viewing. Sections refresh while visible; the server event stream updates prices and recent fills. The screen pauses its polling and stream while the app is in the background.

Predictions has dedicated 5-minute/15-minute, market, and Current/Next/Both selectors. Each current or next window shows the model probability, market midpoint, model-to-market gap, reference, and input freshness. Open **View [market] history** on a card to inspect saved window outcomes and the latest captured probability checkpoints. History is fetched for that market and timeframe and refreshes in the background while the history panel is open.

The app is read-only. It displays the execution mode reported by the server and includes no order entry, strategy mutation, or LIVE-mode switch.

## Connect to the cockpit

1. Make the existing root cockpit reachable from the Android device.
2. Enter the cockpit base URL, username, and password in the app. Do not add credentials to the URL.
3. Remote connections must use HTTPS with a certificate trusted by Android. Plain HTTP is enabled only for `localhost`, `127.0.0.1`, and the Android emulator host `10.0.2.2`.
4. On Android 17, the app requests the system's local-network permission for private IPs and local hostnames. Turn on **Private-network cockpit** for an internal hostname that resolves to a private address but does not look local. You can change this choice in connection settings.
5. Sign in with the same account used for the cockpit. The server remains responsible for account and administrator access checks; restricted sections show the data that account can access.

The address is intentionally not preconfigured. Credentials are stored encrypted with Android Keystore. Backup rules exclude the sign-in preferences from both cloud backup and device transfer. Logging out clears the saved credential record.

## API integration

The app authenticates against `/api/whoami`, polls `/api/live_lite`, and consumes `/api/stream`. The eleven views use the root cockpit endpoints already implemented by the repository, including activity, execution, markets, prediction, prediction history, settlement, fleet, window-bot, strategy, predictor, performance, and log feeds. The client requests JSON and uses HTTP Basic authentication over the configured secure connection.

No new server process is required. The cockpit server must be running and reachable; an inaccessible endpoint, rejected login, account restriction, or empty server feed is shown as such in the app.

## Build

Requirements: JDK 17, Android SDK platform 37, and Android Build Tools 36.0.0. The Gradle wrapper downloads and uses Gradle 9.8.1.

```bash
./gradlew :app:assembleDebug
```

The debug APK is written to:

```text
app/build/outputs/apk/debug/app-debug.apk
```

Install on a connected Android device with:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The application ID is `io.polypulse.observatory`.
