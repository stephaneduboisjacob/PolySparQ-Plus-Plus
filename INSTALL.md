# Installation and development

This guide covers the C++ engine, local cockpit, PostgreSQL, and mobile client. Commands assume Ubuntu or Debian and a checkout at the repository root.

## Requirements

- C++20 compiler, CMake 3.20+, Ninja, pkg-config, and Boost headers.
- Development packages for libpqxx, simdjson, notcurses++, libcurl, libsecp256k1, fmt, spdlog, libsodium, and OpenSSL.
- PostgreSQL for persistent state.
- Python 3.11+ for the cockpit server and Python tools.
- Node.js 20.19 or later and npm for the Expo SDK 56 mobile app, per the [official SDK reference](https://docs.expo.dev/versions/v56.0.0/).
- Android Studio and the Android SDK for native Android builds. iOS builds require macOS and Xcode.

The CMake project checks for its native libraries during configuration. If a package name differs on your distribution, install the development package that provides the matching `pkg-config` module.

## 1. Install native dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config git libboost-dev \
  libpqxx-dev libsimdjson-dev libnotcurses++-dev \
  libcurl4-openssl-dev libsecp256k1-dev libfmt-dev libspdlog-dev \
  libsodium-dev libssl-dev postgresql postgresql-client \
  python3 python3-venv python3-pip
```

Confirm the CMake dependencies are visible:

```bash
pkg-config --modversion libpqxx simdjson notcurses++ libcurl \
  libsecp256k1 fmt spdlog libsodium
```

## 2. Create the local database

The included helper creates the default database if needed and applies the numbered schema migrations in order:

```bash
# If your operating-system account has no PostgreSQL role, create a database-only role once:
sudo -u postgres createuser --createdb "$(id -un)"
./scripts/setup_db.sh
```

Skip `createuser` if that role already exists. The application does not need a PostgreSQL superuser.

The historical database name is `vantagepulse` and the schema namespace is `vp`. Both are retained for compatibility. To use a different database name, set `DB` when applying the schema and update `[postgres].conninfo` in your local config.

## 3. Configure paper mode

Create a local config from the checked-in example:

```bash
cp config/polypulse.toml.example config/polypulse.toml
```

The example selects `paper` mode, disables the optional LLM advisor, and contains no wallet or API credentials. Keep your runtime file out of Git. The root `.gitignore` excludes local TOML configurations and secret files.

Before starting the engine, confirm PostgreSQL is available and `conninfo` points to your local database. The engine connects to external market-data endpoints; it does not need a wallet in paper mode.

## 4. Build and run the engine

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bin/polypulse --config config/polypulse.toml
```

The terminal interface exits with `q` or Ctrl-C. Logs and runtime state are written to ignored local directories.

To build and run the available test suite:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Some persistence tests skip unless a test-only PostgreSQL DSN is configured. Never point tests at a production database.

## 5. Run the cockpit

Install the Python server requirements in a virtual environment:

```bash
python3 -m venv .venv-cockpit
. .venv-cockpit/bin/activate
python -m pip install -r scripts/requirements-cockpit.txt
```

Run the cockpit bound to loopback while developing:

```bash
python scripts/cockpit.py --host 127.0.0.1 --port 8888
```

Open <http://127.0.0.1:8888/>. The cockpit reads the local PostgreSQL database and local configuration. Fleet controls that manage systemd units require a separately configured Linux user service environment. Do not expose the development server directly to the internet; use a secured reverse proxy and a reviewed authentication setup for remote access.

## 6. Run the mobile client

```bash
cd mobile
npm ci
npm run start
```

Enter the URL of your own cockpit server on the login screen. You may set `EXPO_PUBLIC_POLYSPARQ_SERVER_URL` in an ignored local `.env.local` file to provide a development default. Do not commit production endpoints or Firebase configuration.

For an Android development build, install the Android SDK and run:

```bash
npm run android
```

Firebase push notifications are optional. The Expo config enables their native plugins only when local `google-services.json` and `GoogleService-Info.plist` files are present. Obtain those files from your own Firebase project; they are intentionally excluded from this repository.

The Android application ID, iOS bundle ID, URL scheme, and secure-storage key names retain their existing values to preserve compatibility with installed builds. Changing them is a separate app migration.

## Troubleshooting

- **CMake cannot find a dependency:** install its development package and check the matching `pkg-config` module name shown in the error.
- **PostgreSQL connection fails:** check `pg_isready`, the database name, and the local PostgreSQL authentication rules.
- **Mobile app cannot reach the cockpit:** a phone's `localhost` is the phone itself. Use a reachable development host or a secured server URL.
- **Firebase build configuration is missing:** this is expected unless you supply credentials for your own Firebase project; push notifications remain disabled.

## Contact

Stephane Jacob · [jacobstephane@outlook.com](mailto:jacobstephane@outlook.com)
