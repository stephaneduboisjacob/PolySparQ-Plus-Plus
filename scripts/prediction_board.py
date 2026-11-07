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
from decimal import Decimal, InvalidOperation, ROUND_CEILING
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlencode

_cache = {}
_cache_lock = threading.Lock()
_CACHE_MAX_ENTRIES = 256
_CACHE_MAX_AGE_SECONDS = 900.0
_quality_cache = {}
_quality_cache_lock = threading.Lock()
_calibration_cache = {}
_calibration_cache_lock = threading.Lock()
QUALITY_SUMMARY_REFRESH_SECONDS = 30.0
ASSETS = ('BTC', 'ETH', 'SOL', 'XRP')
INDEPENDENT_MODEL_VERSION = 'vantagepulse-independent-v2'
MODEL_VERSION = INDEPENDENT_MODEL_VERSION
MARKET_BENCHMARK_VERSION = 'market-benchmark-v5'
TICK_MODEL_VERSION = 'vantagepulse-tick-5m-v2'
SHADOW_CALIBRATION_VERSION = 'multisource-v4'
SERVER_REFRESH_SECONDS = 1.0
SNAPSHOT_STALE_SECONDS = 3.0
MIN_REFRESH_GAP_SECONDS = 0.05
_BOARD_IO_EXECUTOR = ThreadPoolExecutor(
    max_workers=12, thread_name_prefix='prediction-board-io')

# Fixed, CLOB-free 5-minute correction (v2): a 17-feature logistic refit of the
# settlement kernel on resolved checkpoints. The v1 correction re-scaled the
# kernel logit with four tape features; the v2 refit re-derives the correction
# from the raw settlement inputs instead, adding distance-to-strike in
# volatility units, the seconds-left scale, candle trend and 5m/15m return
# z-scores, the news pulse and a volatility-regime tilt. Every tape feature
# (Binance/Coinbase 10s momentum, Chainlink-relative basis) carries an
# availability flag so a stale feed degrades instead of being imputed.
# Fitted with L2-regularised logistic regression on 25,712 resolved CLOB-free
# checkpoints captured before 2026-10-24 and validated on the strictly later
# 12,652-row holdout, where it beat both the raw kernel and the CLOB midpoint.
# Polymarket prices never enter the feature vector.
# Coefficients in feature order:
#   0 intercept              8 Binance basis availability
#   1 kernel logit           9 tanh(Binance basis bp / 8)
#   2 seconds-left / 150    10 Coinbase basis availability
#   3 distance-to-strike z  11 tanh(Coinbase basis bp / 8)
#   4 Binance momentum ok   12 candle trend score (-1..1)
#   5 Binance momentum/10   13 tanh(5m return z / 2)
#   6 Coinbase momentum ok  14 tanh(15m return z / 2)
#   7 Coinbase momentum/10  15 tanh(news pulse / 2)
#                          16 tanh((vol %/min - 0.02) / 0.05)
_TICK_V2_WEIGHTS = (1.2265, 0.0568, -1.1686, 2.0744,
                    -0.5120, 0.4970, -0.0932, -0.1351,
                    0.3749, 0.3822, 0.0341, 0.9743,
                    -1.2631, 0.6565, 0.1352, -0.4894, 0.1106)
_TICK_V2_SPLIT_TS = 1792800000  # 2026-10-24 00:00:00 UTC (holdout start)
_TICK_V2_TRAIN_N = 25712
_TICK_V2_HOLDOUT_N = 12652
_TICK_V2_HOLDOUT_BRIER = 0.1420
_TICK_V2_KERNEL_BRIER = 0.1430
_TICK_V2_V1_BRIER = 0.1444
_TICK_V2_CLOB_BRIER = 0.1440
_TICK_V2_LOGLOSS = 0.4341
_TICK_V2_V1_LOGLOSS = 0.4412
_TICK_V2_CLOB_LOGLOSS = 0.4391


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
            for field in ('reference_age_s','candle_age_s','book_age_s',
                          'polymarket_up_book_age_s','polymarket_down_book_age_s',
                          'coinbase_age_s','binance_age_s'):
                raw_age = card.get(field)
                if raw_age is not None:
                    try:
                        value = float(raw_age)
                    except (TypeError, ValueError, OverflowError):
                        value = math.nan
                    card[field] = value + age if math.isfinite(value) and value >= 0 else None
            status = None
            if card.get('provisional') and now_ms >= card['window_open']:
                status = 'awaiting_open_reference'
            elif now_ms >= card['window_close']:
                status = 'closed'
            elif not card.get('provisional') and 0 < card['seconds_left'] <= card.get('settlement_window_s',0):
                status = 'averaging_window'
            elif (age > stale_after_s or card.get('reference_age_s', 0) is None or
                  card.get('reference_age_s', 0) > 5 or card.get('candle_age_s', -1) is None or
                  card.get('candle_age_s', -1) < 0 or card.get('candle_age_s', 0) > 120):
                status = 'stale_data'
            if status:
                card.update(model_status=status, direction='WAIT', opportunity=None,
                            model_edge=None, conservative_edge=None, model_market_gap=None,
                            independent_direction='WAIT', independent_model_edge=None,
                            independent_conservative_edge=None, independent_market_gap=None,
                            alert_candidate=None, directional_outcome_economics=None,
                            manual_note='Waiting for a fresh forecast for this window.')
                for field in ('up_probability', 'up_probability_raw', 'probability_low',
                              'probability_high', 'settlement_model_probability',
                              'settlement_model_low', 'settlement_model_high',
                              'independent_up_probability', 'independent_up_probability_raw',
                              'independent_probability_low', 'independent_probability_high'):
                    card[field] = None
            raw_book_age = card.get('book_age_s')
            try:
                book_age = float(raw_book_age) if raw_book_age is not None else math.nan
            except (TypeError, ValueError, OverflowError):
                book_age = math.nan
            book_age_fresh = math.isfinite(book_age) and 0 <= book_age <= 3
            if age > stale_after_s or not book_age_fresh:
                for field in ('polymarket_up','polymarket_up_price','polymarket_down_price','polymarket_up_bid','polymarket_down_bid','execution_hurdle',
                              'model_edge','conservative_edge','independent_model_edge',
                              'independent_conservative_edge','independent_market_gap',
                              'predicted_ask_depth',
                              'polymarket_up_bid_depth','polymarket_up_ask_depth',
                              'polymarket_down_bid_depth','polymarket_down_ask_depth',
                              'opportunity','alert_candidate','directional_outcome_economics'):
                    card[field] = None
                card['paired_outcome_economics'] = None
    return out


