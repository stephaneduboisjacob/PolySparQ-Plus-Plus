"""Reference-aware board with a shared settlement kernel and scored models.

Network metadata is cached across visitors. Candle queries use the existing
(symbol, ts) index; the five-year archive is never scanned by an HTTP request.
"""
import json
import math
import os
import sqlite3
import subprocess
import time
import threading
import copy
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlencode

_cache = {}
_cache_lock = threading.Lock()
_quality_cache = {}
_quality_cache_lock = threading.Lock()
QUALITY_SUMMARY_REFRESH_SECONDS = 30.0
ASSETS = ('BTC', 'ETH', 'SOL', 'XRP')
INDEPENDENT_MODEL_VERSION = 'vantagepulse-independent-v1'
MODEL_VERSION = INDEPENDENT_MODEL_VERSION
MARKET_BENCHMARK_VERSION = 'market-benchmark-v5'
TICK_MODEL_VERSION = 'vantagepulse-tick-5m-v1'
SHADOW_CALIBRATION_VERSION = 'multisource-v4'
SERVER_REFRESH_SECONDS = 1.0
SNAPSHOT_STALE_SECONDS = 3.0
MIN_REFRESH_GAP_SECONDS = 0.05

# Fixed, CLOB-free 5-minute cross-feed correction. Fit on resolved
# reference-v3 checkpoints through 2026-10-01, then scored on the later
# market-benchmark-v5 checkpoint set (2026-10-05..09). The 15-minute and
# provisional forecasts stay on the settlement kernel; their tick correction
# did not survive the same chronological holdout.
_TICK_5M_INTERCEPT = -0.02298204813944169
_TICK_5M_KERNEL_LOGIT = 0.8895640438993003
_TICK_5M_MEANS = (-0.08534611608413564, -0.11399369884483057,
                  3.6612572379982042, 0.14935617321322972,
                  0.9914938304016803, 0.9489104751903387,
                  0.9904961932265687, 0.9508532423208191)
_TICK_5M_SCALES = (3.0430929665938433, 3.1021673522994617,
                   2.6683539205081916, 1.0228320507307487,
                   0.0918358028063144, 0.22018034713472265,
                   0.09702311286619156, 0.21617435992454478)
_TICK_5M_WEIGHTS = (0.07423895323941519, 0.019974643436103598,
                    0.03700499510676714, -0.011664737155675366,
                    0.007560988398551871, 0.015020063289023845,
                    -0.032033292240092257, -0.005424155529094445)
_TICK_5M_TRAINED_THROUGH = 1790836323  # 2026-10-01 06:32:03 UTC
_TICK_5M_TRAIN_N = 19045
_TICK_5M_HOLDOUT_N = 3438
_TICK_5M_HOLDOUT_START = 1791158400  # 2026-10-05 00:00:00 UTC
_TICK_5M_HOLDOUT_END = 1791590400    # 2026-10-10 00:00:00 UTC
_TICK_5M_HOLDOUT_BRIER = 0.168435
_TICK_5M_KERNEL_BRIER = 0.169288
_TICK_5M_CLOB_BRIER = 0.164335