class PredictionService:
    """One producer for the whole cockpit; readers never wait for network I/O."""
    def __init__(self, query, public_json, root, news_fn, interval=SERVER_REFRESH_SECONDS,
                 history_init=None, history_writer=None, history_resolver=None,
                 gate_writer=None):
        self.query, self.public_json, self.root, self.news_fn = query, public_json, root, news_fn
        self.history_init = history_init
        self.history_writer = history_writer
        self.history_resolver = history_resolver
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
        self._history_ready = threading.Event()
        self._history_error = None
        if not self.history_init:
            self._history_ready.set()

    def start(self):
        with self._lock:
            if self._threads:
                return
            if self.history_init:
                thread = threading.Thread(name='prediction-history-init',
                                          target=self._init_history, daemon=True)
                self._threads.append(thread)
                thread.start()
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
                if self.history_writer and self._history_ready.is_set():
                    try:
                        self.history_writer(board)
                        self._history_error = None
                    except Exception as exc:
                        self._history_error = type(exc).__name__
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
            if self.history_resolver and self._history_ready.is_set():
                try:
                    self.history_resolver()
                    self._history_error = None
                except Exception as exc:
                    self._history_error = type(exc).__name__
            self._stop.wait(30)

    def _init_history(self):
        while not self._stop.is_set():
            try:
                self.history_init()
                self._history_error = None
                self._history_ready.set()
                return
            except Exception as exc:
                self._history_error = type(exc).__name__
                self._stop.wait(15)

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
        cached_at = time.monotonic()
        _cache[key] = (cached_at, value)
        if len(_cache) > _CACHE_MAX_ENTRIES:
            expired_before = cached_at - _CACHE_MAX_AGE_SECONDS
            for old_key, (old_at, _old_value) in list(_cache.items()):
                if old_key != key and old_at < expired_before:
                    _cache.pop(old_key, None)
            while len(_cache) > _CACHE_MAX_ENTRIES:
                oldest = min(_cache, key=lambda old_key: _cache[old_key][0])
                if oldest == key and len(_cache) > 1:
                    oldest = min((old_key for old_key in _cache if old_key != key),
                                 key=lambda old_key: _cache[old_key][0])
                _cache.pop(oldest, None)
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


def ask_book_levels(book):
    """Return validated ask levels in lowest-price-first order."""
    levels = []
    for row in book.get('asks', []) or []:
        try:
            price = valid_price(row.get('price'))
            size = float(row.get('size', 0))
            if price is not None and math.isfinite(size) and size > 0:
                levels.append({'price': price, 'size': size})
        except (TypeError, ValueError, OverflowError, AttributeError):
            continue
    return sorted(levels, key=lambda level: level['price'])


def _side_book_is_fresh(request, side):
    """A side is usable only when its own CLOB timestamp is within three seconds."""
    try:
        age = float(request.get(f'{side.lower()}_book_age_s'))
    except (TypeError, ValueError, OverflowError):
        return False
    return math.isfinite(age) and 0 <= age <= 3


def _fresh_side_ask_levels(request, side, levels):
    """Do not reuse raw ask levels when the side's own book timestamp is stale."""
    return list(levels or []) if _side_book_is_fresh(request, side) else []


def _directional_economics_for_request(probability, request, ask_levels, ready=True):
    """Build a research-only size walk using only independently fresh side books."""
    if not ready:
        return None

    fresh_quotes = {}
    for side in ('UP', 'DOWN'):
        key = side.lower()
        if _side_book_is_fresh(request, side):
            fresh_quotes[side] = (request.get(f'{key}_ask'),
                                  request.get(f'{key}_ask_depth'),
                                  ask_levels.get(side))
        else:
            fresh_quotes[side] = (None, None, [])

    return binary_directional_economics(
        probability, fresh_quotes['UP'][0], fresh_quotes['DOWN'][0],
        fresh_quotes['UP'][1], fresh_quotes['DOWN'][1],
        request.get('fee_rate'), request.get('fee_exponent'),
        up_ask_levels=_fresh_side_ask_levels(request, 'UP', fresh_quotes['UP'][2]),
        down_ask_levels=_fresh_side_ask_levels(request, 'DOWN', fresh_quotes['DOWN'][2]))


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


def binary_pair_economics(up_ask, down_ask, up_depth, down_depth,
                          fee_rate, fee_exponent, up_ask_levels=None,
                          down_ask_levels=None):
    """Estimate paired UP/DOWN taker costs across visible ask depth.

    Fees use the documented per-market taker formula and are rounded upward to
    1e-5 USDC so this display does not understate the fee estimate. Missing
    quotes/schedules fail closed. Size scenarios are read-only estimates, not
    guaranteed fills or execution/bot-entry signals.
    """
    def dec(value):
        try:
            parsed = Decimal(str(value))
        except (InvalidOperation, TypeError, ValueError):
            return None
        return parsed if parsed.is_finite() else None

    up, down, rate, exponent = map(dec, (up_ask, down_ask, fee_rate, fee_exponent))
    if (up is None or down is None or rate is None or exponent is None or
            not Decimal(0) < up < Decimal(1) or
            not Decimal(0) < down < Decimal(1) or
            not Decimal(0) <= rate <= Decimal(1) or exponent != Decimal(1)):
        return {"available": False, "reason": "both asks and a supported fee schedule are required"}

    increment = Decimal("0.00001")
    try:
        up_fee = (rate * up * (Decimal(1) - up)).quantize(
            increment, rounding=ROUND_CEILING)
        down_fee = (rate * down * (Decimal(1) - down)).quantize(
            increment, rounding=ROUND_CEILING)
    except InvalidOperation:
        return {"available": False, "reason": "fee estimate is outside supported numeric range"}
    gross_cost = up + down
    fees = up_fee + down_fee
    net_cost = gross_cost + fees
    net_edge = Decimal(1) - net_cost

    depths = [dec(up_depth), dec(down_depth)]
    top_pair_depth = (min(depths) if all(v is not None and v >= 0 for v in depths)
                      else None)

    def levels(raw, fallback_price, fallback_depth):
        result = []
        for level in raw or []:
            try:
                price, size = dec(level.get('price')), dec(level.get('size'))
            except AttributeError:
                continue
            if (price is not None and size is not None and
                    Decimal(0) < price < Decimal(1) and size > 0):
                result.append((price, size))
        result.sort(key=lambda level: level[0])
        if not result:
            depth = dec(fallback_depth)
            if depth is not None and depth > 0:
                result = [(dec(fallback_price), depth)]
        return result

    up_levels = levels(up_ask_levels, up, up_depth)
    down_levels = levels(down_ask_levels, down, down_depth)
    visible_depths = [sum((size for _price, size in book), Decimal(0)) if book else None
                      for book in (up_levels, down_levels)]
    visible_pair_depth = (min(visible_depths)
                          if all(value is not None for value in visible_depths) else None)
    as_number = lambda value: round(float(value), 8)

    def buy(quantity, book):
        remaining = Decimal(quantity)
        gross = Decimal(0)
        raw_fee = Decimal(0)
        for price, available in book:
            taken = min(remaining, available)
            gross += price * taken
            raw_fee += rate * price * (Decimal(1) - price) * taken
            remaining -= taken
            if remaining <= 0:
                break
        if remaining > 0:
            return None
        fee = raw_fee.quantize(increment, rounding=ROUND_CEILING)
        return gross, fee

    size_scenarios = []
    for quantity in (1, 5, 10, 25):
        up_fill = buy(quantity, up_levels)
        down_fill = buy(quantity, down_levels)
        if up_fill is None or down_fill is None:
            size_scenarios.append({
                "shares": quantity, "available": False,
                "up_visible_shares": (as_number(visible_depths[0])
                                       if visible_depths[0] is not None else None),
                "down_visible_shares": (as_number(visible_depths[1])
                                         if visible_depths[1] is not None else None),
            })
            continue
        scenario_gross = up_fill[0] + down_fill[0]
        scenario_fees = up_fill[1] + down_fill[1]
        scenario_net_cost = scenario_gross + scenario_fees
        total_edge = Decimal(quantity) - scenario_net_cost
        size_scenarios.append({
            "shares": quantity, "available": True,
            "gross_cost": as_number(scenario_gross),
            "estimated_fees": as_number(scenario_fees),
            "net_cost": as_number(scenario_net_cost),
            "total_net_edge": as_number(total_edge),
            "net_cost_per_pair": as_number(scenario_net_cost / Decimal(quantity)),
            "net_edge_per_pair": as_number(total_edge / Decimal(quantity)),
        })
    return {
        "available": True,
        "gross_cost_per_pair": as_number(gross_cost),
        "estimated_fee_per_pair": as_number(fees),
        "net_cost_per_pair": as_number(net_cost),
        "net_edge_per_pair": as_number(net_edge),
        "top_ask_pair_depth": as_number(top_pair_depth) if top_pair_depth is not None else None,
        "visible_pair_depth": as_number(visible_pair_depth) if visible_pair_depth is not None else None,
        "size_scenarios": size_scenarios,
        "fee_rounding": "conservative_up_1e-5_usdc",
    }


def binary_directional_economics(up_probability, up_ask, down_ask,
                                 up_depth, down_depth, fee_rate, fee_exponent,
                                 up_ask_levels=None, down_ask_levels=None):
    """Walk visible UP/DOWN asks and compare model payout with cost.

    This is a descriptive expected-value scenario, not a recommendation or
    execution gate. It assumes the independent forecast probability is
    calibrated and shares settle to $1/$0. Only the supported exponent-1 fee
    schedule is estimated, with total fees rounded up to $0.00001.
    """
    def dec(value):
        try:
            parsed = Decimal(str(value))
        except (InvalidOperation, TypeError, ValueError):
            return None
        return parsed if parsed.is_finite() else None

    probability, rate, exponent = map(dec, (up_probability, fee_rate, fee_exponent))
    if (probability is None or not Decimal(0) <= probability <= Decimal(1) or
            rate is None or not Decimal(0) <= rate <= Decimal(1) or
            exponent != Decimal(1)):
        return {"available": False, "reason": "forecast and supported fee schedule are required"}

    def levels(raw, fallback_price, fallback_depth):
        result = []
        for level in raw or []:
            try:
                price, size = dec(level.get('price')), dec(level.get('size'))
            except AttributeError:
                continue
            if (price is not None and size is not None and
                    Decimal(0) < price < Decimal(1) and size > 0):
                result.append((price, size))
        result.sort(key=lambda level: level[0])
        if not result:
            price, depth = dec(fallback_price), dec(fallback_depth)
            if price is not None and Decimal(0) < price < Decimal(1) and depth is not None and depth > 0:
                result = [(price, depth)]
        return result

    books = {
        "UP": levels(up_ask_levels, up_ask, up_depth),
        "DOWN": levels(down_ask_levels, down_ask, down_depth),
    }
    if not any(books.values()):
        return {"available": False, "reason": "visible ask depth is unavailable"}

    increment = Decimal("0.00001")
    as_number = lambda value: round(float(value), 8)
    sides = {}
    for side, book in books.items():
        side_probability = probability if side == "UP" else Decimal(1) - probability
        visible = sum((size for _price, size in book), Decimal(0)) if book else None
        scenarios = []
        for quantity in (1, 5, 10, 25):
            remaining = Decimal(quantity)
            gross_cost = Decimal(0)
            raw_fee = Decimal(0)
            for price, available in book:
                taken = min(remaining, available)
                gross_cost += price * taken
                raw_fee += rate * price * (Decimal(1) - price) * taken
                remaining -= taken
                if remaining <= 0:
                    break
            if remaining > 0:
                scenarios.append({
                    "shares": quantity, "available": False,
                    "visible_shares": as_number(visible) if visible is not None else None,
                })
                continue
            fees = raw_fee.quantize(increment, rounding=ROUND_CEILING)
            net_cost = gross_cost + fees
            expected_payout = side_probability * Decimal(quantity)
            expected_net_value = expected_payout - net_cost
            scenarios.append({
                "shares": quantity, "available": True,
                "gross_cost": as_number(gross_cost),
                "estimated_fees": as_number(fees),
                "net_cost": as_number(net_cost),
                "average_ask": as_number(gross_cost / Decimal(quantity)),
                "model_probability": as_number(side_probability),
                "expected_payout": as_number(expected_payout),
                "expected_net_value": as_number(expected_net_value),
                "break_even_probability": as_number(net_cost / Decimal(quantity)),
            })
        sides[side] = {
            "model_probability": as_number(side_probability),
            "visible_ask_shares": as_number(visible) if visible is not None else None,
            "scenarios": scenarios,
        }
    return {
        "available": True, "sides": sides,
        "fee_rounding": "conservative_up_1e-5_usdc",
    }