def age_board(board, now_ms=None, stale_after_s=SNAPSHOT_STALE_SECONDS):
    """Invalidate old recommendations even when refresh fails or a window rolls."""
    out = copy.deepcopy(board)
    now_ms = int(time.time()*1000) if now_ms is None else now_ms
    age = max(0, (now_ms-out.get('inputs_at', out['generated_at']))/1000)
    out['snapshot_age_s'] = age
    out['stale'] = age > stale_after_s
    for current in out.get('cards', []):
        for card in (current, current.get('next_window')):
            if not card:
                continue
            card['seconds_left'] = max(0, (card['window_close']-now_ms)//1000)
            if card.get('provisional'):
                card['seconds_until_open'] = max(0, (card['window_open']-now_ms)//1000)
            for field in ('reference_age_s','candle_age_s','book_age_s','coinbase_age_s','binance_age_s'):
                if card.get(field) is not None and card[field] >= 0:
                    card[field] += age
            status = None
            if card.get('provisional') and now_ms >= card['window_open']:
                status = 'awaiting_open_reference'
            elif now_ms >= card['window_close']:
                status = 'closed'
            elif not card.get('provisional') and 0 < card['seconds_left'] <= card.get('settlement_window_s',0):
                status = 'averaging_window'
            elif (age > stale_after_s or card.get('reference_age_s', 0) > 5 or
                  card.get('candle_age_s', -1) < 0 or card.get('candle_age_s', 0) > 120):
                status = 'stale_data'
            if status:
                card.update(model_status=status, direction='WAIT', opportunity=None,
                            model_edge=None, conservative_edge=None, model_market_gap=None,
                            independent_direction='WAIT', independent_model_edge=None,
                            independent_conservative_edge=None, independent_market_gap=None,
                            alert_candidate=None,
                            manual_note='Waiting for a fresh forecast for this window.')
                for field in ('up_probability', 'up_probability_raw', 'probability_low',
                              'probability_high', 'settlement_model_probability',
                              'settlement_model_low', 'settlement_model_high',
                              'independent_up_probability', 'independent_up_probability_raw',
                              'independent_probability_low', 'independent_probability_high'):
                    card[field] = None
            if age > stale_after_s or card.get('book_age_s', 0) > 3:
                for field in ('polymarket_up','polymarket_up_price','polymarket_down_price','polymarket_up_bid','polymarket_down_bid','execution_hurdle',
                              'model_edge','conservative_edge','independent_model_edge',
                              'independent_conservative_edge','independent_market_gap',
                              'predicted_ask_depth','opportunity','alert_candidate'):
                    card[field] = None
    return out


class PredictionService:
    """One producer for the whole cockpit; readers never wait for network I/O."""
    def __init__(self, query, public_json, root, news_fn, interval=SERVER_REFRESH_SECONDS, gate_writer=None):
        self.query, self.public_json, self.root, self.news_fn = query, public_json, root, news_fn
        self.gate_writer = gate_writer
        self.interval = max(MIN_REFRESH_GAP_SECONDS, float(interval))
        self._lock = threading.Condition()
        self._stop = threading.Event()
        self._board = None
        self._news = {'stories': [], 'sources': []}
        self._error = None
        self._threads = []
        self._refresh_now = threading.Event()
        self._building = False
        self._version = 0
        self._last_attempt_ms = None
        self._last_success_ms = None

    def start(self):
        with self._lock:
            if self._threads:
                return
            for name, target in (('predictions', self._run), ('prediction-outcomes', self._resolve),
                                 ('prediction-news', self._load_news)):
                thread = threading.Thread(name=name, target=target, daemon=True)
                self._threads.append(thread)
                thread.start()

    def snapshot(self, force=False, after_version=None, wait_s=0.0):
        if force:
            with self._lock:
                # Coalesce a manual refresh with a build already in progress.
                if not self._building:
                    self._refresh_now.set()
        if after_version is not None and wait_s > 0:
            deadline = time.monotonic() + min(3.0, max(0.0, float(wait_s)))
            with self._lock:
                while self._version <= after_version and not self._stop.is_set():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    self._lock.wait(remaining)
        with self._lock:
            board, error, interval = self._board, self._error, self.interval
            version = self._version
            last_attempt_ms = self._last_attempt_ms
            last_success_ms = self._last_success_ms
        if board is None:
            return {'cards': [], 'generated_at': int(time.time()*1000), 'model_version':MODEL_VERSION,
                    'warming_up':True, 'errors':[error] if error else [],
                    'refresh_interval_s':interval, 'producer_version':version,
                    'snapshot_stale_after_s':SNAPSHOT_STALE_SECONDS,
                    'last_attempt_at_ms':last_attempt_ms, 'last_success_at_ms':last_success_ms}
        result = age_board(board, stale_after_s=SNAPSHOT_STALE_SECONDS)
        result['refresh_interval_s'] = interval
        result['snapshot_stale_after_s'] = SNAPSHOT_STALE_SECONDS
        result['producer_version'] = version
        result['last_attempt_at_ms'] = last_attempt_ms
        result['last_success_at_ms'] = last_success_ms
        if error:
            result['errors'].append(error)
        return result

    def _run(self):
        while not self._stop.is_set():
            started = time.monotonic()
            with self._lock:
                self._building = True
                self._last_attempt_ms = int(time.time()*1000)
            try:
                board = build_board(self.query, self.public_json, self.root,
                                    lambda: self._news, resolve=False)
                if self.gate_writer:
                    try:
                        self.gate_writer(board)
                    except Exception:
                        pass  # A database outage must not take down the Predictions page.
                with self._lock:
                    self._board, self._error = board, None
                    self._last_success_ms = int(time.time()*1000)
                    self._version += 1
            except Exception as exc:
                with self._lock:
                    self._error = 'Prediction refresh failed: '+type(exc).__name__
                    self._version += 1
            finally:
                with self._lock:
                    self._building = False
                    self._lock.notify_all()
            with self._lock:
                interval = self.interval
            wait_s = max(0, interval-(time.monotonic()-started))
            if wait_s == 0:
                wait_s = MIN_REFRESH_GAP_SECONDS
            if self._refresh_now.wait(wait_s):
                self._refresh_now.clear()
            if self._stop.is_set():
                break

    def _resolve(self):
        while not self._stop.is_set():
            try:
                resolve_pending(self.root, self.public_json)
            except Exception:
                pass  # first forecast creates the journal; retry next cycle
            self._stop.wait(30)

    def _load_news(self):
        while not self._stop.is_set():
            try:
                self._news = self.news_fn()
            except Exception:
                pass
            self._stop.wait(300)

    def stop(self):
        self._stop.set()


def cached(key, seconds, fetch):
    with _cache_lock:
        old = _cache.get(key)
    if old and time.monotonic() - old[0] < seconds:
        return old[1]
    try:
        value = fetch()
    except Exception:
        # Market metadata and historical candles remain useful during brief
        # upstream failures. Their own timestamps/window keys still determine
        # whether a forecast is usable; live quotes are never served here.
        if old:
            return old[1]
        raise
    with _cache_lock:
        _cache[key] = (time.monotonic(), value)
        if len(_cache) > 256:
            for k in list(_cache):
                if time.monotonic() - _cache[k][0] > 900:
                    del _cache[k]
    return value


def reference_window(market):
    source = market.get('resolutionSource') or ''
    if 'data.chain.link/' not in source:
        return -1
    if 'twap-60s' in source:
        return 60
    if 'twap-30s' in source:
        return 30
    return -1 if 'twap' in source else 0


def array(value):
    return json.loads(value) if isinstance(value, str) else value or []


def valid_price(value):
    try:
        value = float(value)
        return value if math.isfinite(value) and 0 < value < 1 else None
    except (TypeError, ValueError):
        return None


def book_top(book):
    def levels(side):
        for x in book.get(side, []) or []:
            try:
                size = float(x.get('size', 0))
                price = valid_price(x.get('price'))
                if math.isfinite(size) and size > 0 and price is not None:
                    yield price
            except (TypeError, ValueError, AttributeError):
                continue
    bid, ask = max(levels('bids'), default=None), min(levels('asks'), default=None)
    return (None, None) if bid is not None and ask is not None and bid > ask else (bid, ask)


def fee_parameters(market):
    # Absent metadata is not evidence of a fee-free market.
    if market.get('feesEnabled') is False:
        return 0.0, 1
    try:
        schedule = market.get('feeSchedule') or {}
        rate, exponent = float(schedule.get('rate', -1)), float(schedule.get('exponent', 1))
        if math.isfinite(rate) and rate >= 0 and exponent in range(5):
            return rate, int(exponent)
    except (TypeError, ValueError, OverflowError):
        pass
    return -1.0, 1


def top_depth(book, side, price):
    if price is None:
        return None
    try:
        return sum(float(x.get('size', 0)) for x in book.get(side, [])
                   if abs(float(x.get('price', 0))-price) < 1e-9)
    except (TypeError, ValueError):
        return None


def recent_candles(query, public_json, asset, now):
    rows = query("""SELECT EXTRACT(EPOCH FROM ts)*1000 AS ts,open,high,low,close,volume
        FROM vp.candles_1m WHERE symbol=%s AND ts < date_trunc('minute',now())
        ORDER BY ts DESC LIMIT 1441""", (asset+'USDT',))
    rows = [{k: float(v) for k, v in r.items()} for r in reversed(rows)]
    source = 'Local candle store'
    if not rows or now - (rows[-1]['ts']/1000+60) > 120:
        raw = public_json('https://api.binance.com/api/v3/klines?'+urlencode(
            {'symbol':asset+'USDT','interval':'1m','limit':1000}))
        fresh = [{'ts':int(x[0]),'open':float(x[1]),'high':float(x[2]),'low':float(x[3]),
                  'close':float(x[4]),'volume':float(x[5])} for x in raw if int(x[6]) < now*1000]
        merged = {r['ts']:r for r in rows}
        merged.update({r['ts']:r for r in fresh})
        rows = sorted(merged.values(), key=lambda r:r['ts'])[-1441:]
        source = 'Local store + Binance closed candles'
    # Returns spanning a gap are not one-minute observations. Keep the most
    # recent continuous segment; don't invent flat bars over missing minutes.
    start = 0
    for i in range(1,len(rows)):
        if rows[i]['ts'] - rows[i-1]['ts'] != 60000:
            start = i
    return {'rows':rows[start:], 'source':source}


def _journal(root, cards, markets, public_json, resolve=True):
    path = os.path.join(root, 'logs', 'prediction_quality.sqlite3')
    with _quality_cache_lock:
        previous_quality = _quality_cache.get(path)
        cached_quality = (copy.deepcopy(previous_quality[1])
                          if previous_quality and
                          time.monotonic()-previous_quality[0] < QUALITY_SUMMARY_REFRESH_SECONDS
                          else None)
    with sqlite3.connect(path, timeout=2) as db:
        if db.execute('PRAGMA journal_mode').fetchone()[0].lower() != 'wal':
            db.execute('PRAGMA journal_mode=WAL')
        db.execute('''CREATE TABLE IF NOT EXISTS forecasts (
            cid TEXT, slug TEXT, bucket INTEGER, captured INTEGER, closes INTEGER,
            p REAL, market_p REAL, outcome INTEGER, version TEXT,
            PRIMARY KEY(cid,bucket,version))''')
        columns = {row[1] for row in db.execute("PRAGMA table_info(forecasts)")}
        if 'shadow_p' not in columns:
            db.execute('ALTER TABLE forecasts ADD COLUMN shadow_p REAL')
            # Populate the legacy series only during the one-time schema upgrade.
            db.execute('UPDATE forecasts SET shadow_p=p')
        if 'diagnostics_json' not in columns:
            db.execute('ALTER TABLE forecasts ADD COLUMN diagnostics_json TEXT')
        # Indexes keep the occasional historical score refresh bounded.
        db.execute('''CREATE INDEX IF NOT EXISTS forecasts_quality_lookup
            ON forecasts(version,bucket) WHERE outcome IS NOT NULL''')
        db.execute('CREATE INDEX IF NOT EXISTS forecasts_captured_lookup ON forecasts(captured)')
        db.execute('''CREATE INDEX IF NOT EXISTS forecasts_pending_resolution
            ON forecasts(closes) WHERE outcome IS NULL''')
        db.execute('''CREATE TABLE IF NOT EXISTS prediction_model_backfills (
            version TEXT PRIMARY KEY, applied INTEGER NOT NULL, rows_inserted INTEGER NOT NULL,
            applied_at INTEGER NOT NULL)''')
        now = int(time.time())
        insert = '''INSERT OR IGNORE INTO forecasts
            (cid,slug,bucket,captured,closes,p,market_p,outcome,version,shadow_p,diagnostics_json)
            VALUES (?,?,?,?,?,?,?,NULL,?,?,?)'''
        for c in cards:
            if c['model_status'] == 'ready' and c.get('condition_id'):
                bucket = c['seconds_left']//60
                diagnostics = _diagnostic_snapshot(c) if bucket == 2 else None
                db.execute(insert,
                    (c['condition_id'],c['slug'],c['seconds_left']//60,now,c['window_close']//1000,
                     c['up_probability'],c['polymarket_up'],MODEL_VERSION,
                     c.get('experimental_up_probability', c['up_probability']),diagnostics))
                if (INDEPENDENT_MODEL_VERSION != MODEL_VERSION and
                        c.get('independent_up_probability') is not None):
                    db.execute(insert,
                        (c['condition_id'],c['slug'],bucket,now,c['window_close']//1000,
                         c['independent_up_probability'],c['polymarket_up'],
                         INDEPENDENT_MODEL_VERSION,c['independent_up_probability'],diagnostics))
            n = c.get('next_window') or {}
            if n.get('model_status') == 'provisional' and n.get('condition_id'):
                bucket = n['seconds_until_open']//60
                diagnostics = _diagnostic_snapshot(n) if bucket == 2 else None
                db.execute(insert,
                    (n['condition_id'],n['slug'],n['seconds_until_open']//60,now,
                     n['window_close']//1000,n['up_probability'],n.get('polymarket_up'),
                     MODEL_VERSION+'-next',n.get('experimental_up_probability',n['up_probability']),
                     diagnostics))
                if (INDEPENDENT_MODEL_VERSION != MODEL_VERSION and
                        n.get('independent_up_probability') is not None):
                    db.execute(insert,
                        (n['condition_id'],n['slug'],bucket,now,n['window_close']//1000,
                         n['independent_up_probability'],n.get('polymarket_up'),
                         INDEPENDENT_MODEL_VERSION+'-next',n['independent_up_probability'],
                         diagnostics))
        # Seed the model's live score from the strictly later v5 holdout. The
        # source rows retain the settlement-kernel probability and live feed
        # features, so the CLOB-free 5m estimate can be reconstructed without
        # training on (or reading) the Polymarket price. 15m remains kernel-only.
        backfill_done = db.execute(
            'SELECT 1 FROM prediction_model_backfills WHERE version=?',
            (INDEPENDENT_MODEL_VERSION,)).fetchone()
        if not backfill_done:
            source_rows = db.execute('''SELECT cid,slug,bucket,captured,closes,market_p,
                    outcome,diagnostics_json
                FROM forecasts WHERE version=? AND bucket=2 AND captured>?
                    AND captured>=? AND captured<?
                    AND market_p IS NOT NULL AND outcome IS NOT NULL
                    AND diagnostics_json IS NOT NULL ORDER BY captured''',
                (MARKET_BENCHMARK_VERSION, _TICK_5M_TRAINED_THROUGH,
                 _TICK_5M_HOLDOUT_START, _TICK_5M_HOLDOUT_END)).fetchall()
            rows_inserted = 0
            for cid,slug,bucket,captured,closes,market_p,outcome,raw in source_rows:
                try:
                    diagnostic = json.loads(raw)
                    kernel_p = float(diagnostic['settlement_model_probability'])
                    if '-5m-' in str(slug):
                        model_p = _tick_corrected_probability(
                            kernel_p, diagnostic.get('binance_momentum_bps'),
                            diagnostic.get('coinbase_momentum_bps'),
                            diagnostic.get('binance_basis_bps'),
                            diagnostic.get('coinbase_basis_bps'))[0]
                    elif '-15m-' in str(slug):
                        model_p = kernel_p
                    else:
                        continue
                    if not (math.isfinite(model_p) and 0.0 <= model_p <= 1.0):
                        continue
                    cursor = db.execute('''INSERT OR IGNORE INTO forecasts
                        (cid,slug,bucket,captured,closes,p,market_p,outcome,version,
                         shadow_p,diagnostics_json) VALUES (?,?,?,?,?,?,?,?,?,?,?)''',
                        (cid,slug,bucket,captured,closes,model_p,market_p,outcome,
                         INDEPENDENT_MODEL_VERSION,model_p,raw))
                    rows_inserted += cursor.rowcount
                except (KeyError, TypeError, ValueError, json.JSONDecodeError):
                    continue
            db.execute('INSERT INTO prediction_model_backfills VALUES (?,?,?,?)',
                (INDEPENDENT_MODEL_VERSION,1,rows_inserted,now))
        # Seed the provisional next-window score from the already resolved
        # v5 snapshots. Their stored settlement probability is exactly the
        # CLOB-free Chainlink kernel used by the current pre-open forecast.
        # Keep this separate from the current-window bootstrap: the opening
        # reference is unknown at these checkpoints, so the two populations
        # must never be mixed.
        next_backfill_version = INDEPENDENT_MODEL_VERSION+'-next-checkpoint-v1'
        next_backfill_done = db.execute(
            'SELECT 1 FROM prediction_model_backfills WHERE version=?',
            (next_backfill_version,)).fetchone()
        if not next_backfill_done:
            source_rows = db.execute('''SELECT cid,slug,bucket,captured,closes,market_p,
                    outcome,diagnostics_json
                FROM forecasts WHERE version=? AND bucket=2
                    AND market_p IS NOT NULL AND outcome IS NOT NULL
                    AND diagnostics_json IS NOT NULL ORDER BY captured''',
                (MARKET_BENCHMARK_VERSION+'-next',)).fetchall()
            rows_inserted = 0
            for cid,slug,bucket,captured,closes,market_p,outcome,raw in source_rows:
                try:
                    diagnostic = json.loads(raw)
                    model_p = float(diagnostic['settlement_model_probability'])
                    if not (math.isfinite(model_p) and 0.0 <= model_p <= 1.0):
                        continue
                    cursor = db.execute('''INSERT OR IGNORE INTO forecasts
                        (cid,slug,bucket,captured,closes,p,market_p,outcome,version,
                         shadow_p,diagnostics_json) VALUES (?,?,?,?,?,?,?,?,?,?,?)''',
                        (cid,slug,bucket,captured,closes,model_p,market_p,outcome,
                         INDEPENDENT_MODEL_VERSION+'-next',model_p,raw))
                    rows_inserted += cursor.rowcount
                except (KeyError, TypeError, ValueError, json.JSONDecodeError):
                    continue
            db.execute('INSERT INTO prediction_model_backfills VALUES (?,?,?,?)',
                (next_backfill_version,1,rows_inserted,now))
        db.commit()
        if resolve:
            resolve_pending(root, public_json)
        db.execute('DELETE FROM forecasts WHERE captured<?',(now-90*86400,))
        if cached_quality is not None:
            return cached_quality
        # One comparable checkpoint per market avoids pretending correlated
        # repeated refreshes are independent observations.
        rows = db.execute('''SELECT f.p,f.shadow_p,f.market_p,f.outcome FROM forecasts f
            WHERE outcome IS NOT NULL AND bucket=2 AND version=? ''', (MODEL_VERSION,)).fetchall()
        next_rows = db.execute('''SELECT f.p,f.shadow_p,f.market_p,f.outcome FROM forecasts f
            WHERE outcome IS NOT NULL AND bucket=2 AND version=? ''',
            (MODEL_VERSION+'-next',)).fetchall()
        legacy_rows = db.execute('''SELECT p,market_p,outcome FROM forecasts
            WHERE outcome IS NOT NULL AND market_p IS NOT NULL AND bucket=2 AND version=?''',
            (SHADOW_CALIBRATION_VERSION,)).fetchall()
        legacy_next_rows = db.execute('''SELECT p,market_p,outcome FROM forecasts
            WHERE outcome IS NOT NULL AND market_p IS NOT NULL AND bucket=2 AND version=?''',
            (SHADOW_CALIBRATION_VERSION+'-next',)).fetchall()
        independent_rows = db.execute('''SELECT p,market_p,outcome FROM forecasts
            WHERE outcome IS NOT NULL AND market_p IS NOT NULL AND bucket=2 AND version=?''',
            (INDEPENDENT_MODEL_VERSION,)).fetchall()
        independent_next_rows = db.execute('''SELECT p,market_p,outcome FROM forecasts
            WHERE outcome IS NOT NULL AND market_p IS NOT NULL AND bucket=2 AND version=?''',
            (INDEPENDENT_MODEL_VERSION+'-next',)).fetchall()
        total = db.execute('SELECT count(DISTINCT cid) FROM forecasts').fetchone()[0]
        n = len(rows)
        paired = [r for r in rows if r[2] is not None]
        next_paired = [r for r in next_rows if r[2] is not None]
        nn = len(next_rows)
        quality = {'markets_tracked':total,'resolved_markets':n,
                'brier':sum((r[0]-r[3])**2 for r in rows)/n if n else None,
                'shadow_brier':sum((r[1]-r[3])**2 for r in rows if r[1] is not None) /
                    sum(1 for r in rows if r[1] is not None) if any(r[1] is not None for r in rows) else None,
                'market_brier':sum((r[2]-r[3])**2 for r in paired)/len(paired) if paired else None,
                'paired_model_brier':sum((r[1]-r[3])**2 for r in paired if r[1] is not None) /
                    sum(1 for r in paired if r[1] is not None) if any(r[1] is not None for r in paired) else None,
                'paired_markets':len(paired), 'neutral_brier':0.25,
                'legacy_shadow_brier':sum((r[0]-r[2])**2 for r in legacy_rows)/len(legacy_rows) if legacy_rows else None,
                'legacy_market_brier':sum((r[1]-r[2])**2 for r in legacy_rows)/len(legacy_rows) if legacy_rows else None,
                'legacy_paired_markets':len(legacy_rows),
                'independent_brier':sum((r[0]-r[2])**2 for r in independent_rows)/len(independent_rows) if independent_rows else None,
                'independent_market_brier':sum((r[1]-r[2])**2 for r in independent_rows)/len(independent_rows) if independent_rows else None,
                'independent_paired_markets':len(independent_rows),
                'accuracy':sum((r[0]>=0.5)==bool(r[3]) for r in rows)/n if n else None,
                'next_resolved_markets':nn,
                'next_brier':sum((r[0]-r[3])**2 for r in next_rows)/nn if nn else None,
                'next_shadow_brier':sum((r[1]-r[3])**2 for r in next_rows if r[1] is not None) /
                    sum(1 for r in next_rows if r[1] is not None) if any(r[1] is not None for r in next_rows) else None,
                'next_market_brier':sum((r[2]-r[3])**2 for r in next_paired)/len(next_paired) if next_paired else None,
                'legacy_next_shadow_brier':sum((r[0]-r[2])**2 for r in legacy_next_rows)/len(legacy_next_rows) if legacy_next_rows else None,
                'legacy_next_market_brier':sum((r[1]-r[2])**2 for r in legacy_next_rows)/len(legacy_next_rows) if legacy_next_rows else None,
                'independent_next_brier':sum((r[0]-r[2])**2 for r in independent_next_rows)/len(independent_next_rows) if independent_next_rows else None,
                'independent_next_market_brier':sum((r[1]-r[2])**2 for r in independent_next_rows)/len(independent_next_rows) if independent_next_rows else None,
                'independent_next_paired_markets':len(independent_next_rows),
                'next_accuracy':sum((r[0]>=0.5)==bool(r[3]) for r in next_rows)/nn if nn else None,
                'checkpoint':'120–179 seconds remaining', 'status':'collecting' if n<100 else 'experimental'}
        with _quality_cache_lock:
            _quality_cache[path] = (time.monotonic(), copy.deepcopy(quality))
        return quality


def _diagnostic_snapshot(card):
    """Persist one compact, reproducible feature trace at the scored checkpoint."""
    fields = (
        'prediction_basis', 'model_status', 'reference', 'reference_age_s',
        'reference_price_ts', 'reference_strike_ts', 'settlement_source', 'spot', 'strike',
        'settlement_model_probability', 'settlement_model_low',
        'settlement_model_high', 'independent_up_probability',
        'independent_up_probability_raw', 'independent_probability_low',
        'independent_probability_high', 'independent_direction',
        'independent_model_edge', 'independent_conservative_edge',
        'independent_market_gap', 'independent_calibration',
        'independent_components', 'experimental_up_probability',
        'experimental_up_probability_raw', 'experimental_probability_low',
        'experimental_probability_high', 'up_probability', 'probability_low',
        'probability_high', 'polymarket_up', 'market_quote_low', 'market_quote_high', 'polymarket_up_bid',
        'polymarket_up_price', 'polymarket_down_bid', 'polymarket_down_price',
        'model_market_gap', 'fee_rate', 'execution_hurdle', 'model_edge',
        'conservative_edge', 'experimental_model_edge',
        'experimental_conservative_edge', 'input_timestamps', 'candle_age_s',
        'book_age_s', 'technical', 'order_flow', 'news_signal', 'candle_store',
        'calibration', 'components', 'signal_contributions', 'alert_candidate',
        'coinbase_price', 'coinbase_age_s', 'coinbase_basis_bps',
        'coinbase_momentum_bps', 'coinbase_momentum_score', 'binance_price',
        'binance_age_s', 'binance_basis_bps', 'binance_momentum_bps',
        'binance_momentum_score')
    return json.dumps({key: card.get(key) for key in fields},
                      separators=(',', ':'), sort_keys=True, allow_nan=False)


def resolved_outcome(market, cid):
    if (market.get('conditionId') != cid or market.get('closed') is not True or
            market.get('umaResolutionStatus') != 'resolved'):
        return None
    try:
        pairs = dict(zip([str(x).upper() for x in array(market.get('outcomes'))],
                         map(float, array(market.get('outcomePrices')))))
        if pairs.get('UP') in (0.0, 1.0) and pairs.get('DOWN') == 1-pairs['UP']:
            return int(pairs['UP'])
    except (ValueError, TypeError):
        pass
    return None


def resolve_pending(root, public_json):
    path = os.path.join(root, 'logs', 'prediction_quality.sqlite3')
    now = int(time.time())
    with sqlite3.connect(path, timeout=2) as db:
        db.execute('CREATE TABLE IF NOT EXISTS resolution_checks (cid TEXT PRIMARY KEY, checked INTEGER)')
        pending = db.execute('''SELECT f.cid,f.slug FROM forecasts f
            LEFT JOIN resolution_checks r ON r.cid=f.cid
            WHERE f.outcome IS NULL AND f.closes<? AND COALESCE(r.checked,0)<?
            GROUP BY f.cid,f.slug ORDER BY COALESCE(max(r.checked),0),min(f.closes) LIMIT 16''',
            (now-120,now-120)).fetchall()
    def fetch(row):
        cid, slug = row
        try:
            # Gamma defaults to open markets. Without closed=true the journal
            # silently receives [] forever, even for fully resolved markets.
            markets = public_json('https://gamma-api.polymarket.com/markets?'+
                                  urlencode({'slug':slug,'closed':'true'}))
            for market in markets:
                outcome = resolved_outcome(market, cid)
                if outcome is not None:
                    return cid, outcome
        except Exception:
            pass
        return cid, None
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(fetch, pending))
    with sqlite3.connect(path, timeout=2) as db:
        for cid, outcome in results:
            db.execute('INSERT OR REPLACE INTO resolution_checks VALUES (?,?)', (cid,now))
            if outcome is not None:
                db.execute('UPDATE forecasts SET outcome=? WHERE cid=? AND outcome IS NULL', (outcome,cid))


def journal_calibration(root, min_n=20, prior=15.0,
                        version=SHADOW_CALIBRATION_VERSION):
    '''Historical reliability adjustment at the board's scored checkpoint.

    Resolved forecasts in bucket 2 (the 120–179 second checkpoint) are grouped
    by predicted-probability decile. The board shrinks each card's probability
    toward the decile's realized win rate (Beta-style, `prior` pseudo-
    observations) once the decile holds min_n settled samples. Version suffixes
    keep current-window and provisional next-window calibration separate.'''
    path = os.path.join(root, 'logs', 'prediction_quality.sqlite3')
    try:
        with sqlite3.connect(path, timeout=2) as db:
            rows = db.execute('''SELECT CAST(p*10 AS INTEGER), COUNT(*), SUM(outcome), AVG(p)
                FROM forecasts WHERE outcome IS NOT NULL AND bucket=2 AND version=?
                GROUP BY 1''', (version,)).fetchall()
            total, wins = db.execute("""SELECT COUNT(*), COALESCE(SUM(outcome),0) FROM forecasts
                WHERE outcome IS NOT NULL AND bucket=2 AND version=?""", (version,)).fetchone()
    except Exception:
        return {}
    deciles = {}
    for dec, n, w, avg_p in rows:
        if dec is None or not n: continue
        deciles[int(dec)] = {'n': int(n), 'wins': float(w or 0), 'avg_p': float(avg_p or 0)}
    return {'deciles': deciles, 'total': int(total or 0),
            'total_wins': float(wins or 0), 'min_n': min_n, 'prior': prior}


def _calibrate_up(p, calibration):
    '''Shrink the ensemble's UP probability toward the decile's realized win
    rate. Returns (p_calibrated, info); info is None when the decile does not
    yet hold enough settled samples (no adjustment is applied).'''
    if not calibration or not (0.0 <= p <= 1.0):
        return p, None
    d = calibration.get('deciles', {}).get(min(9, max(0, int(p * 10))))
    if not d or d['n'] < calibration.get('min_n', 20):
        return p, None
    a = float(calibration.get('prior', 15.0))
    p2 = (d['wins'] + a * p) / (d['n'] + a)
    p2 = max(0.0, min(1.0, p2))
    return p2, {'n': d['n'], 'wins': d['wins'], 'avg_p': round(d['avg_p'], 4),
                'adjust': round(p2 - p, 4)}


def _number(value, fallback=0.0):
    try:
        result = float(value)
        return result if math.isfinite(result) else fallback
    except (TypeError, ValueError, OverflowError):
        return fallback


def _logit(p):
    p = min(.99, max(.01, _number(p, .5)))
    return math.log(p/(1-p))


def _sigmoid(z):
    z = max(-12.0, min(12.0, z))
    return 1.0/(1.0+math.exp(-z))


def _tick_corrected_probability(base, binance_move, coinbase_move,
                                binance_basis, coinbase_basis, impacts=False):
    """Apply the fitted 5m correction without reading any Polymarket price."""
    availability = (binance_move is not None, coinbase_move is not None,
                    binance_basis is not None, coinbase_basis is not None)
    feature_values = (
        _number(binance_move), _number(coinbase_move),
        _number(binance_basis), _number(coinbase_basis),
        float(availability[0]), float(availability[1]),
        float(availability[2]), float(availability[3]),
    )
    base_logit = _logit(base)
    z = base_logit
    rows = []

    def apply(name, adjustment, available, detail):
        nonlocal z
        before = _sigmoid(z)
        z += adjustment
        after = _sigmoid(z)
        rows.append({'name': name, 'available': bool(available),
                     'logit_adjustment': round(adjustment, 6),
                     'probability_before': round(before, 6),
                     'probability_after': round(after, 6),
                     'delta_pp': round((after-before)*100, 2),
                     'detail': detail})

    apply('5m kernel calibration',
          _TICK_5M_INTERCEPT+(_TICK_5M_KERNEL_LOGIT-1.0)*base_logit,
          True, 'Fit on 19,045 earlier resolved 5m forecasts')
    labels = (
        ('Binance 10s momentum', 'fresh' if binance_move is not None else 'stale'),
        ('Coinbase 10s momentum', 'fresh' if coinbase_move is not None else 'stale'),
        ('Binance vs Chainlink basis', 'fresh' if binance_basis is not None else 'stale'),
        ('Coinbase vs Chainlink basis', 'fresh' if coinbase_basis is not None else 'stale'),
        ('Binance momentum availability', 'source coverage'),
        ('Coinbase momentum availability', 'source coverage'),
        ('Binance basis availability', 'source coverage'),
        ('Coinbase basis availability', 'source coverage'),
    )
    for i, (name, detail) in enumerate(labels):
        adjustment = _TICK_5M_WEIGHTS[i] * (
            feature_values[i]-_TICK_5M_MEANS[i])/_TICK_5M_SCALES[i]
        value_feature = i < 4
        available = availability[i] if value_feature else True
        apply(name, adjustment, available,
              f'{feature_values[i]:+.2f} bp · {detail}' if value_feature else detail)
    return _sigmoid(z), rows if impacts else []


def _technical_features(rows, sigma_minute):
    closes = [r['close'] for r in rows if _number(r.get('close')) > 0]
    volumes = [_number(r.get('volume')) for r in rows]
    if len(closes) < 2:
        return {'trend_score':0.0,'r1_pct':None,'r3_pct':None,'r5_pct':None,
                'r15_pct':None,'ema_gap_pct':None,'rsi14':None,'volume_ratio':None}
    def ret(n):
        if len(closes) <= n: return None
        return 100.0*(closes[-1]/closes[-n-1]-1.0)
    def ema(period):
        alpha, value = 2.0/(period+1), closes[0]
        for price in closes[1:]: value = alpha*price+(1-alpha)*value
        return value
    changes = [closes[i]/closes[i-1]-1.0 for i in range(1,len(closes))]
    tail = changes[-14:]
    gains = sum(max(0,x) for x in tail)
    losses = sum(max(0,-x) for x in tail)
    rsi = 100.0 if losses == 0 else 100.0-100.0/(1.0+gains/max(losses,1e-12))
    gap = 100.0*(ema(min(8,len(closes)))/ema(min(21,len(closes)))-1.0)
    vol_ratio = None
    if len(volumes) >= 25:
        prior = sum(volumes[-25:-5])/20.0
        recent = sum(volumes[-5:])/5.0
        vol_ratio = recent/prior if prior > 0 else None
    sigma = max(_number(sigma_minute), .00005)
    r5_log = math.log(closes[-1]/closes[-6]) if len(closes)>5 else 0.0
    r15_log = math.log(closes[-1]/closes[-16]) if len(closes)>15 else 0.0
    ema_z = (gap/100.0)/(sigma*math.sqrt(10.0))
    z5, z15 = r5_log/(sigma*math.sqrt(5.0)), r15_log/(sigma*math.sqrt(15.0))
    volume_scale = min(1.2,max(.75,math.sqrt(vol_ratio))) if vol_ratio and vol_ratio > 0 else 1.0
    score = volume_scale*(.50*math.tanh(z5/2.0)+.30*math.tanh(z15/2.0)+.20*math.tanh(ema_z/2.0))
    return {'trend_score':max(-1.0,min(1.0,score)),'r1_pct':ret(1),'r3_pct':ret(3),
            'r5_pct':ret(5),'r15_pct':ret(15),'ema_gap_pct':gap,'rsi14':rsi,
            'volume_ratio':vol_ratio}


def _news_signal(asset, stories, now_s):
    aliases = {'BTC':('bitcoin',' btc '),'ETH':('ethereum','ether',' eth '),
               'SOL':('solana',' sol '),'XRP':('xrp','ripple')}[asset]
    broad = ('crypto','fed','interest rate','inflation','risk asset','dollar')
    total, weight, matched = 0.0, 0.0, 0
    for story in stories or []:
        title = ' '+str(story.get('title','')).lower()+' '
        direct = any(a in title for a in aliases)
        general = any(term in title for term in broad)
        if not direct and not general: continue
        age_h = max(0.0,(now_s-_number(story.get('published'),now_s))/3600.0)
        w = math.exp(-age_h/6.0)*(1.0 if direct else .2)
        total += max(-2,min(2,_number(story.get('sentiment'))))*w
        weight += 2.0*w
        matched += 1
    return (max(-1.0,min(1.0,total/weight)) if weight else 0.0), matched


def _asset_tape(query, asset):
    rows = query("""SELECT o.price AS spot,o.ts_s AS spot_ts,
        cb.price AS coinbase,cb.ts_s AS coinbase_ts,cb10.price AS coinbase_10,
        cb10.ts_s AS coinbase_10_ts,bn.price AS binance,bn.ts_s AS binance_ts,
        bn10.price AS binance_10,bn10.ts_s AS binance_10_ts
        FROM (SELECT %s::text AS asset) x
        LEFT JOIN LATERAL (SELECT price,EXTRACT(EPOCH FROM ts) AS ts_s
          FROM vp.oracle_ticks WHERE asset=x.asset||':SPOT' ORDER BY ts DESC LIMIT 1) o ON TRUE
        LEFT JOIN LATERAL (SELECT price,EXTRACT(EPOCH FROM ts) AS ts_s
          FROM vp.coinbase_ticks WHERE asset=x.asset ORDER BY ts DESC LIMIT 1) cb ON TRUE
        LEFT JOIN LATERAL (SELECT price,EXTRACT(EPOCH FROM ts) AS ts_s
          FROM vp.coinbase_ticks WHERE asset=x.asset AND ts<=now()-interval '10 seconds'
          ORDER BY ts DESC LIMIT 1) cb10 ON TRUE
        LEFT JOIN LATERAL (SELECT price,EXTRACT(EPOCH FROM ts) AS ts_s
          FROM vp.binance_ticks WHERE symbol=x.asset||'USDT' ORDER BY ts DESC LIMIT 1) bn ON TRUE
        LEFT JOIN LATERAL (SELECT price,EXTRACT(EPOCH FROM ts) AS ts_s
          FROM vp.binance_ticks WHERE symbol=x.asset||'USDT' AND ts<=now()-interval '10 seconds'
          ORDER BY ts DESC LIMIT 1) bn10 ON TRUE""", (asset,))
    return rows[0] if rows else {}


def _window_reference(query, asset, horizon, opened, market):
    window = reference_window(market)
    if window < 0:
        return {'strike':0.0,'reference_now':0.0,'reference_ts':None,'window_s':window}
    key = asset+(':SPOT' if window == 0 else f':TWAP{window}')
    rows = query("""SELECT o.price AS strike,o.ts_s AS strike_ts,
        n.price AS reference_now,n.ts_s AS reference_ts
        FROM (SELECT %s::text AS ref_key,%s::double precision AS open_s) x
        LEFT JOIN LATERAL (SELECT price,EXTRACT(EPOCH FROM ts) AS ts_s FROM vp.oracle_ticks
          WHERE asset=x.ref_key AND ts<=to_timestamp(x.open_s)
            AND ts>=to_timestamp(x.open_s)-interval '10 seconds'
          ORDER BY ts DESC LIMIT 1) o ON TRUE
        LEFT JOIN LATERAL (SELECT price,EXTRACT(EPOCH FROM ts) AS ts_s
          FROM vp.oracle_ticks WHERE asset=x.ref_key ORDER BY ts DESC LIMIT 1) n ON TRUE""",
        (key,opened))
    row = rows[0] if rows else {}
    return {**row,'window_s':window}


def _momentum(current, current_ts, previous, previous_ts, now_s, sigma):
    if not current or not previous or not current_ts or not previous_ts:
        return None, None
    current_age, previous_age = now_s-current_ts, now_s-previous_ts
    elapsed = current_ts-previous_ts
    if current_age < -2 or current_age > 5 or previous_age < 7 or previous_age > 22 or elapsed <= 0:
        return None, None
    move = math.log(current/previous)
    z = move/(max(sigma,.00005)*math.sqrt(elapsed/60.0))
    return math.tanh(z/2.0), move*10000.0


def _ensemble(base, low, high, market_p, signals, market_weight):
    def evaluate(start):
        z = _logit(start)
        if market_p is not None:
            z = (1.0-market_weight)*z+market_weight*_logit(market_p)
        impacts, before = [], _sigmoid(z)
        for name, (available, adjustment, detail) in signals.items():
            if available: z += adjustment
            after = _sigmoid(z)
            impacts.append({'name':name,'available':bool(available),
                            'logit_adjustment':round(adjustment,6) if available else 0.0,
                            'probability_before':round(before,6),
                            'probability_after':round(after,6),
                            'delta_pp':round((after-before)*100,2) if available else 0.0,
                            'detail':detail})
            before = after
        return _sigmoid(z), impacts
    market_only, _ = _ensemble_without_signals(base, market_p, market_weight)
    p, impacts = evaluate(base)
    low_p, _ = evaluate(low)
    high_p, _ = evaluate(high)
    return p,min(low_p,high_p),max(low_p,high_p),round((market_only-base)*100,2),impacts


def _ensemble_without_signals(base, market_p, market_weight):
    z = _logit(base)
    if market_p is not None:
        z = (1.0-market_weight)*z+market_weight*_logit(market_p)
    return _sigmoid(z), None


def _market_probability(req):
    mids = []
    for side in ('up','down'):
        bid, ask = req.get(side+'_bid'), req.get(side+'_ask')
        if bid is None or ask is None: return None
        mids.append((bid+ask)/2.0)
    total = sum(mids)
    return mids[0]/total if total > 0 else None


def _market_probability_range(req):
    """Quote-derived UP range from fresh complementary UP/DOWN top of book."""
    ub, ua, db, da = (req.get(k) for k in
                      ('up_bid', 'up_ask', 'down_bid', 'down_ask'))
    if any(x is None for x in (ub, ua, db, da)):
        return None
    try:
        ub, ua, db, da = map(float, (ub, ua, db, da))
    except (TypeError, ValueError):
        return None
    if not (0 < ub <= ua < 1 and 0 < db <= da < 1):
        return None
    low_den, high_den = ub + da, ua + db
    if low_den <= 0 or high_den <= 0:
        return None
    low = max(0.01, min(0.99, ub / low_den))
    high = max(0.01, min(0.99, ua / high_den))
    return (min(low, high), max(low, high))


def _primary_probability(model, model_low, model_high):
    """Return the CLOB-free forecast; quotes are execution benchmarks only."""
    return model, model_low, model_high, 'independent_model'


def _lead_lag_alert_candidate(condition_id, ready, market_probability, shadow_probability,
                              direction, conservative_edge, up_book_age_s,
                              down_book_age_s, aligned_leads, detected_at_ms):
    """Strict current-window alert gate; this remains an unvalidated shadow signal."""
    if not condition_id or not ready or market_probability is None or not aligned_leads:
        return None
    if any(age is None or age < 0 or age > 5 for age in (up_book_age_s, down_book_age_s)):
        return None
    gap = ((shadow_probability-market_probability) if direction == 'UP'
           else (market_probability-shadow_probability))
    if gap < .05 or conservative_edge is None or conservative_edge < .02:
        return None
    return {
        'id':f'{condition_id}:{direction}', 'side':direction,
        'model_market_gap_pp':round(gap*100,2),
        'conservative_net_edge_pp':round(conservative_edge*100,2),
        'lead_sources':aligned_leads, 'label':'UNVERIFIED LEAD/LAG EDGE',
        'detected_at_ms':int(detected_at_ms)}


def _edge(probability, low, high, direction, bid, ask, fee_rate, exponent):
    if bid is None or ask is None or fee_rate < 0 or bid <= 0 or ask >= 1 or bid > ask:
        return None,None
    cost = ask+fee_rate*(ask*(1-ask))**exponent+.005
    side_p = probability if direction == 'UP' else 1-probability
    side_low = low if direction == 'UP' else 1-high
    return side_p-cost,side_low-cost


def build_board(query, public_json, root, news_fn, resolve=True):
    started = time.monotonic()
    now_s = time.time()
    now = int(now_s)
    calibration = journal_calibration(root, version=SHADOW_CALIBRATION_VERSION)
    combos = [(a,h,now//h*h) for a in ASSETS for h in (300,900)]
    windows = [(a,h,o+offset) for a,h,o in combos for offset in (0,h)]
    errors, markets, candles, tape = [], {}, {}, {}
    def market(a,h,o):
        slug = f'{a.lower()}-updown-{"5m" if h==300 else "15m"}-{o}'
        rows = cached('market:'+slug,30,lambda:public_json(
            'https://gamma-api.polymarket.com/markets?'+urlencode({'slug':slug}),timeout=2.0))
        return rows[0] if rows else {'slug':slug}
    with ThreadPoolExecutor(max_workers=12) as pool:
        jobs = [(pool.submit(market,*key),'market',key) for key in windows]
        jobs += [(pool.submit(cached,'candles:'+a,20,
                    lambda a=a:recent_candles(query,public_json,a,now)),'candles',a) for a in ASSETS]
        jobs += [(pool.submit(_asset_tape,query,a),'tape',a) for a in ASSETS]
        for future,kind,key in jobs:
            try:
                value=future.result()
                (markets if kind=='market' else candles if kind=='candles' else tape)[key]=value
            except Exception:
                errors.append(f'{kind} unavailable: {key}')
    # One batched CLOB read covers both sides of every current and next market.
    tokens=list({str(t) for m in markets.values() for t in array(m.get('clobTokenIds'))})
    books={};books_received_at_s=now_s
    if tokens:
        try:
            response=public_json('https://clob.polymarket.com/books',
                                 data=[{'token_id':t} for t in tokens],timeout=1.5)
            books={str(b['asset_id']):b for b in response}
            # Book timestamps are generated during this network request. Age
            # them from receipt time, not from the beginning of the board build;
            # otherwise a healthy fresh book can look like a future timestamp
            # and be discarded on a slow response.
            books_received_at_s=time.time()
        except Exception: errors.append('CLOB books unavailable')
    news={'stories':[],'sources':[]}
    try: news=news_fn() or news
    except Exception: errors.append('News context unavailable')
    stories=news.get('stories',[])
    requests=[];metadata={};
    for a,h,o in combos:
        m=markets.get((a,h,o),{})
        ref=_window_reference(query,a,h,o,m)
        current_tape=tape.get(a,{})
        bars=candles.get(a,{}).get('rows',[])
        spot=_number(current_tape.get('spot'))
        spot_ts=_number(current_tape.get('spot_ts'))
        ref_ts=_number(ref.get('reference_ts'))
        reference_age=max(now_s-spot_ts,now_s-ref_ts) if spot_ts and ref_ts else 1e9
        candle_age=now_s-bars[-1]['ts']/1000.0-60 if bars else -1
        fee_rate,fee_exponent=fee_parameters(m)
        req={'spot':spot,'strike':_number(ref.get('strike')),
             'seconds_left':max(0,o+h-now),'twap_window_s':ref['window_s'],
             'reference_age_s':reference_age,'candle_age_s':candle_age,
             'closes':[r['close'] for r in bars],
             'fee_rate':fee_rate,'fee_exponent':fee_exponent}
        metadata[(a,h,o)]={'market':m,'ref':ref,'tape':current_tape,'bars':bars}
        sides=dict(zip([str(x).upper() for x in array(m.get('outcomes'))],array(m.get('clobTokenIds'))))
        for side in ('UP','DOWN'):
            book=books.get(str(sides.get(side)),{})
            bid,ask=book_top(book)
            ts=_number(book.get('timestamp'))/1000.0
            book_age=(max(0.0,books_received_at_s-ts)
                      if ts>0 and books_received_at_s-ts>=-2.0 else None)
            if book_age is None or book_age < -1 or book_age > 3: bid=ask=None
            req[side.lower()+'_bid']=bid
            req[side.lower()+'_ask']=ask
            req[side.lower()+'_ask_depth']=top_depth(book,'asks',ask)
            req[side.lower()+'_book_age_s']=book_age
            if book_age is not None and 0 <= book_age <= 3:
                req.setdefault('book_ages',[]).append(book_age)
        req['book_age_s']=max(req.pop('book_ages',[-1]))
        requests.append(req)
        next_o=o+h
        next_m=markets.get((a,h,next_o),{})
        next_ref=_window_reference(query,a,h,next_o,next_m)
        next_fee,next_exp=fee_parameters(next_m)
        next_req={'spot':spot,'strike':spot,'seconds_left':h,'twap_window_s':next_ref['window_s'],
                  'reference_age_s':now_s-spot_ts if spot_ts else 1e9,'candle_age_s':candle_age,
                  'closes':[r['close'] for r in bars],'use_drift':1,
                  'fee_rate':next_fee,'fee_exponent':next_exp}
        next_sides=dict(zip([str(x).upper() for x in array(next_m.get('outcomes'))],
                            array(next_m.get('clobTokenIds'))))
        for side in ('UP','DOWN'):
            book=books.get(str(next_sides.get(side)),{})
            bid,ask=book_top(book)
            ts=_number(book.get('timestamp'))/1000.0
            book_age=(max(0.0,books_received_at_s-ts)
                      if ts>0 and books_received_at_s-ts>=-2.0 else None)
            if book_age is None or book_age < -1 or book_age > 3: bid=ask=None
            next_req[side.lower()+'_bid']=bid
            next_req[side.lower()+'_ask']=ask
            next_req[side.lower()+'_ask_depth']=top_depth(book,'asks',ask)
            next_req[side.lower()+'_book_age_s']=book_age
            if book_age is not None and 0 <= book_age <= 3:
                next_req.setdefault('book_ages',[]).append(book_age)
        next_req['book_age_s']=max(next_req.pop('book_ages',[-1]))
        requests.append(next_req)
        metadata[(a,h,next_o)]={'market':next_m,'ref':next_ref,'tape':current_tape,'bars':bars}
    proc=subprocess.run([os.path.join(root,'build','bin','polypulse-forecast')],
        input=json.dumps(requests,allow_nan=False),capture_output=True,text=True,timeout=3,check=True)
    forecasts=json.loads(proc.stdout)
    cards=[]
    for index,(a,h,o) in enumerate(combos):
        current_req,current_f=requests[2*index],forecasts[2*index]
        next_req,next_f=requests[2*index+1],forecasts[2*index+1]
        current_data=metadata[(a,h,o)];next_data=metadata[(a,h,o+h)]
        current_tape=current_data['tape'];bars=current_data['bars']
        technical=_technical_features(bars,current_f['sigma_minute'])
        coinbase_age=now_s-_number(current_tape.get('coinbase_ts')) if current_tape.get('coinbase_ts') else None
        binance_age=now_s-_number(current_tape.get('binance_ts')) if current_tape.get('binance_ts') else None
        sigma=current_f['sigma_minute']
        cb_momentum,cb_move=_momentum(_number(current_tape.get('coinbase')),
            _number(current_tape.get('coinbase_ts')),_number(current_tape.get('coinbase_10')),
            _number(current_tape.get('coinbase_10_ts')),now_s,sigma)
        bn_momentum,bn_move=_momentum(_number(current_tape.get('binance')),
            _number(current_tape.get('binance_ts')),_number(current_tape.get('binance_10')),
            _number(current_tape.get('binance_10_ts')),now_s,sigma)
        spot=_number(current_req['spot'])
        spot_age=now_s-_number(current_tape.get('spot_ts')) if current_tape.get('spot_ts') else None
        spot_fresh=spot_age is not None and -2<=spot_age<=5
        cb_basis=math.log(_number(current_tape.get('coinbase'))/spot) if spot>0 and _number(current_tape.get('coinbase'))>0 and coinbase_age is not None and -2<=coinbase_age<=5 and spot_fresh else None
        bn_basis=math.log(_number(current_tape.get('binance'))/spot) if spot>0 and _number(current_tape.get('binance'))>0 and binance_age is not None and -2<=binance_age<=5 and spot_fresh else None
        news_score,news_matches=_news_signal(a,stories,now_s)
        live_signals={
            'Candle trend':(len(bars)>=31,.14*technical['trend_score'],
                f"5m {technical['r5_pct'] if technical['r5_pct'] is not None else 0:+.3f}% · 15m {technical['r15_pct'] if technical['r15_pct'] is not None else 0:+.3f}% · RSI {technical['rsi14'] if technical['rsi14'] is not None else 0:.0f}"),
            'Coinbase lead':(cb_momentum is not None,.07*(cb_momentum or 0),
                f"10s {cb_move if cb_move is not None else 0:+.1f} bp · basis {cb_basis*10000 if cb_basis is not None else 0:+.1f} bp"),
            'Coinbase basis':(cb_basis is not None,.025*math.tanh(cb_basis/.0006) if cb_basis is not None else 0.0,
                f"fresh Chainlink-relative basis {cb_basis*10000 if cb_basis is not None else 0:+.1f} bp"),
            'Binance momentum':(bn_momentum is not None,.05*(bn_momentum or 0),
                f"10s {bn_move if bn_move is not None else 0:+.1f} bp · basis {bn_basis*10000 if bn_basis is not None else 0:+.1f} bp"),
            'Binance basis':(bn_basis is not None,.02*math.tanh(bn_basis/.0006) if bn_basis is not None else 0.0,
                f"fresh Chainlink-relative basis {bn_basis*10000 if bn_basis is not None else 0:+.1f} bp"),
            'News pulse':(news_matches>0,.025*news_score,
                f"sentiment {news_score:+.2f} · {news_matches} relevant headlines"),
        }
        market_p=_market_probability(current_req)
        shadow_raw,shadow_low,shadow_high,market_impact,impacts=_ensemble(current_f['probability'],current_f['low'],
            current_f['high'],market_p,live_signals,.18)
        shadow_p,cal_info=_calibrate_up(shadow_raw,calibration)
        if cal_info:
            shift=shadow_p-shadow_raw
            shadow_low=max(.01,min(.99,shadow_low+shift))
            shadow_high=max(.01,min(.99,shadow_high+shift))
        if h == 300:
            model_cb_basis=cb_basis*10000 if cb_basis is not None else None
            model_bn_basis=bn_basis*10000 if bn_basis is not None else None
            independent_raw,independent_impacts=_tick_corrected_probability(
                current_f['probability'],bn_move,cb_move,model_bn_basis,model_cb_basis,impacts=True)
            independent_low=_tick_corrected_probability(
                current_f['low'],bn_move,cb_move,model_bn_basis,model_cb_basis)[0]
            independent_high=_tick_corrected_probability(
                current_f['high'],bn_move,cb_move,model_bn_basis,model_cb_basis)[0]
            independent_low,independent_high=sorted((independent_low,independent_high))
        else:
            # The 15m tick correction did not improve the chronological
            # holdout; use the settlement kernel without a hand-set blend.
            independent_raw=current_f['probability']
            independent_low=current_f['low']
            independent_high=current_f['high']
            independent_impacts=[]
        independent_p=independent_raw
        independent_cal_info=None
        market_range=_market_probability_range(current_req)
        p,low,high,prediction_basis=_primary_probability(
            independent_p,independent_low,independent_high)
        ready=current_f['ready'];status=current_f['status']
        direction='UP' if p>=.5 else 'DOWN';side=direction.lower()
        shadow_direction='UP' if shadow_p>=.5 else 'DOWN'
        independent_direction='UP' if independent_p>=.5 else 'DOWN'
        up_bid,up_ask,down_bid,down_ask=(current_req[k] for k in
            ('up_bid','up_ask','down_bid','down_ask'))
        ask=up_ask if direction=='UP' else down_ask
        bid=up_bid if direction=='UP' else down_bid
        edge,conservative=_edge(p,low,high,direction,bid,ask,current_req['fee_rate'],current_req['fee_exponent']) if ready else (None,None)
        shadow_ask=up_ask if shadow_direction=='UP' else down_ask
        shadow_bid=up_bid if shadow_direction=='UP' else down_bid
        shadow_edge,shadow_conservative=_edge(shadow_p,shadow_low,shadow_high,
            shadow_direction,shadow_bid,shadow_ask,current_req['fee_rate'],
            current_req['fee_exponent']) if ready else (None,None)
        independent_side=independent_direction.lower()
        independent_ask=up_ask if independent_direction=='UP' else down_ask
        independent_bid=up_bid if independent_direction=='UP' else down_bid
        independent_edge,independent_conservative=_edge(independent_p,independent_low,
            independent_high,independent_direction,independent_bid,independent_ask,
            current_req['fee_rate'],current_req['fee_exponent']) if ready else (None,None)
        aligned_leads=[]
        for name,score,age,move in (
            ('Coinbase',cb_momentum,coinbase_age,cb_move),
            ('Binance',bn_momentum,binance_age,bn_move)):
            if (score is not None and age is not None and age <= 3 and abs(score) >= .5
                    and (score > 0) == (shadow_direction == 'UP')):
                aligned_leads.append({'source':name,'move_bps':round(move,2),
                                      'score':round(score,3),'age_s':round(age,2)})
        alert_candidate=_lead_lag_alert_candidate(
            current_data['market'].get('conditionId'),ready,market_p,shadow_p,
            shadow_direction,shadow_conservative,current_req.get('up_book_age_s'),
            current_req.get('down_book_age_s'),aligned_leads,int(now_s*1000))
        # The former ensemble has not shown incremental value against CLOB.
        # Keep its edge visible for research, but do not badge it as actionable.
        opportunity=None
        note={'missing_reference':'Waiting for the actual Chainlink opening reference.',
              'stale_data':'Waiting for fresh Chainlink and closed-candle inputs.',
              'unsupported_reference':'This market’s settlement reference is not supported.',
              'averaging_window':'Final TWAP interval. Conditional forecast unavailable; wait.',
              'warming_up':'Collecting enough continuous closed candles.',
              'closed':'Window closed. Waiting for the following market.'}.get(status)
        if ready:
            note=((f'The 5m estimate applies a fitted correction to the Chainlink settlement kernel using fresh Binance and '
                   f'Coinbase 10s moves and Chainlink-relative basis. It excludes Polymarket prices. On a later paired '
                   f'holdout it improved Brier from {_TICK_5M_KERNEL_BRIER:.4f} to {_TICK_5M_HOLDOUT_BRIER:.4f}; the CLOB '
                   f'scored {_TICK_5M_CLOB_BRIER:.4f}. This did not establish an advantage over the market.')
                  if h == 300 else
                  'The 15m estimate uses the Chainlink settlement kernel. Candle-based and tick-based corrections did not '
                  'improve the chronological holdout, so they are excluded. The CLOB midpoint remains a separate benchmark.')
        market_data=current_data['market'];window=current_data['ref']['window_s']
        ref_label=f'Chainlink {window}s TWAP' if window>0 else 'Chainlink spot' if window==0 else 'Unsupported reference'
        cb_px=_number(current_tape.get('coinbase')) or None;bn_px=_number(current_tape.get('binance')) or None
        cb_basis_bps=cb_basis*10000 if cb_basis is not None else None
        bn_basis_bps=bn_basis*10000 if bn_basis is not None else None
        components=[{'name':'Chainlink settlement model','score':2*current_f['probability']-1,
                     'delta_pp':0,'available':ready,'detail':f"spot {spot:.5f} · strike {_number(current_req['strike']):.5f} · {ref_label}"},
                    {'name':'Polymarket CLOB benchmark','score':(market_p-.5)*2 if market_p is not None else 0,
                     'delta_pp':market_impact,'available':market_p is not None,
                     'detail':f"UP midpoint {market_p:.3f}" if market_p is not None else 'fresh two-sided books unavailable'}]
        components += [{'name':x['name'],'score':0,'delta_pp':x['delta_pp'],
                        'available':x['available'],'detail':x['detail']} for x in impacts]
        components += [{'name':'Volatility envelope','score':0,'delta_pp':0,'available':ready,
                        'detail':f"EWMA {current_f['sigma_minute']*100:.4f}% / √minute · shadow sensitivity {shadow_low:.3f}–{shadow_high:.3f}"}]
        independent_components=[{'name':'Chainlink settlement kernel','score':2*current_f['probability']-1,
                     'delta_pp':0,'available':ready,'detail':f"spot {spot:.5f} · strike {_number(current_req['strike']):.5f} · {ref_label}"}]
        independent_components += [{'name':x['name'],'score':0,'delta_pp':x['delta_pp'],
                        'available':x['available'],'detail':x['detail']} for x in independent_impacts]
        independent_components += [{'name':'Volatility sensitivity','score':0,'delta_pp':0,'available':ready,
                        'detail':f"EWMA {current_f['sigma_minute']*100:.4f}% / √minute · range {independent_low:.3f}–{independent_high:.3f} (not a confidence interval)"}]
        slug=market_data.get('slug',f'{a.lower()}-updown-{"5m" if h==300 else "15m"}-{o}')
        event_rows=market_data.get('events') or []
        event_slug=(event_rows[0].get('slug') if event_rows and isinstance(event_rows[0],dict) else None) or slug
        outcome_indices={str(name).strip().lower():i for i,name in enumerate(array(market_data.get('outcomes')))}
        trade_outcome_indices={}
        if 'up' in outcome_indices: trade_outcome_indices['UP']=outcome_indices['up']
        if 'down' in outcome_indices: trade_outcome_indices['DOWN']=outcome_indices['down']
        card={'asset':a,'period':'CURRENT','provisional':False,'horizon_seconds':h,
            'window_open':o*1000,'window_close':(o+h)*1000,'seconds_left':max(0,o+h-now),
            'direction':direction if ready else 'WAIT','up_probability':p,'up_probability_raw':independent_raw,
            'experimental_up_probability':shadow_p,'experimental_up_probability_raw':shadow_raw,
            'experimental_direction':shadow_direction,
            'experimental_probability_low':shadow_low,'experimental_probability_high':shadow_high,
            'independent_up_probability':independent_p,
            'independent_up_probability_raw':independent_raw,
            'independent_probability_low':independent_low,
            'independent_probability_high':independent_high,
            'independent_direction':independent_direction,
            'independent_prediction_basis':'chainlink_settlement_plus_fitted_5m_tick_correction' if h==300 else 'chainlink_settlement_kernel',
            'independent_model_version':TICK_MODEL_VERSION if h==300 else 'chainlink-kernel-v3',
            'independent_model_edge':independent_edge,
            'independent_conservative_edge':independent_conservative,
            'independent_market_gap':independent_p-market_p if market_p is not None else None,
            'settlement_model_probability':current_f['probability'],
            'settlement_model_low':current_f['low'],'settlement_model_high':current_f['high'],
            'prediction_basis':prediction_basis,
            'chance':max(p,1-p),'probability_low':low,'probability_high':high,'model_status':status,
            'settlement_window_s':window,'reference':ref_label,'reference_age_s':current_req['reference_age_s'],
            'candle_age_s':current_req['candle_age_s'],'quality':current_f['quality'],
            'polymarket_up':market_p,'polymarket_up_price':up_ask,'polymarket_down_price':down_ask,
            'calibration':cal_info,
            'independent_calibration':independent_cal_info,
            'polymarket_up_bid':up_bid,'polymarket_down_bid':down_bid,
            'market_quote_low':market_range[0] if market_range else None,
            'market_quote_high':market_range[1] if market_range else None,
            'model_market_gap':independent_p-market_p if market_p is not None else None,
            'experimental_model_market_gap':shadow_p-market_p if market_p is not None else None,
            'spot':spot,'strike':_number(current_req['strike']),'fee_rate':current_req['fee_rate'],
            'execution_hurdle':ask+current_req['fee_rate']*(ask*(1-ask))**current_req['fee_exponent']+.005
                if ask is not None and current_req['fee_rate']>=0 else None,
            'predicted_ask_depth':current_req.get(independent_side+'_ask_depth'),'book_age_s':current_req['book_age_s'],
            'experimental_model_edge':shadow_edge,
            'experimental_conservative_edge':shadow_conservative,
            'alert_candidate':alert_candidate,
            'coinbase_price':cb_px,'coinbase_age_s':coinbase_age,'coinbase_basis_bps':cb_basis_bps,
            'binance_price':bn_px,'binance_age_s':binance_age,'binance_basis_bps':bn_basis_bps,
            'coinbase_momentum_bps':cb_move,'binance_momentum_bps':bn_move,
            'coinbase_momentum_score':cb_momentum,'binance_momentum_score':bn_momentum,
            'ensemble_weights':{'model':'fitted 5m cross-feed logistic correction' if h==300 else 'Chainlink settlement kernel'},
            'model_edge':edge,'conservative_edge':conservative,'manual_note':note,
            'condition_id':market_data.get('conditionId'),'slug':slug,
            'settlement_source':market_data.get('resolutionSource'),
            'market_url':'https://polymarket.com/event/'+event_slug,
            'market_outcome_indices':trade_outcome_indices,
            'reference_price_ts':ref.get('reference_ts'),
            'reference_strike_ts':ref.get('strike_ts'),
            'input_timestamps':{
                'chainlink_spot':current_tape.get('spot_ts'),
                'chainlink_reference':ref.get('reference_ts'),
                'opening_reference':ref.get('strike_ts'),
                'coinbase':current_tape.get('coinbase_ts'),
                'coinbase_10s_anchor':current_tape.get('coinbase_10_ts'),
                'binance':current_tape.get('binance_ts'),
                'binance_10s_anchor':current_tape.get('binance_10_ts'),
                'last_closed_candle_ms':bars[-1]['ts'] if bars else None,
                'up_book_age_s':current_req.get('up_book_age_s'),
                'down_book_age_s':current_req.get('down_book_age_s')},
            'oracle_delta_pct':100*(_number(current_data['ref'].get('reference_now'))/
                _number(current_req['strike'])-1) if _number(current_req['strike'])>0 else None,
            'technical':{**technical,'vol_1m_pct':current_f['sigma_minute']*100},
            'order_flow':None,
            'news_signal':{'score':news_score,'matched_headlines':news_matches},
            'candle_store':{'samples':current_f['samples'],'source':candles.get(a,{}).get('source','Unavailable'),
                'last_closed_ms':bars[-1]['ts'] if bars else None},
            'sparkline':[x['close'] for x in bars[-60:]],'components':components,
            'independent_components':independent_components,
            'independent_signal_contributions':independent_impacts,
            'signal_contributions':impacts,'opportunity':opportunity}

        next_market=next_data['market'];next_window=next_data['ref']['window_s']
        next_technical=_technical_features(bars,next_f['sigma_minute'])
        next_req_market=_market_probability(next_req)
        next_shadow_p,next_shadow_low,next_shadow_high,next_market_impact,next_impacts=_ensemble(next_f['probability'],
            next_f['low'],next_f['high'],next_req_market,live_signals,.08)
        # Upcoming windows have no official opening reference. Keep this
        # forecast provisional and use only the kernel's capped drift input.
        next_independent_raw=next_f['probability']
        next_independent_low=next_f['low']
        next_independent_high=next_f['high']
        next_independent_p=next_independent_raw
        next_independent_impacts=[]
        next_independent_cal_info=None
        next_market_range=_market_probability_range(next_req)
        next_p,next_low,next_high,next_prediction_basis=_primary_probability(
            next_independent_p,next_independent_low,next_independent_high)
        next_direction='UP' if next_p>=.5 else 'DOWN';next_side=next_direction.lower()
        next_shadow_direction='UP' if next_shadow_p>=.5 else 'DOWN'
        next_independent_direction='UP' if next_independent_p>=.5 else 'DOWN'
        nup_bid,nup_ask,ndown_bid,ndown_ask=(next_req[k] for k in
            ('up_bid','up_ask','down_bid','down_ask'))
        next_ask=nup_ask if next_direction=='UP' else ndown_ask
        next_bid=nup_bid if next_direction=='UP' else ndown_bid
        next_edge,next_conservative=_edge(next_p,next_low,next_high,next_direction,next_bid,next_ask,
            next_req['fee_rate'],next_req['fee_exponent']) if next_f['ready'] else (None,None)
        next_shadow_ask=nup_ask if next_shadow_direction=='UP' else ndown_ask
        next_shadow_bid=nup_bid if next_shadow_direction=='UP' else ndown_bid
        next_shadow_edge,next_shadow_conservative=_edge(next_shadow_p,next_shadow_low,
            next_shadow_high,next_shadow_direction,next_shadow_bid,next_shadow_ask,
            next_req['fee_rate'],next_req['fee_exponent']) if next_f['ready'] else (None,None)
        next_independent_side=next_independent_direction.lower()
        next_independent_ask=nup_ask if next_independent_direction=='UP' else ndown_ask
        next_independent_bid=nup_bid if next_independent_direction=='UP' else ndown_bid
        next_independent_edge,next_independent_conservative=_edge(
            next_independent_p,next_independent_low,next_independent_high,
            next_independent_direction,next_independent_bid,next_independent_ask,
            next_req['fee_rate'],next_req['fee_exponent']) if next_f['ready'] else (None,None)
        next_slug=next_market.get('slug',f'{a.lower()}-updown-{"5m" if h==300 else "15m"}-{o+h}')
        next_event_rows=next_market.get('events') or []
        next_event_slug=(next_event_rows[0].get('slug') if next_event_rows and isinstance(next_event_rows[0],dict) else None) or next_slug
        next_outcome_indices={str(name).strip().lower():i for i,name in enumerate(array(next_market.get('outcomes')))}
        next_trade_outcome_indices={}
        if 'up' in next_outcome_indices: next_trade_outcome_indices['UP']=next_outcome_indices['up']
        if 'down' in next_outcome_indices: next_trade_outcome_indices['DOWN']=next_outcome_indices['down']
        next_ref_label=(f'Estimated next open · Chainlink {next_window}s TWAP' if next_window>0 else
                        'Estimated next open · Chainlink spot' if next_window==0 else 'Unsupported reference')
        next_status='provisional' if next_f['ready'] else next_f['status']
        next_note=('Pre-open outlook from the Chainlink kernel, using the current spot as a projected opening anchor and a conservative recent-drift estimate. '
                   'The actual opening reference is unknown until this market starts; this provisional model has not established an edge.' if next_f['ready'] else
                   'Next-window outlook is waiting for fresh supported inputs.')
        next_components=[{'name':'Estimated opening anchor','score':0,'delta_pp':0,
                          'available':next_f['ready'],'detail':f"{_number(next_req['strike']):.5f} · provisional"},
                         {'name':'Polymarket CLOB benchmark','score':(next_req_market-.5)*2 if next_req_market is not None else 0,
                          'delta_pp':next_market_impact,'available':next_req_market is not None,
                          'detail':f"UP midpoint {next_req_market:.3f}" if next_req_market is not None else 'fresh two-sided books unavailable'}]
        next_components += [{'name':x['name'],'score':0,'delta_pp':x['delta_pp'],
                             'available':x['available'],'detail':x['detail']} for x in next_impacts]
        next_independent_components=[{'name':'Estimated Chainlink opening anchor','score':0,'delta_pp':0,
                          'available':next_f['ready'],'detail':f"{_number(next_req['strike']):.5f} · provisional"}]
        next_independent_components += [{'name':x['name'],'score':0,'delta_pp':x['delta_pp'],
                             'available':x['available'],'detail':x['detail']} for x in next_independent_impacts]
        next_independent_components += [{'name':'Volatility sensitivity','score':0,'delta_pp':0,
                          'available':next_f['ready'],
                          'detail':f"EWMA {next_f['sigma_minute']*100:.4f}% / √minute · range {next_independent_low:.3f}–{next_independent_high:.3f} (not a confidence interval)"}]
        card['next_window']={'asset':a,'period':'NEXT','provisional':True,'horizon_seconds':h,
            'window_open':(o+h)*1000,'window_close':(o+2*h)*1000,
            'seconds_until_open':max(0,o+h-now),'seconds_left':h,
            'direction':next_direction if next_f['ready'] else 'WAIT','up_probability':next_p,
            'up_probability_raw':next_independent_raw,
            'experimental_up_probability':next_shadow_p,
            'experimental_up_probability_raw':next_shadow_p,
            'experimental_direction':next_shadow_direction,
            'experimental_probability_low':next_shadow_low,
            'experimental_probability_high':next_shadow_high,
            'independent_up_probability':next_independent_p,
            'independent_up_probability_raw':next_independent_raw,
            'independent_probability_low':next_independent_low,
            'independent_probability_high':next_independent_high,
            'independent_direction':next_independent_direction,
            'independent_prediction_basis':'chainlink_settlement_kernel_provisional',
            'independent_model_version':'chainlink-kernel-v3-provisional',
            'independent_model_edge':next_independent_edge,
            'independent_conservative_edge':next_independent_conservative,
            'independent_market_gap':next_independent_p-next_req_market if next_req_market is not None else None,
            'settlement_model_probability':next_f['probability'],
            'settlement_model_low':next_f['low'],'settlement_model_high':next_f['high'],
            'prediction_basis':next_prediction_basis,
            'chance':max(next_p,1-next_p),'probability_low':next_low,'probability_high':next_high,
            'model_status':next_status,'settlement_window_s':next_window,'reference':next_ref_label,
            'reference_age_s':next_req['reference_age_s'],'estimated_strike':next_req['strike'],
            'candle_age_s':next_req['candle_age_s'],'quality':next_f['quality'],
            'polymarket_up':next_req_market,'polymarket_up_price':nup_ask,
            'polymarket_down_price':ndown_ask,'polymarket_up_bid':nup_bid,
            'polymarket_down_bid':ndown_bid,
            'independent_calibration':next_independent_cal_info,
            'market_quote_low':next_market_range[0] if next_market_range else None,
            'market_quote_high':next_market_range[1] if next_market_range else None,
            'model_market_gap':next_independent_p-next_req_market if next_req_market is not None else None,
            'experimental_model_market_gap':next_shadow_p-next_req_market if next_req_market is not None else None,
            'spot':spot,'strike':next_req['strike'],'fee_rate':next_req['fee_rate'],
            'execution_hurdle':next_ask+next_req['fee_rate']*(next_ask*(1-next_ask))**next_req['fee_exponent']+.005
                if next_ask is not None and next_req['fee_rate']>=0 else None,
            'predicted_ask_depth':next_req.get(next_independent_side+'_ask_depth'),'book_age_s':next_req['book_age_s'],
            'experimental_model_edge':next_shadow_edge,
            'experimental_conservative_edge':next_shadow_conservative,
            'coinbase_price':cb_px,'coinbase_age_s':coinbase_age,'coinbase_basis_bps':cb_basis_bps,
            'binance_price':bn_px,'binance_age_s':binance_age,'binance_basis_bps':bn_basis_bps,
            'coinbase_momentum_bps':cb_move,'binance_momentum_bps':bn_move,
            'coinbase_momentum_score':cb_momentum,'binance_momentum_score':bn_momentum,
            'model_edge':next_edge,'conservative_edge':next_conservative,'manual_note':next_note,
            'condition_id':next_market.get('conditionId'),'slug':next_slug,
            'settlement_source':next_market.get('resolutionSource'),
            'market_url':'https://polymarket.com/event/'+next_event_slug,
            'market_outcome_indices':next_trade_outcome_indices,
            'reference_price_ts':next_data['ref'].get('reference_ts'),
            'reference_strike_ts':None,
            'input_timestamps':{
                'chainlink_spot':current_tape.get('spot_ts'),
                'estimated_opening_anchor':spot_ts,
                'coinbase':current_tape.get('coinbase_ts'),
                'coinbase_10s_anchor':current_tape.get('coinbase_10_ts'),
                'binance':current_tape.get('binance_ts'),
                'binance_10s_anchor':current_tape.get('binance_10_ts'),
                'last_closed_candle_ms':bars[-1]['ts'] if bars else None,
                'up_book_age_s':next_req.get('up_book_age_s'),
                'down_book_age_s':next_req.get('down_book_age_s')},
            'oracle_delta_pct':None,'technical':{**next_technical,'vol_1m_pct':next_f['sigma_minute']*100,
                'drift_1m_pct':next_f['drift_minute']*100,'drift_samples':next_f['drift_samples']},
            'order_flow':None,
            'news_signal':{'score':news_score,'matched_headlines':news_matches},
            'candle_store':{'samples':next_f['samples'],'source':candles.get(a,{}).get('source','Unavailable'),
                'last_closed_ms':bars[-1]['ts'] if bars else None},
            'sparkline':[x['close'] for x in bars[-60:]],'components':next_components,
            'independent_components':next_independent_components,
            'independent_signal_contributions':next_independent_impacts,
            'signal_contributions':next_impacts,'opportunity':None}
        cards.append(card)
    try: quality=_journal(root,cards,markets,public_json,resolve=resolve)
    except Exception:
        quality={'status':'unavailable','resolved_markets':0};errors.append('Forecast journal unavailable')
    calibration_meta={'version':SHADOW_CALIBRATION_VERSION,
        'scope':'experimental shadow forecast only',
        'settled':calibration.get('total',0),'deciles':len(calibration.get('deciles',{})),
        'min_n':calibration.get('min_n',20),
        'active':any(d['n']>=calibration.get('min_n',20) for d in calibration.get('deciles',{}).values())} if calibration else None
    independent_model_meta={
        'version':INDEPENDENT_MODEL_VERSION,
        'five_minute_method':TICK_MODEL_VERSION,
        'five_minute_training_rows':_TICK_5M_TRAIN_N,
        'five_minute_trained_through_utc':'2026-10-01T06:32:03Z',
        'five_minute_holdout_rows':_TICK_5M_HOLDOUT_N,
        'five_minute_holdout_start_utc':'2026-10-05',
        'five_minute_holdout_end_utc':'2026-10-09',
        'five_minute_holdout_brier':_TICK_5M_HOLDOUT_BRIER,
        'five_minute_kernel_brier':_TICK_5M_KERNEL_BRIER,
        'five_minute_clob_brier':_TICK_5M_CLOB_BRIER,
        'fifteen_minute_method':'chainlink-kernel-v3',
        'fifteen_minute_holdout_rows':1058,
        'fifteen_minute_kernel_brier':0.089542,
        'fifteen_minute_clob_brier':0.085311,
        'calibration_adjustment_applied':False,
    }
    independent_calibration_meta={
        'version':INDEPENDENT_MODEL_VERSION,
        'scope':'reporting only; no decile adjustment is applied',
        'settled':quality.get('independent_paired_markets',0),
        'active':False,
    }
    return {'generated_at':int(time.time()*1000),'inputs_at':int(now_s*1000),'cards':cards,
        'model_version':MODEL_VERSION,'independent_model_version':INDEPENDENT_MODEL_VERSION,
        'refresh_interval_s':SERVER_REFRESH_SECONDS,'quality':quality,'calibration':calibration_meta,
        'independent_model':independent_model_meta,
        'independent_calibration':independent_calibration_meta,
        'calibration_deciles':[{'bucket':k,'n':v['n'],'wins':v['wins'],
            'realized':round(v['wins']/v['n'],3) if v['n'] else None,'avg_p':round(v['avg_p'],3)}
            for k,v in sorted((calibration or {}).get('deciles',{}).items())],
        'compute_ms':round((time.monotonic()-started)*1000),'news':stories[:8],
        'news_sources':news.get('sources',[]),
        'sources':['Chainlink spot + official settlement TWAP','Coinbase live price and lead/lag',
            'Binance live price and closed candles','Local candle store / Binance backfill',
            'Polymarket CLOB midpoint, spread and executable depth','Asset-matched crypto headlines'],
        'errors':errors,
        'disclaimer':'The displayed independent probability is CLOB-free. For 5m windows it is a fixed logistic correction to the Chainlink settlement kernel, fit on 19,045 earlier resolved forecasts through 2026-10-01 using fresh 10-second Binance/Coinbase momentum, each venue basis to Chainlink, and source-availability flags. Its later 5m holdout Brier was 0.168435 versus 0.169288 for the kernel and 0.164335 for the CLOB midpoint: a small improvement over the kernel, while the market still scored better. Current 15m and provisional next-window estimates use the Chainlink kernel only; candle trend and news are context and do not enter these independent probabilities. Taker flow is not sampled by the prediction refresh. No decile recalibration is applied. The normalized midpoint of fresh UP/DOWN books is displayed separately. No reliable market advantage or net trading edge has been established. For current windows, the kernel is P(UP)=Phi((ln(spot/strike)+mu*(T-W/2)/60)/(sigma_minute*sqrt((T-2W/3)/60))); live mu=0, while upcoming windows use a small capped recent drift and stay provisional. Sigma is an EWMA of completed 1-minute log returns. The displayed range is volatility sensitivity, not a confidence interval. Paired Brier scores compare forecasts and the CLOB at the 120–179 seconds remaining checkpoint; lower is better. Any edge estimate includes the venue fee schedule and 0.5¢ slippage reserve. Legacy lead/lag alerts remain unverified candidates and require a human to recheck the live book.'}