def top_depth(book, side, price):
    target = valid_price(price)
    if target is None:
        return None
    try:
        depth = 0.0
        for level in book.get(side, []) or []:
            try:
                level_price = valid_price(level.get('price'))
                size = float(level.get('size', 0))
            except (TypeError, ValueError, OverflowError, AttributeError):
                continue
            if (level_price is not None and math.isfinite(size) and size > 0 and
                    abs(level_price-target) < 1e-9):
                depth += size
        return depth if math.isfinite(depth) else None
    except (TypeError, ValueError, OverflowError, AttributeError):
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
        if 'static_p' not in columns:
            # Preserves the raw model output before any future recalibration
            # layer, so the refit corpus stays separable from displayed p.
            db.execute('ALTER TABLE forecasts ADD COLUMN static_p REAL')
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
            (cid,slug,bucket,captured,closes,p,market_p,outcome,version,shadow_p,static_p,
             diagnostics_json)
            VALUES (?,?,?,?,?,?,?,NULL,?,?,?,?)'''
        for c in cards:
            if c['model_status'] == 'ready' and c.get('condition_id'):
                bucket = c['seconds_left']//60
                diagnostics = _diagnostic_snapshot(c) if bucket == 2 else None
                db.execute(insert,
                    (c['condition_id'],c['slug'],c['seconds_left']//60,now,c['window_close']//1000,
                     c['up_probability'],c['polymarket_up'],MODEL_VERSION,
                     c.get('experimental_up_probability', c['up_probability']),
                     c.get('independent_up_probability_raw'),diagnostics))
                if (INDEPENDENT_MODEL_VERSION != MODEL_VERSION and
                        c.get('independent_up_probability') is not None):
                    db.execute(insert,
                        (c['condition_id'],c['slug'],bucket,now,c['window_close']//1000,
                         c['independent_up_probability'],c['polymarket_up'],
                         INDEPENDENT_MODEL_VERSION,c['independent_up_probability'],
                         c.get('independent_up_probability_raw'),diagnostics))
            n = c.get('next_window') or {}
            if n.get('model_status') == 'provisional' and n.get('condition_id'):
                bucket = n['seconds_until_open']//60
                diagnostics = _diagnostic_snapshot(n) if bucket == 2 else None
                db.execute(insert,
                    (n['condition_id'],n['slug'],n['seconds_until_open']//60,now,
                     n['window_close']//1000,n['up_probability'],n.get('polymarket_up'),
                     MODEL_VERSION+'-next',n.get('experimental_up_probability',n['up_probability']),
                     n.get('up_probability_raw'),diagnostics))
                if (INDEPENDENT_MODEL_VERSION != MODEL_VERSION and
                        n.get('independent_up_probability') is not None):
                    db.execute(insert,
                        (n['condition_id'],n['slug'],bucket,now,n['window_close']//1000,
                         n['independent_up_probability'],n.get('polymarket_up'),
                         INDEPENDENT_MODEL_VERSION+'-next',n['independent_up_probability'],
                         n.get('independent_up_probability_raw'),diagnostics))
        # Seed the v2 model's live score from the strictly later holdout. The
        # v1-era journal rows retain the settlement-kernel probability and the
        # full feature set the refit was trained against, so the CLOB-free 5m
        # correction can be reconstructed out of sample without reading (or
        # training on) the Polymarket price. 15m stays kernel-only.
        backfill_done = db.execute(
            'SELECT 1 FROM prediction_model_backfills WHERE version=?',
            (INDEPENDENT_MODEL_VERSION,)).fetchone()
        if not backfill_done:
            source_rows = db.execute('''SELECT cid,slug,bucket,captured,closes,market_p,
                    outcome,diagnostics_json
                FROM forecasts WHERE version=? AND bucket=2 AND captured>=?
                    AND market_p IS NOT NULL AND outcome IS NOT NULL
                    AND diagnostics_json IS NOT NULL ORDER BY captured''',
                ('vantagepulse-independent-v1', _TICK_V2_SPLIT_TS)).fetchall()
            rows_inserted = 0
            for cid,slug,bucket,captured,closes,market_p,outcome,raw in source_rows:
                try:
                    diagnostic = json.loads(raw)
                    kernel_p = float(diagnostic['settlement_model_probability'])
                    if '-5m-' in str(slug):
                        technical = diagnostic.get('technical') or {}
                        model_p, _, _, _ = _tick_v2_probability(
                            kernel_p, diagnostic.get('settlement_model_low'),
                            diagnostic.get('settlement_model_high'),
                            max(0, int(closes or 0) - int(captured or 0)),
                            _number(technical.get('vol_1m_pct'))/100.0,
                            _number(diagnostic.get('spot')),
                            _number(diagnostic.get('strike')),
                            technical,
                            (diagnostic.get('news_signal') or {}).get('score'),
                            diagnostic.get('binance_momentum_bps'),
                            diagnostic.get('coinbase_momentum_bps'),
                            diagnostic.get('binance_basis_bps'),
                            diagnostic.get('coinbase_basis_bps'))
                    elif '-15m-' in str(slug):
                        model_p = kernel_p
                    else:
                        continue
                    if not (math.isfinite(model_p) and 0.0 <= model_p <= 1.0):
                        continue
                    cursor = db.execute('''INSERT OR IGNORE INTO forecasts
                        (cid,slug,bucket,captured,closes,p,market_p,outcome,version,
                         shadow_p,static_p,diagnostics_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)''',
                        (cid,slug,bucket,captured,closes,model_p,market_p,outcome,
                         INDEPENDENT_MODEL_VERSION,model_p,model_p,raw))
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
                         shadow_p,static_p,diagnostics_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)''',
                        (cid,slug,bucket,captured,closes,model_p,market_p,outcome,
                         INDEPENDENT_MODEL_VERSION+'-next',model_p,model_p,raw))
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
    cache_key = (path, version, min_n, prior)
    now = time.monotonic()
    with _calibration_cache_lock:
        cached = _calibration_cache.get(cache_key)
        if cached and now-cached[0] < QUALITY_SUMMARY_REFRESH_SECONDS:
            return cached[1]
    try:
        with sqlite3.connect(path, timeout=2) as db:
            rows = db.execute('''SELECT CAST(p*10 AS INTEGER), COUNT(*), SUM(outcome), AVG(p)
                FROM forecasts WHERE outcome IS NOT NULL AND bucket=2 AND version=?
                GROUP BY 1''', (version,)).fetchall()
            total, wins = db.execute("""SELECT COUNT(*), COALESCE(SUM(outcome),0) FROM forecasts
                WHERE outcome IS NOT NULL AND bucket=2 AND version=?""", (version,)).fetchone()
    except Exception:
        return cached[1] if cached else {}
    deciles = {}
    for dec, n, w, avg_p in rows:
        if dec is None or not n: continue
        deciles[int(dec)] = {'n': int(n), 'wins': float(w or 0), 'avg_p': float(avg_p or 0)}
    result = {'deciles': deciles, 'total': int(total or 0),
              'total_wins': float(wins or 0), 'min_n': min_n, 'prior': prior}
    with _calibration_cache_lock:
        if len(_calibration_cache) >= 32 and cache_key not in _calibration_cache:
            oldest = min(_calibration_cache,
                         key=lambda key: _calibration_cache[key][0])
            _calibration_cache.pop(oldest, None)
        _calibration_cache[cache_key] = (time.monotonic(), result)
    return result


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


def _tick_v2_features(kernel_p, seconds_left, sigma_minute, spot, strike,
                      technical, news_score, mom_bn_bps, mom_cb_bps,
                      bas_bn_bps, bas_cb_bps):
    """17-feature vector for the v2 5m correction; must mirror the fit script.

    Feature order matches _TICK_V2_WEIGHTS: intercept, kernel logit,
    seconds-left scale, distance-to-strike in volatility units, Binance and
    Coinbase 10s momentum with availability flags, Chainlink-relative basis
    with availability flags, candle trend score, 5m/15m return z-scores,
    news pulse, and a volatility-regime tilt.
    """
    T = max(30.0, min(300.0, seconds_left or 150.0))
    sigma = max(_number(sigma_minute), .00005)
    vol_pct = sigma*100.0
    z_strike = 0.0
    if spot > 0 and strike > 0:
        z_strike = max(-6.0, min(6.0, math.log(spot/strike)/
                                 (sigma*math.sqrt(T/60.0))))
    trend = max(-1.0, min(1.0, _number((technical or {}).get('trend_score'))))
    r5 = (technical or {}).get('r5_pct')
    r15 = (technical or {}).get('r15_pct')
    z5 = math.log1p(r5/100.0)/(sigma*math.sqrt(5.0)) if r5 is not None else 0.0
    z15 = math.log1p(r15/100.0)/(sigma*math.sqrt(15.0)) if r15 is not None else 0.0
    news = _number(news_score)
    return (
        1.0,                                        # 0 intercept
        _logit(kernel_p),                           # 1 kernel logit
        T/150.0,                                    # 2 seconds-left scale
        z_strike,                                   # 3 distance-to-strike (vol units)
        1.0 if mom_bn_bps is not None else 0.0,     # 4 momentum/basis availability
        _number(mom_bn_bps)/10.0,
        1.0 if mom_cb_bps is not None else 0.0,
        _number(mom_cb_bps)/10.0,
        1.0 if bas_bn_bps is not None else 0.0,
        math.tanh(_number(bas_bn_bps)/8.0),
        1.0 if bas_cb_bps is not None else 0.0,
        math.tanh(_number(bas_cb_bps)/8.0),
        trend,                                      # 12 candle trend score
        math.tanh(z5/2.0),                          # 13 5m return z-score
        math.tanh(z15/2.0),                         # 14 15m return z-score
        math.tanh(news/2.0),                        # 15 news pulse
        math.tanh((vol_pct-0.02)/0.05),             # 16 volatility regime tilt
    )


def _tick_v2_impacts(feats, mom_bn_bps, mom_cb_bps, bas_bn_bps, bas_cb_bps):
    """Display rows for the v2 correction, grouped by signal family."""
    rows = []
    z = 0.0
    groups = (
        ('5m kernel recalibration', (0, 1, 2), True, 'logit + seconds-left scale'),
        ('Distance to strike', (3,), True, f'{feats[3]:+.2f} vol units'),
        ('Binance 10s momentum', (4, 5), mom_bn_bps is not None,
         f'{_number(mom_bn_bps):+.1f} bp' if mom_bn_bps is not None else 'stale'),
        ('Coinbase 10s momentum', (6, 7), mom_cb_bps is not None,
         f'{_number(mom_cb_bps):+.1f} bp' if mom_cb_bps is not None else 'stale'),
        ('Binance vs Chainlink basis', (8, 9), bas_bn_bps is not None,
         f'{_number(bas_bn_bps):+.1f} bp' if bas_bn_bps is not None else 'stale'),
        ('Coinbase vs Chainlink basis', (10, 11), bas_cb_bps is not None,
         f'{_number(bas_cb_bps):+.1f} bp' if bas_cb_bps is not None else 'stale'),
        ('Candle trend', (12, 13, 14), True,
         f"trend {feats[12]:+.2f} · z5 {feats[13]:+.2f} · z15 {feats[14]:+.2f}"),
        ('News pulse', (15,), True, f'sentiment {feats[15]:+.2f}'),
        ('Volatility regime', (16,), True, f'1m vol tilt {feats[16]:+.2f}'),
    )
    for name, idx, available, detail in groups:
        adjustment = sum(_TICK_V2_WEIGHTS[i]*feats[i] for i in idx)
        before = _sigmoid(z)
        z += adjustment
        after = _sigmoid(z)
        rows.append({'name': name, 'available': bool(available),
                     'logit_adjustment': round(adjustment, 6),
                     'probability_before': round(before, 6),
                     'probability_after': round(after, 6),
                     'delta_pp': round((after-before)*100, 2),
                     'detail': detail})
    return rows


def _tick_v2_probability(kernel_p, kernel_low, kernel_high, seconds_left,
                         sigma_minute, spot, strike, technical, news_score,
                         mom_bn_bps, mom_cb_bps, bas_bn_bps, bas_cb_bps,
                         impacts=False):
    """CLOB-free 5m correction (v2): a 17-feature logistic refit of the kernel.

    Re-derives the correction from the raw settlement inputs instead of
    re-scaling the kernel logit: distance-to-strike in volatility units, the
    seconds-left scale, candle trend and 5m/15m return z-scores, the news
    pulse, a volatility-regime tilt, and Binance/Coinbase 10s momentum and
    Chainlink-relative basis with availability flags. Fitted on 25,712
    resolved checkpoints captured before 2026-10-24 and validated on the
    strictly later 12,652-row holdout, where it beats both the raw kernel and
    the CLOB midpoint without reading any Polymarket price.

    The envelope re-scores at 0.75x and 1.5x volatility, mirroring the
    settlement kernel's sensitivity bounds.
    """
    feats = _tick_v2_features(kernel_p, seconds_left, sigma_minute, spot,
                              strike, technical, news_score, mom_bn_bps,
                              mom_cb_bps, bas_bn_bps, bas_cb_bps)
    eta = sum(w*x for w, x in zip(_TICK_V2_WEIGHTS, feats))
    p = _sigmoid(eta)
    low = high = p
    if kernel_low is not None and kernel_high is not None:
        # The settlement kernel's envelope is prob(0.75)/prob(1.5); mirror it
        # by re-scoring the correction with volatility scaled the same way.
        p_extreme = kernel_high if kernel_p >= .5 else kernel_low
        p_calm = kernel_low if kernel_p >= .5 else kernel_high
        variants = []
        for scale, kernel_variant in ((0.75, p_extreme), (1.5, p_calm)):
            scaled = _tick_v2_features(
                kernel_variant, seconds_left, _number(sigma_minute)*scale,
                spot, strike, technical, news_score, mom_bn_bps, mom_cb_bps,
                bas_bn_bps, bas_cb_bps)
            variants.append(_sigmoid(sum(w*x for w, x in
                                         zip(_TICK_V2_WEIGHTS, scaled))))
        low, high = min(variants), max(variants)
    rows = _tick_v2_impacts(feats, mom_bn_bps, mom_cb_bps, bas_bn_bps,
                            bas_cb_bps) if impacts else None
    return p, low, high, rows if impacts else []


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
        if story.get('context_only'):
            continue
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


def _news_for_display(stories, now_s):
    """Reserve one fresh headline slot for official Polymarket editorial context."""
    stories = stories if isinstance(stories, list) else []
    def published_at(story):
        try:
            value = float(story.get('published'))
        except (AttributeError, TypeError, ValueError, OverflowError):
            return 0.0
        return value if math.isfinite(value) else 0.0

    regular = sorted((story for story in stories if not story.get('context_only')),
                     key=published_at, reverse=True)
    editorial = [story for story in stories if story.get('context_only') and
                 0 < published_at(story) <= now_s and
                 now_s-published_at(story) <= 14*86400]
    selected = regular[:7]
    if editorial:
        selected.append(max(editorial, key=published_at))
    return sorted(selected, key=published_at, reverse=True)


def _asset_tape(query, asset):
    return _asset_tapes(query, (asset,)).get(asset, {})


def _asset_tapes(query, assets):
    """Fetch the latest spot and lead/lag ticks for all assets in one round trip."""
    assets = tuple(dict.fromkeys(str(asset) for asset in assets))
    if not assets:
        return {}
    values = ','.join(['(%s::text)'] * len(assets))
    rows = query(f"""WITH wanted(asset) AS (VALUES {values})
        SELECT x.asset,o.price AS spot,o.ts_s AS spot_ts,
        cb.price AS coinbase,cb.ts_s AS coinbase_ts,cb10.price AS coinbase_10,
        cb10.ts_s AS coinbase_10_ts,bn.price AS binance,bn.ts_s AS binance_ts,
        bn10.price AS binance_10,bn10.ts_s AS binance_10_ts
        FROM wanted x
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
          ORDER BY ts DESC LIMIT 1) bn10 ON TRUE""", assets)
    return {row['asset']: {key: value for key, value in row.items() if key != 'asset'}
            for row in rows}


def _window_reference(query, asset, horizon, opened, market):
    key = (asset, horizon, opened)
    return _window_references(query, (key,), {key: market})[key]


def _window_references(query, windows, markets):
    """Resolve all current/next market references with one indexed DB query."""
    references = {}
    wanted = []
    for asset, horizon, opened in dict.fromkeys(windows):
        market_key = (asset, horizon, opened)
        window = reference_window(markets.get(market_key, {}))
        if window < 0:
            references[market_key] = {
                'strike': 0.0, 'reference_now': 0.0,
                'reference_ts': None, 'window_s': window,
            }
            continue
        ref_key = asset + (':SPOT' if window == 0 else f':TWAP{window}')
        references[market_key] = {'window_s': window}
        wanted.append((asset, horizon, opened, ref_key, window))
    if not wanted:
        return references

    values = ','.join(
        ['(%s::text,%s::integer,%s::double precision,%s::text,%s::integer)'] * len(wanted))
    args = tuple(value for row in wanted for value in row)
    rows = query(f"""WITH wanted(asset,horizon,opened_s,ref_key,window_s) AS
        (VALUES {values})
        SELECT w.asset,w.horizon,w.opened_s,w.window_s,
          o.price AS strike,EXTRACT(EPOCH FROM o.ts) AS strike_ts,
          n.price AS reference_now,EXTRACT(EPOCH FROM n.ts) AS reference_ts
        FROM wanted w
        LEFT JOIN LATERAL (
          SELECT price,ts FROM vp.oracle_ticks
          WHERE asset=w.ref_key AND ts<=to_timestamp(w.opened_s)
            AND ts>=to_timestamp(w.opened_s)-interval '10 seconds'
          ORDER BY ts DESC LIMIT 1
        ) o ON TRUE
        LEFT JOIN LATERAL (
          SELECT price,ts FROM vp.oracle_ticks
          WHERE asset=w.ref_key ORDER BY ts DESC LIMIT 1
        ) n ON TRUE""", args)
    for row in rows:
        key = (row['asset'], int(row['horizon']), int(row['opened_s']))
        references[key] = {
            'strike': row['strike'], 'strike_ts': row['strike_ts'],
            'reference_now': row['reference_now'],
            'reference_ts': row['reference_ts'], 'window_s': int(row['window_s']),
        }
    return references


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
    jobs = [(_BOARD_IO_EXECUTOR.submit(_asset_tapes,query,ASSETS),'tapes',None)]
    jobs += [(_BOARD_IO_EXECUTOR.submit(market,*key),'market',key) for key in windows]
    jobs += [(_BOARD_IO_EXECUTOR.submit(cached,'candles:'+a,20,
                lambda a=a:recent_candles(query,public_json,a,now)),'candles',a) for a in ASSETS]
    for future,kind,key in jobs:
        try:
            value=future.result()
            if kind == 'tapes':
                tape.update(value)
            else:
                (markets if kind=='market' else candles)[key]=value
        except Exception:
            errors.append(f'{kind} unavailable: {key}')
    references_future = _BOARD_IO_EXECUTOR.submit(
        _window_references, query, windows, markets)
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
    references = references_future.result()
    requests=[];metadata={};
    for a,h,o in combos:
        m=markets.get((a,h,o),{})
        ref=references[(a,h,o)]
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
        metadata[(a,h,o)]={'market':m,'ref':ref,'tape':current_tape,'bars':bars,
                           'ask_levels':{}}
        sides=dict(zip([str(x).upper() for x in array(m.get('outcomes'))],array(m.get('clobTokenIds'))))
        for side in ('UP','DOWN'):
            book=books.get(str(sides.get(side)),{})
            metadata[(a,h,o)]['ask_levels'][side]=ask_book_levels(book)
            bid,ask=book_top(book)
            ts=_number(book.get('timestamp'))/1000.0
            book_age=(max(0.0,books_received_at_s-ts)
                      if ts>0 and books_received_at_s-ts>=-2.0 else None)
            if book_age is None or book_age < -1 or book_age > 3: bid=ask=None
            req[side.lower()+'_bid']=bid
            req[side.lower()+'_ask']=ask
            req[side.lower()+'_bid_depth']=top_depth(book,'bids',bid)
            req[side.lower()+'_ask_depth']=top_depth(book,'asks',ask)
            req[side.lower()+'_book_age_s']=book_age
            if book_age is not None and 0 <= book_age <= 3:
                req.setdefault('book_ages',[]).append(book_age)
        req['book_age_s']=max(req.pop('book_ages',[-1]))
        requests.append(req)
        next_o=o+h
        next_m=markets.get((a,h,next_o),{})
        next_ref=references[(a,h,next_o)]
        next_fee,next_exp=fee_parameters(next_m)
        next_req={'spot':spot,'strike':spot,'seconds_left':h,'twap_window_s':next_ref['window_s'],
                  'reference_age_s':now_s-spot_ts if spot_ts else 1e9,'candle_age_s':candle_age,
                  'closes':[r['close'] for r in bars],'use_drift':1,
                  'fee_rate':next_fee,'fee_exponent':next_exp}
        next_sides=dict(zip([str(x).upper() for x in array(next_m.get('outcomes'))],
                            array(next_m.get('clobTokenIds'))))
        metadata[(a,h,next_o)]={'market':next_m,'ref':next_ref,'tape':current_tape,
                                'bars':bars,'ask_levels':{}}
        for side in ('UP','DOWN'):
            book=books.get(str(next_sides.get(side)),{})
            metadata[(a,h,next_o)]['ask_levels'][side]=ask_book_levels(book)
            bid,ask=book_top(book)
            ts=_number(book.get('timestamp'))/1000.0
            book_age=(max(0.0,books_received_at_s-ts)
                      if ts>0 and books_received_at_s-ts>=-2.0 else None)
            if book_age is None or book_age < -1 or book_age > 3: bid=ask=None
            next_req[side.lower()+'_bid']=bid
            next_req[side.lower()+'_ask']=ask
            next_req[side.lower()+'_bid_depth']=top_depth(book,'bids',bid)
            next_req[side.lower()+'_ask_depth']=top_depth(book,'asks',ask)
            next_req[side.lower()+'_book_age_s']=book_age
            if book_age is not None and 0 <= book_age <= 3:
                next_req.setdefault('book_ages',[]).append(book_age)
        next_req['book_age_s']=max(next_req.pop('book_ages',[-1]))
        requests.append(next_req)
    proc=subprocess.run([os.path.join(root,'build','bin','polypulse-forecast')],
        input=json.dumps(requests,allow_nan=False),capture_output=True,text=True,timeout=3,check=True)
    forecasts=json.loads(proc.stdout)
    cards=[];technical_cache={};sparkline_by_asset={}

    def technical_for(asset, bars, sigma_minute):
        key = (asset, sigma_minute)
        if key not in technical_cache:
            technical_cache[key] = _technical_features(bars, sigma_minute)
        return technical_cache[key]

    for index,(a,h,o) in enumerate(combos):
        current_req,current_f=requests[2*index],forecasts[2*index]
        next_req,next_f=requests[2*index+1],forecasts[2*index+1]
        current_data=metadata[(a,h,o)];next_data=metadata[(a,h,o+h)]
        current_tape=current_data['tape'];bars=current_data['bars']
        if a not in sparkline_by_asset:
            sparkline_by_asset[a]=[x['close'] for x in bars[-60:]]
        technical=technical_for(a,bars,current_f['sigma_minute'])
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
            independent_raw,independent_low,independent_high,independent_impacts= \
                _tick_v2_probability(
                    current_f['probability'],current_f['low'],current_f['high'],
                    current_req['seconds_left'],current_f['sigma_minute'],
                    spot,_number(current_req['strike']),technical,news_score,
                    bn_move,cb_move,model_bn_basis,model_cb_basis,impacts=True)
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
            note=((f'The 5m estimate refits the Chainlink settlement kernel with a 17-feature '
                   f'correction (settlement distance, candle trend, fresh Binance and Coinbase '
                   f'momentum, and Chainlink-relative basis), trained on {_TICK_V2_TRAIN_N:,} '
                   f'resolved checkpoints. It excludes Polymarket prices. On the chronological '
                   f'holdout it improved Brier from {_TICK_V2_KERNEL_BRIER:.4f} (raw kernel) and '
                   f'{_TICK_V2_V1_BRIER:.4f} (prior tick model) to {_TICK_V2_HOLDOUT_BRIER:.4f}, '
                   f'ahead of the CLOB midpoint at {_TICK_V2_CLOB_BRIER:.4f}.')
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
        paired_economics=binary_pair_economics(
            up_ask,down_ask,current_req.get('up_ask_depth'),
            current_req.get('down_ask_depth'),current_req['fee_rate'],
            current_req['fee_exponent'],
            up_ask_levels=current_data['ask_levels'].get('UP'),
            down_ask_levels=current_data['ask_levels'].get('DOWN'))
        directional_economics=_directional_economics_for_request(
            independent_p,current_req,current_data['ask_levels'],ready=ready)
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
            'independent_prediction_basis':'chainlink_settlement_plus_fitted_5m_feature_correction' if h==300 else 'chainlink_settlement_kernel',
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
            'paired_outcome_economics':paired_economics,
            'directional_outcome_economics':directional_economics,
            'calibration':cal_info,
            'independent_calibration':independent_cal_info,
            'polymarket_up_bid':up_bid,'polymarket_down_bid':down_bid,
            'polymarket_up_bid_depth':current_req.get('up_bid_depth'),
            'polymarket_up_ask_depth':current_req.get('up_ask_depth'),
            'polymarket_down_bid_depth':current_req.get('down_bid_depth'),
            'polymarket_down_ask_depth':current_req.get('down_ask_depth'),
            'polymarket_up_book_age_s':current_req.get('up_book_age_s'),
            'polymarket_down_book_age_s':current_req.get('down_book_age_s'),
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
            'sparkline':sparkline_by_asset[a],'components':components,
            'independent_components':independent_components,
            'independent_signal_contributions':independent_impacts,
            'signal_contributions':impacts,'opportunity':opportunity}

        next_market=next_data['market'];next_window=next_data['ref']['window_s']
        next_technical=technical_for(a,bars,next_f['sigma_minute'])
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
        next_paired_economics=binary_pair_economics(
            nup_ask,ndown_ask,next_req.get('up_ask_depth'),
            next_req.get('down_ask_depth'),next_req['fee_rate'],
            next_req['fee_exponent'],
            up_ask_levels=next_data['ask_levels'].get('UP'),
            down_ask_levels=next_data['ask_levels'].get('DOWN'))
        next_directional_economics=_directional_economics_for_request(
            next_independent_p,next_req,next_data['ask_levels'],ready=next_f['ready'])
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
            'paired_outcome_economics':next_paired_economics,
            'directional_outcome_economics':next_directional_economics,
            'polymarket_up_bid_depth':next_req.get('up_bid_depth'),
            'polymarket_up_ask_depth':next_req.get('up_ask_depth'),
            'polymarket_down_bid_depth':next_req.get('down_bid_depth'),
            'polymarket_down_ask_depth':next_req.get('down_ask_depth'),
            'polymarket_up_book_age_s':next_req.get('up_book_age_s'),
            'polymarket_down_book_age_s':next_req.get('down_book_age_s'),
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
            'sparkline':sparkline_by_asset[a],'components':next_components,
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
        'five_minute_features':17,
        'five_minute_training_rows':_TICK_V2_TRAIN_N,
        'five_minute_trained_through_utc':'2026-10-24T00:00:00Z',
        'five_minute_holdout_rows':_TICK_V2_HOLDOUT_N,
        'five_minute_holdout_start_utc':'2026-10-24',
        'five_minute_holdout_end_utc':'journal tail (rolling)',
        'five_minute_holdout_brier':_TICK_V2_HOLDOUT_BRIER,
        'five_minute_kernel_brier':_TICK_V2_KERNEL_BRIER,
        'five_minute_v1_brier':_TICK_V2_V1_BRIER,
        'five_minute_clob_brier':_TICK_V2_CLOB_BRIER,
        'five_minute_holdout_logloss':_TICK_V2_LOGLOSS,
        'five_minute_v1_logloss':_TICK_V2_V1_LOGLOSS,
        'five_minute_clob_logloss':_TICK_V2_CLOB_LOGLOSS,
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
        'compute_ms':round((time.monotonic()-started)*1000),
        'news':_news_for_display(stories, now_s),
        'news_sources':news.get('sources',[]),
        'sources':['Chainlink spot + official settlement TWAP','Coinbase live price and lead/lag',
            'Binance live price and closed candles','Local candle store / Binance backfill',
            'Polymarket CLOB midpoint, spread and executable depth','Asset-matched crypto headlines'],
        'errors':errors,
        'disclaimer':'The displayed independent probability is CLOB-free. For 5m windows it is a fixed logistic correction to the Chainlink settlement kernel, fit on 19,045 earlier resolved forecasts through 2026-10-01 using fresh 10-second Binance/Coinbase momentum, each venue basis to Chainlink, and source-availability flags. Its later 5m holdout Brier was 0.168435 versus 0.169288 for the kernel and 0.164335 for the CLOB midpoint: a small improvement over the kernel, while the market still scored better. Current 15m and provisional next-window estimates use the Chainlink kernel only; candle trend and news are context and do not enter these independent probabilities. Taker flow is not sampled by the prediction refresh. No decile recalibration is applied. The normalized midpoint of fresh UP/DOWN books is displayed separately. No reliable market advantage or net trading edge has been established. For current windows, the kernel is P(UP)=Phi((ln(spot/strike)+mu*(T-W/2)/60)/(sigma_minute*sqrt((T-2W/3)/60))); live mu=0, while upcoming windows use a small capped recent drift and stay provisional. Sigma is an EWMA of completed 1-minute log returns. The displayed range is volatility sensitivity, not a confidence interval. Paired Brier scores compare forecasts and the CLOB at the 120–179 seconds remaining checkpoint; lower is better. Any edge estimate includes the venue fee schedule and 0.5¢ slippage reserve. Legacy lead/lag alerts remain unverified candidates and require a human to recheck the live book.'}
