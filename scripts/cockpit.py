#!/usr/bin/env python3
"""
PolySparQ++ Cockpit — browser dashboard.

A self-contained mission-control panel for the live trading bot. Serves a
single-page UI (scripts/cockpit_ui.html) plus a mostly read-only JSON API backed
by the Postgres `vp` schema and the config/log files. The only mutation is the
token-gated force-sell command, which writes a small command file that the bot
polls. No secrets are exposed.

Run:   python3 scripts/cockpit.py [--port 8888] [--host 0.0.0.0]
Then open http://<host>:<port>/  (expose the port to watch remotely).

Live exchange prices (Binance + Coinbase) stream client-side in the browser
straight from the public exchange websockets, so the server stays light.
"""
import argparse
import base64
import hashlib
import hmac
import json
import math
import os
import re
import secrets as _secrets
import smtplib
import stat
import subprocess
import sys
import tempfile
import threading
import time
import tomllib
from datetime import datetime, timezone, timedelta
from email.mime.text import MIMEText
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, urlencode, quote

import psycopg2
import psycopg2.extras
from psycopg2 import pool as pgpool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cockpit_ui.html")
CFG_FILE = os.path.join(ROOT, "config", "polypulse-cb.toml")
if not os.path.isfile(CFG_FILE):
    CFG_FILE = os.path.join(ROOT, "config", "polypulse.toml")
LOG_FILE = os.path.join(ROOT, "logs", "polypulse.log")
PID_FILE = os.path.join(ROOT, "logs", "bot.pid")
CONFIG_DIR = os.path.join(ROOT, "config")
REPORTS_DIR = os.path.join(ROOT, "session_reports")
CONTROL_DIR = os.path.join(ROOT, "control")
DSN = "dbname=vantagepulse"
BOT_SERVICE = "polypulse-bot.service"

# systemctl --user needs XDG_RUNTIME_DIR pointing at the user's runtime dir.
# When cockpit runs as a service the variable may not be inherited.
_SYSTEMD_ENV = {**os.environ, "XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}"}

# HTTP Basic Auth — password hash stored in secrets/cockpit_password.hash (sha256 hex)
def _load_cockpit_pw_hash():
    path = os.path.join(ROOT, "secrets", "cockpit_password.hash")
    try:
        with open(path) as source:
            return source.read().strip()
    except Exception:
        return None

COCKPIT_PW_HASH = _load_cockpit_pw_hash()

USERS_FILE         = os.path.join(ROOT, "secrets", "cockpit_users.json")
TOKENS_FILE        = os.path.join(ROOT, "secrets", "set_password_tokens.json")
BTCPAY_SECRET_FILE = os.path.join(ROOT, "secrets", "btcpay_webhook_secret.txt")
BTCPAY_API_KEY_FILE = os.path.join(ROOT, "secrets", "btcpay_api_key.txt")
SMTP_FILE          = os.path.join(ROOT, "secrets", "cockpit_smtp.json")
PLAN_MODELS_FILE   = os.path.join(ROOT, "secrets", "plan_models.json")
TEMPLATE_TOML      = os.path.join(ROOT, "config", "polypulse-template.toml")
DEMO_TOML          = os.path.join(ROOT, "config", "polypulse-demo.toml")
DEMO_BASELINE_TOML = os.path.join(ROOT, "config", "polypulse-demo-baseline.toml")
if not os.path.isfile(TEMPLATE_TOML):
    TEMPLATE_TOML = CFG_FILE
if not os.path.isfile(DEMO_TOML):
    DEMO_TOML = TEMPLATE_TOML
if not os.path.isfile(DEMO_BASELINE_TOML):
    DEMO_BASELINE_TOML = TEMPLATE_TOML
DEMO_INSTANCE      = "demo"
DEMO_SESSION_SECS  = 3600  # 60 minutes

def _load_commerce_config():
    """Load optional, operator-owned checkout settings from the ignored secrets directory."""
    path = os.path.join(ROOT, "secrets", "commercial.local.json")
    try:
        with open(path, encoding="utf-8") as source:
            config = json.load(source)
        return config if isinstance(config, dict) else {}
    except (OSError, ValueError):
        return {}


_COMMERCE_CONFIG = _load_commerce_config()
BTCPAY_STORE_ID = os.environ.get("POLYSPARQ_BTCPAY_STORE_ID", _COMMERCE_CONFIG.get("store_id", ""))
BTCPAY_BASE_URL = os.environ.get("POLYSPARQ_BTCPAY_BASE_URL", _COMMERCE_CONFIG.get("base_url", "")).rstrip("/")
BTCPAY_PUBLIC_URL = os.environ.get("POLYSPARQ_PUBLIC_URL", _COMMERCE_CONFIG.get("public_url", "")).rstrip("/")
BTCPAY_PLANS = _COMMERCE_CONFIG.get("plans", {})
if not isinstance(BTCPAY_PLANS, dict):
    BTCPAY_PLANS = {}


def _inject_public_site_config(page):
    public_url_json = json.dumps(BTCPAY_PUBLIC_URL).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    bootstrap = f"<script>window.POLYSPARQ_PUBLIC_URL={public_url_json};</script>"
    return page.replace("</head>", bootstrap + "</head>", 1)

_users_cache   = [0.0, None]   # [mtime, dict]
_tokens_cache  = [0.0, None]
_users_lock    = threading.Lock()
_tokens_lock   = threading.Lock()
_demo_sessions = {}            # token -> {"instance": str, "expires_mono": float}
_demo_lock     = threading.Lock()
_demo_rate     = {}            # ip -> last_signup monotonic (rate limiting)
DEMO_MAX_CONCURRENT = 5        # hard cap on simultaneous demo sessions
DEMO_RATE_COOLDOWN  = 30       # seconds between signups from the same IP

ASSETS = ["BTC", "ETH", "SOL", "XRP"]
SYMBOL = {"BTC": "BTCUSDT", "ETH": "ETHUSDT", "SOL": "SOLUSDT", "XRP": "XRPUSDT"}

# Wallet fields users can configure themselves vs. admin-only fields
WALLET_USER_FIELDS  = {"api_key", "api_secret", "api_passphrase"}
WALLET_ADMIN_FIELDS = {"api_key", "api_secret", "api_passphrase",
                       "private_key_file", "starting_balance_usd"}

# ── Cockpit strategy specs ────────────────────────────────────────
# Known trading knobs use tighter validation and useful ranges. Admins can edit
# every existing scalar/list in a config; unknown keys receive broad type-safe
# specs inferred from their current TOML values. Non-admin users remain limited
# to the explicit catalog below.
ASSET_SET = set(ASSETS)
COCKPIT_TUNABLE = {
    # ── sizing / pricing ($ amounts + risk caps; wide ranges, user-tunable) ──
    "per_click_size_usd":          ("float", 0.0, 100000.0),
    "min_order_usd":               ("float", 0.0, 100000.0),
    "min_size_usd":                ("float", 0.0, 100000.0),
    "max_size_usd":                ("float", 0.0, 100000.0),
    "max_per_window_usd":          ("float", 0.0, 100000.0),
    "max_order_usd":               ("float", 0.0, 100000.0),
    "max_deployed_usd":            ("float", 0.0, 1000000.0),
    "equity_sizing_enabled":       ("bool",),
    "equity_order_pct":            ("float", 0.0, 1.0),
    "equity_max_deployed_pct":     ("float", 0.0, 10.0),
    # ── signal thresholds ──
    "min_oracle_delta_pct":        ("float", 0.0, 5.0),
    "coinbase_lead_min_delta_pct": ("float", 0.0, 5.0),
    "autopilot_conf_floor":        ("float", 0.0, 1.0),
    # ── entry band (share price 0..1) ──
    "min_entry_share_price":       ("float", 0.0, 1.0),
    "max_entry_share_price":       ("float", 0.0, 1.0),
    "ideal_entry_share_price":     ("float", 0.0, 1.0),
    "oracle_max_token_price":      ("float", 0.0, 1.0),
    "hard_share_price_ceil":       ("float", 0.0, 1.0),
    # ── signal / mode toggles ──
    "oracle_lag_mode":             ("bool",),
    "coinbase_lead_mode":          ("bool",),
    "coinbase_veto_mode":          ("bool",),
    "predictor_mode":              ("bool",),
    "trend_filter_enabled":        ("bool",),
    "momentum_scalp_enabled":      ("bool",),
    # ── exit toggles ──
    "profit_take_mode":            ("bool",),
    "stop_loss_mode":              ("bool",),
    "smart_exit_mode":             ("bool",),
    "oracle_reversal_exit":        ("bool",),
    "oracle_panic_exit":           ("bool",),
    "momentum_flip_exit":          ("bool",),
    "endgame_mode":                ("bool",),
    # ── exit parameters ──
    "profit_take_min_pct":         ("float", 0.0, 100.0),
    "profit_take_price_thresh":    ("float", 0.0, 1.0),
    "stop_loss_pct":               ("float", 0.0, 100.0),
    "hard_stop_pct":               ("float", 0.0, 100.0),
    "lead_lag_scalp_hard_stop_ticks": ("int", 0, 1000),
    "lead_lag_scalp_impulse_only": ("bool",),
    "lead_lag_scalp_impulse_require_value_edge": ("bool",),
    "lead_lag_scalp_model_value_enabled": ("bool",),
    "prediction_page_gate_enabled": ("bool",),
    "prediction_page_min_chance_pct": ("float", 0.0, 100.0),
    "lead_lag_scalp_max_secs_remaining_15m": ("int", 0, 899),
    "lead_lag_scalp_max_secs_remaining_5m": ("int", 0, 299),
    "lead_lag_scalp_reversal_oracle_confirm_bps": ("float", 0.0, 100.0),
    "lead_lag_scalp_min_signal_confidence": ("float", 0.55, 0.92),
    "lead_lag_scalp_max_entries_per_window": ("int", 1, 10),
    "lead_lag_scalp_order_usd": ("float", 1.0, 5.0),
    "late_window_sniper_hard_stop_ticks": ("int", 0, 1000),
    "late_window_sniper_max_entries_per_window": ("int", 1, 10),
    "late_window_sniper_min_net_edge_pct": ("float", 0.0, 25.0),
    "late_window_sniper_order_usd": ("float", 1.0, 5.0),
    "timed_exit_secs":             ("int", 0, 3600),
    "ride_to_settle_secs":         ("int", 0, 3600),
    # ── entry counts / pacing ──
    "max_entries_per_window":      ("int", 0, 20),
    "max_same_side_positions":     ("int", 0, 20),
    "autopilot_max_per_sweep":     ("int", 0, 20),
    "consecutive_loss_pause":      ("int", 0, 100),
    "consecutive_pause_secs":      ("int", 0, 7200),
    "order_cooldown_ms":           ("int", 0, 120000),
    'recycler_mode': ('enum', ['off', 'sniper', 'range']),
    'recycler_entry_min_price': ('float', 0.01, 0.98),
    'recycler_entry_max_price': ('float', 0.02, 0.99),
    'recycler_target_profit_pct': ('float', 5, 50),
    'recycler_momentum_lookback_secs': ('int', 1, 10),
    'recycler_min_momentum_ticks': ('int', 1, 10),
    'recycler_entry_cutoff_secs': ('int', 360, 600),
    'recycler_entry_open_grace_secs': ('int', 0, 600),
    'recycler_first_entry_scale_pct': ('float', 1, 100),
    'recycler_late_taker_min_bid': ('float', 0, 0.99),
    'recycler_min_settlement_edge_pct': ('float', 0, 50),
    'recycler_reference_max_age_ms': ('int', 100, 30000),
    'recycler_late_risk_exit_enabled': ('bool',),
    'recycler_late_risk_min_prob': ('float', 0, 1),
    'recycler_late_risk_bid_loss_pct': ('float', 0, 100),
    'recycler_late_target_6m_pct': ('float', 0, 50),
    'recycler_late_target_4m_pct': ('float', 0, 50),
    'recycler_late_target_2m_pct': ('float', 0, 50),
    'recycler_late_maker_secs': ('int', 10, 360),
    'recycler_late_taker_secs': ('int', 10, 120),
    'recycler_late_taker_exit_enabled': ('bool',),
    'recycler_settlement_entry_enabled': ('bool',),
    'recycler_settlement_entry_secs': ('int', 1, 120),
    'recycler_settlement_entry_min_price': ('float', 0.85, 0.98),
    'recycler_settlement_entry_max_price': ('float', 0.85, 0.98),
    'recycler_settlement_only': ('bool',),
    'recycler_max_cycles': ('int', 1, 15),
    'recycler_lookback_secs': ('int', 30, 300),
    'recycler_min_range': ('float', 0.02, 0.4),
    'recycler_max_window_loss': ('float', 0.01, 100),
    'recycler_max_session_loss': ('float', 0.01, 1000),
    "entry_order_mode":            ("enum", ["maker"]),
    "maker_improve_ticks":         ("int", 0, 20),
    "maker_reprice_ms":            ("int", 100, 60000),
    "maker_poll_ms":               ("int", 100, 60000),
    "maker_max_age_ms":            ("int", 0, 300000),
    "maker_cancel_before_close_ms": ("int", 0, 120000),
    "reversal_cooldown_secs":      ("int", 0, 7200),
    # ── window timing gates (secs remaining) ──
    "min_secs_remaining_5m":       ("int", 0, 600),
    "min_secs_remaining_15m":      ("int", 0, 1800),
    "min_secs_remaining_60m":      ("int", 0, 7200),
    "max_secs_remaining_5m":       ("int", 0, 600),
    "max_secs_remaining_15m":      ("int", 0, 1800),
    "max_secs_remaining_60m":      ("int", 0, 7200),
    # ── asset filters ──
    "blocked_assets":              ("assets",),
    "scalp_v3_blocked_assets":     ("assets",),
    # ── scalp v3 exit knobs ──
    "scalp_v3_profit_take":            ("bool",),
    "scalp_v3_profit_target_pct":      ("float", 0.0, 100.0),
    "scalp_v3_profit_target":          ("float", 0.0, 1.0),
    "scalp_v3_reversal_exit":          ("bool",),
    "scalp_v3_reversal_min_profit":    ("float", 0.0, 1.0),
    "scalp_v3_trailing_stop":          ("bool",),
    "scalp_v3_trail_ratchet_pct":      ("float", 0.0, 1.0),
    "scalp_v3_trail_min_distance":     ("float", 0.0, 1.0),
    "scalp_v3_trail_start_loss":       ("float", 0.0, 1.0),
    "scalp_v3_trail_stop_pct":         ("float", 0.0, 100.0),
    "scalp_v3_min_hold_ms":            ("int", 0, 300000),
    # ── scalp v3 entry knobs ──
    "scalp_v3_enabled":                ("bool",),
    "scalp_v3_min_price":              ("float", 0.0, 1.0),
    "scalp_v3_max_price":              ("float", 0.0, 1.0),
    "scalp_v3_min_secs":               ("int", 0, 3600),
    "scalp_v3_max_secs":               ("int", 0, 7200),
    "scalp_v3_max_entries_per_window": ("int", 0, 50),
    "scalp_v3_cooldown_ms":            ("int", 0, 60000),
    "scalp_v3_per_entry_usd":          ("float", 0.0, 10000.0),
    "scalp_v3_velocity_thresh":        ("float", 0.0, 1.0),
    "scalp_v3_require_oracle_agree":   ("bool",),
    # ── WPRED window-fraction checkpoint predictor ──
    "window_predictor_fractions":      ("floats", 0.0, 1.0),
    "window_predictor_amounts_usd":    ("floats", 0.0, 100000.0),
    "window_predictor_buy_mode":       ("enum", ("usd", "shares")),
    "window_predictor_price_min":      ("float", 0.0, 1.0),
    "window_predictor_price_max":      ("float", 0.0, 1.0),
    "window_predictor_skip_late":      ("bool",),
    "window_predictor_late_grace_ms":  ("int", 0, 120000),
    "window_predictor_min_secs_left":  ("int", 0, 600),
    "window_predictor_assets":         ("assets",),
    "window_predictor_horizons":       ("ints", 60, 7200),
    "window_predictor_pm_flow":        ("bool",),
    "window_predictor_depth_topn":     ("int", 1, 50),
    "window_predictor_vote_mode":      ("bool",),
    "window_predictor_vote_min_delta_pct": ("float", 0.0, 1.0),
    "window_predictor_min_net_edge":   ("float", 0.0, 1.0),
    "window_predictor_size_max_mult":  ("float", 1.0, 10.0),
    # ── Adaptive Confidence-Timed Entry ──
    "adaptive_mode":                   ("bool",),
    "adaptive_target_conf":            ("float", 0.0, 1.0),
    "adaptive_min_secs_left":          ("int", 0, 3600),
    "adaptive_max_secs_left":          ("int", 0, 7200),
    "adaptive_trajectory_window_ms":   ("int", 0, 300000),
    "adaptive_slope_threshold":        ("float", -1.0, 1.0),
    "adaptive_conf_trend_min":         ("float", 0.0, 1.0),
    "adaptive_trajectory_samples":     ("int", 1, 100),
    "adaptive_require_predictor_agree":("bool",),
    "adaptive_oracle_agree_margin":    ("float", 0.0, 0.5),
    "adaptive_use_wpred_payload":      ("bool",),
    # ── core engine / pacing ──
    "autopilot_on_start":              ("bool",),
    "autopilot_startup_delay_secs":    ("int", 0, 3600),
    "autopilot_throttle_ms":           ("int", 0, 120000),
    "autopilot_heartbeat_secs":        ("int", 1, 3600),
    "entry_persist_gate":              ("bool",),
    "entry_persist_ms":                ("int", 0, 300000),
    "entry_early_edge_mult":           ("float", 0.0, 20.0),
    "entry_mid_edge_mult":             ("float", 0.0, 20.0),
    "entry_late_edge_mult":            ("float", 0.0, 20.0),
    "max_reversals_per_window":        ("int", 0, 20),
    # ── market universe ──
    "universe_horizons_seconds":       ("ints", 60, 86400),
    "universe_max_windows_per_series": ("int", 1, 100),
    "universe_max_markets":            ("int", 1, 1000),
    "universe_prewarm_ahead_minutes":  ("int", 0, 1440),
    # ── signal confirmation / trend ──
    "binance_book_confirm":            ("bool",),
    "binance_book_imb_min":            ("float", 0.0, 1.0),
    "multi_source_vote":               ("bool",),
    "multi_source_quorum":             ("int", 1, 10),
    "oracle_require_pred_agree":       ("bool",),
    "pred_agree_margin":               ("float", 0.0, 0.5),
    "pred_agree_min_conf":             ("float", 0.0, 1.0),
    "conviction_min_delta_pct":        ("float", 0.0, 5.0),
    "trend_block_pct":                 ("float", 0.0, 5.0),
    "trend_lookback_secs":             ("int", 1, 7200),
    "max_gap_btc_usd":                 ("float", 0.0, 1000000.0),
    "max_gap_eth_usd":                 ("float", 0.0, 1000000.0),
    # ── order construction / book health ──
    "manual_buy_slippage_pct":         ("float", 0.0, 1.0),
    "manual_buy_slippage_extra":       ("float", 0.0, 1.0),
    "max_entry_spread_pct":            ("float", 0.0, 100.0),
    "max_book_age_ms":                 ("int", 0, 600000),
    "token_quarantine_payload_ms":     ("int", 0, 3600000),
    "token_quarantine_no_match_ms":    ("int", 0, 3600000),
    "min_position_shares":             ("float", 0.0, 1000000.0),
    "dust_position_shares":            ("float", 0.0, 1000000.0),
    # ── predictor engine internals ──
    "min_predictor_edge":              ("float", 0.0, 1.0),
    "min_predictor_conf":              ("float", 0.0, 1.0),
    "predictor_max_token_price":       ("float", 0.0, 1.0),
    "predictor_no_oracle_conf":        ("float", 0.0, 1.0),
    "predictor_refresh_ms":            ("int", 10, 120000),
    "predict_budget_per_sweep":        ("int", 0, 1000),
    "predictor_recent_1m_bars":        ("int", 1, 5000000),
    "predictor_recent_1m_ohlcv_bars":  ("int", 1, 5000000),
    "predictor_long_60m_bars":         ("int", 1, 5000000),
    "predictor_5m_closes":             ("int", 1, 5000000),
    "predictor_15m_closes":            ("int", 1, 5000000),
    "predictor_5m_bars":               ("int", 1, 5000000),
    "predictor_15m_bars":              ("int", 1, 5000000),
    "predictor_60m_bars":              ("int", 1, 5000000),
    # ── digital fair-value entry ──
    "digital_fv_mode":                 ("bool",),
    "digital_fv_use_coinbase":         ("bool",),
    "digital_fv_min_edge_pct":         ("float", 0.0, 100.0),
    "digital_fv_fee_buffer":           ("float", 0.0, 100.0),
    "digital_fv_down_slip_pct":        ("float", 0.0, 100.0),
    "digital_fv_vol_floor":            ("float", 0.0, 1.0),
    "digital_fv_vol_ceil":             ("float", 0.0, 1.0),
    "digital_fv_jump_z":               ("float", 0.0, 1000.0),
    # ── exits / reversals / settlement ──
    "profit_take_momentum_max":        ("float", 0.0, 5.0),
    "stop_loss_price":                 ("float", 0.0, 1.0),
    "stop_loss_reverse":               ("bool",),
    "smart_stop_max_votes":            ("int", 0, 20),
    "oracle_reversal_min_hold_secs":   ("int", 0, 7200),
    "oracle_panic_delta_pct":          ("float", 0.0, 5.0),
    "oracle_panic_min_hold_secs":      ("int", 0, 7200),
    "momentum_flip_margin":            ("float", 0.0, 1.0),
    "momentum_flip_min_conf":          ("float", 0.0, 1.0),
    "momentum_flip_reverse":           ("bool",),
    "momentum_flip_min_secs":          ("int", 0, 7200),
    "momentum_flip_min_hold_ms":       ("int", 0, 300000),
    "stop_loss_cooldown_secs":         ("int", 0, 7200),
    "reverse_min_secs_left":           ("int", 0, 7200),
    "reverse_slippage_pct":            ("float", 0.0, 1.0),
    "reverse_slippage_extra":          ("float", 0.0, 1.0),
    "stop_loss_min_hold_secs":         ("int", 0, 7200),
    "timed_exit_min_bid":              ("float", 0.0, 1.0),
    "timed_exit_throttle_ms":          ("int", 0, 120000),
    "settlement_grace_ms":             ("int", 0, 300000),
    "endgame_secs_left":               ("int", 0, 3600),
    "endgame_min_profit_pct":          ("float", 0.0, 1000.0),
    "endgame_min_price":               ("float", 0.0, 1.0),
    "endgame_max_price":               ("float", 0.0, 1.0),
    "endgame_min_oracle_pct":          ("float", 0.0, 5.0),
    # ── scalp v2 / v3 advanced controls ──
    "scalp_v2_enabled":                ("bool",),
    "scalp_v2_reversal_exit":          ("bool",),
    "scalp_v2_trailing_stop":          ("bool",),
    "scalp_v2_profit_take":            ("bool",),
    "scalp_v2_min_hold_ms":            ("int", 0, 300000),
    "scalp_v3_accel_min":              ("float", -1.0, 1.0),
    "scalp_v3_dominance_thresh":       ("float", 0.0, 1.0),
    "scalp_v3_min_confidence":         ("float", 0.0, 1.0),
    "scalp_v3_max_spread_pct":         ("float", 0.0, 100.0),
    "scalp_v3_max_book_age_ms":        ("int", 0, 600000),
    "scalp_v3_reversal_thresh":        ("float", -1.0, 1.0),
    "scalp_v3_reversal_window_us":     ("int", 0, 60000000),
    "scalp_v3_runner_enabled":         ("bool",),
    "scalp_v3_runner_min_bid":         ("float", 0.0, 1.0),
    "scalp_v3_runner_min_profit":      ("float", 0.0, 1.0),
    "scalp_v3_runner_max_secs_left":   ("int", 0, 7200),
    "scalp_v3_runner_velocity_floor":  ("float", -1.0, 1.0),
    # ── fee model / diagnostics / safety ──
    "taker_fee_rate":                  ("float", 0.0, 1.0),
    "fee_exponent":                    ("int", 0, 10),
    "price_history_max_points":        ("int", 1, 100000),
    "price_velocity_lookback_ms":      ("int", 1, 3600000),
    "sparkline_points":                ("int", 2, 100000),
    "tick_buffer_max_age_us":          ("int", 1, 3600000000),
    "tick_buffer_max_entries":         ("int", 1, 1000000),
    "clob_min_shares_fallback":        ("float", 0.0, 1000000.0),
    "wallet_balance_warn_above":       ("float", 0.0, 1000000000.0),
    "daily_loss_kill_pct":             ("float", 0.0, 100.0),
    "daily_kill_check_interval_ms":    ("int", 100, 3600000),
    "daily_kill_grace_secs":           ("int", 0, 3600),
    # ── WPRED controls present in specialized configs ──
    "window_predictor_mode":                    ("bool",),
    "window_predictor_pairs":                   ("pairs",),
    "window_predictor_source":                  ("string", 1, 64),
    "window_predictor_require_unanimous":       ("bool",),
    "window_predictor_min_abs_oracle_delta_pct":("float", 0.0, 5.0),
    "window_predictor_min_win_return_pct":      ("float", 0.0, 1000.0),
}

SERVER_WRITABLE_CONFIG = {
    "mode": ("runtime", "mode"),
    "api_key": ("wallet", "string"),
    "api_secret": ("wallet", "string"),
    "api_passphrase": ("wallet", "string"),
    "private_key_file": ("wallet", "path"),
    "starting_balance_usd": ("wallet", "float"),
}


def _validate_config_spec(spec, val):
    """Validate and coerce a value against a cockpit config specification."""
    kind = spec[0]
    try:
        if kind == "bool":
            if isinstance(val, bool):
                return (True, val, None)
            s = str(val).strip().lower()
            if s in ("true", "1", "yes", "on"):
                return (True, True, None)
            if s in ("false", "0", "no", "off"):
                return (True, False, None)
            return (False, None, "expected true/false")
        if kind in ("int", "float"):
            num = float(val)
            if not math.isfinite(num) or (kind == "int" and not num.is_integer()):
                return (False, None, "expected a finite integer" if kind == "int" else "expected a finite number")
            lo, hi = spec[1], spec[2]
            if num < lo or num > hi:
                return (False, None, f"out of range [{lo}, {hi}]")
            return (True, int(num) if kind == "int" else num, None)
        if kind == "assets":
            if not isinstance(val, list):
                return (False, None, "expected list of assets")
            bad = [a for a in val if a not in ASSET_SET]
            if bad:
                return (False, None, f"unknown asset(s): {bad}")
            return (True, val, None)
        if kind == "pairs":
            if not isinstance(val, list):
                return (False, None, "expected list such as [\"BTC:300\"]")
            out = []
            for item in val:
                pair = str(item).strip().upper()
                match = re.fullmatch(r"(BTC|ETH|SOL|XRP):(\d+)", pair)
                if not match or not 60 <= int(match.group(2)) <= 86400:
                    return (False, None, f"invalid asset:horizon pair: {item}")
                out.append(pair)
            return (True, out, None)
        if kind == "string":
            s = str(val).strip()
            lo, hi = spec[1], spec[2]
            if not lo <= len(s) <= hi or "\x00" in s or "\n" in s or "\r" in s:
                return (False, None, f"expected {lo}..{hi} characters")
            return (True, s, None)
        if kind == "secret":
            # Secret values are accepted for replacement but are never returned
            # by api_config_read. An empty value intentionally clears the field.
            s = str(val).strip()
            lo, hi = spec[1], spec[2]
            if not lo <= len(s) <= hi or "\x00" in s or "\n" in s or "\r" in s:
                return (False, None, f"expected {lo}..{hi} characters")
            return (True, s, None)
        if kind == "strings":
            if not isinstance(val, list):
                return (False, None, "expected a JSON list of strings")
            lo, hi = spec[1], spec[2]
            out = []
            for item in val:
                s = str(item).strip()
                if not lo <= len(s) <= hi or "\x00" in s or "\n" in s or "\r" in s:
                    return (False, None, f"each item must be {lo}..{hi} characters")
                out.append(s)
            return (True, out, None)
        if kind in ("floats", "ints"):
            if not isinstance(val, list) or not val:
                return (False, None, "expected non-empty list of numbers")
            lo, hi = spec[1], spec[2]
            out = []
            for v in val:
                num = float(v)
                if num < lo or num > hi:
                    return (False, None, f"out of range [{lo}, {hi}]")
                out.append(int(round(num)) if kind == "ints" else num)
            return (True, out, None)
        if kind == "enum":
            choices = spec[1]
            s = str(val).strip().lower()
            if s not in choices:
                return (False, None, f"expected one of: {', '.join(choices)}")
            return (True, s, None)
    except (TypeError, ValueError):
        return (False, None, "invalid value")
    return (False, None, "unsupported spec")


def validate_tunable(key, val):
    """Validate one normal strategy-page key from the explicit safe catalog."""
    spec = COCKPIT_TUNABLE.get(key)
    if not spec:
        return (False, None, "locked: server-side only")
    return _validate_config_spec(spec, val)


CONFIG_SECRET_KEYS = {
    "api_key", "api_secret", "api_passphrase", "password", "private_key",
    # These commonly embed credentials even though their names do not say so.
    "conninfo", "rpc_url",
}


def _config_spec_for_value(key, value):
    """Infer a broad, type-safe editor spec for an admin-visible TOML value."""
    if key.lower() in CONFIG_SECRET_KEYS:
        return ("secret", 0, 4096)
    if key in COCKPIT_TUNABLE:
        return COCKPIT_TUNABLE[key]
    if key == "mode":
        return ("enum", ("paper", "live"))
    if isinstance(value, bool):
        return ("bool",)
    if isinstance(value, int):
        return ("int", -1_000_000_000_000, 1_000_000_000_000)
    if isinstance(value, float):
        return ("float", -1_000_000_000_000.0, 1_000_000_000_000.0)
    if isinstance(value, list):
        if all(isinstance(v, int) and not isinstance(v, bool) for v in value):
            return ("ints", -1_000_000_000_000, 1_000_000_000_000)
        if all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in value):
            return ("floats", -1_000_000_000_000.0, 1_000_000_000_000.0)
        return ("strings", 0, 4096)
    return ("string", 0, 4096)


def _config_value_at(data, qualified_key):
    node = data
    parts = qualified_key.split(".")
    for part in parts:
        if not isinstance(node, dict) or part not in node:
            return (False, None)
        node = node[part]
    return (True, node)


def _redact_config_tree(data):
    """Blank credential values while retaining their write-only editor fields."""
    if not isinstance(data, dict):
        return data
    out = {}
    for key, value in data.items():
        if key.lower() in CONFIG_SECRET_KEYS:
            out[key] = ""
        else:
            out[key] = _redact_config_tree(value) if isinstance(value, dict) else value
    return out


def _config_editor_catalog(data, full=False):
    """Return qualified editable keys/specs for the supplied redacted config."""
    keys, specs = [], {}

    def walk(node, path=""):
        for key, value in node.items():
            qkey = f"{path}.{key}" if path else key
            if isinstance(value, dict):
                walk(value, qkey)
                continue
            # Daily loss liquidation was explicitly disabled fleet-wide by the
            # operator; keep legacy TOML compatibility without offering a live UI knob.
            if key in {"daily_loss_kill_pct", "daily_kill_check_interval_ms", "daily_kill_grace_secs"}:
                continue
            if full or key in COCKPIT_TUNABLE:
                keys.append(qkey)
                specs[qkey] = list(_config_spec_for_value(key, value))

    walk(data)
    return sorted(keys), specs


def validate_server_config_value(key, val):
    """Validate server-only config writes used by wallet/go-live endpoints.
    These keys are never accepted through the public strategy editor path."""
    spec = SERVER_WRITABLE_CONFIG.get(key)
    if not spec:
        return (False, None, None, "locked: server-side only")
    section, kind = spec
    try:
        if kind == "mode":
            mode = str(val).strip().lower()
            if mode not in ("paper", "live"):
                return (False, None, section, "mode must be paper or live")
            return (True, mode, section, None)
        if kind == "float":
            num = float(val)
            if num < 0 or num > 1000000:
                return (False, None, section, "starting balance out of range")
            return (True, num, section, None)
        if kind in ("string", "path"):
            s = str(val).strip()
            if not s:
                return (False, None, section, "empty value")
            if "\x00" in s or len(s) > 4096:
                return (False, None, section, "invalid string")
            return (True, s, section, None)
    except (TypeError, ValueError):
        return (False, None, section, "invalid value")
    return (False, None, section, "unsupported server spec")


def toml_scalar(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, list):
        return json.dumps(value)
    if isinstance(value, str):
        return json.dumps(value)
    if isinstance(value, float):
        return repr(value)
    return str(value)


def invalidate_runtime_caches():
    _since_cache.clear()
    for name in ("_live_cache", "_live_lite_cache", "_activity_cache"):
        cache = globals().get(name)
        if isinstance(cache, dict):
            cache.clear()
    for name in ("_fleet_cache", "_pnl_cache", "_stream_cache", "_bot_status_cache"):
        val = globals().get(name)
        if isinstance(val, list) and val:
            val[0] = 0.0


# ── Plan tiers: per-plan bot quota + allowed AI models (secrets/plan_models.json)
_plan_models_cache = [0.0, None]
_plan_models_lock = threading.Lock()
_PLAN_MODELS_DEFAULT = {
    "bot_base": {"cb": "cb", "maniac": "scalp", "conv": "conv", "demo": "demo-baseline"},
    "plans": {
        "standard": {"label": "Standard", "max_bots": 1, "bots": ["cb"],
                     "default_model": "openrouter/free", "models": ["openrouter/free"]},
        "pro": {"label": "Pro", "max_bots": 3, "bots": ["cb", "maniac", "conv"],
                "default_model": "openrouter/free", "models": ["openrouter/free"]},
        "demo": {"label": "Demo", "max_bots": 1, "bots": ["demo"],
                 "default_model": "openrouter/free", "models": ["openrouter/free"]},
        "admin": {"label": "Admin", "max_bots": None, "bots": "*",
                  "default_model": "openrouter/free", "models": "*"},
    },
}


def _load_plan_models():
    """plan_models.json (mtime-cached). Falls back to a safe built-in default."""
    try:
        mtime = os.stat(PLAN_MODELS_FILE).st_mtime
    except OSError:
        return _PLAN_MODELS_DEFAULT
    with _plan_models_lock:
        if _plan_models_cache[1] is not None and _plan_models_cache[0] == mtime:
            return _plan_models_cache[1]
    try:
        with open(PLAN_MODELS_FILE) as f:
            data = json.load(f)
    except Exception:
        return _PLAN_MODELS_DEFAULT
    with _plan_models_lock:
        _plan_models_cache[0], _plan_models_cache[1] = mtime, data
    return data


def plan_def(plan):
    """Plan definition dict for `plan`, falling back to 'standard'."""
    plans = _load_plan_models().get("plans", {})
    return plans.get(plan) or plans.get("standard") or {}


def plan_bot_base():
    return _load_plan_models().get("bot_base", {})


def plan_allowed_models(plan):
    """Model IDs allowed on `plan`. '*' → union of every plan's models plus any
    model already configured in supervisor-models.env (admin sees everything)."""
    models = plan_def(plan).get("models")
    if models == "*":
        out = []
        for p in _load_plan_models().get("plans", {}).values():
            if isinstance(p.get("models"), list):
                out.extend(p["models"])
        for v in _models_env().values():
            if v.get("model"):
                out.append(v["model"])
        return sorted(set(out))
    return list(models or [])

DB = None  # connection pool, lazily created


def get_pool():
    global DB
    if DB is None:
        DB = pgpool.ThreadedConnectionPool(1, 32, dsn=DSN + " options='-c statement_timeout=8000'")
    return DB


def q(sql, args=None, one=False):
    """Run a read-only SELECT and return list[dict] (or a single dict)."""
    p = get_pool()
    conn = p.getconn()
    broken = False
    try:
        conn.set_session(readonly=True, autocommit=True)
        with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, args or ())
            rows = cur.fetchall()
    except Exception:
        # broken/aborted connection — drop it from the pool instead of
        # handing the next caller a dead session
        broken = True
        raise
    finally:
        try:
            p.putconn(conn, close=broken)
        except Exception:
            pass
    rows = [_clean(dict(r)) for r in rows]
    if one:
        return rows[0] if rows else None
    return rows


def normalize_since(since):
    """Coerce a session-start cutoff into a value safe for `%s::timestamptz`.
    Guards /api/live against a 500 (which freezes the whole dashboard) if any
    upstream path hands us epoch-ms — e.g. a datetime that went through _clean.
    Accepts: None | datetime | ISO/wall-clock str | epoch-seconds | epoch-ms."""
    if since is None:
        return None
    if isinstance(since, datetime):
        return since
    if isinstance(since, (int, float)):
        secs = since / 1000.0 if since > 1e11 else float(since)
        try:
            return datetime.fromtimestamp(secs).isoformat()
        except (OverflowError, OSError, ValueError):
            return None
    return since  # already a string Postgres can cast


def _clean(d):
    """JSON-safe: datetimes -> epoch ms, Decimal -> float."""
    out = {}
    for k, v in d.items():
        if isinstance(v, datetime):
            out[k] = int(v.timestamp() * 1000)
        elif hasattr(v, "__float__") and not isinstance(v, (int, float, bool)):
            out[k] = float(v)
        else:
            out[k] = v
    return out


# ── config / runtime detection ─────────────────────────────────────────────
def _instance_config_file(instance=None):
    if instance:
        return os.path.join(CONFIG_DIR, f"polypulse-{instance}.toml")
    return CFG_FILE


def load_config(instance=None):
    path = _instance_config_file(instance)
    if instance and not os.path.exists(path):
        path = CFG_FILE
    try:
        with open(path, "rb") as f:
            t = tomllib.load(f)
    except Exception as e:
        return {"error": str(e)}
    # Whitelist non-secret sections only (NEVER expose [wallet]/[postgres]).
    return {
        "mode": t.get("runtime", {}).get("mode", "?"),
        "strategy": t.get("strategy", {}).get("hot", {}),
        "risk": t.get("risk", {}),
    }


def _config_stake(path):
    try:
        with open(path, "rb") as f:
            t = tomllib.load(f)
        return float(t.get("wallet", {}).get("starting_balance_usd", 100.0))
    except Exception:
        return 100.0


def starting_balance_usd(instance=None):
    """Base stake for paper equity.
    Per-instance view: return that instance's config stake.
    ALL view: sum stakes of all running instances."""
    if instance:
        return _config_stake(os.path.join(ROOT, "config", f"polypulse-{instance}.toml"))
    # Tournament services are discovered dynamically.  Keeping this tied to
    # the old hard-coded INSTANCES list silently omitted predictor/endgame from
    # the ALL-account equity base.
    running = list(bot_status_detail().get("instances_running") or [])
    if not running:
        running = [i for i in fleet_instances() if instance_running(i)]
    if running:
        return sum(_config_stake(os.path.join(ROOT, "config", f"polypulse-{i}.toml"))
                   for i in running if current_mode(i).lower()==current_mode().lower())
    return _config_stake(CFG_FILE)


def _swap_origin_path(inst):
    return os.path.join(CONTROL_DIR, f".config_origin_{inst}")


def _read_swap_origin(inst):
    """Return the config source name if this instance had its config swapped, else None."""
    try:
        with open(_swap_origin_path(inst)) as f:
            val = f.read().strip()
            return val if val else None
    except OSError:
        return None


def _write_swap_origin(inst, config_name):
    os.makedirs(CONTROL_DIR, exist_ok=True)
    with open(_swap_origin_path(inst), "w") as f:
        f.write(config_name)


def parse_systemd_wall_ts(value):
    if not value:
        return None
    m = re.search(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+([A-Z]+)", value)
    if not m:
        return value
    offsets = {
        "UTC": "+00",
        "GMT": "+00",
        "EST": "-05",
        "EDT": "-04",
        "CST": "-06",
        "CDT": "-05",
        "MST": "-07",
        "MDT": "-06",
        "PST": "-08",
        "PDT": "-07",
    }
    return f"{m.group(1)}{offsets.get(m.group(2), '')}"


def _systemd_bot_status(service=None):
    try:
        r = subprocess.run(
            ["systemctl", "--user", "show", service or BOT_SERVICE,
             "--property=ActiveState", "--property=SubState",
             "--property=MainPID", "--property=ExecMainStatus",
             "--property=ActiveEnterTimestamp"],
            capture_output=True, text=True, timeout=1.0, env=_SYSTEMD_ENV)
    except Exception:
        return None
    if r.returncode != 0 or not r.stdout.strip():
        return None
    data = {}
    for line in r.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            data[k] = v
    pid = int(data.get("MainPID") or 0)
    active = data.get("ActiveState") or "unknown"
    sub = data.get("SubState") or "unknown"
    return {
        "running": active == "active" and pid > 0,
        "pid": pid or None,
        "launcher": "systemd",
        "service": service or BOT_SERVICE,
        "active_state": active,
        "sub_state": sub,
        "exit_status": int(data.get("ExecMainStatus") or 0),
        "session_start": parse_systemd_wall_ts(data.get("ActiveEnterTimestamp")),
    }


def _pidfile_bot_status():
    try:
        with open(PID_FILE) as f:
            pid = int(f.read().strip())
        os.kill(pid, 0)
        return {"running": True, "pid": pid, "launcher": "pidfile"}
    except Exception:
        return None


def _pgrep_bot_status():
    try:
        r = subprocess.run(
            ["pgrep", "-f", r"[b]uild/bin/polypulse.*polypulse\.toml"],
            capture_output=True, text=True, timeout=1.0)
    except Exception:
        return None
    pids = [int(x) for x in r.stdout.split() if x.strip().isdigit()]
    if not pids:
        return None
    return {"running": True, "pid": pids[0], "launcher": "pgrep"}


_bot_status_cache = [0.0, None]  # [built_monotonic, payload]


def bot_status_detail():
    # 5s TTL: this fans out to several systemctl subprocesses and is hit by
    # status/health/orderstats on every dashboard poll.
    now = time.monotonic()
    if _bot_status_cache[1] is not None and now - _bot_status_cache[0] < 5.0:
        return _bot_status_cache[1]
    s = _bot_status_detail_uncached()
    _bot_status_cache[0], _bot_status_cache[1] = now, s
    return s


def _bot_status_detail_uncached():
    for probe in (_systemd_bot_status, _pidfile_bot_status, _pgrep_bot_status):
        s = probe()
        if s and s.get("running"):
            return s
    # Tournament mode: discover any running polypulse-bot@*.service dynamically.
    up = []
    try:
        r = subprocess.run(
            ["systemctl", "--user", "list-units", "polypulse-bot@*.service",
             "--state=active", "--no-legend", "--no-pager"],
            capture_output=True, text=True, timeout=3.0, env=_SYSTEMD_ENV)
        for line in r.stdout.splitlines():
            unit = line.strip().split()[0] if line.strip() else ""
            if "@" in unit and unit.endswith(".service"):
                inst = unit.split("@")[1].replace(".service", "")
                s = _systemd_bot_status(f"polypulse-bot@{inst}.service")
                if s and s.get("running"):
                    s["instance"] = inst
                    up.append(s)
    except Exception:
        pass
    if up:
        lead = dict(up[0])
        lead["launcher"] = f"tournament x{len(up)}"
        lead["service"] = ", ".join(f"bot@{s['instance']}" for s in up)
        lead["instances_running"] = [s["instance"] for s in up]
        return lead
    return _systemd_bot_status() or {"running": False, "pid": None, "launcher": "unknown"}


def bot_running():
    s = bot_status_detail()
    return bool(s.get("running")), s.get("pid")


def tail(path, n=400):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            buf = b""
            block = 8192
            while size > 0 and buf.count(b"\n") <= n:
                step = min(block, size)
                size -= step
                f.seek(size)
                buf = f.read(step) + buf
            return buf.decode("utf-8", "replace").splitlines()[-n:]
    except Exception:
        return []


def latest_log_value(lines, needle, after="= $"):
    for ln in reversed(lines):
        if needle in ln and after in ln:
            try:
                return float(ln.split(after, 1)[1].split()[0].replace("$", ""))
            except Exception:
                pass
    return None


def current_mode(instance=None):
    return load_config(instance).get("mode", "paper")


def empty_positions(source="fresh_paper_session"):
    return {
        "positions": [],
        "ts": int(time.time() * 1000),
        "total_unrealized": 0.0,
        "deployed_usd": 0.0,
        "market_value": 0.0,
        "source": source,
    }


def session_start_for_mode(mode=None, bot=None):
    """Paper accounting is per bot process. Live mode keeps normal history."""
    mode = (mode or current_mode()).lower()
    if mode != "paper":
        return None
    bot = bot or bot_status_detail()
    if not bot.get("running"):
        return None
    # normalize at the source: every caller feeds this straight into a
    # %s::timestamptz cast, so it must never be epoch-ms.
    return normalize_since(bot.get("session_start"))


def accounting_since(instance=None, mode=None, bot=None):
    """Paper accounts reset with the process; live holdings survive restarts."""
    mode = (mode or current_mode(instance)).lower()
    if mode == 'live':
        return None  # owned positions survive a process restart
    if instance:
        return normalize_since(instance_session_start(instance))
    if mode == "paper":
        return normalize_since(session_start_for_mode(mode, bot))
    return None


# ── Synthetic live mark (fallback when the Polymarket book feed is empty) ────
# When book_snapshots_enabled is off (or the feed is down), vp.pm_book_snapshots
# has no rows, so the real share-price mark is unavailable. Rather than freeze on
# the last fill, we estimate a *live-moving* implied price from the oracle: the
# UP token is worth P(asset_close > strike), modelled as a driftless lognormal
# walk from the current oracle price. strike = oracle at window_open.
_sigma_cache = [0.0, {}]


def _asset_sigma():
    """Per-asset stdev of 1-tick oracle log-returns over the last 5 min (cached 5s).
    Used as the volatility input for the synthetic mark."""
    now = time.monotonic()
    if _sigma_cache[1] and now - _sigma_cache[0] < 5.0:
        return _sigma_cache[1]
    try:
        rows = q("""
            SELECT asset, stddev_samp(r) AS sigma FROM (
              SELECT asset,
                     price / NULLIF(lag(price) OVER (PARTITION BY asset ORDER BY ts), 0) - 1 AS r
              FROM vp.oracle_ticks WHERE ts > now() - interval '5 minutes'
            ) s WHERE r IS NOT NULL GROUP BY asset""")
    except Exception:
        return _sigma_cache[1] or {}
    out = {r["asset"]: float(r["sigma"]) for r in rows if r.get("sigma")}
    _sigma_cache[0], _sigma_cache[1] = now, out
    return out


def synthetic_mark(side, oracle_now, strike, secs_left, sigma):
    """Implied UP/DOWN share price in [0.02, 0.98], or None if inputs are missing.
    P(close > strike) = Phi( ln(now/strike) / (sigma * sqrt(secs_left)) )."""
    if oracle_now is None or strike is None:
        return None
    try:
        now_p, k = float(oracle_now), float(strike)
    except (TypeError, ValueError):
        return None
    if now_p <= 0 or k <= 0:
        return None
    t = max(1.0, float(secs_left or 0))
    # Volatility floor: the realtime per-tick sigma (~1e-5 for BTC) is far too
    # small for a multi-minute horizon — left raw it makes z explode and pegs the
    # mark to the 0.02/0.98 clamp, which on a $0.02 scalp entry reads as +4800%.
    # Floor at a per-second sigma that implies a realistic ~0.15% move over 300s.
    s = max(float(sigma or 0.0), 1.0e-4)
    denom = s * math.sqrt(t)
    if denom <= 1e-9:
        up = 0.85 if now_p > k else 0.15 if now_p < k else 0.5
    else:
        z = math.log(now_p / k) / denom
        up = 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))
    p = up if str(side).upper() == "UP" else 1.0 - up
    return max(0.02, min(0.98, p))


_ledger_cache = {}
_ledger_lock = threading.Lock()


def scoped_fill_ledger(mode, since=None, instance=None):
    from accounting import replay_fills
    if not instance:
        names = bot_status_detail().get('instances_running') or []
        ledgers = [scoped_fill_ledger(mode,accounting_since(i,mode),i)
                   for i in names if current_mode(i).lower()==mode.lower()]
        return {**{k:sum((r[k] for r in ledgers),[]) for k in ('positions','exits','errors')},
                **{k:sum(r[k] for r in ledgers) for k in ('buy_value','sell_value','fees')}}
    key = (mode, since, instance)
    with _ledger_lock:
        cached = _ledger_cache.get(key)
        if cached and time.monotonic()-cached[0] < 0.5:
            return cached[1]
    filters, args = ["o.mode=%s"], [mode]
    if since:
        filters.append("o.ts_created >= %s::timestamptz")
        args.append(since)
    if instance:
        filters.append("o.instance=%s")
        args.append(instance)
    fills = q(f"""SELECT f.id,f.ts,f.price,f.size_shares,f.fee_usd,
                o.client_order_id,o.mode,o.instance,o.session_id,o.condition_id,
                o.token_id,o.side_label,o.direction,o.source,o.ts_created
            FROM vp.orders o JOIN vp.fills f USING(client_order_id)
            WHERE {' AND '.join(filters)} ORDER BY f.ts,f.id""", tuple(args))
    result = replay_fills(fills)
    with _ledger_lock:
        if len(_ledger_cache) > 128:
            _ledger_cache.clear()
        _ledger_cache[key] = (time.monotonic(),result)
    return result


def reconstructed_positions(mode=None, since=None, instance=None):
    mode = mode or current_mode(instance)
    if mode.lower() == 'paper' and since is None:
        since = accounting_since(instance, mode)
    if mode.lower() == 'paper' and not since:
        return empty_positions()
    since = normalize_since(since)
    ledger = scoped_fill_ledger(mode, since, instance)
    held = ledger['positions']
    metadata = {}
    outcome_metadata = {}
    if held:
        rows = q("""SELECT m.condition_id AS cid,m.asset,m.horizon_seconds,m.target_price,
                m.window_open,m.window_close,m.question,b.token_id,b.best_bid,b.best_ask,b.ts AS mark_ts
            FROM vp.pm_markets m LEFT JOIN LATERAL (
                SELECT DISTINCT ON (token_id) token_id,best_bid,best_ask,ts
                FROM vp.pm_book_snapshots WHERE condition_id=m.condition_id
                  AND ts>now()-interval '15 seconds' ORDER BY token_id,ts DESC
            ) b ON true WHERE m.condition_id=ANY(%s)""",
            (list({p['condition_id'] for p in held}),))
        for r in rows:
            metadata[(r['cid'],r.get('token_id'))] = r
            metadata[(r['cid'],None)] = r
        outcome_rows = q("""SELECT DISTINCT ON (o.condition_id,o.token_id)
                o.condition_id AS cid,o.token_id,
                NULLIF(to_jsonb(o)->>'outcome_label','') AS outcome_label,
                NULLIF(to_jsonb(o)->>'manual_market_question','') AS manual_question
            FROM vp.orders o
            WHERE o.condition_id=ANY(%s) AND o.token_id=ANY(%s)
            ORDER BY o.condition_id,o.token_id,o.ts_created DESC""",
            (list({p['condition_id'] for p in held}),
             list({p['token_id'] for p in held})))
        outcome_metadata = {(r['cid'], r['token_id']): r for r in outcome_rows}
    positions = []
    now_ms = int(time.time()*1000)
    from cockpit_analytics import live_position_marks
    service = globals().get('_manual_prediction_service')
    board_marks = live_position_marks(service.snapshot(False),now_ms) if service and held else {}
    for p in held:
        cid = p['condition_id']
        m = {**metadata.get((cid,p['token_id']), metadata.get((cid,None),{}))}
        outcome = outcome_metadata.get((cid,p['token_id']), {})
        if outcome.get('outcome_label'):
            m['outcome_label'] = outcome['outcome_label']
        if outcome.get('manual_question'):
            m['question'] = outcome['manual_question']
        # Do not borrow the opposite token's bid when its own book is missing.
        own_book = m.get('token_id') == p['token_id']
        cost = p['cost']+p['entry_fees']
        avg = cost/p['shares']
        close = m.get('window_close')
        secs = int((close-now_ms)/1000) if close else None
        pending = secs is not None and secs <= 0
        bid, ask = m.get('best_bid'),m.get('best_ask')
        live = bool(own_book and not pending and bid is not None and ask is not None
                    and 0 < bid <= ask < 1)
        mark_source = 'book'
        observed = board_marks.get((cid,p['side_label']))
        if not live and not pending and observed:
            bid, ask, live, own_book = observed['bid'], observed['ask'], True, True
            m = {**m,'mark_ts':observed['ts']}
            mark_source = 'public_clob_book'
        # Accounting uses an executable bid, otherwise explicitly unpriced cost.
        # A model estimate is not liquidatable cash and must not inflate equity.
        mark = float(bid) if live else avg
        value = mark*p['shares']
        positions.append({**m,'cid':cid,'token_id':p['token_id'],'instance':p['instance'],
            'session_id':p.get('session_id'),'side':p['side_label'],
            'shares':p['shares'],'avg':avg,'cost_basis':cost,'market_value':value,
            'mark':mark,'ask':ask if own_book else None,
            'mark_source':mark_source if live else 'pending' if pending else 'cost',
            'mark_is_live':live,'valuation_estimated':not live,
            'pnl_usd':value-cost,'pnl_pct':100*(value/cost-1) if cost else None,
            'seconds_left':secs,'awaiting_settlement':pending,'last_ts':p.get('last_ts')})
    return {'positions':positions,'ts':now_ms,
            'total_unrealized':sum(p['pnl_usd'] for p in positions),
            'deployed_usd':sum(p['cost_basis'] for p in positions),
            'market_value':sum(p['market_value'] for p in positions),
            'source':'fills_replay','accounting_errors':ledger['errors']}


def ledger_accounting(mode=None, position_snapshot=None, since=None, instance=None):
    """Truth source for paper accounting: fills cashflow + live open-position marks.
    When instance is set, restricts to that bot's fills and uses its starting stake."""
    mode = mode or current_mode(instance)
    pos = position_snapshot or reconstructed_positions(mode, since, instance)
    if mode.lower() == "paper" and not since:
        start = starting_balance_usd(instance)
        return {
            "source": "fresh_paper_session",
            "cash_usd": start,
            "equity_usd": start,
            "realized_pnl": 0.0,
            "unrealized_pnl": 0.0,
            "net_pnl": 0.0,
            "deployed_usd": 0.0,
            "market_value_usd": 0.0,
            "buy_value_usd": 0.0,
            "sell_value_usd": 0.0,
            "fees_usd": 0.0,
            "session_start": None,
        }
    row = pos.get('_ledger') or scoped_fill_ledger(mode, since, instance)
    buy_value = float(row.get("buy_value") or 0.0)
    sell_value = float(row.get("sell_value") or 0.0)
    fees = float(row.get("fees") or 0.0)
    cashflow = sell_value - buy_value - fees
    market_value = float(pos.get("market_value") or 0.0)
    unreal = float(pos.get("total_unrealized") or 0.0)
    net = cashflow + market_value
    start = starting_balance_usd(instance)
    return {
        "source": "fills_ledger",
        "cash_usd": start + cashflow,
        "equity_usd": start + net,
        "realized_pnl": net - unreal,
        "unrealized_pnl": unreal,
        "net_pnl": net,
        "deployed_usd": float(pos.get("deployed_usd") or 0.0),
        "market_value_usd": market_value,
        "buy_value_usd": buy_value,
        "sell_value_usd": sell_value,
        "fees_usd": fees,
        "session_start": since,
    }


# ── API endpoints ───────────────────────────────────────────────────────────
def api_status(position_snapshot=None, since=None, instance=None):
    mode = current_mode(instance)
    fleet_bot = bot_status_detail()
    bot = fleet_bot
    if instance:
        own = _systemd_bot_status(f"polypulse-bot@{instance}.service")
        if own:
            bot = {**own, "instance": instance,
                   "instances_running": fleet_bot.get("instances_running", [])}
    if since is None:
        since = accounting_since(instance, mode, bot)
    since = normalize_since(since)
    ph_since = "AND ts >= %s::timestamptz" if since else ""
    ph_args = [mode]
    if since:
        ph_args.append(since)
    if instance:
        ph_since += ' AND instance=%s'
        ph_args.append(instance)
    ph = q(f"""SELECT ts, cash_usd, equity_usd, realized_pnl, unrealized_pnl, deployed_usd
               FROM vp.positions_history WHERE mode=%s {ph_since}
               ORDER BY ts DESC LIMIT 1""",
           tuple(ph_args), one=True)
    # Use instance-specific log when viewing a single bot
    log_path = instance_log(instance) if instance else LOG_FILE
    lines = tail(log_path, 600)
    wallet = latest_log_value(lines, "wallet balance")
    pos = position_snapshot or reconstructed_positions(mode, since, instance)
    ledger = scoped_fill_ledger(mode, since, instance) if since or mode.lower() != 'paper' else {'exits':[]}
    wins = sum(e['matched'] and e['realized_pnl'] > 1e-9 for e in ledger['exits'])
    losses = sum(e['matched'] and e['realized_pnl'] < -1e-9 for e in ledger['exits'])
    acct = ledger_accounting(mode, pos, since, instance)
    realized = acct["realized_pnl"]
    unreal = acct["unrealized_pnl"]
    deployed = acct["deployed_usd"]
    equity = acct["equity_usd"]
    cash = acct["cash_usd"]
    if mode.lower() == 'live':
        # Actual free wallet cash already includes deposits, withdrawals and
        # redemptions. Never synthesize it from a paper starting-stake setting.
        fresh_wallet = ph and time.time()*1000-ph['ts'] < 90000
        cash = float(ph['cash_usd']) if fresh_wallet else None
        equity = cash+acct['market_value_usd'] if cash is not None else None
        acct['source'] = 'wallet_snapshot_and_fills' if fresh_wallet else 'wallet_snapshot_unavailable'
    health = None
    health_fresh = health_total = health_stale = None
    for ln in reversed(lines):
        if "orderbook health" in ln:
            health = ln.split("orderbook health:", 1)[1].strip()
            m = re.search(r"(\d+)\s*/\s*(\d+)\s+(fresh|stale)", health)
            if m:
                first, total, label = int(m.group(1)), int(m.group(2)), m.group(3)
                health_total = total
                if label == "fresh":
                    health_fresh = first
                    health_stale = max(0, total - first)
                else:
                    health_stale = first
                    health_fresh = max(0, total - first)
            break
    hb = None
    for ln in reversed(lines):
        if "autopilot heartbeat" in ln:
            hb = ln.split("autopilot heartbeat:", 1)[1].strip()
            break
    return {
        "mode": mode,
        "running": bool(bot.get("running")),
        "pid": bot.get("pid"),
        "bot": bot,
        "wallet_usd": wallet,
        "cash_usd": cash,
        "equity_usd": equity,
        "realized_pnl": realized,
        "unrealized_pnl": unreal,
        "net_pnl": acct["net_pnl"],
        "deployed_usd": deployed,
        "market_value_usd": acct["market_value_usd"],
        "buy_value_usd": acct["buy_value_usd"],
        "sell_value_usd": acct["sell_value_usd"],
        "fees_usd": acct["fees_usd"],
        "accounting_source": acct["source"],
        "accounting_errors": pos.get("accounting_errors", []),
        "session_start": bot.get("session_start"),
        "open_positions": len(pos.get("positions") or []),
        "wins": wins,
        "losses": losses,
        "win_pct": (100.0 * wins / (wins + losses)) if (wins + losses) else None,
        "snapshot_ts": ph.get("ts") if ph else None,
        "health": health,
        "health_fresh": health_fresh,
        "health_stale": health_stale,
        "health_total": health_total,
        "heartbeat": hb,
        "instances_running": fleet_bot.get("instances_running", []),
        "server_ts": int(time.time() * 1000),
    }


def api_positions():
    return reconstructed_positions()


def _synthetic_price_line(cid, side, minutes):
    """Estimated share-price line from oracle history, for when the book feed is
    empty. Samples oracle ticks across [window_open, now] (~1 point / 10s) and
    applies synthetic_mark so the chart still shows a live-moving curve."""
    m = q("""SELECT asset, window_open, window_close FROM vp.pm_markets
             WHERE condition_id=%s LIMIT 1""", (cid,), one=True)
    if not m or not m.get("asset") or not m.get("window_open"):
        return []
    asset, wopen_ms, wclose_ms = m["asset"], m["window_open"], m.get("window_close")
    strike = q("""SELECT price FROM vp.oracle_ticks
                  WHERE asset=%s AND ts <= to_timestamp(%s)
                  ORDER BY ts DESC LIMIT 1""", (asset, wopen_ms / 1000.0), one=True)
    if not strike or strike.get("price") is None:
        return []
    strike = float(strike["price"])
    sigma = _asset_sigma().get(asset)
    rows = q("""SELECT DISTINCT ON (bucket) extract(epoch from ts)*1000 AS ts_ms, price
                FROM (
                  SELECT ts, price, floor(extract(epoch from ts)/10)::bigint AS bucket
                  FROM vp.oracle_ticks
                  WHERE asset=%s
                    AND ts >= GREATEST(to_timestamp(%s), now() - %s::interval)
                    AND ts <= now()
                ) s ORDER BY bucket, ts DESC""",
             (asset, wopen_ms / 1000.0, f"{minutes} minutes"))
    wclose_s = (wclose_ms / 1000.0) if wclose_ms else None
    out = []
    for r in rows:
        ts_ms, price = r.get("ts_ms"), r.get("price")
        if ts_ms is None or price is None:
            continue
        secs_left = (wclose_s - float(ts_ms) / 1000.0) if wclose_s else 0
        mk = synthetic_mark(side, float(price), strike, secs_left, sigma)
        if mk is not None:
            out.append({"ts": int(ts_ms), "price": mk})
    out.sort(key=lambda p: p["ts"])
    return out


def api_price_history(cid, side, minutes=30):
    # Snake chart: share price (0..1) over time.
    # Primary: pm_book_snapshots (live feed midprice).
    # Fallback: synthetic oracle-derived line, then sparse fill prices.
    rows = q("""SELECT ts, midprice, best_bid, best_ask
                FROM vp.pm_book_snapshots
                WHERE condition_id=%s AND side_label=%s
                  AND ts > now() - %s::interval
                ORDER BY ts ASC""", (cid, side, f"{minutes} minutes"))
    pts = []
    for r in rows:
        v = r.get("midprice") or r.get("best_bid")
        if v:
            pts.append({"ts": r["ts"], "price": float(v)})
    mode = current_mode()
    since = session_start_for_mode(mode)
    marker_since = "AND ts_created >= %s::timestamptz" if since else ""
    marker_args = [mode, cid, side, f"{minutes} minutes"]
    if since:
        marker_args.append(since)
    markers = q(f"""SELECT ts_created, direction, source, price, size_shares,
                          notional_usd, status
                   FROM vp.orders
                   WHERE mode=%s AND condition_id=%s AND side_label=%s
                     AND status='filled'
                     AND ts_created > now() - %s::interval
                     {marker_since}
                   ORDER BY ts_created ASC""", tuple(marker_args))
    # When no live book data: prefer a synthetic oracle-derived line (moves
    # live), then fall back to sparse fill prices so the canvas is never blank.
    source = "book"
    if not pts:
        syn = _synthetic_price_line(cid, side, minutes)
        if syn:
            pts = syn
            source = "synthetic"
        elif markers:
            source = "fills"
            for m in markers:
                p = m.get("price")
                ts = m.get("ts_created")
                if p and ts:
                    pts.append({"ts": ts, "price": float(p)})
    # Also include current mark from the book if recent snapshot exists anywhere
    cur_book = q("""SELECT best_bid, best_ask, midprice, ts
                    FROM vp.pm_book_snapshots
                    WHERE token_id IN (
                        SELECT yes_token_id FROM vp.pm_markets WHERE condition_id=%s
                        UNION ALL
                        SELECT no_token_id  FROM vp.pm_markets WHERE condition_id=%s)
                    ORDER BY ts DESC LIMIT 1""", (cid, cid), one=True)
    return {"cid": cid, "side": side, "points": pts, "markers": markers,
            "source": source, "cur_book": cur_book}


def api_health():
    lines = tail(LOG_FILE, 800)
    def last(needle):
        for ln in reversed(lines):
            if needle in ln:
                return ln
        return None
    bot = bot_status_detail()
    out = {
        "running": bool(bot.get("running")),
        "pid": bot.get("pid"),
        "bot": bot,
        "server_ts": int(time.time() * 1000)
    }
    # WebSocket connection states (most recent line mentioning each feed)
    ws = {}
    for ln in reversed(lines):
        if "ws state" in ln or "ws disconnected" in ln or "connected + subscribed" in ln:
            for feed, key in (("market", "pm-md"), ("user", "user-ws"), ("rtds", "rtds")):
                if key in ln and feed not in ws:
                    ws[feed] = ("disconnected" if "disconnect" in ln else
                                "connected" if ("connected" in ln or "ws state" in ln) else "?")
        if len(ws) >= 3:
            break
    out["ws"] = ws
    # Latest pm-md events line → frames/s + parsefail
    ev = last("pm-md events/10s")
    if ev:
        out["events"] = ev.split("pm-md events/10s:", 1)[1].strip()
        try:
            import re
            pf = re.search(r"parsefail=(\d+)", ev)
            out["parsefail"] = int(pf.group(1)) if pf else None
        except Exception:
            pass
    # Orderbook health (stale ratio) + heartbeat
    hb = last("autopilot heartbeat")
    if hb:
        out["heartbeat"] = hb.split("autopilot heartbeat:", 1)[1].strip()
    oh = last("orderbook health")
    if oh:
        out["orderbook_health"] = oh.split("orderbook health:", 1)[1].strip()
    out["wallet_usd"] = latest_log_value(lines, "wallet balance")
    # Oracle failover state: who is feeding the oracle right now?
    # "FAILOVER ACTIVE ... from Coinbase/Binance" vs "failover OFF — RTDS resumed".
    out["oracle_source"] = "chainlink"
    for ln in reversed(lines):
        if "FAILOVER ACTIVE" in ln:
            out["oracle_source"] = ("coinbase-failover" if "Coinbase" in ln
                                    else "binance-failover")
            break
        if "failover OFF" in ln:
            break
    # Session line → id + instance label
    ses = last("session: id=")
    if ses:
        try:
            out["session_id"] = ses.split("id=")[1].split(" ")[0]
            if "instance='" in ses:
                out["instance"] = ses.split("instance='")[1].split("'")[0] or "default"
        except Exception:
            pass
    return out


def api_orders(limit=60, instance=None, all_modes=False):
    """Return orders for current-session accounting or durable UI history.

    ``all_modes`` is intentionally used only by the Positions activity feed:
    switching a bot between paper/live must not hide the trades it just made.
    P&L and risk accounting continue to use the current mode and session.
    """
    mode = current_mode(instance)
    since = None if all_modes else accounting_since(instance, mode)
    if not all_modes and mode.lower() == "paper" and not since:
        return {"orders": []}
    filters = []
    args = []
    if not all_modes:
        filters.append("o.mode = %s")
        args.append(mode)
    if since:
        filters.append("o.ts_created >= %s::timestamptz")
        args.append(since)
    if instance:
        filters.append("o.instance = %s")
        args.append(instance)
    where_clause = " AND ".join(filters) if filters else "TRUE"
    args.append(limit)
    rows = q(f"""
        SELECT o.client_order_id::text AS client_order_id,
               o.ts_created, o.source, o.direction,
               COALESCE(NULLIF(to_jsonb(o)->>'outcome_label',''),o.side_label::text) AS side_label,
               COALESCE(NULLIF(to_jsonb(o)->>'manual_market_question',''),m.question) AS question,
               o.price, o.size_shares,
               COALESCE(SUM(f.size_shares),0) AS filled_shares,
               CASE WHEN o.status IN ('filled','partial','cancelled')
                    THEN COALESCE(SUM(f.price * f.size_shares), o.notional_usd)
                    ELSE o.notional_usd END AS notional_usd,
               COALESCE(SUM(f.fee_usd), 0) AS fee_usd,
               o.status, o.reject_reason, o.condition_id, o.instance,
               m.asset, m.horizon_seconds, o.execution_mode, o.post_only,
               o.exchange_order_id, o.mode
        FROM vp.orders o
        LEFT JOIN vp.fills f ON f.client_order_id = o.client_order_id
        LEFT JOIN vp.pm_markets m ON m.condition_id = o.condition_id
        WHERE {where_clause}
        GROUP BY o.client_order_id, m.asset, m.horizon_seconds, m.question
        ORDER BY o.ts_created DESC LIMIT %s
    """, tuple(args))
    for row in rows:
        if row.get("reject_reason"):
            row["reject_reason"] = _polytron_redact(str(row["reject_reason"]))
    return {"orders": rows}


def _queue_cancel_order(instance, client_order_id, exchange_order_id):
    if not instance or not re.fullmatch(r"[\w-]+", instance):
        return 400, {"error": "select one bot instance"}
    if not re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", str(client_order_id or "")):
        return 400, {"error": "invalid client order id"}
    order = q("""SELECT client_order_id::text, exchange_order_id, status::text AS status,
                       mode::text AS mode, instance
                FROM vp.orders WHERE client_order_id=%s::uuid AND instance=%s LIMIT 1""",
              (client_order_id, instance), one=True)
    if not order:
        return 404, {"error": "order was not found for the selected bot"}
    if str(order.get("status") or "").lower() not in ("submitted", "acked", "partial"):
        return 409, {"error": "order is no longer open; refresh the order list"}
    mode = str(order.get("mode") or "").lower()
    if mode not in ("paper", "live"):
        return 409, {"error": "order mode is unavailable; refresh the order list"}
    stored_id = str(order.get("exchange_order_id") or "")
    if mode == "paper":
        stored_id = str(order.get("client_order_id") or "")
        if exchange_order_id and not hmac.compare_digest(stored_id, str(exchange_order_id)):
            return 409, {"error": "order identifier changed; refresh before cancelling"}
    elif not stored_id or not exchange_order_id or not hmac.compare_digest(stored_id, str(exchange_order_id)):
        return 409, {"error": "order identifier changed; refresh before cancelling"}
    service = _systemd_bot_status(f"polypulse-bot@{instance}.service") or {}
    observed = _observed_runtime_status(instance, service.get("pid")) if service.get("running") else None
    if not observed or str(observed.get("mode") or "").lower() != mode:
        return 409, {"error": "bot runtime is stopped or its mode no longer matches this order"}
    os.makedirs(COMMANDS_DIR, exist_ok=True)
    ts = int(time.time() * 1000)
    filename = f"cancel_order_{instance}_{ts}.json"
    _write_control_atomic(os.path.join(COMMANDS_DIR, filename), {
        "op": "cancel_order", "instance": instance, "ts": ts,
        "expected_mode": mode.upper(),
        "client_order_id": str(order["client_order_id"]),
        "exchange_order_id": stored_id,
    })
    return 200, {"ok": True, "status": "queued", "instance": instance, "queued": filename,
                 "client_order_id": str(order["client_order_id"])}


def api_source_pnl(instance=None):
    """Fee-aware realized P&L attributed to the originating buy strategy.

    Exit proceeds (including generic `settle` orders) are allocated back to buy
    sources pro-rata by shares for the same token. Open inventory is not shown
    as a realized loss merely because its entry cashflow is negative.
    """
    mode = current_mode(instance)
    since = accounting_since(instance, mode)
    if mode.lower() == "paper" and not since:
        return {"sources": []}
    since_clause = "AND o.ts_created >= %s::timestamptz" if since else ""
    inst_clause = "AND o.instance = %s" if instance else ""
    args = [mode]
    if since:
        args.append(since)
    if instance:
        args.append(instance)
    rows = q(f"""
        WITH buys AS (
          SELECT o.condition_id,o.token_id,COALESCE(NULLIF(o.source,''),'unknown') source,
                 COUNT(DISTINCT o.client_order_id) buys,
                 SUM(f.size_shares) shares,SUM(f.price*f.size_shares) cost,SUM(f.fee_usd) entry_fees
        FROM vp.orders o
        JOIN vp.fills f ON f.client_order_id = o.client_order_id
        WHERE o.mode=%s AND o.direction='buy'
          {since_clause}
          {inst_clause}
        GROUP BY o.condition_id,o.token_id,COALESCE(NULLIF(o.source,''),'unknown')
        ), totals AS (
          SELECT condition_id,token_id,SUM(shares) shares FROM buys GROUP BY 1,2
        ), sells AS (
          SELECT o.condition_id,o.token_id,COUNT(DISTINCT o.client_order_id) sells,
                 SUM(f.size_shares) shares,
                 SUM(f.price*f.size_shares)-SUM(f.fee_usd) net_proceeds,
                 SUM(f.fee_usd) exit_fees
          FROM vp.orders o JOIN vp.fills f ON f.client_order_id=o.client_order_id
          WHERE o.mode=%s AND o.direction='sell'
            {since_clause} {inst_clause}
          GROUP BY o.condition_id,o.token_id
        ), attributed AS (
          SELECT b.source,b.buys,COALESCE(s.sells,0) sells,
                 CASE WHEN s.shares>0 AND t.shares>0 THEN
                   s.net_proceeds*b.shares/t.shares -
                   (b.cost+b.entry_fees)*LEAST(1.0,s.shares/t.shares)
                 ELSE 0 END realized,
                 CASE WHEN s.shares>0 AND t.shares>0 THEN
                   b.entry_fees*LEAST(1.0,s.shares/t.shares)+s.exit_fees*b.shares/t.shares
                 ELSE 0 END fees
          FROM buys b JOIN totals t USING(condition_id,token_id)
          LEFT JOIN sells s USING(condition_id,token_id)
        )
        SELECT source,SUM(buys) buys,SUM(sells) sells,SUM(realized) realized,SUM(fees) fees
        FROM attributed GROUP BY source ORDER BY SUM(realized) DESC
    """, tuple(args + args))
    out = []
    for r in rows:
        realized = float(r.get("realized") or 0)
        fees = float(r.get("fees") or 0)
        out.append({"source": r["source"], "buys": r["buys"], "sells": r["sells"],
                    "cashflow": round(realized, 2), "fees": round(fees, 2),
                    "net": round(realized, 2)})
    return {"sources": out}


def api_fills(limit=60):
    mode = current_mode()
    since = session_start_for_mode(mode)
    if mode.lower() == "paper" and not since:
        return {"fills": []}
    since_clause = "AND o.ts_created >= %s::timestamptz" if since else ""
    args = [mode]
    if since:
        args.append(since)
    args.append(limit)
    rows = q(f"""SELECT f.ts, f.price, f.size_shares, f.fee_usd,
                       o.source, o.direction, o.side_label
                FROM vp.fills f JOIN vp.orders o ON o.client_order_id=f.client_order_id
                WHERE o.mode=%s {since_clause}
                ORDER BY f.ts DESC LIMIT %s""", tuple(args))
    return {"fills": rows}


def api_settles(limit=40, instance=None, all_modes=False):
    """Durable exit history, costed from preceding fills of the same token/session."""
    from accounting import replay_fills
    filters, args = ["o.direction='sell'", "EXISTS (SELECT 1 FROM vp.fills f WHERE f.client_order_id=o.client_order_id)"], []
    if not all_modes:
        filters.append('o.mode=%s')
        args.append(current_mode(instance))
    if instance:
        filters.append('o.instance=%s')
        args.append(instance)
    recent = q(f"""SELECT o.client_order_id,o.condition_id FROM vp.orders o
        WHERE {' AND '.join(filters)} ORDER BY o.ts_created DESC LIMIT %s""", tuple(args+[limit]))
    if not recent:
        return {'settles':[]}
    fills = q("""SELECT f.id,f.ts,f.price,f.size_shares,f.fee_usd,
            o.client_order_id,o.mode,o.instance,o.session_id,o.condition_id,
            o.token_id,o.side_label,o.direction,o.source,o.ts_created,
            m.asset,m.horizon_seconds,m.target_price,LEFT(m.question,80) AS question
        FROM vp.orders o JOIN vp.fills f USING(client_order_id)
        LEFT JOIN vp.pm_markets m USING(condition_id)
        WHERE o.condition_id=ANY(%s) ORDER BY f.ts,f.id""",
        (list({r['condition_id'] for r in recent}),))
    ledger = replay_fills(fills)
    exits = {e['client_order_id']:e for e in ledger['exits']}
    rows = []
    for r in recent:
        e = exits[r['client_order_id']]
        shares = e['shares']
        exit_fees = e['fees']-(e['cost_basis']-e['entry_cost'])
        rows.append({**{k:e.get(k) for k in ('ts_created','condition_id','side_label','instance',
            'source','mode','asset','horizon_seconds','target_price','question')},
            'settle_price':e['gross_proceeds']/shares, 'size_shares':shares,
            'payout_usd':e['gross_proceeds']-exit_fees,
            'avg_entry':e['entry_cost']/shares if e['matched'] else None,
            'unit_cost':e['cost_basis']/shares if e['matched'] else None,
            'cost_basis':e['cost_basis'] if e['matched'] else None,
            'realized_pnl':e['realized_pnl'] if e['matched'] else None,
            'fee_usd':e['fees'], 'accounting_complete':e['matched']})
    return {'settles':rows}


def _settlement_cursor_encode(ts, order_id):
    raw = json.dumps([str(ts), str(order_id)], separators=(",", ":")).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _settlement_cursor_decode(cursor):
    if not cursor:
        return None
    try:
        raw = base64.urlsafe_b64decode(str(cursor) + "=" * (-len(str(cursor)) % 4))
        ts, order_id = json.loads(raw.decode())
        if not re.fullmatch(r"[0-9a-fA-F-]{36}", str(order_id)):
            raise ValueError
        datetime.fromisoformat(str(ts).replace("Z", "+00:00"))
        return str(ts), str(order_id)
    except (ValueError, TypeError, UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError("Invalid settlement history cursor")


def _settlement_result(source, price, pnl, avg_entry):
    if source == "settle":
        return "WIN" if (price or 0) > 0.5 else "LOSS"
    if pnl is not None:
        return "WIN" if pnl >= 0 else "LOSS"
    if avg_entry is not None:
        return "WIN" if (price or 0) >= avg_entry else "LOSS"
    return "UNKNOWN"


def _settlement_row(candidate, exit_record):
    shares = exit_record["shares"]
    gross = exit_record["gross_proceeds"]
    matched = bool(exit_record["matched"])
    entry_cost = exit_record["entry_cost"]
    cost_basis = exit_record["cost_basis"]
    total_fees = exit_record["fees"]
    exit_fees = total_fees - (cost_basis - entry_cost)
    avg_entry = entry_cost / shares if matched and shares else None
    price = gross / shares if shares else None
    pnl = exit_record["realized_pnl"] if matched else None
    source = candidate.get("source") or "settle"
    return {
        "exit_order_id": candidate["client_order_id"],
        "exit_ts": candidate["exit_ts"],
        "ts_created": candidate["ts_created"],
        "condition_id": candidate["condition_id"],
        "token_id": candidate["token_id"],
        "side_label": candidate["side_label"],
        "instance": candidate["instance"],
        "session_id": candidate.get("session_id"),
        "exchange_order_id": candidate.get("exchange_order_id"),
        "source": source,
        "mode": candidate["mode"],
        "asset": candidate.get("asset") or "?",
        "horizon_seconds": candidate.get("horizon_seconds"),
        "target_price": candidate.get("target_price"),
        "window_open": candidate.get("window_open"),
        "window_close": candidate.get("window_close"),
        "resolved_outcome": candidate.get("resolved_outcome"),
        "question": candidate.get("question"),
        "slug": candidate.get("slug"),
        "execution_mode": candidate.get("execution_mode"),
        "post_only": candidate.get("post_only"),
        "status": candidate.get("status"),
        "order_price": candidate.get("price"),
        "order_size_shares": candidate.get("size_shares"),
        "settle_price": price,
        "size_shares": shares,
        "gross_proceeds": gross,
        "payout_usd": gross - exit_fees,
        "avg_entry": avg_entry,
        "unit_cost": cost_basis / shares if matched and shares else None,
        "entry_cost": entry_cost if matched else None,
        "cost_basis": cost_basis if matched else None,
        "realized_pnl": pnl,
        "fee_usd": total_fees,
        "accounting_complete": matched,
        "result": _settlement_result(source, price, pnl, avg_entry),
        "_cursor_ts": candidate["cursor_ts"],
    }


def api_settlement_history(instance=None, params=None):
    """Filterable, cursor-paginated exit history costed by the durable fill ledger."""
    from accounting import replay_fills

    params = params or {}
    mode = str(params.get("mode") or "").lower()
    if mode not in ("", "paper", "live"):
        raise ValueError("Invalid trading mode")
    asset = str(params.get("asset") or "").upper()
    if asset not in ("", "BTC", "ETH", "SOL", "XRP"):
        raise ValueError("Invalid asset")
    side = str(params.get("side") or "").upper()
    if side not in ("", "UP", "DOWN"):
        raise ValueError("Invalid side")
    result_filter = str(params.get("result") or "all").lower()
    if result_filter not in ("all", "win", "loss", "unknown"):
        raise ValueError("Invalid result filter")
    search = str(params.get("search") or "").strip()[:100]
    source = str(params.get("source") or "")[:100]
    try:
        horizon = int(params.get("horizon") or 0)
    except (TypeError, ValueError, OverflowError):
        raise ValueError("Invalid market duration")
    if horizon not in (0, 300, 900, 3600, 86400):
        raise ValueError("Invalid market duration")
    try:
        limit = max(1, min(100, int(params.get("limit") or 50)))
    except (TypeError, ValueError, OverflowError):
        limit = 50

    def epoch_ms(value, label):
        if value in (None, ""):
            return None
        try:
            parsed = int(value)
        except (TypeError, ValueError, OverflowError):
            raise ValueError("Invalid " + label + " timestamp")
        if parsed < 0 or parsed > 4_102_444_800_000:
            raise ValueError("Invalid " + label + " timestamp")
        return parsed

    from_ms = epoch_ms(params.get("from_ms"), "start")
    to_ms = epoch_ms(params.get("to_ms"), "end")
    if from_ms is not None and to_ms is not None and from_ms > to_ms:
        raise ValueError("Start time must be before end time")
    cursor = _settlement_cursor_decode(params.get("cursor"))

    filters = ["o.direction='sell'", "EXISTS (SELECT 1 FROM vp.fills f WHERE f.client_order_id=o.client_order_id)"]
    args = []
    if instance:
        filters.append("o.instance=%s")
        args.append(instance)
    if mode:
        filters.append("o.mode::text=%s")
        args.append(mode)
    if asset:
        filters.append("m.asset=%s")
        args.append(asset)
    if horizon:
        filters.append("m.horizon_seconds=%s")
        args.append(horizon)
    if side:
        filters.append("o.side_label::text=%s")
        args.append(side)
    if source:
        filters.append("o.source=%s")
        args.append(source)
    if search:
        filters.append("(o.condition_id ILIKE %s OR o.source ILIKE %s OR o.instance ILIKE %s OR COALESCE(m.question,'') ILIKE %s)")
        needle = "%" + search + "%"
        args.extend([needle] * 4)
    outer_filters, outer_args = [], []
    if from_ms is not None:
        outer_filters.append("x.exit_ts >= to_timestamp(%s / 1000.0)")
        outer_args.append(from_ms)
    if to_ms is not None:
        outer_filters.append("x.exit_ts <= to_timestamp(%s / 1000.0)")
        outer_args.append(to_ms)
    def fetch_candidates(after, batch_size):
        candidate_filters = list(outer_filters)
        candidate_args = list(outer_args)
        if after:
            candidate_filters.append("(x.exit_ts < %s::timestamptz OR (x.exit_ts = %s::timestamptz AND o.client_order_id::text < %s))")
            candidate_args.extend([after[0], after[0], after[1]])
        where_outer = ("WHERE " + " AND ".join(candidate_filters)) if candidate_filters else ""
        return q(f"""
            WITH exit_orders AS (
              SELECT o.client_order_id, MAX(f.ts) AS exit_ts
              FROM vp.orders o JOIN vp.fills f USING(client_order_id)
              LEFT JOIN vp.pm_markets m USING(condition_id)
              WHERE {' AND '.join(filters)}
              GROUP BY o.client_order_id
            )
            SELECT o.client_order_id::text, x.exit_ts,
                   to_char(x.exit_ts AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US') || '+00:00' AS cursor_ts,
                   o.ts_created, o.ts_submitted, o.ts_terminal, o.instance, o.mode::text AS mode,
                   o.session_id::text AS session_id, o.condition_id, o.token_id,
                   o.side_label::text AS side_label, o.direction::text AS direction,
                   o.source, o.execution_mode, o.post_only, o.status::text AS status,
                   o.price, o.size_shares, o.notional_usd,
                   o.exchange_order_id,
                   m.asset, m.horizon_seconds, m.target_price, m.window_open, m.window_close,
                   m.resolved_outcome::text AS resolved_outcome, m.question, m.slug
            FROM exit_orders x JOIN vp.orders o USING(client_order_id)
            LEFT JOIN vp.pm_markets m USING(condition_id)
            {where_outer}
            ORDER BY x.exit_ts DESC, o.client_order_id::text DESC
            LIMIT %s
        """, tuple(args + candidate_args + [batch_size]))

    # Bot/source choices are scoped to this request's authorized instance.
    option_filters = ["o.direction='sell'", "EXISTS (SELECT 1 FROM vp.fills f WHERE f.client_order_id=o.client_order_id)"]
    option_args = []
    if instance:
        option_filters.append("o.instance=%s")
        option_args.append(instance)
    options = q(f"""SELECT DISTINCT o.instance, o.source
        FROM vp.orders o WHERE {' AND '.join(option_filters)}
        ORDER BY o.instance, o.source""", tuple(option_args))
    bots = sorted({str(r["instance"]) for r in options if r.get("instance")})
    sources = sorted({str(r["source"]) for r in options if r.get("source")})

    rows, scan_cursor, scanned, exhausted = [], cursor, 0, False
    next_cursor = None
    batch_size, max_scan = 200, 1200
    while scanned < max_scan:
        batch = fetch_candidates(scan_cursor, min(batch_size, max_scan - scanned))
        if not batch:
            exhausted = True
            break
        fills = q("""SELECT f.id, f.ts, f.price, f.size_shares, f.fee_usd,
                    o.client_order_id::text, o.mode::text AS mode, o.instance,
                    o.session_id::text AS session_id, o.condition_id, o.token_id,
                    o.side_label::text AS side_label, o.direction::text AS direction,
                    o.source, o.ts_created, m.asset, m.horizon_seconds
             FROM vp.orders o JOIN vp.fills f USING(client_order_id)
             LEFT JOIN vp.pm_markets m USING(condition_id)
             WHERE o.condition_id=ANY(%s) ORDER BY f.ts,f.id""",
             (list({r["condition_id"] for r in batch}),))
        ledger = replay_fills(fills)
        exits = {e["client_order_id"]: e for e in ledger["exits"]}
        for candidate in batch:
            scanned += 1
            scan_cursor = (candidate["cursor_ts"], candidate["client_order_id"])
            exit_record = exits.get(candidate["client_order_id"])
            if not exit_record:
                continue
            row = _settlement_row(candidate, exit_record)
            if result_filter != "all" and row["result"].lower() != result_filter:
                continue
            row["_cursor_id"] = candidate["client_order_id"]
            rows.append(row)
            if len(rows) > limit:
                boundary = rows[limit - 1]
                next_cursor = _settlement_cursor_encode(boundary["_cursor_ts"], boundary["_cursor_id"])
                rows = rows[:limit]
                break
        if next_cursor:
            break
        if len(batch) < min(batch_size, max_scan - (scanned - len(batch))):
            exhausted = True
            break
        if scanned >= max_scan:
            next_cursor = _settlement_cursor_encode(*scan_cursor)
            break

    for row in rows:
        row.pop("_cursor_ts", None)
        row.pop("_cursor_id", None)
    return {"rows": rows, "next_cursor": next_cursor, "has_more": bool(next_cursor),
            "scanned": scanned, "bots": bots, "sources": sources,
            "server_ts": int(time.time() * 1000)}


def _settlement_public_payload(value, depth=0):
    if depth > 5:
        return None
    if isinstance(value, dict):
        blocked = re.compile(r"(?:private|secret|credential|authorization|password|api.?key)", re.I)
        return {str(k): _settlement_public_payload(v, depth + 1)
                for k, v in value.items() if not blocked.search(str(k))}
    if isinstance(value, list):
        return [_settlement_public_payload(v, depth + 1) for v in value[:100]]
    if isinstance(value, str):
        return value[:3000]
    return value


def api_settlement_detail(order_id, instance=None):
    """Full, safe transaction detail for one matched exit order."""
    from accounting import replay_fills

    order_id = str(order_id or "")
    if not re.fullmatch(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}", order_id):
        raise ValueError("Invalid transaction id")
    where = "o.client_order_id=%s AND o.direction='sell'"
    args = [order_id]
    if instance:
        where += " AND o.instance=%s"
        args.append(instance)
    trade = q(f"""SELECT o.client_order_id::text, o.ts_created, o.ts_submitted, o.ts_terminal,
            o.instance, o.mode::text AS mode, o.session_id::text AS session_id,
            o.condition_id, o.token_id, o.side_label::text AS side_label,
            o.direction::text AS direction, o.source, o.execution_mode, o.post_only,
            o.status::text AS status, o.price, o.size_shares, o.notional_usd,
            o.exchange_order_id, MAX(f.ts) AS exit_ts,
            to_char(MAX(f.ts) AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US') || '+00:00' AS cursor_ts,
            m.asset, m.horizon_seconds, m.target_price, m.window_open, m.window_close,
            m.resolved_outcome::text AS resolved_outcome, m.question, m.slug
        FROM vp.orders o JOIN vp.fills f USING(client_order_id)
        LEFT JOIN vp.pm_markets m USING(condition_id)
        WHERE {where}
        GROUP BY o.client_order_id,m.asset,m.horizon_seconds,m.target_price,m.window_open,
                 m.window_close,m.resolved_outcome,m.question,m.slug""", tuple(args), one=True)
    if not trade:
        return {"error": "Settlement transaction not found"}

    fills = q("""SELECT f.id, f.ts, f.price, f.size_shares, f.fee_usd,
            o.client_order_id::text, o.instance, o.mode::text AS mode,
            o.session_id::text AS session_id, o.condition_id, o.token_id,
            o.side_label::text AS side_label, o.direction::text AS direction,
            o.source, o.execution_mode, o.post_only, o.status::text AS status,
            o.price AS order_price, o.size_shares AS order_size_shares,
            o.notional_usd AS order_notional_usd, o.ts_created AS order_created,
            o.ts_submitted, o.ts_terminal, o.exchange_order_id
        FROM vp.orders o JOIN vp.fills f USING(client_order_id)
        WHERE o.condition_id=%s AND o.token_id=%s AND o.mode::text=%s
          AND o.instance IS NOT DISTINCT FROM %s
          AND o.session_id IS NOT DISTINCT FROM %s
          AND f.ts <= %s::timestamptz
        ORDER BY f.ts,f.id""", (trade["condition_id"], trade["token_id"], trade["mode"],
                                  trade["instance"], trade["session_id"], trade["cursor_ts"]))
    ledger = replay_fills(fills)
    exit_record = next((e for e in ledger["exits"] if e["client_order_id"] == order_id), None)
    if not exit_record:
        return {"error": "Transaction fill accounting is unavailable"}
    summary = _settlement_row(trade, exit_record)
    summary.pop("_cursor_ts", None)
    entry = next((f for f in fills if f.get("direction") == "buy"), None)
    signal = None
    if entry:
        signal = q("""SELECT ts, side_label::text AS side_label, confidence, recommended,
                      reason, model_version, regime_label, payload
               FROM vp.signals WHERE condition_id=%s AND side_label::text=%s
                 AND direction='buy' AND ts <= to_timestamp(%s / 1000.0)
                 AND payload->>'instance' IS NOT DISTINCT FROM %s
                 AND (payload->>'session_id' IS NULL OR payload->>'session_id' IS NOT DISTINCT FROM %s)
               ORDER BY (payload->>'client_order_id'=%s) DESC, ts DESC LIMIT 1""",
               (trade["condition_id"], trade["side_label"], entry["ts"],
                trade["instance"], trade["session_id"], entry["client_order_id"]), one=True)
    for fill in fills:
        gross = float(fill["price"] or 0) * float(fill["size_shares"] or 0)
        fee = float(fill["fee_usd"] or 0)
        fill["gross_cashflow_usd"] = gross if fill["direction"] == "sell" else -gross
        fill["net_cashflow_usd"] = fill["gross_cashflow_usd"] - fee
    window_events = []
    open_tick = close_tick = None
    try:
        window_events = q("""SELECT ts,event_type,payload FROM vp.window_events
            WHERE condition_id=%s ORDER BY ts,id LIMIT 40""", (trade["condition_id"],))
    except Exception:
        pass
    if trade.get("asset") and trade.get("window_open"):
        try:
            open_tick = q("""SELECT ts,price,source FROM vp.oracle_ticks
                WHERE asset=%s AND ts BETWEEN to_timestamp(%s / 1000.0)-interval '10 seconds'
                    AND to_timestamp(%s / 1000.0)+interval '10 seconds'
                ORDER BY ABS(EXTRACT(EPOCH FROM(ts-to_timestamp(%s / 1000.0)))) LIMIT 1""",
                (trade["asset"], trade["window_open"], trade["window_open"], trade["window_open"]), one=True)
            if trade.get("window_close"):
                close_tick = q("""SELECT ts,price,source FROM vp.oracle_ticks
                    WHERE asset=%s AND ts BETWEEN to_timestamp(%s / 1000.0)-interval '30 seconds'
                        AND to_timestamp(%s / 1000.0)+interval '10 seconds'
                    ORDER BY ABS(EXTRACT(EPOCH FROM(ts-to_timestamp(%s / 1000.0)))) LIMIT 1""",
                    (trade["asset"], trade["window_close"], trade["window_close"], trade["window_close"]), one=True)
        except Exception:
            open_tick = close_tick = None
    if signal and signal.get("payload") is not None:
        signal["payload"] = _settlement_public_payload(signal["payload"])
    for event in window_events:
        if event.get("payload") is not None:
            event["payload"] = _settlement_public_payload(event["payload"])
    allocated_entry_fees = exit_record["cost_basis"] - exit_record["entry_cost"]
    return {"trade": summary,
            "market": {k: trade.get(k) for k in ("condition_id", "asset", "horizon_seconds", "target_price",
                "window_open", "window_close", "resolved_outcome", "question", "slug")},
            "signal": signal, "fills": fills, "window_events": window_events,
            "oracle_open": open_tick, "oracle_close": close_tick,
            "math": {"shares": exit_record["shares"], "gross_proceeds": exit_record["gross_proceeds"],
                "entry_cost": exit_record["entry_cost"], "allocated_entry_fees": allocated_entry_fees,
                "exit_fees": exit_record["fees"] - allocated_entry_fees,
                "total_fees": exit_record["fees"], "net_realized_pnl": exit_record["realized_pnl"],
                "accounting_complete": bool(exit_record["matched"])},
            "server_ts": int(time.time() * 1000)}


def api_predictor(instance=None, limit=150):
    """PREDICTOR tab feed: WPRED checkpoint predictions joined to their outcome.

    Each row = one checkpoint buy (vp.orders source='WPRED') LATERAL-joined to
    the nearest WPRED signal (±5s, carries fraction in payload jsonb) and to the
    settlement fill for the same (condition_id, side). correct = settle price 1.
    """
    inst = instance or "predictor"
    mode = current_mode(inst)
    since = accounting_since(inst, mode)
    hot = load_config(inst).get("strategy", {})
    engine = {
        "running": instance_running(inst),
        "enabled": bool(hot.get("window_predictor_mode")),
        "fractions": hot.get("window_predictor_fractions") or [],
        "pairs": hot.get("window_predictor_pairs") or [],
        "assets": hot.get("window_predictor_assets") or [],
        "horizons": hot.get("window_predictor_horizons") or [],
        "adaptive": bool(hot.get("adaptive_mode")),
        "session_start": since,
    }
    if mode.lower() == "paper" and not since:
        return {"predictions": [], "by_fraction": [], "by_asset": [],
                "engine": engine,
                "summary": {"total": 0, "settled": 0, "correct": 0,
                            "pending": 0, "accuracy": None, "pnl": 0.0}}
    rows = q(f"""
        SELECT o.ts_created, o.condition_id, o.side_label,
               m.asset, m.horizon_seconds, LEFT(m.question, 80) AS question,
               ROUND(b.fill_price::numeric, 4) AS fill_price,
               ROUND(b.size_shares::numeric, 3) AS size_shares,
               ROUND(b.fee_usd::numeric, 4) AS fee_usd,
               st.settle_price,
               s.confidence AS sig_confidence, s.reason AS sig_reason, s.payload
        FROM vp.orders o
        LEFT JOIN vp.pm_markets m ON m.condition_id = o.condition_id
        JOIN LATERAL (
          SELECT SUM(f.price * f.size_shares) / NULLIF(SUM(f.size_shares), 0) AS fill_price,
                 SUM(f.size_shares) AS size_shares,
                 SUM(f.fee_usd) AS fee_usd
          FROM vp.fills f WHERE f.client_order_id = o.client_order_id
        ) b ON true
        LEFT JOIN LATERAL (
          SELECT s2.confidence, s2.reason, s2.payload
          FROM vp.signals s2
          WHERE s2.condition_id = o.condition_id
            AND s2.side_label = o.side_label
            AND s2.model_version IN ('WPRED-1', 'WPRED-2')
            AND s2.ts BETWEEN o.ts_created - interval '5 seconds'
                         AND o.ts_created + interval '5 seconds'
          ORDER BY ABS(EXTRACT(EPOCH FROM (s2.ts - o.ts_created)))
          LIMIT 1
        ) s ON true
        LEFT JOIN LATERAL (
          SELECT ROUND(f3.price::numeric, 4) AS settle_price
          FROM vp.orders o3
          JOIN vp.fills f3 ON f3.client_order_id = o3.client_order_id
          WHERE o3.condition_id = o.condition_id
            AND o3.instance = o.instance AND o3.mode = o.mode
            AND o3.direction = 'sell' AND o3.status = 'filled'
            AND o3.side_label = o.side_label
          ORDER BY o3.ts_created DESC LIMIT 1
        ) st ON true
        WHERE o.instance = %s AND o.mode = %s
          AND o.source = 'WPRED' AND o.direction = 'buy' AND o.status = 'filled'
          {"AND o.ts_created >= %s::timestamptz" if since else ""}
        ORDER BY o.ts_created DESC LIMIT %s
    """, tuple(([inst, mode, since] if since else [inst, mode]) + [limit]))

    import re as _re
    fr_re = _re.compile(r"f=([0-9.]+)")
    preds, fr_acc, asset_acc = [], {}, {}
    tot = settled = correct = pending = 0
    pnl_sum = 0.0
    for r in rows:
        payload = r.get("payload") or {}
        frac = None
        try:
            if payload and payload.get("fraction") is not None:
                frac = float(payload["fraction"])
        except (TypeError, ValueError):
            frac = None
        if frac is None and r.get("sig_reason"):
            mm = fr_re.search(r["sig_reason"])
            if mm:
                try:
                    frac = float(mm.group(1))
                except ValueError:
                    frac = None
        try:
            fill = float(r["fill_price"] or 0)
            shares = float(r["size_shares"] or 0)
            fee = float(r["fee_usd"] or 0)
            settle = float(r["settle_price"]) if r.get("settle_price") is not None else None
        except (TypeError, ValueError):
            continue
        won = (settle == 1.0) if settle is not None else None
        pnl = ((settle - fill) * shares - fee) if settle is not None else None
        p = {
            "ts": r["ts_created"], "asset": r.get("asset"),
            "horizon_s": r.get("horizon_seconds"),
            "condition_id": r["condition_id"], "side": r["side_label"],
            "fraction": frac, "fill_price": fill, "shares": shares,
            "settle_price": settle, "correct": won, "pnl": (round(pnl, 4)
                                                            if pnl is not None else None),
            "confidence": r.get("sig_confidence"),
            "prediction": (payload or {}).get("pred"),
            "question": r.get("question"),
        }
        preds.append(p)
        tot += 1
        if settle is None:
            pending += 1
        else:
            settled += 1
            pnl_sum += pnl
            if won:
                correct += 1
            for key, acc in ((frac, fr_acc), (r.get("asset"), asset_acc)):
                if key is None:
                    continue
                a = acc.setdefault(key, {"n": 0, "correct": 0, "pnl": 0.0})
                a["n"] += 1
                a["pnl"] += pnl
                if won:
                    a["correct"] += 1

    def _fmt(acc):
        out = []
        for k, a in acc.items():
            out.append({"key": k, "n": a["n"], "correct": a["correct"],
                        "accuracy": round(a["correct"] / a["n"], 4) if a["n"] else None,
                        "pnl": round(a["pnl"], 4)})
        return out

    by_fraction = sorted(_fmt(fr_acc), key=lambda x: x["key"] or 0)
    by_asset = _fmt(asset_acc)
    return {"predictions": preds, "by_fraction": by_fraction, "by_asset": by_asset,
            "engine": engine,
            "summary": {"total": tot, "settled": settled, "correct": correct,
                        "pending": pending,
                        "accuracy": (round(correct / settled, 4) if settled else None),
                        "pnl": round(pnl_sum, 4)}}


def api_predictor_trajectory(condition_id=None, limit=200):
    """PREDICTOR trajectory feed: confidence trajectory samples for a market.
    
    Returns per-market confidence samples over time, including the adaptive
    entry readiness state and trajectory slope.
    """
    inst = "predictor"
    mode = current_mode(inst)
    since = accounting_since(inst, mode)
    rows = q(f"""
        SELECT ts, condition_id, payload
        FROM vp.signals
        WHERE model_version IN ('WPRED-1', 'WPRED-2')
          {"AND ts >= %s::timestamptz" if since else ""}
          {("AND condition_id = %s" if condition_id else "")}
        ORDER BY ts DESC
        LIMIT %s
    """, tuple(([since] if since else []) +
               ([condition_id] if condition_id else []) + [limit]))
    
    samples = []
    for r in rows:
        payload = r.get("payload") or {}
        conf = payload.get("conf")
        pred = payload.get("pred")
        frac = payload.get("fraction")
        if conf is not None:
            samples.append({
                "ts": r["ts"],
                "condition_id": r["condition_id"],
                "conf": conf,
                "pred": pred,
                "fraction": frac,
            })
    
    # Group by condition_id
    by_market = {}
    for s in samples:
        cid = s["condition_id"]
        if cid not in by_market:
            by_market[cid] = []
        by_market[cid].append(s)
    
    return {"samples": samples, "by_market": by_market}


_prediction_gate_schema_lock = threading.Lock()
_prediction_gate_schema_initialized = False


def _ensure_prediction_gate_schema():
    global _prediction_gate_schema_initialized
    if _prediction_gate_schema_initialized:
        return
    with _prediction_gate_schema_lock:
        if _prediction_gate_schema_initialized:
            return
        with open(os.path.join(ROOT, "sql", "030_prediction_gate_current.sql"),
                  encoding="utf-8") as source:
            migration = source.read()
        pool = get_pool()
        conn = pool.getconn()
        try:
            conn.set_session(readonly=False, autocommit=False)
            with conn.cursor() as cur:
                cur.execute(migration)
            conn.commit()
            _prediction_gate_schema_initialized = True
        except Exception:
            conn.rollback()
            raise
        finally:
            pool.putconn(conn)


def _record_prediction_gate_current(board):
    """Publish exact current-window page forecasts for the bot-side gate."""
    try:
        updated_ms = float(board.get("inputs_at") or board.get("generated_at"))
    except (TypeError, ValueError, OverflowError):
        return
    if not math.isfinite(updated_ms) or updated_ms <= 0:
        return
    rows = []
    for card in board.get("cards") or []:
        if not isinstance(card, dict) or card.get("period") != "CURRENT":
            continue
        condition_id = str(card.get("condition_id") or "")
        asset = str(card.get("asset") or "").upper()
        try:
            horizon = int(card.get("horizon_seconds") or 0)
            opened_ms = float(card.get("window_open"))
            closed_ms = float(card.get("window_close"))
        except (TypeError, ValueError, OverflowError):
            continue
        if (not condition_id or asset not in ("BTC", "ETH", "SOL", "XRP") or
                horizon not in (300, 900) or not math.isfinite(opened_ms) or
                not math.isfinite(closed_ms) or closed_ms <= opened_ms):
            continue
        model_status = str(card.get("model_status") or "unknown")
        raw_probability = card.get("independent_up_probability")
        if raw_probability is None:
            raw_probability = card.get("up_probability")
        try:
            probability = float(raw_probability) if raw_probability is not None else None
        except (TypeError, ValueError, OverflowError):
            probability = None
        if (probability is None or not math.isfinite(probability) or
                probability < 0.0 or probability > 1.0 or model_status != "ready"):
            probability = None
        rows.append((condition_id, asset, horizon, str(card.get("slug") or ""),
            datetime.fromtimestamp(opened_ms / 1000.0, timezone.utc),
            datetime.fromtimestamp(closed_ms / 1000.0, timezone.utc), model_status,
            probability,
            str(card.get("independent_model_version") or board.get("independent_model_version") or board.get("model_version") or ""),
            str(card.get("prediction_basis") or ""),
            datetime.fromtimestamp(updated_ms / 1000.0, timezone.utc)))

    _ensure_prediction_gate_schema()
    if not rows:
        return
    pool = get_pool()
    conn = pool.getconn()
    try:
        conn.set_session(readonly=False, autocommit=False)
        with conn.cursor() as cur:
            psycopg2.extras.execute_values(cur, """INSERT INTO vp.prediction_gate_current
                (condition_id,asset,horizon_seconds,slug,window_open,window_close,
                 model_status,up_probability,model_version,model_basis,source_updated_at)
                VALUES %s ON CONFLICT(condition_id) DO UPDATE SET
                  asset=EXCLUDED.asset,horizon_seconds=EXCLUDED.horizon_seconds,
                  slug=EXCLUDED.slug,window_open=EXCLUDED.window_open,
                  window_close=EXCLUDED.window_close,model_status=EXCLUDED.model_status,
                  up_probability=EXCLUDED.up_probability,model_version=EXCLUDED.model_version,
                  model_basis=EXCLUDED.model_basis,source_updated_at=EXCLUDED.source_updated_at,
                  published_at=clock_timestamp()""", rows)
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        pool.putconn(conn)


# ── Manual-trading prediction board ────────────────────────────────────────
_manual_prediction_service = None
_manual_prediction_lock = threading.Lock()
_manual_news_cache = [0.0, None]
_manual_news_lock = threading.Lock()


def _public_json(url, timeout=5.0, data=None):
    import urllib.request
    req = urllib.request.Request(
        url, data=json.dumps(data).encode() if data is not None else None,
        headers={"User-Agent": "PolySparQ++-Prediction-Board/2.0",
                 "Content-Type": "application/json", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.load(response)


def _prediction_news():
    """Fetch a small, cached multi-source crypto headline set and sentiment."""
    with _manual_news_lock:
        if _manual_news_cache[1] is not None and time.monotonic() - _manual_news_cache[0] < 300:
            return _manual_news_cache[1]
    import email.utils
    import urllib.request
    import xml.etree.ElementTree as ET
    from concurrent.futures import ThreadPoolExecutor, as_completed

    feeds = {
        "CoinDesk": "https://www.coindesk.com/arc/outboundfeeds/rss/",
        "Cointelegraph": "https://cointelegraph.com/rss",
        "Decrypt": "https://decrypt.co/feed",
    }
    positive = ("surge", "rally", "gain", "bull", "approval", "inflow", "record high",
                "adoption", "partnership", "upgrade", "breakout", "rate cut", "buy")
    negative = ("hack", "exploit", "crash", "plunge", "drop", "bear", "outflow",
                "lawsuit", "crackdown", "liquidation", "sell-off", "selloff", "ban")

    def fetch_one(source, url):
        req = urllib.request.Request(url, headers={"User-Agent": "PolySparQ++/1.0"})
        with urllib.request.urlopen(req, timeout=6) as response:
            root = ET.fromstring(response.read(1_500_000))
        out = []
        for item in root.findall(".//item")[:20]:
            title = (item.findtext("title") or "").strip()
            link = (item.findtext("link") or "").strip()
            pub = (item.findtext("pubDate") or "").strip()
            try:
                published = int(email.utils.parsedate_to_datetime(pub).timestamp())
            except Exception:
                published = int(time.time())
            low = title.lower()
            sentiment = sum(1 for word in positive if word in low) - sum(
                1 for word in negative if word in low)
            out.append({"source": source, "title": title, "url": link,
                        "published": published,
                        "sentiment": max(-2, min(2, sentiment))})
        return out

    stories = []
    with ThreadPoolExecutor(max_workers=3) as pool:
        futures = [pool.submit(fetch_one, source, url) for source, url in feeds.items()]
        for future in as_completed(futures):
            try:
                stories.extend(future.result())
            except Exception:
                pass
    dedup = {}
    for story in stories:
        dedup.setdefault(story["title"].lower(), story)
    stories = sorted(dedup.values(), key=lambda row: row["published"], reverse=True)[:24]
    result = {"stories": stories, "sources": sorted({s["source"] for s in stories})}
    with _manual_news_lock:
        _manual_news_cache[:] = [time.monotonic(), result]
    return result


def _asset_news_score(asset, stories):
    aliases = {
        "BTC": ("bitcoin", " btc "), "ETH": ("ethereum", "ether", " eth "),
        "SOL": ("solana", " sol "), "XRP": ("xrp", "ripple"),
    }[asset]
    general = ("crypto", "fed", "interest rate", "inflation", "risk asset", "dollar")
    now_s, weighted, weight = time.time(), 0.0, 0.0
    relevant = []
    for story in stories:
        title = f" {story['title'].lower()} "
        direct = any(alias in title for alias in aliases)
        broad = any(term in title for term in general)
        if not direct and not broad:
            continue
        age_h = max(0.0, (now_s - story["published"]) / 3600.0)
        recency = math.exp(-age_h / 8.0) * (1.0 if direct else 0.35)
        weighted += story["sentiment"] * recency
        weight += 2.0 * recency
        if direct:
            relevant.append(story)
    score = max(-1.0, min(1.0, weighted / weight)) if weight else 0.0
    return score, relevant[:3]


def _online_klines(asset):
    symbol = f"{asset}USDT"
    url = ("https://api.binance.com/api/v3/klines?" +
           urlencode({"symbol": symbol, "interval": "1m", "limit": 120}))
    rows = _public_json(url, timeout=6)
    return [{"ts": int(row[0]), "open": float(row[1]), "high": float(row[2]),
             "low": float(row[3]), "close": float(row[4]), "volume": float(row[5])}
            for row in rows]


def _online_polymarket(asset, horizon, open_epoch):
    label = "5m" if horizon == 300 else "15m"
    slug = f"{asset.lower()}-updown-{label}-{open_epoch}"
    market_url = f"https://polymarket.com/event/{slug}"
    url = "https://gamma-api.polymarket.com/markets?" + urlencode({"slug": slug})
    rows = _public_json(url, timeout=6)
    if not rows:
        return {"slug": slug, "up_probability": None, "condition_id": None,
                "market_url": market_url}
    market = rows[0]
    prices = market.get("outcomePrices") or []
    outcomes = market.get("outcomes") or []
    if isinstance(prices, str):
        try:
            prices = json.loads(prices)
        except Exception:
            prices = []
    if isinstance(outcomes, str):
        try:
            outcomes = json.loads(outcomes)
        except Exception:
            outcomes = []
    tokens = market.get("clobTokenIds") or []
    if isinstance(tokens, str):
        try:
            tokens = json.loads(tokens)
        except Exception:
            tokens = []
    up_index = next((i for i, outcome in enumerate(outcomes)
                     if str(outcome).strip().lower() == "up"), 0)
    up = float(prices[up_index]) if len(prices) > up_index else None
    outcome_prices = {str(outcome).strip().upper(): float(prices[i])
                      for i, outcome in enumerate(outcomes) if len(prices) > i}
    token_ids = {str(outcome).strip().upper(): str(tokens[i])
                 for i, outcome in enumerate(outcomes) if len(tokens) > i}
    return {"slug": slug, "up_probability": up,
            "down_probability": outcome_prices.get("DOWN"),
            "outcome_prices": outcome_prices, "token_ids": token_ids,
            "condition_id": market.get("conditionId"),
            "market_url": market_url,
            "question": market.get("question") or ""}


def _online_clob_price(token_id, side="SELL"):
    """CLOB best price. SELL returns the best ask (the price a buyer pays)."""
    url = "https://clob.polymarket.com/price?" + urlencode(
        {"token_id": token_id, "side": side})
    result = _public_json(url, timeout=3)
    return float(result["price"]) if result.get("price") is not None else None


def _technical_snapshot(rows):
    if len(rows) < 20:
        return None
    closes = [row["close"] for row in rows]
    volumes = [row["volume"] for row in rows]

    def ret(minutes):
        return 100.0 * (closes[-1] / closes[-minutes - 1] - 1.0) if len(closes) > minutes else 0.0

    def ema(period):
        alpha, value = 2.0 / (period + 1.0), closes[0]
        for price in closes[1:]:
            value = alpha * price + (1.0 - alpha) * value
        return value

    changes = [closes[i] - closes[i - 1] for i in range(1, len(closes))]
    last14 = changes[-14:]
    gain = sum(max(0.0, x) for x in last14) / 14.0
    loss = sum(max(0.0, -x) for x in last14) / 14.0
    rsi = 100.0 if loss == 0 else 100.0 - 100.0 / (1.0 + gain / loss)
    minute_returns = [100.0 * (closes[i] / closes[i - 1] - 1.0)
                      for i in range(max(1, len(closes) - 31), len(closes))]
    mean = sum(minute_returns) / len(minute_returns)
    vol = math.sqrt(sum((x - mean) ** 2 for x in minute_returns) / len(minute_returns))
    prior_volume = sum(volumes[-25:-5]) / max(1, len(volumes[-25:-5]))
    recent_volume = sum(volumes[-5:]) / 5.0
    return {"price": closes[-1], "r1": ret(1), "r3": ret(3), "r5": ret(5),
            "r15": ret(15), "ema_gap_pct": 100.0 * (ema(5) / ema(20) - 1.0),
            "rsi14": rsi, "vol_1m_pct": vol,
            "volume_ratio": recent_volume / prior_volume if prior_volume else 1.0,
            "last_candle_ts": rows[-1]["ts"]}


def _local_candle_baselines():
    rows = q("""
        WITH wanted(symbol) AS (VALUES ('BTCUSDT'),('ETHUSDT'),('SOLUSDT'),('XRPUSDT'))
        SELECT wanted.symbol, c.ts, c.close
        FROM wanted CROSS JOIN LATERAL (
          SELECT ts, close FROM vp.candles_1m c
          WHERE c.symbol=wanted.symbol ORDER BY ts DESC LIMIT 1440
        ) c ORDER BY wanted.symbol,c.ts
    """)
    grouped = {}
    for row in rows:
        grouped.setdefault(row["symbol"][:3] if row["symbol"] != "XRPUSDT" else "XRP", []).append(row)
    out = {}
    for asset in ("BTC", "ETH", "SOL", "XRP"):
        data = grouped.get(asset, [])
        returns = [100.0 * (float(data[i]["close"]) / float(data[i - 1]["close"]) - 1.0)
                   for i in range(1, len(data)) if float(data[i - 1]["close"]) > 0]
        mean = sum(returns) / len(returns) if returns else 0.0
        vol = math.sqrt(sum((x - mean) ** 2 for x in returns) / len(returns)) if returns else 0.0
        out[asset] = {"baseline_vol_1m_pct": vol,
                      "last_ts": data[-1]["ts"] if data else None,
                      "samples": len(data)}
    return out


def api_manual_predictions(force=False, after_version=None, wait_s=0):
    """Return a fresh snapshot, optionally waiting for the next producer update."""
    global _manual_prediction_service
    from prediction_board import PredictionService
    try:
        after_version = max(0, int(after_version)) if after_version is not None else None
    except (TypeError, ValueError, OverflowError):
        after_version = None
    try:
        wait_s = min(3.0, max(0.0, float(wait_s)))
    except (TypeError, ValueError, OverflowError):
        wait_s = 0.0
    with _manual_prediction_lock:
        if _manual_prediction_service is None:
            _manual_prediction_service = PredictionService(
                q, _public_json, ROOT, _prediction_news,
                gate_writer=_record_prediction_gate_current)
            _manual_prediction_service.start()
    return _manual_prediction_service.snapshot(force, after_version, wait_s)


def api_log_tail(instance=None, lines=300):
    """Tail a bot instance log file and return redacted lines (newest-first)."""
    if instance and not re.fullmatch(r'[\w-]+', instance):
        return {"error": "invalid instance", "lines": [], "total": 0}
    lines = max(1, min(1000, int(lines)))
    path = instance_log(instance) if instance else LOG_FILE
    raw = tail(path, lines)
    def redact(line):
        line = re.sub(r'(?i)(postgres(?:ql)?://[^\s:/]+:)[^@\s]+@', r'\1[REDACTED]@', line)
        return _polytron_redact(line)
    return {"lines": [redact(line) for line in raw], "path": os.path.basename(path), "total": len(raw)}


def api_session_reports():
    """List session report .md files newest-first."""
    out = []
    try:
        for fn in sorted(os.listdir(REPORTS_DIR), reverse=True)[:40]:
            if fn.endswith(".md"):
                p = os.path.join(REPORTS_DIR, fn)
                out.append({"file": fn,
                             "mtime": int(os.path.getmtime(p) * 1000),
                             "size": os.path.getsize(p)})
    except Exception:
        pass
    return {"reports": out}


def api_session_report_read(filename):
    if not re.match(r'^[\w._-]+\.md$', filename):
        return {"error": "invalid filename"}
    p = os.path.join(REPORTS_DIR, filename)
    if not os.path.exists(p):
        return {"error": "not found"}
    try:
        with open(p, encoding="utf-8") as f:
            return {"file": filename, "content": f.read(100_000)}
    except Exception as e:
        return {"error": str(e)}


def api_v2_live():
    """Live state for the v2 dashboard: per-bot snapshots (open positions),
    recent settlements and the raw order tape. Shapes owned by v2."""
    out = {"ok": True, "bots": [], "settles": [], "tape": []}
    try:
        rows = q("""SELECT DISTINCT ON (instance) instance, ts, deployed_usd,
                           equity_usd::float8, realized_pnl::float8, open_positions
                    FROM vp.positions_history
                    WHERE ts > now() - interval '3 minutes'
                    ORDER BY instance, ts DESC""") or []
        for r in rows:
            ops = r["open_positions"] or []
            if isinstance(ops, str):
                import json as _json
                try: ops = _json.loads(ops)
                except Exception: ops = []
            out["bots"].append({"instance": r["instance"], "ts": str(r["ts"]),
                                "deployed": float(r["deployed_usd"] or 0),
                                "equity": float(r["equity_usd"] or 0),
                                "realized": float(r["realized_pnl"] or 0),
                                "positions": ops})
    except Exception:
        pass
    try:
        rows = q("""SELECT o.instance, m.asset, f.price::float8, f.size_shares::float8, f.ts
                    FROM vp.orders o
                    JOIN vp.fills f ON f.client_order_id=o.client_order_id
                    LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
                    WHERE o.source='settle'
                    ORDER BY f.ts DESC LIMIT 25""") or []
        out["settles"] = [{"instance": r["instance"], "asset": r["asset"] or "?", "price": float(r["price"] or 0),
                           "shares": float(r["size_shares"] or 0), "ts": str(r["ts"]),
                           "win": (r["price"] or 0) > 0.5} for r in rows]
    except Exception:
        pass
    try:
        rows = q("""SELECT o.instance, o.source, o.status, o.execution_mode, o.ts_created,
                           f.price::float8, f.size_shares::float8
                    FROM vp.orders o
                    LEFT JOIN vp.fills f ON f.client_order_id=o.client_order_id
                    ORDER BY o.ts_created DESC LIMIT 30""") or []
        out["tape"] = [{"instance": r["instance"], "source": r["source"], "status": r["status"],
                        "mode": r["execution_mode"] or "taker", "ts": str(r["ts_created"]),
                        "price": float(r["price"]) if r["price"] is not None else None,
                        "shares": float(r["size_shares"]) if r["size_shares"] is not None else None} for r in rows]
    except Exception:
        pass
    return out


_analytics_lock = threading.Lock()
_analytics_cache = {}
_review_lock = threading.Lock()
_review_cache = {}


def api_v2_analytics(days=14, instance="", mode=""):
    """Realized exits, matched to scoped entry costs; cached across cockpit views."""
    from cockpit_analytics import build_analytics
    # Fractional days allow short windows (e.g. 15 minutes) in the Win/Loss view.
    days = max(0.0, min(3650.0, float(days)))
    now_ms = int(time.time() * 1000)
    with _analytics_lock:
        cache_key = (days, instance, mode)
        cached = _analytics_cache.get(cache_key)
        if cached and now_ms - cached[0] < 15_000:
            return cached[1]
        fills = q("""SELECT f.id, f.ts, f.price, f.size_shares, f.fee_usd,
                    o.client_order_id::text, o.mode::text, o.instance,
                    o.session_id::text, o.condition_id, o.token_id,
                    o.direction::text, o.source, o.side_label::text,
                    o.execution_mode, m.asset, m.horizon_seconds
             FROM vp.fills f JOIN vp.orders o USING(client_order_id)
             LEFT JOIN vp.pm_markets m USING(condition_id)
             ORDER BY f.ts, f.id""")
        fills = [r for r in fills if (not instance or r.get("instance") == instance) and (not mode or r.get("mode") == mode)]
        result = build_analytics(fills, now_ms-days*86_400_000 if days else 0, now_ms)
        result["scope"] = instance or "ALL BOTS"
        _analytics_cache[cache_key] = (now_ms, result)
        return result


def api_v4_loss_review(days=7, instance="", mode="paper", cohort_since_ms=None):
    """Complete exposure economics, scoped to the selected bot and mode."""
    from cockpit_review import performance_review, prediction_calibration
    days = max(0.0, min(3650.0, float(days)))
    if mode not in ("paper", "live", ""):
        raise ValueError("Invalid review mode")
    if cohort_since_ms is not None:
        cohort_since_ms = int(cohort_since_ms)
        if cohort_since_ms < 1_500_000_000_000 or cohort_since_ms > int(time.time()*1000):
            raise ValueError("Invalid cohort timestamp")
    now_ms = int(time.time()*1000)
    key = (days, instance, mode, cohort_since_ms)
    with _review_lock:
        cached = _review_cache.get(key)
        if cached and now_ms-cached[0] < 15_000:
            return cached[1]
        rows = q("""SELECT f.id, f.ts, f.price, f.size_shares, f.fee_usd,
                    o.client_order_id::text, o.mode::text, o.instance,
                    o.session_id::text, o.condition_id, o.token_id,
                    o.direction::text, o.source, o.side_label::text,
                    o.execution_mode, m.asset, m.horizon_seconds
              FROM vp.fills f JOIN vp.orders o USING(client_order_id)
              LEFT JOIN vp.pm_markets m USING(condition_id)
              WHERE (%s='' OR o.instance=%s) AND (%s='' OR o.mode::text=%s)
              ORDER BY f.ts, f.id""", (instance, instance, mode, mode))
        result = performance_review(rows, now_ms-days*86_400_000 if days else 0,
                                    now_ms, cohort_since_ms)
        candidates = result.pop('model_candidates')
        payloads = []
        if candidates:
            ids = list({c['order_id'] for c in candidates})
            payloads = [r['payload'] for r in q("""SELECT payload FROM vp.signals
                 WHERE model_version LIKE 'WPRED%%'
                   AND payload->>'client_order_id'=ANY(%s)""", (ids,))]
        result['model_calibration'] = prediction_calibration(candidates, payloads)
        result["scope"] = instance or "ALL BOTS"
        result["mode_scope"] = mode or "ALL MODES"
        _review_cache.clear()
        _review_cache[key] = (now_ms, result)
        return result


# ══════════════════════════════════════════════════════════════════════
# V3 COCKPIT — knob registry, validation+audit writes, live endpoints
# ══════════════════════════════════════════════════════════════════════

_V3_GROUPS = ["Sizing & Exposure", "Oracle & Entry Gates", "Confidence & Trend",
              "Autopilot & Timing", "Order Execution", "Predictor & Digital Fair Value",
              "Momentum Scalp V2/V3", "WPRED Checkpoints", "Adaptive Entry",
              "Exit & Settlement", "Endgame", "Fees & Safety", "Advanced Admin"]

def _k(group, key, section, label, unit="", typ="num", lo=None, hi=None, hint="", enum=None, step=None):
    return {"g": group, "key": key, "sec": section, "label": label, "unit": unit,
            "type": typ, "lo": lo, "hi": hi, "hint": hint, "enum": enum, "step": step}

_V3_REGISTRY = [
    # Sizing & Exposure
    _k("Sizing & Exposure","per_click_size_usd","strategy.hot","Per Click Size (USD)","USD",lo=1,hi=10000,hint="Size per entry click. Larger = fewer clicks, higher impact.",step=1),
    _k("Sizing & Exposure","min_order_usd","risk","Minimum Order (USD)","USD",lo=0.5,hi=1000,hint="Must be >= exchange minimum.",step=0.5),
    _k("Sizing & Exposure","min_size_usd","strategy.hot","Minimum Size (USD)","USD",lo=0.5,hi=1000,hint="Total position must be >= this value.",step=0.5),
    _k("Sizing & Exposure","max_size_usd","strategy.hot","Maximum Size (USD)","USD",lo=1,hi=100000,hint="Caps single position size.",step=1),
    _k("Sizing & Exposure","max_per_window_usd","strategy.hot","Max Per Window (USD)","USD",lo=1,hi=500000,hint="Limits total buys per time window.",step=1),
    _k("Sizing & Exposure","max_per_window_usd","risk","Max Per Window (USD) [risk]","USD",lo=1,hi=500000,hint="Risk-layer per-window cap (executor enforces this one).",step=1),
    _k("Sizing & Exposure","max_order_usd","risk","Maximum Order (USD)","USD",lo=1,hi=50000,hint="Highest single order size.",step=1),
    _k("Sizing & Exposure","max_deployed_usd","risk","Max Deployed (USD)","USD",lo=1,hi=1000000,hint="Total capital allowed in positions.",step=1),
    _k("Sizing & Exposure","equity_sizing_enabled","strategy.hot","Equity Sizing Enabled","",typ="bool",hint="Use % of equity for position sizing."),
    _k("Sizing & Exposure","equity_order_pct","strategy.hot","Equity Order %","%",lo=0.1,hi=100,hint="Size each order as % of equity.",step=0.1),
    _k("Sizing & Exposure","equity_max_deployed_pct","strategy.hot","Equity Max Deployed %","%",lo=0,hi=100,hint="Maximum capital as % of equity. 0 disables.",step=0.1),
    _k("Sizing & Exposure","hard_share_price_ceil","strategy.hot","Hard Share Price Ceil","",lo=0.3,hi=0.99,step=0.01,hint="Absolute safety ceiling on any entry price."),
    # Oracle & Entry Gates
    _k("Oracle & Entry Gates","min_entry_share_price","strategy.hot","Minimum Entry Share Price","",lo=0.001,hi=1,step=0.01,hint="Reject entries below this price."),
    _k("Oracle & Entry Gates","max_entry_share_price","strategy.hot","Maximum Entry Share Price","",lo=0.01,hi=1,step=0.01,hint="Reject entries above this price."),
    _k("Oracle & Entry Gates","ideal_entry_share_price","strategy.hot","Ideal Entry Share Price","",lo=0.01,hi=1,step=0.01,hint="Target entry price for scoring."),
    _k("Oracle & Entry Gates","oracle_max_token_price","strategy.hot","Oracle Max Token Price","",lo=0.01,hi=1,step=0.01,hint="Hard cap from oracle data."),
    _k("Oracle & Entry Gates","min_oracle_delta_pct","strategy.hot","Min Oracle Delta %","%",lo=0.001,hi=5,step=0.001,hint="Minimum oracle move to confirm an entry. Selectivity IS the edge."),
    _k("Oracle & Entry Gates","coinbase_lead_mode","strategy.hot","Coinbase Lead Mode","",typ="bool",hint="Use Coinbase as leading signal."),
    _k("Oracle & Entry Gates","coinbase_veto_mode","strategy.hot","Coinbase Veto Mode","",typ="bool",hint="Block entries if Coinbase vetoes."),
    _k("Oracle & Entry Gates","coinbase_lead_min_delta_pct","strategy.hot","Coinbase Lead Min Delta %","%",lo=0.001,hi=5,step=0.001,hint="CB move required to lead while oracle is flat."),
    _k("Oracle & Entry Gates","blocked_assets","strategy.hot","Blocked Assets","list",typ="list",hint="Assets never traded, e.g. [\"SOL\",\"XRP\"]"),
    _k("Oracle & Entry Gates","max_entry_spread_pct","strategy.hot","Max Entry Spread %","%",lo=0.1,hi=50,step=0.1,hint="Reject entries when book spread is wider."),
    # Confidence & Trend
    _k("Confidence & Trend","autopilot_conf_floor","strategy.hot","Autopilot Confidence Floor","",lo=0,hi=1,step=0.01,hint="Minimum signal confidence to trade."),
    _k("Confidence & Trend","trend_filter_enabled","strategy.hot","Trend Filter Enabled","",typ="bool",hint="Refuse entries against recent trend."),
    _k("Confidence & Trend","trend_block_pct","strategy.hot","Trend Block %","%",lo=0,hi=5,step=0.01,hint="Trend magnitude that blocks counter-entries."),
    _k("Confidence & Trend","trend_lookback_secs","strategy.hot","Trend Lookback (s)","s",lo=30,hi=3600,step=10,hint="Trend measurement window."),
    _k("Confidence & Trend","oracle_require_pred_agree","strategy.hot","Require Predictor Agree","",typ="bool",hint="Oracle path needs predictor agreement."),
    _k("Confidence & Trend","pred_agree_margin","strategy.hot","Predictor Agree Margin","",lo=0,hi=0.2,step=0.005,hint="How far predictor must lean to agree."),
    _k("Confidence & Trend","pred_agree_min_conf","strategy.hot","Predictor Agree Min Conf","",lo=0,hi=1,step=0.01,hint="Predictor confidence needed to agree."),
    # Autopilot & Timing
    _k("Autopilot & Timing","autopilot_on_start","strategy.hot","Autopilot On Start","",typ="bool",hint="Arm trading on startup. (Takes effect on restart.)"),
    _k("Autopilot & Timing","autopilot_startup_delay_secs","strategy.hot","Startup Delay (s)","s",lo=0,hi=600,step=5,hint="Warm-up before first sweep."),
    _k("Autopilot & Timing","autopilot_throttle_ms","strategy.hot","Sweep Throttle (ms)","ms",lo=100,hi=10000,step=50,hint="Min ms between sweeps."),
    _k("Autopilot & Timing","autopilot_max_per_sweep","strategy.hot","Max Per Sweep","",lo=1,hi=20,step=1,hint="Max entries submitted per sweep."),
    _k("Autopilot & Timing","max_entries_per_window","strategy.hot","Max Entries Per Window","",lo=1,hi=20,step=1,hint="Per-market entry limit."),
    _k("Autopilot & Timing","max_same_side_positions","strategy.hot","Max Same-Side Positions","",lo=1,hi=20,step=1,hint="Correlation cap on one side."),
    _k("Autopilot & Timing","min_secs_remaining_5m","strategy.hot","5m Min Secs Left","s",lo=0,hi=300,step=5,hint="No 5m entries before this point."),
    _k("Autopilot & Timing","max_secs_remaining_5m","strategy.hot","5m Max Secs Left","s",lo=10,hi=300,step=5,hint="No 5m entries after this point."),
    _k("Autopilot & Timing","min_secs_remaining_15m","strategy.hot","15m Min Secs Left","s",lo=0,hi=900,step=5,hint="15m window entry floor."),
    _k("Autopilot & Timing","max_secs_remaining_15m","strategy.hot","15m Max Secs Left","s",lo=10,hi=900,step=5,hint="15m window entry ceiling."),
    # Order Execution
    _k("Order Execution","entry_order_mode","strategy.hot","Entry Order Mode","",typ="enum",enum=["maker"],hint="Fixed policy: every order is a post-only LIMIT-MAKER quote."),
    _k("Order Execution","order_cooldown_ms","strategy.hot","Order Cooldown (ms)","ms",lo=0,hi=60000,step=100,hint="Per-market cooldown between orders."),
    _k("Order Execution","maker_improve_ticks","strategy.hot","Maker Improve Ticks","ticks",lo=0,hi=10,step=1,hint="Ticks above best bid for post-only quotes."),
    _k("Order Execution","maker_reprice_ms","strategy.hot","Maker Reprice (ms)","ms",lo=100,hi=10000,step=50,hint="Quote refresh cadence."),
    _k("Order Execution","maker_max_age_ms","strategy.hot","Maker Max Age (ms)","ms",lo=0,hi=60000,step=50,hint="Entry quote lifetime; expiry cancels without taker fallback."),
    _k("Order Execution","maker_cancel_before_close_ms","strategy.hot","Maker Cancel Before Close (ms)","ms",lo=0,hi=60000,step=250,hint="Pull quotes this long before window close."),
    # Predictor & Digital FV
    _k("Predictor & Digital Fair Value","prediction_page_gate_enabled","strategy.hot","Prediction Page Gate","",typ="bool",hint="Blocks automatic buys unless the current Predictions page forecast is fresh, agrees with the order side, and meets the configured side-chance floor."),
    _k("Predictor & Digital Fair Value","prediction_page_min_chance_pct","strategy.hot","Prediction Page Min Side Chance (%)","percentage points",lo=0,hi=100,step=1,hint="Minimum UP or DOWN probability shown by the current Predictions page before an automatic buy can be posted."),
    _k("Predictor & Digital Fair Value","predictor_mode","strategy.hot","Predictor Mode","",typ="bool",hint="Main prediction-model entry path."),
    _k("Predictor & Digital Fair Value","min_predictor_edge","strategy.hot","Min Predictor Edge","",lo=0,hi=0.5,step=0.005,hint="Fee-inclusive edge required."),
    _k("Predictor & Digital Fair Value","min_predictor_conf","strategy.hot","Min Predictor Confidence","",lo=0,hi=1,step=0.01,hint="Model confidence floor."),
    _k("Predictor & Digital Fair Value","predictor_max_token_price","strategy.hot","Predictor Max Token Price","",lo=0.05,hi=1,step=0.01,hint="Cap on model-path entry price."),
    _k("Predictor & Digital Fair Value","predictor_refresh_ms","strategy.hot","Predictor Refresh (ms)","ms",lo=250,hi=10000,step=250,hint="Ensemble recompute cadence."),
    _k("Predictor & Digital Fair Value","digital_fv_mode","strategy.hot","Digital Fair Value Mode","",typ="bool",hint="N(d2)-style fair value path."),
    _k("Predictor & Digital Fair Value","digital_fv_min_edge_pct","strategy.hot","Digital FV Min Edge %","%",lo=0,hi=20,step=0.1,hint="FV edge required over cost."),
    _k("Predictor & Digital Fair Value","digital_fv_jump_z","strategy.hot","Digital FV Jump Z","",lo=0,hi=10,step=0.1,hint="Oracle jump z-score veto. 0 disables."),
    # Momentum Scalp
    _k("Momentum Scalp V2/V3","scalp_v3_enabled","strategy.hot","Scalp V3 Enabled","",typ="bool",hint="Momentum scalper (fallback path)."),
    _k("Momentum Scalp V2/V3","scalp_v3_require_oracle_agree","strategy.hot","Scalp Require Oracle Agree","",typ="bool",hint="Silent killer when true — see journal."),
    _k("Momentum Scalp V2/V3","scalp_v3_velocity_thresh","strategy.hot","Velocity Threshold","",lo=0,hi=0.05,step=0.0005,hint="Min price velocity to trigger."),
    _k("Momentum Scalp V2/V3","scalp_v3_min_confidence","strategy.hot","Scalp Min Confidence","",lo=0,hi=1,step=0.01,hint="Scalp signal confidence floor."),
    _k("Momentum Scalp V2/V3","scalp_v3_min_price","strategy.hot","Scalp Min Price","",lo=0.05,hi=0.99,step=0.01,hint="Scalp entry band floor."),
    _k("Momentum Scalp V2/V3","scalp_v3_max_price","strategy.hot","Scalp Max Price","",lo=0.05,hi=0.99,step=0.01,hint="Scalp entry band ceiling."),
    # WPRED
    _k("WPRED Checkpoints","window_predictor_mode","strategy.hot","WPRED Mode","",typ="bool",hint="Checkpoint predictor entries."),
    _k("WPRED Checkpoints","window_predictor_fractions","strategy.hot","Checkpoint Fractions","list",typ="list",hint="Window fractions to fire at, e.g. [0.6]"),
    _k("WPRED Checkpoints","window_predictor_amounts_usd","strategy.hot","Checkpoint Amounts (USD)","list",typ="list",hint="USD per checkpoint, e.g. [30.0]"),
    _k("WPRED Checkpoints","window_predictor_price_min","strategy.hot","WPRED Price Min","",lo=0.01,hi=1,step=0.01,hint="Entry ask floor."),
    _k("WPRED Checkpoints","window_predictor_price_max","strategy.hot","WPRED Price Max","",lo=0.01,hi=1,step=0.01,hint="Entry ask ceiling."),
    _k("WPRED Checkpoints","window_predictor_require_unanimous","strategy.hot","Require Unanimous Vote","",typ="bool",hint="All three voters must agree."),
    _k("WPRED Checkpoints","window_predictor_min_net_edge","strategy.hot","WPRED Min Net Edge","",lo=0,hi=0.2,step=0.005,hint="Model fair must clear ask+fee."),
    _k("WPRED Checkpoints","window_predictor_min_win_return_pct","strategy.hot","WPRED Min Win Return %","%",lo=0,hi=100,step=1,hint="Payout upside required if it wins."),
    _k("WPRED Checkpoints","window_predictor_calibration_min_samples","strategy.hot","Calibration Min Samples","",lo=0,hi=500,step=5,hint="Settled samples before calibration gate bites. 0=off."),
    # Adaptive
    _k("Adaptive Entry","adaptive_mode","strategy.hot","Adaptive Mode","",typ="bool",hint="Confidence-trajectory entries (telemetry)."),
    _k("Adaptive Entry","adaptive_target_conf","strategy.hot","Adaptive Target Conf","",lo=0,hi=1,step=0.01,hint="Confidence level to trigger entry."),
    _k("Adaptive Entry","adaptive_min_secs_left","strategy.hot","Adaptive Min Secs Left","s",lo=0,hi=3600,step=10,hint="Earliest adaptive entry."),
    _k("Adaptive Entry","adaptive_max_secs_left","strategy.hot","Adaptive Max Secs Left","s",lo=10,hi=7200,step=10,hint="Latest adaptive entry."),
    # Exit & Settlement
    _k("Exit & Settlement","profit_take_mode","strategy.hot","Profit Take Mode","",typ="bool",hint="Take profit at target."),
    _k("Exit & Settlement","profit_take_min_pct","strategy.hot","Profit Take Min %","%",lo=0,hi=500,step=1,hint="Minimum gain to take."),
    _k("Exit & Settlement","stop_loss_mode","strategy.hot","Stop Loss Mode","",typ="bool",hint="Fixed stop. Historic note: stops underperformed settlement."),
    _k("Exit & Settlement","stop_loss_pct","strategy.hot","Stop Loss %","%",lo=1,hi=100,step=1,hint="Stop distance from entry."),
    _k("Exit & Settlement","stop_loss_price","strategy.hot","Stop Loss Price","",lo=0,hi=1,step=0.01,hint="Absolute stop price. 0 = use percent."),
    _k("Exit & Settlement","timed_exit_secs","strategy.hot","Timed Exit (s)","s",lo=0,hi=600,step=5,hint="Cut losses N secs before close. 0=off."),
    _k("Exit & Settlement","smart_exit_mode","strategy.hot","Smart Exit Mode","",typ="bool",hint="Conviction-vote gated stops."),
    _k("Exit & Settlement","conviction_min_delta_pct","strategy.hot","Conviction Min Delta %","%",lo=0,hi=5,step=0.005,hint="Per-vote conviction threshold."),
    _k("Exit & Settlement","smart_stop_max_votes","strategy.hot","Smart Stop Max Votes","",lo=0,hi=3,step=1,hint="Stops only when votes <= this."),
    _k("Exit & Settlement","hard_stop_pct","strategy.hot","Hard Stop %","%",lo=10,hi=100,step=1,hint="Catastrophe floor — always fires."),
    _k("Exit & Settlement","ride_to_settle_secs","strategy.hot","Ride To Settle (s)","s",lo=0,hi=600,step=10,hint="Winners this close to settle skip profit-take."),
    _k("Exit & Settlement","oracle_reversal_exit","strategy.hot","Oracle Reversal Exit","",typ="bool",hint="Exit when oracle re-crosses against us."),
    _k("Exit & Settlement","oracle_panic_exit","strategy.hot","Oracle Panic Exit","",typ="bool",hint="Exit on adverse oracle spike."),
    _k("Exit & Settlement","oracle_panic_delta_pct","strategy.hot","Panic Delta %","%",lo=0.001,hi=5,step=0.001,hint="Adverse move that triggers panic."),
    _k("Exit & Settlement","momentum_flip_exit","strategy.hot","Momentum Flip Exit","",typ="bool",hint="Exit when predictor flips sides."),
    # Endgame
    _k("Endgame","endgame_mode","strategy.hot","Endgame Mode","",typ="bool",hint="Late-window favourite sniper."),
    _k("Endgame","endgame_secs_left","strategy.hot","Endgame Window (s)","s",lo=10,hi=300,step=5,hint="Zone: last N seconds of window."),
    _k("Endgame","endgame_min_profit_pct","strategy.hot","Min Net Profit %","%",lo=0,hi=100,step=0.5,hint="Fee-covered return required. Keep >= 12: pays pennies below."),
    _k("Endgame","endgame_min_price","strategy.hot","Endgame Min Price","",lo=0.05,hi=1,step=0.01,hint="Favourite band floor."),
    _k("Endgame","endgame_max_price","strategy.hot","Endgame Max Price","",lo=0.05,hi=1,step=0.01,hint="Favourite band ceiling. 0.80+ = pays pennies, risks dollars."),
    _k("Endgame","endgame_min_oracle_pct","strategy.hot","Endgame Min Oracle %","%",lo=0,hi=5,step=0.005,hint="Direction confirmation only."),
    # Fees & Safety
    _k("Fees & Safety","taker_fee_rate","strategy.hot","Taker Fee Rate","",lo=0,hi=0.2,step=0.001,hint="Venue taker fee coefficient."),
    _k("Fees & Safety","fee_exponent","strategy.hot","Fee Exponent","",lo=0,hi=4,step=1,hint="Fee curve exponent (p(1-p))^e."),
    _k("Fees & Safety","consecutive_loss_pause","strategy.hot","Consecutive Loss Pause","",lo=0,hi=50,step=1,hint="Pause entries after N straight losses. 0=off."),
    _k("Fees & Safety","consecutive_pause_secs","strategy.hot","Loss Pause Duration (s)","s",lo=0,hi=86400,step=30,hint="How long the pause lasts."),
    _k("Fees & Safety","settlement_grace_ms","strategy.hot","Settlement Grace (ms)","ms",lo=0,hi=120000,step=1000,hint="Wait after window close before settling."),
    # Advanced Admin
    _k("Advanced Admin","config_hot_reload_interval_ms","runtime","Config Hot Reload (ms)","ms",lo=1000,hi=60000,step=500,hint="Config re-read cadence."),
    _k("Advanced Admin","cockpit_command_poll_ms","runtime","Cockpit Command Poll (ms)","ms",lo=250,hi=10000,step=250,hint="Command polling cadence."),
    _k("Advanced Admin","log_level","runtime","Log Level","",typ="enum",enum=["trace","debug","info","warn","error"],hint="Verbosity."),
]

_V3_BY_GROUP = {}
for _rk in _V3_REGISTRY:
    _V3_BY_GROUP.setdefault(_rk["g"], []).append(_rk)


def _v3_toml_get(parsed, sec, key):
    tbl = parsed.get(sec, {})
    if isinstance(tbl, dict):
        return tbl.get(key)
    return None


# group assignment for the V3 editor sidebar (mockup order)
_V3_GROUP_ORDER = ["Sizing & Exposure", "Oracle & Entry Gates", "Confidence & Trend",
                   "Autopilot & Timing", "Order Execution", "Predictor & Digital Fair Value",
                   "Momentum Scalp V2/V3", "WPRED Checkpoints", "Adaptive Entry",
                   "Exit & Settlement", "Endgame", "Fees & Safety", "Advanced Admin"]

_V3_GROUP_HINTS = {
    "Sizing & Exposure": "Position sizing, capital allocation and exposure limits.",
    "Oracle & Entry Gates": "Price validation, oracle sources, and entry eligibility gates.",
    "Confidence & Trend": "Signal quality gauntlet: confidence, confirmation, trend, persistence.",
    "Autopilot & Timing": "Cadence, blast radius and per-horizon entry windows.",
    "Order Execution": "Taker/maker/hybrid routing, book quality gates and retry behavior.",
    "Predictor & Digital Fair Value": "Model edge, confidence and fair-value controls.",
    "Momentum Scalp V2/V3": "Short-horizon momentum and microstructure scaling.",
    "WPRED Checkpoints": "Windowed predictor execution and position sizing.",
    "Adaptive Entry": "Confidence-trajectory entries rather than fixed checkpoint timing.",
    "Exit & Settlement": "Protects capital: exits, loss control and settlement timing.",
    "Endgame": "Late-window rules and final guards.",
    "Fees & Safety": "Trading frictions, daily limits and protective controls.",
    "Advanced Admin": "Runtime, infrastructure and operational controls.",
}

def _v3_group_for(key):
    if key.startswith("recycler_"): return "Order Execution"
    if key.startswith("scalp_v2_") or key in ("momentum_scalp_enabled",) or key.startswith("scalp_v3_"):
        return "Momentum Scalp V2/V3"
    if key.startswith("window_predictor_"):
        return "WPRED Checkpoints"
    if key.startswith("adaptive_"):
        return "Adaptive Entry"
    if key.startswith("prediction_page_"):
        return "Predictor & Digital Fair Value"
    if key.startswith("digital_fv_") or key.startswith("predictor_") or key in ("min_predictor_edge","min_predictor_conf"):
        return "Predictor & Digital Fair Value"
    if key.startswith("maker_") or key in ("entry_order_mode","manual_buy_slippage_pct","manual_buy_slippage_extra","token_quarantine_payload_ms","token_quarantine_no_match_ms","min_position_shares","dust_position_shares","price_history_max_points","price_velocity_lookback_ms","sparkline_points","tick_buffer_max_age_us","tick_buffer_max_entries","max_book_age_ms"):
        return "Order Execution"
    if key.startswith("min_secs_remaining") or key.startswith("max_secs_remaining") or key.startswith("autopilot_") or key in ("max_entries_per_window","max_same_side_positions","consecutive_loss_pause","consecutive_pause_secs","order_cooldown_ms","reversal_cooldown_secs","max_reversals_per_window","entry_persist_gate","entry_persist_ms"):
        return "Autopilot & Timing"
    if key in ("profit_take_mode","profit_take_min_pct","profit_take_price_thresh","profit_take_momentum_max","stop_loss_mode","stop_loss_pct","stop_loss_price","stop_loss_reverse","stop_loss_cooldown_secs","stop_loss_min_hold_secs","smart_exit_mode","smart_stop_max_votes","hard_stop_pct","ride_to_settle_secs","timed_exit_secs","timed_exit_min_bid","timed_exit_throttle_ms","oracle_reversal_exit","oracle_reversal_min_hold_secs","oracle_panic_exit","oracle_panic_delta_pct","oracle_panic_min_hold_secs","momentum_flip_exit","momentum_flip_margin","momentum_flip_min_conf","momentum_flip_reverse","momentum_flip_min_secs","momentum_flip_min_hold_ms","reverse_min_secs_left","reverse_slippage_pct","reverse_slippage_extra","settlement_grace_ms"):
        return "Exit & Settlement"
    if key.startswith("endgame_"):
        return "Endgame"
    if key in ("taker_fee_rate","fee_exponent","daily_loss_kill_pct","daily_kill_check_interval_ms","daily_kill_grace_secs") or key.startswith("fee"):
        return "Fees & Safety"
    if key in ("oracle_lag_mode","min_oracle_delta_pct","coinbase_lead_mode","coinbase_veto_mode","coinbase_lead_min_delta_pct","min_entry_share_price","max_entry_share_price","ideal_entry_share_price","oracle_max_token_price","hard_share_price_ceil","blocked_assets","max_gap_btc_usd","max_gap_eth_usd","oracle_max_token_price"):
        return "Oracle & Entry Gates"
    if key in ("autopilot_conf_floor","binance_book_confirm","binance_book_imb_min","multi_source_vote","multi_source_quorum","oracle_require_pred_agree","pred_agree_margin","pred_agree_min_conf","conviction_min_delta_pct","trend_filter_enabled","trend_filter_windows","trend_filter_strength","trend_block_pct","trend_lookback_secs","entry_early_edge_mult","entry_mid_edge_mult","entry_late_edge_mult","midcycle_mode","midcycle_trigger_secs","midcycle_min_distance_bps","midcycle_max_share_price","midcycle_window_s","book_imb_filter","book_imb_min","arbitrage_enabled","arbitrage_max_total","arbitrage_min_profit","kelly_fraction"):
        return "Confidence & Trend"
    if key in ("mode","log_level","journal_dir","instance_label","config_hot_reload_interval_ms","cockpit_command_poll_ms","wallet_balance_refresh_ms","live_heartbeat_ms","position_snapshot_interval_ms") or key.startswith(("universe_","market_prune","closed_market","book_health","rest_book","poll_seconds","rtds_","clob_","gamma_","log_","disk_","candle_store_max","trade_flow_max","features_","pm_trades_enabled","book_snapshots_enabled","oracle_ticks_enabled","binance_ticks_enabled","coinbase_ticks_enabled","window_events_enabled","features_write_interval_ms","fps_","truecolor","confirm_default","clob_min_shares_fallback","wallet_balance_warn_above","target_price","neg_risk","signature_type","starting_balance_usd","builder_code","proxy_address","signer_address","llm")):
        return "Advanced Admin"
    return "Sizing & Exposure"


def _v3_pretty(key):
    words = key.replace("_", " ").split()
    small = {"pct","usd","ms","secs","s","of","the","and","for","to","per","min","max"}
    out = [w if (w in small and i>0) else w.upper() if len(w)<=3 and w not in ("pct","usd","fee") else w.capitalize() for i,w in enumerate(words)]
    return " ".join(out)


def _v3_knobs_impl(instance):
    """Discover catalog keys present in the instance TOML.

    api_v3_knobs adds engine capability status before controls reach the UI.
    """
    if not re.match(r'^[\w-]+$', instance):
        return {"error": "invalid instance"}
    path = os.path.join(CONFIG_DIR, f"polypulse-{instance}.toml")
    if not os.path.exists(path):
        return {"error": "config not found"}
    try:
        with open(path, "rb") as f:
            parsed = tomllib.load(f)
    except Exception as e:
        return {"error": f"toml parse: {e}"}
    # index every scalar present in the file -> (section, key, value)
    # [strategy.hot] is nested two levels: walk recursively, recording the
    # dotted section path (e.g. "strategy.hot") — the flat one-level walk
    # silently hid every strategy knob from the Editor (fixed 2026-09-23).
    present = {}
    LIST_OK = ("blocked_assets","window_predictor_fractions","window_predictor_amounts_usd",
               "window_predictor_assets","window_predictor_horizons","window_predictor_pairs","streams")
    def walk(prefix, tbl):
        for k, v in tbl.items():
            path = f"{prefix}.{k}" if prefix else k
            if isinstance(v, dict):
                walk(path, v)
            elif isinstance(v, list) and k not in LIST_OK:
                continue
            elif k in COCKPIT_TUNABLE and prefix in ("strategy.hot", "risk"):
                present[(prefix, k)] = v
    walk("", parsed)
    from cockpit_fleet import EXECUTION_DEFAULTS
    for key, value in EXECUTION_DEFAULTS.items():
        present.setdefault(("strategy.hot", key), value)
    cur_labels = {rk["key"]: rk for rk in _V3_REGISTRY}
    groups = {g: [] for g in _V3_GROUP_ORDER}
    for (sec, key), val in sorted(present.items()):
        spec = COCKPIT_TUNABLE.get(key)
        g = _v3_group_for(key)
        curated = cur_labels.get(key, {})
        raw_typ = spec[0] if spec else "str"
        typ = "list" if raw_typ in ("assets", "pairs", "floats", "ints", "strings") else raw_typ
        lo, hi = (spec[1], spec[2]) if spec and len(spec) >= 3 else (None, None)
        if typ == "int":
            lo = int(lo) if lo is not None else None; hi = int(hi) if hi is not None else None
        groups[g].append({
            "key": f"{sec}.{key}", "bare": key, "label": curated.get("label") or _v3_pretty(key),
            "value": val, "unit": curated.get("unit", ""), "type": typ,
            "lo": lo, "hi": hi, "enum": spec[1] if spec and raw_typ == "enum" else curated.get("enum"),
            "step": curated.get("step") or (1 if typ == "int" else None),
            "hint": ("BTC 15m cycle strategy. Entry also requires a fresh settlement-reference estimate with the configured minimum edge. Late exits post maker-only limit orders and may remain unfilled until settlement." if key.startswith("recycler_") else curated.get("hint", "")), "section": sec, "set": True,
            "source": "instance config" if key in parsed.get("strategy",{}).get("hot",{}) or key in parsed.get("risk",{}) else "engine default", "active": True,
        })
    ordered = [{"group": g, "desc": _V3_GROUP_HINTS[g], "controls": len(groups[g]), "rows": groups[g]}
               for g in _V3_GROUP_ORDER if groups[g]]
    with open(path, "rb") as f:
        config_hash = hashlib.sha256(f.read()).hexdigest()[:12]
    return {"instance": instance, "groups": ordered, "config_hash": config_hash,
            "total": sum(x["controls"] for x in ordered),
            "catalog_size": len(COCKPIT_TUNABLE)}


def api_v3_knobs(instance):
    result = _v3_knobs_impl(instance)
    if "error" in result:
        return result
    registry = {x["feature_id"]: x for x in api_v3_capabilities()["features"]}
    for group in result["groups"]:
        for row in group["rows"]:
            cap = registry.get(row["key"])
            row["capability_status"] = cap["status"] if cap else "UNSUPPORTED BY CURRENT ENGINE"
            row["active"] = bool(cap and cap["backend_supported"] and cap["frontend_supported"])
            row["hot_reload"] = bool(cap and cap["hot_reload"])
            row["restart_required"] = bool(cap and cap["restart_required"])
    return result


def api_v3_audit(instance, limit=5):
    if not re.match(r'^[\w-]+$', instance):
        return {"records": []}
    path = os.path.join(ROOT, "logs", "config_audit.jsonl")
    recs = []
    try:
        with open(path) as f:
            for ln in f:
                try:
                    r = json.loads(ln)
                    if r.get("instance") == instance:
                        recs.append(r)
                except Exception:
                    pass
    except FileNotFoundError:
        pass
    return {"records": recs[-limit:][::-1]}


_config_write_lock = threading.RLock()


def api_v3_config_write(body):
    with _config_write_lock:
        return _api_v3_config_write_locked(body)


def _api_v3_config_write_locked(body):
    """Validate a complete batch against the explicit engine-backed catalog."""
    import tomllib as _tm
    instance = str(body.get("instance") or "")
    changes = body.get("changes", [])
    reason = _polytron_redact(str(body.get("reason", ""))[:200])
    ticket = _polytron_redact(str(body.get("ticket", ""))[:60])
    if not re.match(r'^[\w-]+$', instance):
        return 400, {"error": "invalid instance"}
    if not isinstance(changes, list) or not 1 <= len(changes) <= 100 or \
       any(not isinstance(change, dict) for change in changes):
        return 400, {"error": "changes must be a list of 1–100 objects"}
    path = os.path.join(CONFIG_DIR, f"polypulse-{instance}.toml")
    if not os.path.exists(path):
        return 404, {"error": "config not found"}
    try:
        with open(path, "rb") as f:
            parsed = _tm.load(f)
    except Exception as e:
        return 500, {"error": f"toml parse: {e}"}
    cur = {}
    bare_map = {}
    def _walk2(prefix, tbl):
        for k, v in tbl.items():
            dk = f"{prefix}.{k}" if prefix else k
            if isinstance(v, dict):
                _walk2(dk, v)
            else:
                cur[dk] = v
                bare_map.setdefault(k, []).append(dk)
    _walk2("", parsed)
    from cockpit_fleet import EXECUTION_DEFAULTS, set_value
    for key,value in EXECUTION_DEFAULTS.items():
        dk="strategy.hot."+key
        if dk not in cur:
            cur[dk]=value;bare_map.setdefault(key,[]).append(dk)

    with open(path,"rb") as source:
        current_hash = hashlib.sha256(source.read()).hexdigest()[:12]
    if body.get("expected_hash") and body["expected_hash"] != current_hash:
        return 409, {"ok":False,"error":"Configuration changed since you opened it. Reload and review the diff."}
    capability_by_key = {item["feature_id"]: item for item in api_v3_capabilities()["features"]}
    wanted, results, olds, seen = {}, [], {}, set()
    for ch in changes:
        key = str(ch.get("key", "")).strip()
        if key in seen:
            results.append({"key": key, "ok": False, "error": "duplicate key in batch"})
            continue
        seen.add(key)
        raw = ch.get("value")
        # resolve dotted or bare key
        if key in cur:
            dk = key
        elif key in bare_map and len(bare_map[key]) == 1:
            dk = bare_map[key][0]
        else:
            results.append({"key": key, "ok": False, "error": "not present in this instance config"})
            continue
        section, _, bare = dk.rpartition(".")
        if section not in ("strategy.hot", "risk") or bare not in COCKPIT_TUNABLE:
            results.append({"key": dk, "ok": False, "error": "not an engine-backed cockpit control"})
            continue
        cap = capability_by_key.get(dk)
        if not cap or not cap["backend_supported"] or not cap["frontend_supported"]:
            results.append({"key": dk, "ok": False, "error": "engine support is not verified for this control"})
            continue
        current = cur[dk]
        if isinstance(raw, str) and COCKPIT_TUNABLE[bare][0] in ("floats", "ints", "strings", "assets", "pairs"):
            try:
                raw = json.loads(raw)
            except ValueError:
                pass
        ok, coerced, err = validate_tunable(bare, raw)
        if bare == "mode":
            ok, err = False, "mode changes require a separate operational workflow"
        if not ok:
            results.append({"key": dk, "ok": False, "error": err})
            continue
        olds[dk] = current
        wanted[dk] = coerced
        results.append({"key": dk, "ok": True, "value": coerced})
    failed = [r for r in results if not r["ok"]]
    if failed:
        return 422, {"ok": False, "error": "validation failed", "results": results}

    def effective(key):
        return wanted.get(key, cur.get(key))
    for low, high in (("min_entry_share_price", "max_entry_share_price"),
                      ("min_size_usd", "max_size_usd"),
                      ("window_predictor_price_min", "window_predictor_price_max"),
                      ("scalp_v3_min_price", "scalp_v3_max_price"),
                      ("scalp_v3_min_secs", "scalp_v3_max_secs"),
                      ("adaptive_min_secs_left", "adaptive_max_secs_left"),
                      ("endgame_min_price", "endgame_max_price")):
        a, b = effective("strategy.hot." + low), effective("strategy.hot." + high)
        if a is not None and b is not None and a > b:
            return 422, {"ok": False, "error": f"{low} exceeds {high}"}
    rc={key:effective("strategy.hot."+key) for key in COCKPIT_TUNABLE if key.startswith("recycler_")}
    from cockpit_fleet import validate_recycler
    try:
        validate_recycler({k:v for k,v in rc.items() if v is not None}, parsed.get('runtime',{}).get('mode','paper'))
        if 'strategy.hot.recycler_mode' in wanted and wanted['strategy.hot.recycler_mode']!=cur.get('strategy.hot.recycler_mode','off'):
            from cockpit_fleet import _flat
            _flat(_CockpitServices(),instance)
    except ValueError as exc:
        return 422, {"ok":False,"error":str(exc)}
    fractions = effective("strategy.hot.window_predictor_fractions")
    amounts = effective("strategy.hot.window_predictor_amounts_usd")
    if isinstance(fractions, list) and isinstance(amounts, list) and len(fractions) != len(amounts):
        return 422, {"ok": False, "error": "WPRED fractions/amounts length mismatch"}

    # ── self-write: replace in place, or append under the section header when
    # the key is engine-known but absent from this instance file (toggles the
    # user switches ON must be creatable). Then RE-READ the file and verify
    # every key actually landed — success is proven, never assumed.
    def _toml_lit(v):
        if isinstance(v, bool): return "true" if v else "false"
        if isinstance(v, (int, float)): return str(v)
        if isinstance(v, list):
            return "[" + ", ".join(_toml_lit(x) for x in v) + "]"
        return json.dumps(str(v))
    with open(path) as f:
        content = f.read()
    applied = []
    for dk, v in wanted.items():
        sec, _, bare = dk.rpartition(".")
        nv = _toml_lit(v)
        content2, n = _set_config_value(content, sec or None, bare, nv)
        if n:
            content = content2
            applied.append(dk)
            continue
        if sec=="strategy.hot" and bare in EXECUTION_DEFAULTS:
            content=set_value(_CockpitServices(),content,sec,bare,v);applied.append(dk);continue
        return 422, {"ok": False, "error": f"key not found in file: {dk}", "results": results}
    try:
        parsed_after = _tm.loads(content)
    except Exception as e:
        return 500, {"ok": False, "error": f"generated TOML invalid: {e}"}
    # verify: every wanted key now reads back the coerced value
    def _get_nested(t, dk):
        node = t
        for part in dk.split("."):
            if not isinstance(node, dict) or part not in node: return None
            node = node[part]
        return node
    for dk, v in wanted.items():
        got = _get_nested(parsed_after, dk)
        if isinstance(v, float):
            match = got is not None and abs(float(got) - v) < 1e-9
        elif isinstance(v, list):
            match = got == v
        else:
            match = got == v
        if not match:
            return 500, {"ok": False, "error": f"verification failed for {dk}: file has {got!r}, expected {v!r}",
                         "results": results}
    safe_before = {key:value for key,value in cur.items()
                   if key.rpartition(".")[0] in ("strategy.hot","risk")
                   and key.rpartition(".")[2] in COCKPIT_TUNABLE}
    store = platform_store()
    before_version = store.config(instance,safe_before,body.get("_actor","operator"),"before edit")
    import tempfile
    import stat
    fd, tmp = tempfile.mkstemp(prefix=".cockpit-", dir=CONFIG_DIR, text=True)
    try:
        os.fchmod(fd, stat.S_IMODE(os.stat(path).st_mode))
        with os.fdopen(fd, "w") as f:
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    invalidate_runtime_caches()
    # audit trail with old values
    try:
        audit = os.path.join(ROOT, "logs", "config_audit.jsonl")
        rec = {"ts": int(time.time() * 1000), "user": body.get("_actor", "operator"), "instance": instance,
               "reason": reason, "ticket": ticket,
               "changes": [{"key": dk, "new": wanted[dk], "old": olds.get(dk)} for dk in wanted]}
        with open(audit, "a") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception:
        pass
    after_version = store.config(instance,{**safe_before,**wanted},body.get("_actor","operator"),reason)
    store.record(body.get("_actor","operator"),"config.applied",instance,
                 {"before":before_version,"after":after_version,"changes":rec["changes"],"reason":reason})
    return 200, {"ok": True, "results": results, "changed": applied,
                 "config_hash":hashlib.sha256(content.encode()).hexdigest()[:12],
                 "before_version":before_version,"after_version":after_version}


def api_v3_preset_save(body):
    name = str(body.get("name", ""))[:40]
    src = body.get("instance", "")
    if not re.match(r'^[\w-]+$', name) or not re.match(r'^[\w-]+$', src):
        return 400, {"error": "bad name/instance"}
    srcp = os.path.join(CONFIG_DIR, f"polypulse-{src}.toml")
    if not os.path.exists(srcp): return 404, {"error": "instance config missing"}
    pdir = os.path.join(CONFIG_DIR, "presets")
    os.makedirs(pdir, exist_ok=True)
    dst = os.path.join(pdir, f"preset-{name}.toml")
    with open(srcp) as f: content = f.read()
    with open(dst, "w") as f: f.write(content)
    return 200, {"ok": True, "preset": f"preset-{name}"}


def api_v3_fleet_control(body):
    import subprocess
    action = body.get("action", ""); instance = body.get("instance", "")
    if action not in ("start", "stop", "restart") or not re.match(r'^[\w-]+$', instance):
        return 400, {"error": "bad action/instance"}
    if instance not in fleet_instances():
        return 400, {"error": "Unknown instance"}
    if action in ("start", "restart") and current_mode(instance).lower() == "live" and body.get("confirm_live") is not True:
        return 422, {"error": "Starting or restarting LIVE requires explicit confirm_live=true"}
    try:
        r = subprocess.run(["systemctl", "--user", action, f"polypulse-bot@{instance}"],
                           capture_output=True, text=True, timeout=20, env=_SYSTEMD_ENV)
        return 200, {"ok": r.returncode == 0, "out": r.stdout[-200:] + r.stderr[-200:]}
    except Exception as e:
        return 500, {"error": str(e)}


def _manual_buy_preflight(body, instance):
    """Validate a human order against verified runtime, limits, and fresh venue data."""
    if not re.fullmatch(r'0x[0-9a-fA-F]{64}', str(body.get("condition_id") or "")):
        return 400, {"error": "a valid Polymarket condition id is required"}
    side = str(body.get("side") or "").upper()
    if side not in ("UP", "DOWN"):
        return 400, {"error": "side must be UP or DOWN"}
    requested_mode = str(body.get("expected_mode") or "").upper()
    if requested_mode not in ("PAPER", "LIVE"):
        return 400, {"error": "select a verified PAPER or LIVE bot before trading"}

    service = _systemd_bot_status(f"polypulse-bot@{instance}.service") or {}
    observed = _observed_runtime_status(instance, service.get("pid")) if service.get("running") else None
    if not observed:
        return 409, {"error": "bot runtime is stopped or unverified; manual orders are disabled"}
    mode = str(observed.get("mode") or "").upper()
    if mode != requested_mode:
        return 409, {"error": f"bot runtime changed to {mode}; refresh the trade preview"}
    if observed.get("risk_state") == "ORDER_UNCERTAIN_HALT" or observed.get("daily_kill_fired"):
        return 409, {"error": "bot is halted for order reconciliation or its session loss limit"}
    if mode == "LIVE":
        if body.get("confirm_live") is not True:
            return 422, {"error": "confirm_live=true is required for a LIVE manual order"}
        wallet = api_wallet_shared_status()
        bot_wallet = next((bot for bot in wallet.get("bots", [])
                           if bot.get("instance") == instance), None)
        if not wallet.get("credentials_ready") or not (bot_wallet or {}).get("uses_shared_credentials"):
            return 409, {"error": "shared ADMIN wallet credentials are not ready"}

    condition_id = str(body["condition_id"])
    generic = body.get("outcome_index") is not None
    direction = str(body.get("direction") or "BUY").upper()
    if direction not in ("BUY", "SELL") or (direction == "SELL" and not generic):
        return 400, {"error": "only BUY is supported for prediction-board markets"}
    if generic:
        raw_index = body.get("outcome_index")
        if isinstance(raw_index, bool) or not isinstance(raw_index, int) or raw_index not in (0, 1):
            return 400, {"error": "choose one of the market's two outcomes"}
        market, market_stale = _mobile_market_for_condition(condition_id)
        if not market and market_stale:
            return 409, {"error": "market details could not be refreshed; try again before trading"}
        if not market or not market.get("active") or market.get("closed") or not market.get("accepting_orders"):
            return 409, {"error": "market is closed or no longer accepting orders"}
        if market_stale:
            return 409, {"error": "market details are stale; refresh before submitting"}
        if len(market.get("outcomes", [])) != 2:
            return 422, {"error": "manual trading currently supports binary markets only"}
        outcome = market["outcomes"][raw_index]
        book_code, book = api_mobile_market_book(condition_id, raw_index)
        if book_code != 200 or not book.get("fresh"):
            return 409, {"error": "a fresh outcome order book is unavailable; refresh before submitting"}
        book_age = book.get("book_age_s")
        ask = book.get("best_ask") if direction == "BUY" else book.get("best_bid")
        if ask is None or float(ask) <= 0:
            return 409, {"error": "the selected outcome has no executable top of book"}
        side = "UP" if raw_index == 0 else "DOWN"
        asset = "MANUAL"
        question = market["question"]
        outcome_label = outcome["label"]
        token_id = outcome["token_id"]
        yes_token_id = market["outcomes"][0]["token_id"]
        no_token_id = market["outcomes"][1]["token_id"]
        window_close_ms = 0
        if market.get("end_date"):
            try:
                end_date = datetime.fromisoformat(str(market["end_date"]).replace("Z", "+00:00"))
                if end_date.tzinfo is None:
                    end_date = end_date.replace(tzinfo=timezone.utc)
                window_close_ms = int(end_date.timestamp() * 1000)
            except (TypeError, ValueError, OverflowError):
                window_close_ms = 0
        if direction == "BUY":
            raw_size = body.get("size_usd")
            if isinstance(raw_size, bool) or not isinstance(raw_size, (int, float)):
                return 400, {"error": "spend amount must be a number"}
            size_usd = float(raw_size)
            if not math.isfinite(size_usd) or size_usd <= 0:
                return 400, {"error": "spend amount must be a finite positive number"}
            size_shares = 0.0
        else:
            raw_shares = body.get("size_shares")
            if isinstance(raw_shares, bool) or not isinstance(raw_shares, (int, float)):
                return 400, {"error": "share amount must be a number"}
            size_shares = float(raw_shares)
            if not math.isfinite(size_shares) or size_shares <= 0:
                return 400, {"error": "share amount must be a finite positive number"}
            size_usd = 0.0
    else:
        board = api_manual_predictions()
        if board.get("stale") or float(board.get("snapshot_age_s") or 0) > 10:
            return 409, {"error": "market preview is stale; refresh before submitting"}
        market = next((card for card in board.get("cards", [])
                       if card.get("period") == "CURRENT"
                       and card.get("condition_id") == condition_id
                       and int(card.get("seconds_left") or 0) > 0), None)
        if not market:
            return 409, {"error": "market is not in a current, tradeable window"}
        ask_key = "polymarket_up_price" if side == "UP" else "polymarket_down_price"
        ask = market.get(ask_key)
        book_age = market.get("book_age_s")
        if ask is None or book_age is None or float(book_age) > 15:
            return 409, {"error": "a fresh executable ask is unavailable for this outcome"}
        raw_size = body.get("size_usd")
        if isinstance(raw_size, bool) or not isinstance(raw_size, (int, float)):
            return 400, {"error": "spend amount must be a number"}
        size_usd = float(raw_size)
        if not math.isfinite(size_usd) or size_usd <= 0:
            return 400, {"error": "spend amount must be a finite positive number"}
        size_shares = 0.0
        outcome_label = ""
        token_id = ""
        yes_token_id = ""
        no_token_id = ""
        window_close_ms = int(float(market.get("window_close") or 0) * 1000)
        asset = market.get("asset")
        question = market.get("question")
    try:
        config = load_config(instance)
        risk = config.get("risk", {})
        minimum = float(risk.get("min_order_usd", 0))
        maximum = float(risk.get("max_order_usd", 0))
        strategy = config.get("strategy", {})
        default_limit = (float(ask) if direction == "SELL" else
                         min(0.99, float(ask) * (1 + float(strategy.get("manual_buy_slippage_pct", 0)))
                             + float(strategy.get("manual_buy_slippage_extra", 0))))
    except (TypeError, ValueError, OSError):
        return 409, {"error": "bot order limits could not be read"}
    if minimum <= 0 or maximum < minimum:
        return 409, {"error": "bot order limits are not configured for manual trading"}
    if direction == "BUY" and (size_usd < minimum or size_usd > maximum):
        return 422, {"error": f"spend must be between ${minimum:.2f} and ${maximum:.2f} for this bot"}

    raw_limit = body.get("limit_price")
    if raw_limit is None or raw_limit == "":
        limit_price = default_limit
    elif isinstance(raw_limit, bool) or not isinstance(raw_limit, (int, float)):
        return 400, {"error": "limit price must be a number"}
    else:
        limit_price = float(raw_limit)
    if not math.isfinite(limit_price) or not 0.01 <= limit_price <= 0.99:
        return 422, {"error": "limit price must be between 1¢ and 99¢ per share"}
    if generic and direction == "BUY" and limit_price + 1e-9 < float(ask):
        return 422, {"error": "buy maximum is below the current ask; raise the limit or refresh the book"}
    if generic and direction == "SELL" and limit_price - 1e-9 > float(ask):
        return 422, {"error": "sell minimum is above the current bid; lower the limit or refresh the book"}

    return 200, {
        "instance": instance,
        "mode": mode,
        "size_usd": size_usd,
        "limit_price": limit_price,
        "indicative_ask": float(ask),
        "estimated_shares": size_usd / limit_price if direction == "BUY" else size_shares,
        "book_age_s": float(book_age),
        "asset": asset,
        "question": question,
        "outcome_label": outcome_label,
        "token_id": token_id,
        "yes_token_id": yes_token_id,
        "no_token_id": no_token_id,
        "window_close_ms": window_close_ms,
        "outcome_index": body.get("outcome_index"),
        "direction": direction,
        "size_shares": size_shares,
        "condition_id": condition_id,
        "side": side,
    }


_manual_buy_queue_lock = threading.Lock()
_force_sell_queue_lock = threading.Lock()


def api_v3_command(body, actor=None):
    instance = body.get("instance", "")
    op = body.get("op", "")
    if instance not in fleet_instances() or op not in ("autopilot", "cancel_open_orders", "manual_buy"):
        return 400, {"error": "bad instance/op"}
    if op == "autopilot" and not isinstance(body.get("enabled"), bool):
        return 422, {"error": "autopilot requires a boolean enabled value"}
    if op == "autopilot" and body.get("enabled"):
        service = _systemd_bot_status(f"polypulse-bot@{instance}.service") or {}
        observed = _observed_runtime_status(instance,service.get("pid")) or {}
        if observed.get("daily_kill_fired") or observed.get("risk_state") == "ORDER_UNCERTAIN_HALT":
            return 409, {"error":"Entry resume blocked by session loss limit or unresolved order. Review and reconcile first."}
    if op == "autopilot" and body["enabled"] and current_mode(instance).lower() == "live" and body.get("confirm_live") is not True:
        return 422, {"error": "arming LIVE autopilot requires explicit confirm_live=true"}
    manual = None
    if op == "manual_buy":
        code, manual = _manual_buy_preflight(body, instance)
        if code != 200:
            return code, manual
    os.makedirs(COMMANDS_DIR, exist_ok=True)
    ts = int(time.time() * 1000)
    payload = {"op": op, "instance": instance, "ts": ts}
    if "enabled" in body: payload["enabled"] = bool(body["enabled"])
    if manual:
        payload.update({
            "condition_id": manual["condition_id"],
            "side": manual["side"],
            "size_usd": manual["size_usd"],
            "limit_price": manual["limit_price"],
            "expected_mode": manual["mode"],
            "direction": manual.get("direction", "BUY"),
            "size_shares": manual.get("size_shares", 0.0),
            "token_id": manual.get("token_id", ""),
            "yes_token_id": manual.get("yes_token_id", ""),
            "no_token_id": manual.get("no_token_id", ""),
            "window_close_ms": manual.get("window_close_ms", 0),
            "outcome_index": manual.get("outcome_index"),
            "outcome_label": manual.get("outcome_label", ""),
            "question": manual.get("question", ""),
        })
        with _manual_buy_queue_lock:
            for filename in os.listdir(COMMANDS_DIR):
                if not filename.endswith(".json") or not filename.startswith(f"manual_buy_{instance}_"):
                    continue
                try:
                    with open(os.path.join(COMMANDS_DIR, filename), encoding="utf-8") as queued_file:
                        queued = json.load(queued_file)
                    age_ms = ts - int(queued.get("ts") or 0)
                    if queued.get("op") == "manual_buy" and 0 <= age_ms <= 30_000:
                        return 409, {"error": "a manual order is already waiting for this bot"}
                except (OSError, ValueError, TypeError):
                    continue
            cid = re.sub(r"[^0-9a-fA-F]", "", manual["condition_id"])[-16:]
            fp = os.path.join(COMMANDS_DIR, f"manual_buy_{instance}_{cid}_{ts}.json")
            _write_control_atomic(fp, payload)
        try:
            platform_store().record(actor or "admin", "trade.manual_buy.queued", instance,
                                    {k: manual[k] for k in ("mode", "side", "direction", "size_usd",
                                                            "size_shares", "limit_price", "indicative_ask",
                                                            "condition_id", "outcome_label")})
        except Exception:
            pass
        return 200, {"ok": True, "status": "queued", "queued": os.path.basename(fp),
                     **manual}
    fp = os.path.join(COMMANDS_DIR, f"{op}_{instance}_{ts}.json")
    _write_control_atomic(fp, payload)
    return 200, {"ok": True, "queued": os.path.basename(fp)}


def api_pnl_today():
    """Fleet fill-cash-flow summary: UTC-day totals and 30 calendar dates.

    Reads the net-of-fee view (vp.v_strategy_net_pnl) so the numbers match
    scripts/accounting.py exactly. Read-only, cheap (~1 indexed scan).
    The weekly total is deliberately restricted to the last seven UTC dates;
    the chart series is the current UTC date plus the preceding 29 dates."""
    try:
        rows = q("""SELECT instance,
                          (day = (now() at time zone 'utc')::date) AS is_today,
                          (day >= (now() at time zone 'utc')::date - 6) AS is_week,
                          day, settles_won, settles_lost,
                          net_pnl_usd::float8 AS net, fees_usd::float8 AS fees
                   FROM vp.v_strategy_net_pnl
                   WHERE day >= (now() at time zone 'utc')::date - 29
                   ORDER BY day, instance""") or []
    except Exception as e:
        return {"ok": False, "error": str(e), "instances": [], "days": []}
    today, days, week_net = [], {}, 0.0
    for r in rows:
        d = r["day"]
        key = d.isoformat() if hasattr(d, "isoformat") else str(d)
        agg = days.setdefault(key, {"day": key, "net": 0.0, "w": 0, "l": 0, "fees": 0.0})
        net = r["net"] or 0.0
        agg["net"] += net
        agg["w"] += r["settles_won"] or 0
        agg["l"] += r["settles_lost"] or 0
        agg["fees"] += r["fees"] or 0.0
        if r["is_week"]:
            week_net += net
        if r["is_today"]:
            today.append({"instance": r["instance"], "w": r["settles_won"] or 0,
                          "l": r["settles_lost"] or 0, "net": net,
                          "fees": r["fees"] or 0.0})
    series = [days[k] for k in sorted(days)]
    return {"ok": True, "instances": today, "days": series,
            "today_net": round(sum(t["net"] for t in today), 2),
            "today_w": sum(t["w"] for t in today),
            "today_l": sum(t["l"] for t in today),
            "today_fees": round(sum(t["fees"] for t in today), 2),
            "week_net": round(week_net, 2)}


def api_pnl_history(hours=24):
    mode = current_mode()
    since = session_start_for_mode(mode)
    if mode.lower() == "paper" and not since:
        return {"history": []}
    start = starting_balance_usd()
    ph_since = "AND ts >= %s::timestamptz" if since else ""
    order_since = "AND o.ts_created >= %s::timestamptz" if since else ""
    args = [mode, f"{hours} hours"]
    if since:
        args.append(since)
    args.append(mode)
    if since:
        args.append(since)
    args.extend([start, start])
    rows = q(f"""
        WITH ph AS (
          SELECT ts, deployed_usd
          FROM vp.positions_history
          WHERE mode=%s AND ts > now() - %s::interval
            {ph_since}
        ),
        cf AS (
          SELECT ph.ts,
                 COALESCE(SUM(CASE WHEN o.direction='sell'
                                    THEN f.price * f.size_shares
                                    ELSE -(f.price * f.size_shares) END
                              - f.fee_usd), 0) AS cashflow
          FROM ph
          LEFT JOIN vp.orders o
            ON o.mode=%s AND o.status='filled' AND o.ts_created <= ph.ts
            {order_since}
          LEFT JOIN vp.fills f ON f.client_order_id = o.client_order_id
          GROUP BY ph.ts
        )
        SELECT ph.ts,
               (%s + cf.cashflow) AS cash_usd,
               (%s + cf.cashflow + ph.deployed_usd) AS equity_usd,
               (cf.cashflow + ph.deployed_usd) AS realized_pnl,
               0.0 AS unrealized_pnl,
               ph.deployed_usd
        FROM ph
        JOIN cf USING (ts)
        ORDER BY ph.ts ASC
    """, tuple(args))
    return {"history": rows}


def api_candles(symbol="BTCUSDT", limit=240):
    rows = q("""SELECT ts, open, high, low, close, volume
                FROM vp.candles_1m WHERE symbol=%s
                ORDER BY ts DESC LIMIT %s""", (symbol, limit))
    rows.reverse()
    # Check staleness: if newest stored candle is >2 h old, supplement with
    # live data aggregated from binance_ticks (which is always current).
    stale_h = None
    live_rows = []
    if rows:
        last = rows[-1].get("ts")
        if hasattr(last, "timestamp"):
            age_s = time.time() - last.timestamp()
        else:
            age_s = time.time() - (last / 1000 if last > 1e10 else last)
        stale_h = round(age_s / 3600, 1)
    # Always append live 1-min candles from binance_ticks for the last `limit` minutes
    # so the chart is always current even when candles_1m is stale.
    live_rows = q("""
        SELECT DATE_TRUNC('minute', ts) AS ts,
               (ARRAY_AGG(price ORDER BY ts ASC))[1]  AS open,
               MAX(price)                               AS high,
               MIN(price)                               AS low,
               (ARRAY_AGG(price ORDER BY ts DESC))[1]  AS close,
               SUM(qty)                                 AS volume
        FROM vp.binance_ticks
        WHERE symbol=%s AND ts > now() - (%s || ' minutes')::interval
        GROUP BY 1
        ORDER BY 1 ASC""", (symbol, str(limit)))
    # Merge: live rows override any stored candle at the same minute
    combined = {r["ts"]: r for r in rows}
    for r in live_rows:
        combined[r["ts"]] = r
    merged = sorted(combined.values(), key=lambda r: r["ts"])
    return {"symbol": symbol, "candles": merged, "stale_hours": stale_h}


def api_prices():
    out = {}
    for a in ASSETS:
        orc = q("SELECT price, ts FROM vp.oracle_ticks WHERE asset=%s ORDER BY ts DESC LIMIT 1",
                (a,), one=True)
        cb = q("SELECT price, ts FROM vp.coinbase_ticks WHERE asset=%s ORDER BY ts DESC LIMIT 1",
               (a,), one=True)
        bn = q("SELECT price, ts FROM vp.binance_ticks WHERE symbol=%s ORDER BY ts DESC LIMIT 1",
               (SYMBOL[a],), one=True)
        out[a] = {
            "oracle": orc.get("price") if orc else None,
            "oracle_ts": orc.get("ts") if orc else None,
            "coinbase": cb.get("price") if cb else None,
            "binance": bn.get("price") if bn else None,
        }
    return {"prices": out}


def api_markets(limit=40):
    rows = q("""
        SELECT m.condition_id, m.asset, m.horizon_seconds, m.target_price,
               m.window_open, m.window_close, m.resolved_outcome, m.question,
               up.best_bid AS up_bid, up.best_ask AS up_ask, up.midprice AS up_mid,
               dn.best_bid AS down_bid, dn.best_ask AS down_ask, dn.midprice AS down_mid,
               GREATEST(up.ts, dn.ts) AS book_ts
        FROM vp.pm_markets m
        LEFT JOIN LATERAL (
          SELECT best_bid, best_ask, midprice, ts FROM vp.pm_book_snapshots
          WHERE condition_id=m.condition_id AND side_label='UP'
            AND ts > now() - interval '2 hours'
          ORDER BY ts DESC LIMIT 1
        ) up ON true
        LEFT JOIN LATERAL (
          SELECT best_bid, best_ask, midprice, ts FROM vp.pm_book_snapshots
          WHERE condition_id=m.condition_id AND side_label='DOWN'
            AND ts > now() - interval '2 hours'
          ORDER BY ts DESC LIMIT 1
        ) dn ON true
        WHERE m.window_close > now() - interval '5 minutes'
          AND m.window_open < now() + interval '45 minutes'
        ORDER BY m.window_close ASC LIMIT %s
    """, (limit,))
    now_ms = int(time.time() * 1000)
    for r in rows:
        if r.get("window_open") and r["window_open"] > now_ms:
            r["phase"] = "upcoming"
        elif r.get("window_close") and r["window_close"] < now_ms:
            r["phase"] = "closing"
        else:
            r["phase"] = "live"
    return {"markets": rows}


# Mobile market data is proxied by the cockpit so the app uses the same trusted
# venue endpoints as the trading service and never needs exchange credentials.
_mobile_gamma_cache = {}
_mobile_gamma_lock = threading.Lock()
_mobile_book_cache = {}


def _mobile_cached_json(key, url, ttl=15.0, timeout=5.0):
    now = time.monotonic()
    with _mobile_gamma_lock:
        cached = _mobile_gamma_cache.get(key)
        if cached and now - cached[0] < ttl:
            return cached[1], False
    try:
        value = _public_json(url, timeout=timeout)
        with _mobile_gamma_lock:
            _mobile_gamma_cache[key] = (time.monotonic(), value)
        return value, False
    except Exception:
        with _mobile_gamma_lock:
            cached = _mobile_gamma_cache.get(key)
        if cached:
            return cached[1], True
        raise


def _mobile_json_array(value):
    if isinstance(value, list):
        return value
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
            return parsed if isinstance(parsed, list) else []
        except (ValueError, TypeError):
            return []
    return []


def _mobile_number(value):
    try:
        number = float(value)
        return number if math.isfinite(number) else None
    except (TypeError, ValueError):
        return None


def _mobile_normalize_market(raw, event_title=""):
    if not isinstance(raw, dict):
        return None
    condition_id = str(raw.get("conditionId") or raw.get("condition_id") or "")
    outcomes = _mobile_json_array(raw.get("outcomes"))
    token_ids = _mobile_json_array(raw.get("clobTokenIds") or raw.get("clob_token_ids"))
    prices = _mobile_json_array(raw.get("outcomePrices") or raw.get("outcome_prices"))
    if not condition_id or not outcomes:
        return None
    normalized_outcomes = []
    for index, label in enumerate(outcomes):
        normalized_outcomes.append({
            "index": index,
            "label": str(label),
            "last_price": _mobile_number(prices[index]) if index < len(prices) else None,
            "token_id": str(token_ids[index]) if index < len(token_ids) and token_ids[index] else None,
        })
    if len(normalized_outcomes) < 2 or any(not outcome["token_id"] for outcome in normalized_outcomes[:2]):
        return None
    return {
        "id": str(raw.get("id") or ""),
        "condition_id": condition_id,
        "slug": str(raw.get("slug") or ""),
        "question": str(raw.get("question") or raw.get("title") or "Untitled market"),
        "event_title": event_title or str(raw.get("eventTitle") or raw.get("event_title") or ""),
        "description": str(raw.get("description") or ""),
        "category": str(raw.get("category") or ""),
        "end_date": raw.get("endDate") or raw.get("end_date"),
        "active": bool(raw.get("active", True)),
        "closed": bool(raw.get("closed", False)),
        "accepting_orders": bool(raw.get("acceptingOrders", raw.get("accepting_orders", True))),
        "volume_24h": _mobile_number(raw.get("volume24hr") or raw.get("volume24hrClob")),
        "volume": _mobile_number(raw.get("volumeNum") or raw.get("volume")),
        "liquidity": _mobile_number(raw.get("liquidityNum") or raw.get("liquidity")),
        "outcomes": normalized_outcomes,
    }


def _mobile_catalog_market(market):
    """Keep public outcome token IDs in the authenticated detail response only."""
    return {
        **market,
        "outcomes": [
            {key: value for key, value in outcome.items() if key != "token_id"}
            for outcome in market.get("outcomes", [])
        ],
    }


def api_mobile_market_search(query="", limit=30, offset=0):
    """Public venue catalog/search, normalized and cached for the mobile client."""
    query = str(query or "").strip()[:100]
    try:
        limit = max(1, min(50, int(limit)))
        offset = max(0, min(5000, int(offset)))
    except (TypeError, ValueError):
        return 400, {"error": "invalid market page"}
    gamma = "https://gamma-api.polymarket.com"
    stale = False
    try:
        if len(query) >= 2:
            cache_key = f"search:{query.casefold()}"
            search, was_stale = _mobile_cached_json(
                cache_key, gamma + "/public-search?" + urlencode({"q": query}), ttl=20.0)
            stale = stale or was_stale
            event_rows = search.get("events", []) if isinstance(search, dict) else []
            markets = []
            # Search returns event hits. Hydrate a small page of event metadata
            # concurrently so the app can render and trade actual outcome markets.
            selected = event_rows[offset:offset + min(limit, 12)] if isinstance(event_rows, list) else []
            slugs = [(str(ev.get("slug") or ""), str(ev.get("title") or ""))
                     for ev in selected if isinstance(ev, dict) and ev.get("slug")]
            if slugs:
                from concurrent.futures import ThreadPoolExecutor, as_completed
                with ThreadPoolExecutor(max_workers=min(6, len(slugs))) as workers:
                    futures = {
                        workers.submit(_mobile_cached_json, "event:" + slug,
                                       gamma + "/events/slug/" + quote(slug, safe="-"), 30.0, 5.0): title
                        for slug, title in slugs
                    }
                    for future in as_completed(futures):
                        try:
                            event, was_stale = future.result()
                            stale = stale or was_stale
                            if isinstance(event, list):
                                event = event[0] if event else {}
                            if not isinstance(event, dict):
                                continue
                            title = futures[future] or str(event.get("title") or "")
                            for raw in event.get("markets", []) if isinstance(event.get("markets"), list) else []:
                                item = _mobile_normalize_market(raw, title)
                                if item and item["active"] and not item["closed"]:
                                    markets.append(item)
                        except Exception:
                            continue
            total = (search.get("pagination") or {}).get("totalResults") if isinstance(search, dict) else None
            has_more = offset + len(selected) < int(total or len(event_rows)) if isinstance(event_rows, list) else False
            markets.sort(key=lambda item: item.get("volume_24h") or item.get("volume") or 0, reverse=True)
            return 200, {"markets": [_mobile_catalog_market(item) for item in markets[:limit]], "query": query, "offset": offset,
                         "next_offset": offset + len(selected),
                         "has_more": bool(has_more), "stale": stale,
                         "server_ts": int(time.time() * 1000)}

        params = {"active": "true", "closed": "false", "limit": str(min(100, limit + offset)),
                  "offset": str(offset), "order": "volume24hr", "ascending": "false",
                  "enableOrderBook": "true"}
        cache_key = f"catalog:{offset}:{limit}"
        raw_rows, stale = _mobile_cached_json(
            cache_key, gamma + "/markets?" + urlencode(params), ttl=20.0, timeout=8.0)
        if not isinstance(raw_rows, list):
            raw_rows = raw_rows.get("markets", []) if isinstance(raw_rows, dict) else []
        markets = []
        for raw in raw_rows:
            item = _mobile_normalize_market(raw)
            if item and item["active"] and not item["closed"]:
                markets.append(item)
        return 200, {"markets": [_mobile_catalog_market(item) for item in markets[:limit]], "query": "", "offset": offset,
                     "next_offset": offset + len(markets[:limit]),
                     "has_more": len(markets) >= limit, "stale": stale,
                     "server_ts": int(time.time() * 1000)}
    except Exception:
        return 502, {"error": "Polymarket market discovery is temporarily unavailable"}


def _mobile_market_for_condition(condition_id):
    if not re.fullmatch(r"0x[0-9a-fA-F]{64}", str(condition_id or "")):
        return None, False
    url = "https://gamma-api.polymarket.com/markets?" + urlencode({"condition_ids": condition_id})
    try:
        rows, stale = _mobile_cached_json("market:" + condition_id.lower(), url, ttl=10.0)
    except Exception:
        return None, True
    if isinstance(rows, dict):
        rows = rows.get("markets", [rows])
    if not isinstance(rows, list):
        return None, stale
    raw = next((row for row in rows if isinstance(row, dict) and
                str(row.get("conditionId") or row.get("condition_id") or "").lower() == condition_id.lower()), None)
    return (_mobile_normalize_market(raw), stale) if raw else (None, stale)


def api_mobile_market_detail(condition_id):
    try:
        market, stale = _mobile_market_for_condition(str(condition_id or ""))
        if not market:
            return (502 if stale else 404), {
                "error": "market details are temporarily unavailable" if stale
                         else "market was not found or has no tradable outcome tokens"}
        return 200, {"market": market, "stale": stale, "server_ts": int(time.time() * 1000)}
    except Exception:
        return 502, {"error": "market detail is temporarily unavailable"}


def api_mobile_market_book(condition_id, outcome_index):
    try:
        index = int(outcome_index)
    except (TypeError, ValueError):
        return 400, {"error": "invalid outcome index"}
    market, market_stale = _mobile_market_for_condition(str(condition_id or ""))
    if not market and market_stale:
        return 502, {"error": "market details are temporarily unavailable"}
    if not market or index < 0 or index >= len(market["outcomes"]):
        return 404, {"error": "market outcome was not found"}
    if market_stale:
        return 409, {"error": "market details are stale; refresh before trading"}
    token_id = market["outcomes"][index]["token_id"]
    cache_key = f"book:{token_id}"
    now = time.monotonic()
    with _mobile_gamma_lock:
        cached = _mobile_book_cache.get(cache_key)
        if cached and now - cached[0] < 1.0:
            return 200, cached[1]
    try:
        book = _public_json("https://clob.polymarket.com/book?" + urlencode({"token_id": token_id}), timeout=4.0)
        returned_token = str(book.get("asset_id") or "") if isinstance(book, dict) else ""
        returned_condition = str(book.get("market") or "") if isinstance(book, dict) else ""
        if returned_token and returned_token != token_id:
            return 502, {"error": "Polymarket returned a mismatched outcome book"}
        if returned_condition and returned_condition.lower() != market["condition_id"].lower():
            return 502, {"error": "Polymarket returned a mismatched market book"}
        bids = book.get("bids") if isinstance(book, dict) else []
        asks = book.get("asks") if isinstance(book, dict) else []
        best_bid = max((_mobile_number(row.get("price")) or 0 for row in bids if isinstance(row, dict)), default=0)
        positive_asks = [price for row in asks if isinstance(row, dict)
                         for price in [_mobile_number(row.get("price"))] if price and price > 0]
        best_ask = min(positive_asks, default=0)
        stamp = _mobile_number(book.get("timestamp")) if isinstance(book, dict) else None
        now_ms = time.time() * 1000
        age_s = max(0.0, (now_ms - stamp) / 1000) if stamp else None
        timestamp_valid = bool(stamp and stamp <= now_ms + 5_000)
        payload = {"condition_id": market["condition_id"], "outcome_index": index,
                   "best_bid": best_bid or None, "best_ask": best_ask or None,
                   "book_age_s": age_s,
                   "fresh": bool(timestamp_valid and age_s is not None and age_s <= 15
                                 and (best_bid or best_ask)),
                   "timestamp": stamp, "tick_size": _mobile_number(book.get("tick_size") or book.get("minimum_tick_size")),
                   "min_order_size": _mobile_number(book.get("min_order_size")),
                   "server_ts": int(time.time() * 1000)}
        with _mobile_gamma_lock:
            _mobile_book_cache[cache_key] = (time.monotonic(), payload)
        return 200, payload
    except Exception:
        return 502, {"error": "Polymarket order book is temporarily unavailable"}


def api_strategy():
    return load_config()


def api_orderstats(hours=24):
    mode = current_mode()
    since = session_start_for_mode(mode)
    if mode.lower() == "paper" and not since:
        return {"by_source": [], "reject_reasons": []}
    since_clause = "AND o.ts_created >= %s::timestamptz" if since else ""
    args = [mode, f"{hours} hours"]
    if since:
        args.append(since)
    rows = q(f"""
        WITH per_order AS (
          SELECT o.client_order_id, o.source, o.direction, o.status,
                 CASE WHEN o.status='filled'
                      THEN COALESCE(SUM(f.price * f.size_shares), o.notional_usd)
                      ELSE o.notional_usd END AS notional,
                 COALESCE(SUM(f.fee_usd), 0) AS fees
          FROM vp.orders o
          LEFT JOIN vp.fills f ON f.client_order_id = o.client_order_id
          WHERE o.mode=%s AND o.ts_created > now() - %s::interval
            {since_clause}
          GROUP BY o.client_order_id
        )
        SELECT source, direction, status, count(*) n,
               round(sum(notional)::numeric,2) notional,
               round(sum(fees)::numeric,2) fees
        FROM per_order
        GROUP BY source, direction, status
        ORDER BY n DESC
    """, tuple(args))
    reason_args = [mode, f"{hours} hours"]
    if since:
        reason_args.append(since)
    reasons = q(f"""SELECT reject_reason, count(*) n FROM vp.orders
                   WHERE mode=%s AND status='rejected'
                     AND ts_created > now() - %s::interval AND reject_reason IS NOT NULL
                     {since_clause.replace('o.ts_created', 'ts_created')}
                   GROUP BY reject_reason ORDER BY n DESC LIMIT 12""",
                tuple(reason_args))
    for row in reasons:
        row["reject_reason"] = _polytron_redact(str(row.get("reject_reason") or ""))
    return {"by_source": rows, "reject_reasons": reasons}


# ── Multi-instance (LLM tournament) helpers ─────────────────────────────
INSTANCES = ["cb"]
MODELS_ENV = os.path.join(ROOT, "config", "supervisor-models.env")


def instance_names():
    """NAME_i1=Jesse etc. from supervisor-models.env (re-read each call so
    renames apply live)."""
    names = {}
    try:
        for ln in open(MODELS_ENV):
            m = re.match(r'NAME_(\w+)=(.+)', ln.strip())
            if m:
                names[m.group(1)] = m.group(2).strip()
    except OSError:
        pass
    return names


def instance_log(inst):
    return os.path.join(ROOT, "logs", f"polypulse-{inst}.log") if inst else LOG_FILE


def instance_session_start(inst):
    """When this instance's current session began. systemd's start stamp is
    authoritative (paper sessions are per-process); the old log scan broke
    silently once the boot line scrolled past the tail window."""
    try:
        out = subprocess.run(
            ["systemctl", "--user", "show", f"polypulse-bot@{inst}.service",
             "--property=ActiveEnterTimestamp", "--value"],
            capture_output=True, text=True, timeout=5, env=_SYSTEMD_ENV).stdout.strip()
        # "Thu 2026-06-11 00:02:12 EDT" -> "2026-06-11 00:02:12 EDT"
        # Preserve timezone so PostgreSQL ::timestamptz cast is correct.
        # Stripping it caused a 4-hour offset (EDT interpreted as UTC).
        m = re.search(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+([A-Z]{2,4})", out)
        if m:
            return f"{m.group(1)} {m.group(2)}"
    except Exception:
        pass
    try:  # fallback: the instance's own boot line, if still in the tail window
        lines = tail(instance_log(inst), 6000)
    except OSError:
        return None
    for ln in reversed(lines):
        if "session: id=" in ln and ln.startswith("["):
            return ln[1:20]   # "YYYY-mm-dd HH:MM:SS"
    return None


def instance_running(inst):
    """Authoritative check via systemd is-active (fast, works regardless of
    which log file the bot writes). Falls back to the instance log mtime."""
    cache = _instance_running_cache
    now = time.monotonic()
    hit = cache.get(inst)
    if hit and hit[0] > now:
        return hit[1]
    running = False
    authoritative = False
    try:
        out = subprocess.run(
            ["systemctl", "--user", "is-active", f"polypulse-bot@{inst}.service"],
            capture_output=True, text=True, timeout=3, env=_SYSTEMD_ENV).stdout.strip()
        running = (out == "active")
        authoritative = out in ("active", "inactive", "failed", "activating", "deactivating", "unknown")
    except Exception:
        pass
    if not authoritative:
        try:
            running = time.time() - os.stat(instance_log(inst)).st_mtime < 60
        except OSError:
            running = False
    cache[inst] = (now + 2.0, running)
    return running


_instance_running_cache = {}  # inst -> (expires_monotonic, running)


_since_cache = {}  # instance-key -> (expires_monotonic, value)


def paper_since(instance=None):
    """Session-start cutoff for paper accounting, instance-aware.
    ALL view = earliest start among running instances + the main bot.
    TTL-cached: the underlying lookup tails thousands of log lines per
    instance, which at dashboard poll rates was most of /api/live's cost
    (session start only changes when a bot restarts)."""
    mode = current_mode(instance).lower()
    key = f"{instance or '*'}:{mode}"
    now = time.monotonic()
    hit = _since_cache.get(key)
    if hit and hit[0] > now:
        return hit[1]
    val = None
    if mode == "paper":
        if instance:
            val = instance_session_start(instance)
        else:
            starts = [instance_session_start(i) for i in fleet_instances()
                      if instance_running(i)]
            starts.append(session_start_for_mode())
            starts = [s for s in starts if s]
            val = min(starts) if starts else None
    val = normalize_since(val)
    _since_cache[key] = (now + 15, val)
    return val


def fleet_instances():
    """Canonical list of every bot instance that has a config file (excluding
    timestamped backups and the template). Single source of truth shared by the
    fleet tab AND the top instance-tab bar, so a newly-launched/deleted bot shows
    up / disappears in both automatically instead of being hard-coded."""
    all_insts = list(dict.fromkeys(INSTANCES))
    backup_re = re.compile(r'-\d{8}')
    try:
        for fn in sorted(os.listdir(CONFIG_DIR)):
            if not fn.startswith("polypulse-") or not fn.endswith(".toml"):
                continue
            inst = fn[len("polypulse-"):-len(".toml")]
            if inst not in all_insts and re.match(r'^[\w-]+$', inst) and \
                    not backup_re.search(inst) and inst != "template":
                all_insts.append(inst)
    except Exception:
        pass
    return [i for i in all_insts
            if os.path.exists(os.path.join(CONFIG_DIR, f"polypulse-{i}.toml"))]


def _admin_wallet_instances():
    """Current bot configs owned by ADMIN, excluding tenant and demo bots."""
    tenant_instances = set()
    users = _load_users()
    if not isinstance(users, dict):
        users = {}
    for user in users.values():
        if not isinstance(user, dict) or user.get("role") == "admin":
            continue
        tenant_instances.update(i for i in (user.get("instances") or []) if isinstance(i, str))
        if isinstance(user.get("instance"), str) and user["instance"]:
            tenant_instances.add(user["instance"])
    return [inst for inst in fleet_instances()
            if inst not in tenant_instances and inst != "demo"
            and not inst.startswith("demo-")]


def api_tournament():
    """Leaderboard: per instance — active strategy, model, live PnL, status."""
    out = []
    names = instance_names()
    for inst in fleet_instances():
        mode = current_mode(inst)
        row = {"instance": inst, "name": names.get(inst, inst),
               "running": instance_running(inst), "mode": mode}
        srow = q("""SELECT s.strategy_id, s.llm_model, s.reason, r.started_at
                    FROM vp.strategy_runs r JOIN vp.strategies s USING (strategy_id)
                    WHERE r.instance=%s ORDER BY r.started_at DESC LIMIT 1""", (inst,))
        if srow:
            row.update({"strategy_id": srow[0]["strategy_id"],
                        "model": srow[0]["llm_model"],
                        "reason": srow[0]["reason"],
                        "run_started": srow[0]["started_at"]})
        since = accounting_since(inst, mode)
        if mode.lower() != "paper" or since:
            since_clause = "AND f.ts >= %s::timestamptz" if since else ""
            args = [inst, mode]
            if since:
                args.append(since)
            agg = q(f"""SELECT count(*) AS fills,
                               count(*) FILTER (WHERE o.source='settle' AND f.price>0.5)  AS wins,
                               count(*) FILTER (WHERE o.source='settle' AND f.price<=0.5) AS losses,
                               COALESCE(SUM(CASE WHEN o.direction='buy'
                                    THEN -f.price*f.size_shares ELSE f.price*f.size_shares END
                                    - COALESCE(f.fee_usd,0)), 0) AS net
                        FROM vp.orders o JOIN vp.fills f ON f.client_order_id=o.client_order_id
                        WHERE o.instance=%s AND o.mode=%s AND o.status='filled'
                          {since_clause}""", tuple(args))
            if agg:
                row.update({"fills": agg[0]["fills"], "wins": agg[0]["wins"],
                            "losses": agg[0]["losses"],
                            "net_pnl": round(float(agg[0]["net"] or 0), 2)})
        out.append(row)
    return {"instances": out}


def api_supervisor_feed(limit=15):
    rows = q("""SELECT s.strategy_id, s.created_at, s.instance, s.llm_model,
                       s.reason, s.config, s.parent_id,
                       p.config AS parent_config
                FROM vp.strategies s
                LEFT JOIN vp.strategies p ON p.strategy_id = s.parent_id
                ORDER BY s.created_at DESC LIMIT %s""", (limit,))
    feed = []
    names = instance_names()
    for r in rows:
        cfg = r["config"] if isinstance(r["config"], dict) else json.loads(r["config"] or "{}")
        par = r["parent_config"]
        par = par if isinstance(par, dict) else json.loads(par or "{}")
        diff = {k: v for k, v in cfg.items() if par.get(k) != v} if par else {}
        feed.append({"strategy_id": r["strategy_id"], "ts": r["created_at"],
                     "instance": r["instance"],
                     "name": names.get(r["instance"], r["instance"]),
                     "model": r["llm_model"],
                     "reason": r["reason"],
                     "diff": diff if par else {"(baseline)": True}})
    return {"feed": feed}


_live_cache = {}   # instance-key -> (built_monotonic, payload)
_live_lite_cache = {}
_activity_cache = {}
_exit_performance_cache = {}
_wallet_mode_lock = threading.Lock()
ADMIN_WALLET_CREDENTIALS_FILE = "./secrets/polymarket_admin_credentials.json"
ADMIN_WALLET_PRIVATE_KEY_FILE = "./secrets/polymarket_admin_private_key.txt"
_fleet_cache = [0.0, None]  # (built_monotonic, payload)
_pnl_cache   = [0.0, None]  # (built_monotonic, payload)


def api_live(instance=None):
    # 1s whole-response cache: concurrent pollers (tabs, overlapping
    # refreshes) share one build instead of stacking duplicate query storms.
    key = instance or ""
    now = time.monotonic()
    hit = _live_cache.get(key)
    if hit and now - hit[0] < 1.0:
        return hit[1]
    mode = current_mode(instance)
    since = accounting_since(instance, mode)
    positions = reconstructed_positions(mode, since, instance)
    out = {
        "status": api_status(positions, since=since, instance=instance),
        "positions": positions,
        "orders": api_orders(90, instance),
        "settles": api_settles(40, instance),
        "markets": api_markets(60),
        "orderstats": api_orderstats(24),
        "source_pnl": api_source_pnl(instance),
        "health": api_health(),
        "tournament": api_tournament(),
        "instance": instance or "",
        "server_ts": int(time.time() * 1000),
    }
    _live_cache[key] = (now, out)
    return out


def build_operator_radar(status, positions_payload, markets_payload, health):
    alerts = []
    st_mode = str(status.get("mode") or "paper").lower()
    if st_mode == "live":
        alerts.append({
            "kind": "live",
            "level": "critical",
            "title": "LIVE TRADING",
            "detail": "Real-money mode is active",
        })
    if health.get("oracle_source") and health.get("oracle_source") != "chainlink":
        alerts.append({
            "kind": "oracle",
            "level": "warn",
            "title": "ORACLE FAILOVER",
            "detail": health.get("oracle_source"),
        })
    stale = status.get("health_stale")
    total = status.get("health_total")
    if stale:
        alerts.append({
            "kind": "books",
            "level": "warn" if stale < (total or 1) else "critical",
            "title": "STALE BOOKS",
            "detail": f"{stale}/{total or '?'} stale",
        })
    if health.get("parsefail"):
        alerts.append({
            "kind": "feed",
            "level": "warn",
            "title": "PARSE FAILURES",
            "detail": str(health.get("parsefail")),
        })
    positions = positions_payload.get("positions") or []
    for p in positions:
        sec = p.get("seconds_left")
        pnl_pct = p.get("pnl_pct")
        if sec is not None and sec <= 45 and not p.get("awaiting_settlement"):
            alerts.append({
                "kind": "timer",
                "level": "critical",
                "title": f"{p.get('asset') or '?'} {p.get('side') or ''} expiring",
                "detail": f"{max(0, int(sec))}s left",
                "cid": p.get("cid"),
            })
        if pnl_pct is not None and pnl_pct <= -20:
            alerts.append({
                "kind": "drawdown",
                "level": "warn",
                "title": f"{p.get('asset') or '?'} drawdown",
                "detail": f"{pnl_pct:.1f}%",
                "cid": p.get("cid"),
            })
    markets = markets_payload.get("markets") or []
    closing = [
        m for m in markets
        if m.get("phase") == "live" and m.get("window_close")
        and 0 < (m["window_close"] - int(time.time() * 1000)) < 90_000
    ][:5]
    for m in closing:
        left = max(0, int((m["window_close"] - int(time.time() * 1000)) / 1000))
        alerts.append({
            "kind": "market",
            "level": "info",
            "title": f"{m.get('asset') or '?'} {m.get('horizon_seconds') or ''} closing",
            "detail": f"{left}s",
            "cid": m.get("condition_id"),
        })
    order = {"critical": 0, "warn": 1, "info": 2}
    return sorted(alerts, key=lambda a: order.get(a.get("level"), 9))[:12]


def api_live_lite(instance=None):
    """Slim high-frequency payload for the cockpit paint loop."""
    key = instance or ""
    now = time.monotonic()
    hit = _live_lite_cache.get(key)
    if hit and now - hit[0] < 0.65:
        return hit[1]
    mode = current_mode(instance)
    since = accounting_since(instance, mode)
    positions = reconstructed_positions(mode, since, instance)
    status = api_status(positions, since=since, instance=instance)
    markets = api_markets(80)
    health = api_health()
    out = {
        "status": status,
        "positions": positions,
        "markets": markets,
        "health": health,
        "tournament": api_tournament(),
        "radar": build_operator_radar(status, positions, markets, health),
        "instance": instance or "",
        "server_ts": int(time.time() * 1000),
    }
    _live_lite_cache[key] = (now, out)
    return out


def api_exit_performance(instance=None, hours=24):
    """Cache matched exit-ledger P&L by bot, exit reason and execution route."""
    mode = current_mode(instance)
    cache_key = (instance or "", mode, int(hours))
    now = time.monotonic()
    hit = _exit_performance_cache.get(cache_key)
    if hit and now - hit[0] < 30.0:
        return hit[1]

    params = {
        "mode": mode,
        "from_ms": str(int((time.time() - int(hours) * 3600) * 1000)),
        "limit": "100",
    }
    groups = {}
    instance_groups = {}
    incomplete = 0
    scanned = 0
    truncated = False
    try:
        for _ in range(12):
            page = api_settlement_history(instance, params)
            scanned += int(page.get("scanned") or 0)
            for row in page.get("rows") or []:
                if not row.get("accounting_complete") or row.get("realized_pnl") is None:
                    incomplete += 1
                    continue
                source = str(row.get("source") or "unknown")
                if source.lower() == "settle":
                    route = "settlement"
                elif row.get("post_only") is True:
                    route = "maker"
                elif row.get("post_only") is False:
                    route = "taker"
                else:
                    route = "unknown"
                asset = str(row.get("asset") or "?").upper()
                horizon = int(row.get("horizon_seconds") or 0)
                key = (str(row.get("instance") or "unknown"), source, route,
                       asset, horizon)
                agg = groups.setdefault(key, {
                    "instance": key[0], "source": source, "route": route,
                    "asset": asset, "horizon_seconds": horizon,
                    "count": 0, "wins": 0, "losses": 0, "flats": 0, "net": 0.0,
                    "fees": 0.0, "winner_net": 0.0, "loser_net": 0.0,
                })
                instance_agg = instance_groups.setdefault(key[0], {
                    "instance": key[0], "count": 0, "wins": 0, "losses": 0,
                    "flats": 0,
                    "net": 0.0, "fees": 0.0,
                    "winner_net": 0.0, "loser_net": 0.0,
                })
                pnl = float(row["realized_pnl"])
                for target in (agg, instance_agg):
                    target["count"] += 1
                    if pnl > 0.0:
                        target["wins"] += 1
                        target["winner_net"] += pnl
                    elif pnl < 0.0:
                        target["losses"] += 1
                        target["loser_net"] += abs(pnl)
                    else:
                        target["flats"] += 1
                    target["net"] += pnl
                    target["fees"] += float(row.get("fee_usd") or 0.0)
            if not page.get("has_more"):
                break
            cursor = page.get("next_cursor")
            if not cursor:
                truncated = True
                break
            params["cursor"] = cursor
        else:
            truncated = True
    except Exception:
        result = {"mode": mode, "hours": int(hours), "groups": [],
                  "incomplete": incomplete, "truncated": truncated,
                  "error": "Exit ledger temporarily unavailable"}
        _exit_performance_cache[cache_key] = (now, result)
        return result

    def finalize(agg):
        agg["net"] = round(agg["net"], 2)
        agg["fees"] = round(agg["fees"], 2)
        winner_net = agg.pop("winner_net")
        loser_net = agg.pop("loser_net")
        avg_win = winner_net / agg["wins"] if agg["wins"] else None
        avg_loss = loser_net / agg["losses"] if agg["losses"] else None
        agg["avg_win"] = round(avg_win, 2) if avg_win is not None else None
        agg["avg_loss"] = round(avg_loss, 2) if avg_loss is not None else None
        agg["breakeven_win_pct"] = round(
            100.0 * avg_loss / (avg_win + avg_loss), 1
        ) if avg_win is not None and avg_loss is not None and avg_win + avg_loss > 0 else None
        return agg

    summary = [finalize(agg) for agg in groups.values()]
    summary.sort(key=lambda x: (
        x["net"], x["instance"], x["source"], x["asset"], x["horizon_seconds"]
    ))
    by_instance = [finalize(agg) for agg in instance_groups.values()]
    by_instance.sort(key=lambda x: (x["net"], x["instance"]))
    result = {"mode": mode, "hours": int(hours), "groups": summary,
              "by_instance": by_instance,
              "exits": sum(x["count"] for x in summary),
              "incomplete": incomplete, "scanned": scanned,
              "truncated": truncated,
              "as_of": datetime.now(timezone.utc).isoformat()}
    _exit_performance_cache[cache_key] = (now, result)
    return result


def api_activity(instance=None):
    """Slower activity payload with durable cross-mode trade history."""
    key = instance or ""
    now = time.monotonic()
    hit = _activity_cache.get(key)
    if hit and now - hit[0] < 1.5:
        return hit[1]
    try:
        exit_performance = api_exit_performance(instance)
    except Exception:
        exit_performance = {"groups": [], "error": "Exit ledger temporarily unavailable"}
    out = {
        "orders": api_orders(200, instance, all_modes=True),
        "settles": api_settles(100, instance, all_modes=True),
        "orderstats": api_orderstats(24),
        "source_pnl": api_source_pnl(instance),
        "exit_performance": exit_performance,
        "server_ts": int(time.time() * 1000),
    }
    _activity_cache[key] = (now, out)
    return out


def api_execution(instance=None):
    """Maker/taker control-plane snapshot for the execution console."""
    mode = current_mode(instance)
    inst_clause = "AND o.instance=%s" if instance else ""
    args = [mode] + ([instance] if instance else [])
    open_orders = q(f"""
        SELECT o.client_order_id::text, o.exchange_order_id, o.ts_submitted,
               o.instance, o.execution_mode, o.post_only, o.status, o.source,
               o.condition_id, o.token_id, o.side_label, o.direction,
               o.price, o.size_shares, o.notional_usd,
               COALESCE(SUM(f.size_shares),0) AS matched_shares,
               COALESCE(m.asset,'') AS asset,
               COALESCE(m.horizon_seconds,0) AS horizon_seconds,
               COALESCE(EXTRACT(EPOCH FROM m.window_close)*1000,0)::bigint AS window_close
        FROM vp.orders o
        LEFT JOIN vp.fills f ON f.client_order_id=o.client_order_id
        LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
        WHERE o.mode=%s AND o.status IN ('submitted','acked','partial','uncertain')
          AND (o.mode::text <> 'paper' OR EXISTS (
              SELECT 1 FROM vp.strategy_runs r
              WHERE r.instance IS NOT DISTINCT FROM o.instance
                AND r.session_id IS NOT DISTINCT FROM o.session_id
                AND r.ended_at IS NULL
          ))
          {inst_clause}
        GROUP BY o.client_order_id,m.asset,m.horizon_seconds,m.window_close
        ORDER BY o.ts_submitted DESC LIMIT 100
    """, tuple(args))
    stats = q(f"""
        SELECT execution_mode,
               COUNT(*) AS orders,
               COUNT(*) FILTER (WHERE status='filled') AS filled,
               COUNT(*) FILTER (WHERE status='partial') AS partial,
               COUNT(*) FILTER (WHERE status='cancelled') AS cancelled,
               COUNT(*) FILTER (WHERE status='rejected') AS rejected,
               COALESCE(SUM(notional_usd) FILTER (WHERE status='filled'),0) AS filled_usd,
               COALESCE(AVG(EXTRACT(EPOCH FROM (COALESCE(ts_terminal,now())-ts_submitted))*1000)
                        FILTER (WHERE status IN ('filled','cancelled','rejected')),0) AS avg_lifetime_ms
        FROM vp.orders o
        WHERE o.mode=%s AND o.ts_created>now()-interval '24 hours' {inst_clause}
        GROUP BY execution_mode ORDER BY execution_mode
    """, tuple(args))
    policy = {}
    if instance:
        try:
            cfg = api_config_read(instance).get("config", {})
            hot = ((cfg.get("strategy") or {}).get("hot") or {})
            for key in ("entry_order_mode", "maker_improve_ticks", "maker_reprice_ms",
                        "maker_poll_ms", "maker_max_age_ms",
                        "maker_cancel_before_close_ms"):
                policy[key] = hot.get(key)
        except Exception:
            pass
    return {"mode": mode, "instance": instance or "", "policy": policy,
            "open_orders": open_orders, "stats": stats,
            "server_ts": int(time.time() * 1000)}


# ── Extended APIs: configs, bot-control, cb-edge, performance, SSE ──────────

def api_configs():
    out = []
    try:
        for fn in sorted(os.listdir(CONFIG_DIR)):
            if fn.startswith("polypulse-") and fn.endswith(".toml"):
                name = fn[len("polypulse-"):-len(".toml")]
                path = os.path.join(CONFIG_DIR, fn)
                out.append({"name": name, "file": fn,
                             "mtime": int(os.path.getmtime(path) * 1000)})
    except Exception:
        pass
    return {"configs": out}


def _redact_sensitive_toml(raw_text):
    """Remove credential-bearing TOML tables before returning raw config text."""
    sensitive = {"wallet", "postgres", "polygon"}
    output = []
    hidden = False
    for line in raw_text.splitlines():
        match = re.match(r'^\s*\[([^\]]+)\]\s*(?:#.*)?$', line)
        if match:
            root_table = match.group(1).split('.', 1)[0].strip()
            hidden = root_table in sensitive
            if hidden:
                output.append(f"# [{root_table}] redacted")
            else:
                output.append(line)
            continue
        if not hidden:
            output.append(line)
    return "\n".join(output) + ("\n" if raw_text.endswith("\n") else "")


def api_config_read(name, full=True):
    if not name or not re.match(r'^[\w-]+$', name):
        return {"error": "invalid name"}
    path = os.path.join(CONFIG_DIR, f"polypulse-{name}.toml")
    if not os.path.exists(path):
        return {"error": "not found"}
    try:
        with open(path, "rb") as f:
            raw = f.read()
        t = _redact_config_tree(tomllib.loads(raw.decode()))
        tunable_keys, tunable_specs = _config_editor_catalog(t, full=full)
        return {"name": name, "config": t, "raw": _redact_sensitive_toml(raw.decode()),
                "tunable_keys": tunable_keys, "tunable_specs": tunable_specs,
                "full_editor": bool(full)}
    except Exception as e:
        return {"error": str(e)}


def _set_config_value(content, section, key, nv):
    """Line-based, section-aware value replace. `section` is the TOML table path
    (e.g. 'risk' or 'strategy.hot') or None for first-match-anywhere. Tracks the
    current [section] header while scanning so a key that appears in two tables
    (e.g. max_per_window_usd in both [strategy.hot] and [risk]) hits the right one.
    Preserves the inline annotation comment. Returns (content, matched_bool)."""
    lines = content.split('\n')
    cur = None
    pat = re.compile(rf'^(\s*{re.escape(key)}\s*=\s*)([^\n#]+?)([ \t]*(?:#[^\n]*)?)$')
    for i, line in enumerate(lines):
        hm = re.match(r'^\s*\[([^\]]+)\]\s*$', line)
        if hm:
            cur = hm.group(1)
            continue
        m = pat.match(line)
        if m and (section is None or cur == section):
            lines[i] = f"{m.group(1)}{nv}{m.group(3)}"
            return '\n'.join(lines), True
    return content, False


def api_config_write(name, updates, allow_server_keys=False, allow_all_config=False):
    if not re.match(r'^[\w-]+$', name):
        return (400, {"error": "invalid name"})
    path = os.path.join(CONFIG_DIR, f"polypulse-{name}.toml")
    if not os.path.exists(path):
        return (404, {"error": "config not found"})
    try:
        with open(path) as f:
            content = f.read()
        parsed = tomllib.loads(content)
    except Exception as e:
        return (500, {"error": str(e)})
    changed = []
    rejected = {}
    for raw_key, val in updates.items():
        # Keys may be section-qualified ("risk.max_per_window_usd") to target a
        # specific TOML table; validate against the bare key name.
        if '.' in raw_key:
            section, _, key = raw_key.rpartition('.')
        else:
            section, key = None, raw_key
        if key == "daily_loss_kill_pct":
            try:
                disabled_value = float(val) == 0.0
            except (TypeError, ValueError):
                disabled_value = False
            if not disabled_value:
                rejected[raw_key] = "daily loss kill is disabled fleet-wide by operator policy"
                continue
        # Admins may edit every TOML scalar/list already present in the selected
        # file. Credential reads are blank, but supplied replacements write here.
        # Other users retain the explicit strategy catalog.
        ok = False
        coerced = None
        err = "locked: server-side only"
        if allow_all_config and section is not None:
            found, current = _config_value_at(parsed, raw_key)
            if found and not isinstance(current, dict):
                ok, coerced, err = _validate_config_spec(
                    _config_spec_for_value(key, current), val)
            else:
                err = "key not present in this config"
        else:
            ok, coerced, err = validate_tunable(key, val)
        if not ok and allow_server_keys:
            ok, coerced, forced_section, err = validate_server_config_value(key, val)
            if ok:
                section = forced_section
        if not ok:
            rejected[raw_key] = err
            continue
        nv = toml_scalar(coerced)
        content, n = _set_config_value(content, section, key, nv)
        if n:
            changed.append(raw_key)
        else:
            rejected[raw_key] = "key not present in this config"
    if rejected:
        return (422, {"ok": False, "changed": [], "rejected": rejected})
    if changed:
        # The legacy editor uses the same all-or-nothing write discipline as v3.
        tomllib.loads(content)
        st = os.stat(path)
        fd, tmp = tempfile.mkstemp(prefix=".cockpit-config-", dir=CONFIG_DIR)
        try:
            os.fchmod(fd, stat.S_IMODE(st.st_mode))
            with os.fdopen(fd, "w") as f:
                f.write(content)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
        invalidate_runtime_caches()
    return (200, {"ok": True, "changed": changed, "rejected": {}})


def api_bot_control(action, inst, strategy_id=None, config_name=None,
                    is_admin=False, confirm_live=False):
    if not re.match(r'^[\w-]+$', inst or ""):
        return (400, {"error": "invalid instance"})
    if action == "recall_strategy":
        if not strategy_id or not re.match(r'^\d+$', str(strategy_id)):
            return (400, {"error": "strategy_id required (integer)"})
        script = os.path.join(ROOT, "scripts", "supervisor_brain.py")
        try:
            r = subprocess.run(
                ["python3", script, "--instance", inst,
                 "--apply-strategy", str(strategy_id)],
                capture_output=True, text=True, timeout=15)
            _since_cache.clear()
            return (200, {"ok": r.returncode == 0, "strategy_id": strategy_id,
                          "output": r.stdout[:300], "stderr": r.stderr[:200]})
        except Exception as e:
            return (500, {"error": str(e)})
    if action == "swap_config":
        if not config_name or not re.match(r'^[\w.-]+$', config_name):
            return (400, {"error": "config_name required (alphanumeric/dash/dot)"})
        src = os.path.join(CONFIG_DIR, f"{config_name}.toml")
        if not os.path.exists(src):
            return (404, {"error": f"config '{config_name}.toml' not found"})
        dst = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
        # Stop the bot if running
        was_running = instance_running(inst)
        if is_admin and current_mode(inst).lower() == "live" and confirm_live is not True:
            return (422, {"error": "Swapping a LIVE bot configuration requires explicit confirm_live=true"})
        if was_running:
            subprocess.run(["systemctl", "--user", "--no-block", "stop", f"polypulse-bot@{inst}.service"],
                           capture_output=True, text=True, timeout=10, env=_SYSTEMD_ENV)
            time.sleep(0.5)
        # Preserve per-bot runtime mode and keep ADMIN credentials pointed at
        # the shared wallet when a strategy profile is swapped.
        try:
            with open(src, "r", encoding="utf-8") as f:
                content = f.read()
            if is_admin:
                previous = {}
                if os.path.isfile(dst):
                    with open(dst, "rb") as f:
                        previous = tomllib.load(f)
                prior_mode = previous.get("runtime", {}).get("mode", "paper")
                prior_balance = previous.get("wallet", {}).get("starting_balance_usd")
                content = _upsert_config_value(content, "runtime", "mode", prior_mode)
                content = _upsert_config_value(content, "runtime", "instance_label", inst)
                if not previous:
                    content = _upsert_config_value(content, "strategy.hot", "autopilot_on_start", False)
                if prior_balance is not None:
                    content = _upsert_config_value(content, "wallet", "starting_balance_usd", prior_balance)
                content = _admin_shared_wallet_config(content)
            tomllib.loads(content)
            with open(dst, "w", encoding="utf-8") as f:
                f.write(content)
                f.flush()
                os.fsync(f.fileno())
        except OSError as e:
            return (500, {"error": f"copy failed: {e}"})
        # Track the swap origin so the fleet view shows the right label
        _write_swap_origin(inst, config_name)
        # Restart the bot with the new config (only if it was running)
        r = None
        if was_running:
            r = subprocess.run(["systemctl", "--user", "--no-block", "start", f"polypulse-bot@{inst}.service"],
                               capture_output=True, text=True, timeout=5, env=_SYSTEMD_ENV)
            ok = r.returncode == 0
        else:
            ok = True
        _bot_status_cache[0] = 0.0
        _since_cache.clear()
        return (200, {"ok": ok, "action": "swap_config", "instance": inst,
                      "config": config_name, "was_running": was_running,
                      "stderr": r.stderr[:200] if r and not ok else None})
    if action == "delete":
        # DELETE BOT: stop the unit (+ its supervisor), clear failed state, and
        # archive its config aside so the instance disappears from the fleet.
        # The config is moved to polypulse-<inst>.toml.deleted-<ts> (recoverable),
        # NOT hard-deleted, so a fat-finger is reversible.
        PROTECTED = {"cb", "scalp", "conv"}
        if inst in PROTECTED:
            return (403, {"error": f"'{inst}' is a protected core bot — deletion is disabled "
                                   f"to prevent accidents (remove it manually if you really mean to)"})
        svc = f"polypulse-bot@{inst}.service"
        sup = f"polypulse-supervisor@{inst}.service"
        for s in (svc, sup):
            subprocess.run(["systemctl", "--user", "--no-block", "stop", s],
                           capture_output=True, text=True, timeout=10, env=_SYSTEMD_ENV)
        time.sleep(0.6)
        for s in (svc, sup):
            subprocess.run(["systemctl", "--user", "reset-failed", s],
                           capture_output=True, text=True, timeout=10, env=_SYSTEMD_ENV)
        cfg = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
        archived = None
        if os.path.exists(cfg):
            ts = time.strftime("%Y%m%d_%H%M%S")
            dst = f"{cfg}.deleted-{ts}"
            try:
                os.rename(cfg, dst)
                archived = os.path.basename(dst)
            except OSError as e:
                return (500, {"error": f"stopped '{inst}' but failed to archive its config: {e}"})
        # drop the config-origin marker if present
        try:
            origin = _swap_origin_path(inst)
            if os.path.exists(origin):
                os.remove(origin)
        except OSError:
            pass
        _bot_status_cache[0] = 0.0
        _since_cache.clear()
        print(f"[cockpit] delete: stopped polypulse-bot@{inst}, archived config -> {archived}")
        return (200, {"ok": True, "action": "delete", "instance": inst,
                      "config_archived": archived})
    if action not in ("start", "stop", "restart"):
        return (400, {"error": "action must be start|stop|restart|delete|recall_strategy|swap_config"})

    # LAUNCH NEW BOT: starting an instance that has no config file yet. The
    # "Launch New Bot" modal passes the chosen base config as config_name; clone
    # it to config/polypulse-<inst>.toml so the service can actually boot. Without
    # this, systemd starts polypulse-bot@<inst> with no config, the bot ABRTs on
    # startup, auto-restart-loops, and never registers on the dashboard.
    if action in ("start", "restart"):
        inst_cfg = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
        if not os.path.exists(inst_cfg):
            if not config_name:
                return (400, {"error": f"no config/polypulse-{inst}.toml exists and no base "
                                       f"config was selected — pick a STRATEGY CONFIG to clone"})
            # Accept both conventions: bare ("cb") and full stem ("polypulse-cb").
            base = config_name[len("polypulse-"):] if config_name.startswith("polypulse-") else config_name
            if not re.match(r'^[\w.-]+$', base):
                return (400, {"error": "invalid config_name"})
            src = os.path.join(CONFIG_DIR, f"polypulse-{base}.toml")
            if not os.path.exists(src):
                return (404, {"error": f"base config 'polypulse-{base}.toml' not found"})
            try:
                with open(src, "r", encoding="utf-8") as f:
                    content = f.read()
                if is_admin:
                    content = _upsert_config_value(content, "runtime", "mode", "paper")
                    content = _upsert_config_value(content, "runtime", "instance_label", inst)
                    content = _upsert_config_value(content, "strategy.hot", "autopilot_on_start", False)
                    content = _admin_shared_wallet_config(content)
                    tomllib.loads(content)
                    fd, tmp = tempfile.mkstemp(prefix=".admin-bot-", dir=CONFIG_DIR, text=True)
                    try:
                        os.fchmod(fd, 0o600)
                        with os.fdopen(fd, "w", encoding="utf-8") as f:
                            f.write(content)
                            f.flush()
                            os.fsync(f.fileno())
                        os.replace(tmp, inst_cfg)
                    finally:
                        if os.path.exists(tmp):
                            os.unlink(tmp)
                else:
                    import shutil
                    shutil.copy2(src, inst_cfg)
            except OSError as e:
                return (500, {"error": f"failed to create config for '{inst}': {e}"})
            _write_swap_origin(inst, base)  # fleet view labels it with its origin
            print(f"[cockpit] launch: provisioned config/polypulse-{inst}.toml from polypulse-{base}.toml")

    if action in ("start", "restart") and current_mode(inst).lower() == "live" and confirm_live is not True:
        return (422, {"error": "Starting or restarting LIVE requires explicit confirm_live=true"})

    svc = f"polypulse-bot@{inst}.service"
    try:
        r = subprocess.run(["systemctl", "--user", "--no-block", action, svc],
                           capture_output=True, text=True, timeout=5, env=_SYSTEMD_ENV)
        _bot_status_cache[0] = 0.0
        _since_cache.clear()
        return (200, {"ok": r.returncode == 0, "action": action, "service": svc,
                      "stderr": r.stderr[:200]})
    except Exception as e:
        return (500, {"error": str(e)})


def api_supervisor_control(action, inst):
    if action not in ("start", "stop", "restart"):
        return (400, {"error": "action must be start|stop|restart"})
    if not re.match(r'^[\w-]+$', inst):
        return (400, {"error": "invalid instance"})
    svc = f"polypulse-supervisor@{inst}.service"
    try:
        r = subprocess.run(["systemctl", "--user", "--no-block", action, svc],
                           capture_output=True, text=True, timeout=5, env=_SYSTEMD_ENV)
        return (200, {"ok": r.returncode == 0, "action": action, "service": svc,
                      "stderr": r.stderr[:200]})
    except Exception as e:
        return (500, {"error": str(e)})


def api_cb_edge_live():
    out = {}
    for a in ASSETS:
        orc = q("SELECT price,ts FROM vp.oracle_ticks WHERE asset=%s ORDER BY ts DESC LIMIT 1", (a,), one=True)
        cb  = q("SELECT price,ts FROM vp.coinbase_ticks WHERE asset=%s ORDER BY ts DESC LIMIT 1", (a,), one=True)
        op = float(orc["price"]) if orc else None
        cp = float(cb["price"])  if cb  else None
        d  = round((cp - op) / op * 100, 4) if (op and cp and op > 0) else None
        out[a] = {"oracle": op, "oracle_ts": orc.get("ts") if orc else None,
                  "coinbase": cp, "coinbase_ts": cb.get("ts") if cb else None,
                  "delta_pct": d}
    try:
        lag = q("""SELECT asset,
                          ROUND(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY
                            EXTRACT(EPOCH FROM (o.ts - c.ts)))::numeric, 2) AS median_lag,
                          COUNT(*) AS samples
                   FROM vp.coinbase_ticks c
                   JOIN LATERAL (
                     SELECT ts FROM vp.oracle_ticks
                     WHERE asset=c.asset AND ts BETWEEN c.ts AND c.ts+interval '15 seconds'
                     ORDER BY ts ASC LIMIT 1
                   ) o ON true
                   WHERE c.ts > now() - interval '1 hour'
                   GROUP BY asset""")
        for r in lag:
            if r["asset"] in out:
                out[r["asset"]]["median_lag"] = r["median_lag"]
                out[r["asset"]]["lag_samples"] = r["samples"]
    except Exception:
        pass
    return {"assets": out}


def api_signal_history(limit=30):
    mode = current_mode()
    since = paper_since(None)
    if not since:
        return {"signals": []}
    rows = q("""
        WITH entries AS (
          SELECT o.client_order_id, o.condition_id, o.ts_created,
                 o.side_label, f.price AS entry_price, f.size_shares,
                 o.instance, m.asset, o.source
          FROM vp.orders o
          JOIN vp.fills f ON f.client_order_id=o.client_order_id
          LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
          WHERE o.mode=%s AND o.direction='buy' AND o.status='filled'
            AND o.ts_created >= %s::timestamptz
          ORDER BY o.ts_created DESC LIMIT %s
        )
        SELECT e.*,
               sx.price AS settle_price,
               CASE WHEN sx.price > 0.5 THEN 'W'
                    WHEN sx.price IS NOT NULL THEN 'L' ELSE NULL END AS outcome
        FROM entries e
        LEFT JOIN LATERAL (
          SELECT f2.price FROM vp.orders o2
          JOIN vp.fills f2 ON f2.client_order_id=o2.client_order_id
          WHERE o2.condition_id=e.condition_id AND o2.source='settle'
            AND o2.status='filled' LIMIT 1
        ) sx ON true
        ORDER BY e.ts_created DESC
    """, (mode, since, limit))
    return {"signals": rows}


def api_perf_stats():
    mode = current_mode()
    since = paper_since(None)
    if not since:
        return {"stats": {}}
    row = q("""
        SELECT
          COUNT(*) FILTER (WHERE o.direction='buy') AS entries,
          COUNT(*) FILTER (WHERE o.source='settle' AND f.price>0.5) AS wins,
          COUNT(*) FILTER (WHERE o.source='settle' AND f.price<=0.5) AS losses,
          COALESCE(MAX(f.price*f.size_shares) FILTER (WHERE o.source='settle' AND f.price>0.5),0) AS best_win,
          COALESCE(SUM(f.fee_usd),0) AS total_fees,
          COALESCE(SUM(CASE WHEN o.direction='buy' THEN -f.price*f.size_shares
                            ELSE f.price*f.size_shares END) - SUM(f.fee_usd), 0) AS net_pnl
        FROM vp.orders o
        JOIN vp.fills f ON f.client_order_id=o.client_order_id
        WHERE o.mode=%s AND o.status='filled' AND f.ts >= %s::timestamptz
    """, (mode, since), one=True) or {}
    return {"stats": dict(row)}


def api_heatmap():
    mode = current_mode()
    since = paper_since(None)
    if not since:
        return {"heatmap": []}
    rows = q("""
        WITH buys AS (
          SELECT o.condition_id, o.token_id,
                 SUM(f.price * f.size_shares) / NULLIF(SUM(f.size_shares), 0) AS avg_price
          FROM vp.orders o JOIN vp.fills f ON f.client_order_id=o.client_order_id
          WHERE o.mode=%s AND o.direction='buy' AND o.status='filled'
            AND f.ts >= %s::timestamptz
          GROUP BY o.condition_id, o.token_id
        )
        SELECT EXTRACT(HOUR FROM f.ts AT TIME ZONE 'America/New_York')::int AS hour,
               COUNT(*) FILTER (WHERE
                 (o.source='settle' AND f.price>0.5) OR
                 (o.source!='settle' AND f.price >= COALESCE(b.avg_price, f.price))
               ) AS wins,
               COUNT(*) FILTER (WHERE
                 (o.source='settle' AND f.price<=0.5) OR
                 (o.source!='settle' AND f.price < COALESCE(b.avg_price, f.price))
               ) AS losses
        FROM vp.orders o
        JOIN vp.fills f ON f.client_order_id=o.client_order_id
        LEFT JOIN buys b ON b.condition_id=o.condition_id AND b.token_id=o.token_id
        WHERE o.mode=%s AND o.status='filled' AND o.direction='sell'
          AND f.ts >= %s::timestamptz
        GROUP BY 1 ORDER BY 1
    """, (mode, since, mode, since))
    return {"heatmap": rows}


def api_supervisor_log(limit=20):
    try:
        rows = q("""SELECT ts, trigger, model, LEFT(response,800) AS response,
                           latency_ms, error
                    FROM vp.llm_exchanges
                    ORDER BY ts DESC LIMIT %s""", (limit,))
    except Exception:
        rows = []
    return {"exchanges": rows}


def api_pnl_by_instance():
    now = time.monotonic()
    if _pnl_cache[1] and now - _pnl_cache[0] < 3.0:
        return _pnl_cache[1]
    mode = current_mode()
    names = instance_names()
    all_insts = fleet_instances()
    # fallback since: earliest paper fill in the last 48h (covers the case where
    # session_start is indeterminate but bots are clearly running)
    _fallback_since = None
    if mode.lower() == "paper":
        # ::text so _clean() keeps it a castable string (not epoch-ms int)
        row = q("""SELECT MIN(ts_created)::text AS t FROM vp.orders
                   WHERE mode='paper' AND status='filled'
                     AND ts_created > now() - interval '48 hours'""", one=True)
        _fallback_since = row.get("t") if row else None
    out = {}
    for inst in all_insts:
        since = normalize_since(paper_since(inst) or _fallback_since)
        if not since:
            continue
        rows = q("""
            SELECT DATE_TRUNC('minute', f.ts) AS ts,
                   SUM(CASE WHEN o.direction='buy' THEN -f.price*f.size_shares
                            ELSE f.price*f.size_shares END
                       - COALESCE(f.fee_usd,0)) AS delta
            FROM vp.orders o
            JOIN vp.fills f ON f.client_order_id=o.client_order_id
            WHERE o.mode=%s AND o.status='filled' AND o.instance=%s
              AND f.ts >= %s::timestamptz
            GROUP BY 1 ORDER BY 1
        """, (mode, inst, since))
        if rows:
            cum, pts = 0.0, []
            for r in rows:
                cum += float(r["delta"] or 0)
                pts.append({"ts": r["ts"], "pnl": round(cum, 2)})
            out[inst] = {"name": names.get(inst, inst), "points": pts}
    result = {"instances": out}
    _pnl_cache[0], _pnl_cache[1] = time.monotonic(), result
    return result


def _supervisor_running(inst):
    try:
        r = subprocess.run(["systemctl", "--user", "is-active",
                            f"polypulse-supervisor@{inst}.service"],
                           capture_output=True, text=True, timeout=5, env=_SYSTEMD_ENV)
        if r.stdout.strip() == "active":
            return True
    except Exception:
        pass
    try:  # fallback: manually launched supervisor
        r = subprocess.run(["pgrep", "-f", f"supervisor_brain.py --instance {inst}( |$)"],
                           capture_output=True, text=True, timeout=5)
        return bool(r.stdout.strip())
    except Exception:
        return False


def _models_env():
    out = {}
    try:
        for ln in open(MODELS_ENV):
            m = re.match(r'(MODEL|NAME|EXTRA)_(\w+)=(.+)', ln.strip())
            if m:
                out.setdefault(m.group(2), {})[m.group(1).lower()] = m.group(3).strip()
    except OSError:
        pass
    return out


def _strategy_summary(inst):
    """Human-readable strategy chips from the instance's TOML config."""
    path = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
    try:
        with open(path, "rb") as f:
            t = tomllib.load(f)
    except Exception:
        return {}
    st = t.get("strategy", {}).get("hot", {})
    risk = t.get("risk", {})
    scalp = bool(st.get("scalp_v3_enabled"))
    signals = []
    if st.get("coinbase_lead_mode"):
        signals.append(f"CB≥{st.get('coinbase_lead_min_delta_pct', '?')}%")
    if st.get("oracle_lag_mode"):
        signals.append("ORACLE-LAG")
    if scalp:
        signals.append("SCALP-V3")
    if st.get("predictor_mode"):
        signals.append("PREDICTOR")
    exits = []
    if scalp:
        if st.get("scalp_v3_reversal_exit"): exits.append("REVERSAL")
        if st.get("scalp_v3_trailing_stop"): exits.append(f"TRAIL-{st.get('scalp_v3_trail_start_loss','?')}")
        if st.get("scalp_v3_profit_take"): exits.append(f"TP@{st.get('scalp_v3_profit_target','?')}")
    else:
        if st.get("profit_take_mode"): exits.append("PROFIT-TAKE")
        if st.get("stop_loss_mode"): exits.append(f"SL-{st.get('stop_loss_pct','?')}%")
        if not exits: exits.append("HOLD→SETTLE")
    lo = st.get("scalp_v3_min_price") if scalp else st.get("min_entry_share_price")
    hi = st.get("scalp_v3_max_price") if scalp else st.get("max_entry_share_price")
    per = st.get("scalp_v3_per_entry_usd") if scalp else st.get("per_click_size_usd")
    return {
        "band": f"{lo}–{hi}" if lo is not None else "?",
        "per_entry_usd": per,
        "max_window_usd": risk.get("max_per_window_usd"),
        "max_deployed_usd": risk.get("max_deployed_usd"),
        "signals": signals,
        "exits": exits,
        "blocked": st.get("blocked_assets") or [],
        "entries_per_window": st.get("scalp_v3_max_entries_per_window") if scalp
                              else st.get("max_entries_per_window"),
        "stake": float(t.get("wallet", {}).get("starting_balance_usd") or 0),
    }


def api_fleet_detail():
    """Everything the fleet tab needs in one call."""
    now = time.monotonic()
    if _fleet_cache[1] and now - _fleet_cache[0] < 3.0:
        return _fleet_cache[1]
    names = instance_names()
    env = _models_env()
    all_insts = fleet_instances()
    # Available configs: all .toml files that are NOT timestamped backups
    # (backups match polypulse-*-YYYYMMDD*.toml or *-YYYYMMDD_*.toml patterns)
    _backup_re = re.compile(r'-\d{8}')
    avail_configs = []
    try:
        for fn in sorted(os.listdir(CONFIG_DIR)):
            if not fn.endswith(".toml"):
                continue
            name = fn[:-len(".toml")]
            if _backup_re.search(name):
                continue  # skip timestamped backups
            avail_configs.append({"name": name, "file": fn})
    except Exception:
        pass
    out = []
    for inst in all_insts:
        if not os.path.exists(os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")):
            continue
        sup_run = _supervisor_running(inst)
        last = q("""SELECT s.strategy_id, s.llm_model, s.reason, s.created_at
                    FROM vp.strategies s WHERE s.instance=%s
                    ORDER BY s.created_at DESC LIMIT 1""", (inst,))
        nstrat = q("SELECT COUNT(*) AS n FROM vp.strategies WHERE instance=%s", (inst,))
        recent = q("""SELECT strategy_id, LEFT(reason,60) AS reason
                      FROM vp.strategies WHERE instance=%s
                      ORDER BY created_at DESC LIMIT 12""", (inst,))
        out.append({
            "instance": inst,
            "name": names.get(inst, inst),
            "running": instance_running(inst),
            "current_config": _read_swap_origin(inst) or f"polypulse-{inst}",  # resolved source, not just the file name
            "strategy": _strategy_summary(inst),
            "supervisor": {
                "running": sup_run,
                "model": env.get(inst, {}).get("model"),
                "extra": env.get(inst, {}).get("extra"),
                "last_decision": last[0] if last else None,
                "strategies_total": nstrat[0]["n"] if nstrat else 0,
            },
            "recent_strategies": [{"id": r["strategy_id"], "reason": r.get("reason") or ""} for r in recent],
            "wallet": _wallet_status_for(inst) or {},
        })
    result = {"fleet": out, "available_configs": avail_configs}
    _fleet_cache[0], _fleet_cache[1] = time.monotonic(), result
    return result


_window_bot_desk_cache = {}
_WINDOW_BOT_IDS = {
    "btc-5m-article-sim-test",
    "btc-5m-ofi-impulse-paper-test",
    "btc-5m-lead-scalper",
    "btc-15m-lead-scalper",
    "btc-15m-conditional-edge-candidate",
    "btc-5m-lead-scalper-rr-test",
    "btc-15m-lead-scalper-rr-test",
    "btc-5m-lead-scalper-stop15-test",
    "btc-15m-lead-scalper-stop15-test",
    "btc-5m-lead-scalper-stop10-tp20-test",
    "btc-15m-lead-scalper-stop10-tp20-test",
    "btc-5m-lead-scalper-stop10-tp50-test",
    "btc-15m-lead-scalper-stop10-tp50-test",
    "btc-5m-lead-scalper-stop15-tp20-reversal-test",
    "btc-15m-lead-scalper-stop15-tp20-reversal-test",
    "btc-5m-lead-scalper-stop15-tp20-reversal-oracle-test",
    "btc-15m-lead-scalper-stop15-tp20-reversal-oracle-test",
    "btc-5m-lead-scalper-stop15-tp20-test",
    "btc-15m-lead-scalper-stop15-tp20-test",
    "btc-late-settlement-sniper",
}
_WINDOW_BOT_ASSETS = ("BTC", "ETH", "SOL", "XRP")


def api_window_bot_desk(allowed_instances=None):
    """Live operational view for the window-bot profiles and PAPER challengers."""
    scope = None if allowed_instances is None else tuple(sorted(allowed_instances))
    cache_key = ("admin",) if scope is None else ("user", *scope)
    now = time.monotonic()
    hit = _window_bot_desk_cache.get(cache_key)
    if hit and now - hit[0] < 1.5:
        return hit[1]

    fleet_rows = {x["instance"]: x for x in api_fleet_detail().get("fleet", [])}
    names = sorted((_WINDOW_BOT_IDS & set(fleet_rows)) if scope is None
                   else (_WINDOW_BOT_IDS & set(fleet_rows) & set(scope)))
    bots = []
    for inst in names:
        path = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
        try:
            with open(path, "rb") as stream:
                cfg = tomllib.load(stream)
        except Exception:
            continue
        hot = cfg.get("strategy", {}).get("hot", {})
        blocked_assets = {str(asset).upper() for asset in hot.get("blocked_assets", [])}
        bot_assets = [asset for asset in _WINDOW_BOT_ASSETS if asset not in blocked_assets]
        if hot.get("lead_lag_scalp_impulse_only"):
            bot_assets = ["BTC"]
        mode = str(cfg.get("runtime", {}).get("mode", "unknown")).lower()
        horizons = cfg.get("polymarket", {}).get("universe_horizons_seconds", [])
        fleet_row = fleet_rows[inst]
        since = accounting_since(inst, mode)
        positions = reconstructed_positions(mode, since, inst)
        accounting = ledger_accounting(mode, positions, since, inst)
        execution = api_execution(inst)
        exit_review = api_exit_performance(inst, 24)
        exit_total = next((x for x in (exit_review.get("by_instance") or [])
                           if x.get("instance") == inst), None)
        exit_24h = {
            "count": int((exit_total or {}).get("count") or 0),
            "wins": int((exit_total or {}).get("wins") or 0),
            "losses": int((exit_total or {}).get("losses") or 0),
            "net": float((exit_total or {}).get("net") or 0.0),
            "fees": float((exit_total or {}).get("fees") or 0.0),
            "avg_win": (float(exit_total["avg_win"])
                        if exit_total and exit_total.get("avg_win") is not None else None),
            "avg_loss": (float(exit_total["avg_loss"])
                         if exit_total and exit_total.get("avg_loss") is not None else None),
            "breakeven_win_pct": (float(exit_total["breakeven_win_pct"])
                                  if exit_total and exit_total.get("breakeven_win_pct") is not None else None),
            "incomplete": int(exit_review.get("incomplete") or 0),
            "available": not bool(exit_review.get("error")),
        }
        recent = api_orders(12, inst, all_modes=True).get("orders", [])
        if scope is not None:
            for order in recent:
                order.pop("exchange_order_id", None)
        # Keep enough history to retain a loss-pause event for the duration of
        # the configured cooldown, even while per-market scan logs are busy.
        log_lines = tail(instance_log(inst), 6000)
        # A restarted bot can leave less than a minute of old gate messages in
        # the same log tail. Anchor diagnostics to this process run so the
        # cockpit never mixes yesterday's/current-session filters together.
        def log_timestamp(line):
            match = re.match(
                r"^\[(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2}\.\d{3})\]", line
            )
            if not match:
                return None
            try:
                return datetime.strptime(
                    f"{match.group(1)} {match.group(2)}", "%Y-%m-%d %H:%M:%S.%f"
                )
            except ValueError:
                return None

        process_started_at = None
        for line in reversed(log_lines):
            if "tui: headless run loop active" not in line:
                continue
            process_started_at = log_timestamp(line)
            break
        # Loss-streak cooldowns are intentionally silent in the scan loop.
        # Surface the current process's last pause event so operators can tell
        # a deliberate entry pause from a stalled strategy/feed.
        entry_pause = None
        pause_now_ms = int(time.time() * 1000)
        for line in reversed(log_lines):
            if "autopilot paused after " not in line:
                continue
            pause_ts = log_timestamp(line)
            if process_started_at is not None and (
                    pause_ts is None or pause_ts < process_started_at):
                continue
            pause_match = re.search(
                r"autopilot paused after (\d+) consecutive loss\(es\) until (\d+)",
                line,
            )
            if not pause_match:
                continue
            pause_until_ms = int(pause_match.group(2))
            remaining_s = max(0, (pause_until_ms - pause_now_ms + 999) // 1000)
            entry_pause = {
                "active": pause_until_ms > pause_now_ms,
                "consecutive_losses": int(pause_match.group(1)),
                "until_ms": pause_until_ms,
                "remaining_s": remaining_s,
                "age_s": (round(max(0.0, (datetime.now(timezone.utc).replace(tzinfo=None)
                                         - pause_ts).total_seconds()), 1)
                          if pause_ts is not None else None),
            }
            break
        # Each scalper writes a second WINDOW_BOT startup line for impulse
        # shadow mode. Prefer the newest state-bearing line so that auxiliary
        # telemetry cannot hide the bot's actual monitoring/entry state.
        engine_line = next((line for line in reversed(log_lines)
                            if "WINDOW_BOT " in line and " state=" in line
                            and (process_started_at is None or
                                 (log_timestamp(line) is not None and
                                  log_timestamp(line) >= process_started_at))), "")
        engine_state = None
        engine_match = re.search(
            r"WINDOW_BOT asset=(BTC|ETH|SOL|XRP) horizon=(300|900) "
            r"state=([a-z_]+) source=([^ ]+) reason=([a-z0-9_.]+)(?: t=(\d+)s)?",
            engine_line,
        )
        match = re.search(r"WINDOW_BOT .*state=([^ ]+)", engine_line)
        if match:
            engine_state = match.group(1)
        configured_horizon = int(hot.get("lead_lag_scalp_horizon_s") or 0) \
            if hot.get("lead_lag_scalp_enabled") else 0
        gate_horizons = {configured_horizon} if configured_horizon else {
            int(value) for value in horizons if str(value).isdigit()
        }
        gate_now = datetime.now(timezone.utc).replace(tzinfo=None)
        gate_checks = []
        latest_gate_by_asset = {}
        for line in reversed(log_lines):
            gate_match = re.search(
                r"^\[(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2}\.\d{3})\].*?"
                r"WINDOW_BOT asset=(BTC|ETH|SOL|XRP) horizon=(300|900) "
                r"state=([a-z_]+) source=([^ ]+) reason=([a-z0-9_.]+)(?: t=(\d+)s)?",
                line,
            )
            if not gate_match:
                continue
            gate_ts = log_timestamp(line)
            if gate_ts is None:
                continue
            if process_started_at is not None and gate_ts < process_started_at:
                continue
            horizon_s = int(gate_match.group(4))
            if gate_horizons and horizon_s not in gate_horizons:
                continue
            age_s = max(0.0, (gate_now - gate_ts).total_seconds())
            gate = {
                "asset": gate_match.group(3), "horizon_seconds": horizon_s,
                "state": gate_match.group(5), "source": gate_match.group(6),
                "reason": gate_match.group(7),
                "seconds_left": int(gate_match.group(8)) if gate_match.group(8) else None,
                "age_s": round(age_s, 1),
            }
            if age_s <= 60.0:
                gate_checks.append(gate)
            if gate["asset"] not in latest_gate_by_asset:
                latest_gate_by_asset[gate["asset"]] = gate
        gate_reason_counts = {}
        for gate in gate_checks:
            if gate["state"] == "no_entry":
                gate_reason_counts[gate["reason"]] = gate_reason_counts.get(gate["reason"], 0) + 1
        top_gate_blockers = [
            {"reason": reason, "count": count}
            for reason, count in sorted(gate_reason_counts.items(), key=lambda item: (-item[1], item[0]))[:3]
        ]
        entry_gate = {
            "checks_60s": len(gate_checks),
            "no_entry_60s": sum(gate["state"] == "no_entry" for gate in gate_checks),
            "top_blockers": top_gate_blockers,
            "latest_by_asset": list(latest_gate_by_asset.values()),
            "age_s": min((gate["age_s"] for gate in latest_gate_by_asset.values()), default=None),
        }
        market_feed_state = "unknown"
        market_feed_frames = None
        discovered_markets = None
        for line in reversed(log_lines):
            if market_feed_frames is None and "pm-md frames:" in line:
                frames_match = re.search(r"pm-md frames: total=(\d+).*connected=(true|false)", line)
                if frames_match:
                    market_feed_frames = int(frames_match.group(1))
                    market_feed_state = "connected" if frames_match.group(2) == "true" else "disconnected"
            if discovered_markets is None and "gamma discovered " in line:
                markets_match = re.search(r"gamma discovered (\d+) up-down markets", line)
                if markets_match:
                    discovered_markets = int(markets_match.group(1))
            if market_feed_frames is not None and discovered_markets is not None:
                break
        if market_feed_state == "unknown":
            for line in reversed(log_lines):
                state_match = re.search(r"pm-md ws state: (connected|disconnected)", line)
                if state_match:
                    market_feed_state = state_match.group(1)
                    break
        open_orders = execution.get("open_orders", [])
        if scope is not None:
            for order in open_orders:
                order.pop("exchange_order_id", None)
        is_scalper = bool(hot.get("lead_lag_scalp_enabled"))
        horizon = int(hot.get("lead_lag_scalp_horizon_s") or 0)
        sniper = bool(hot.get("late_window_sniper_enabled"))
        article_simulation = bool(hot.get("simulation_strategy_enabled")) and mode == "paper"
        settlement_only = sniper and not is_scalper
        if is_scalper:
            scalp_horizon_label = f"{horizon // 60}m" if horizon else "5m+15m"
            strategy_name = (
                f"Binance OFI Impulse · BTC · {scalp_horizon_label} · research challenger"
                if hot.get("lead_lag_scalp_impulse_only") else
                f"Coinbase Lead Scalper · {scalp_horizon_label} · {len(bot_assets)} assets"
            )
            entry_usd = float(hot.get("lead_lag_scalp_order_usd") or 0)
            entry_min = float(hot.get("lead_lag_scalp_min_price") or 0)
            entry_max = float(hot.get("lead_lag_scalp_max_price") or 0)
            entry_window_secs = int(hot.get("lead_lag_scalp_stop_entry_secs") or 0)
            target_pct = float(hot.get("lead_lag_scalp_profit_target_pct") or 0)
            hard_stop_pct = float(hot.get("lead_lag_scalp_hard_stop_pct") or 0)
            hard_stop_ticks = int(hot.get("lead_lag_scalp_hard_stop_ticks") or 0)
            reversal_exit_enabled = bool(hot.get("lead_lag_scalp_reversal_exit_enabled", True))
            flatten_secs = int(hot.get("lead_lag_scalp_flatten_secs") or 0)
            flatten_secs_5m = int(hot.get("lead_lag_scalp_flatten_5m_secs") or flatten_secs)
            flatten_secs_15m = int(hot.get("lead_lag_scalp_flatten_15m_secs") or flatten_secs)
            if target_pct > 0:
                entry_max = min(entry_max, 0.99 / (1.0 + target_pct / 100.0))
            strategy_horizons = [horizon] if horizon else [int(x) for x in horizons if str(x).isdigit()]
        elif sniper:
            strategy_horizons = [int(x) for x in horizons
                                 if str(x).isdigit() and int(x) in (300, 900)]
            horizon_label = "/".join(f"{value // 60}m" for value in strategy_horizons)
            strategy_name = (f"Late Settlement Sniper · {len(bot_assets)} assets · "
                             f"{horizon_label or 'configured windows'}")
            entry_usd = float(hot.get("late_window_sniper_order_usd") or 0)
            entry_min = float(hot.get("late_window_sniper_min_price") or 0)
            entry_max = float(hot.get("late_window_sniper_max_price") or 0)
            entry_window_secs = int(hot.get("late_window_sniper_max_secs") or 0)
            entry_windows = []
            for horizon_s in strategy_horizons:
                suffix = "5m" if horizon_s == 300 else "15m"
                min_key = f"late_window_sniper_min_secs_{suffix}"
                max_key = f"late_window_sniper_max_secs_{suffix}"
                min_secs = hot.get(min_key)
                max_secs = hot.get(max_key)
                entry_windows.append({
                    "horizon_seconds": horizon_s,
                    "min_secs": int(min_secs if min_secs is not None else
                                    hot.get("late_window_sniper_min_secs") or 0),
                    "max_secs": int(max_secs if max_secs is not None else
                                    hot.get("late_window_sniper_max_secs") or 0),
                })
            target_pct = hard_stop_pct = hard_stop_ticks = flatten_secs = 0
            flatten_secs_5m = flatten_secs_15m = 0
            reversal_exit_enabled = False
        elif article_simulation:
            bot_assets = ["BTC"]
            strategy_horizons = [int(x) for x in horizons
                                 if str(x).isdigit() and int(x) == 300]
            strategy_name = "Article Reference Simulation · BTC · 5m"
            entry_usd = min(2.45, float(cfg.get("risk", {}).get("max_order_usd") or 2.45))
            entry_min = entry_max = 0.49
            entry_window_secs = 0
            target_pct = hard_stop_pct = hard_stop_ticks = flatten_secs = 0
            flatten_secs_5m = flatten_secs_15m = 0
            reversal_exit_enabled = False
            entry_windows = []
        else:
            strategy_name = "Unconfigured multi-asset window bot"
            entry_usd = entry_min = entry_max = target_pct = hard_stop_pct = hard_stop_ticks = 0
            entry_window_secs = flatten_secs = 0
            flatten_secs_5m = flatten_secs_15m = 0
            reversal_exit_enabled = False
            strategy_horizons = [int(x) for x in horizons if str(x).isdigit()]
            entry_windows = []
        bots.append({
            "instance": inst,
            "name": fleet_row.get("name") or inst,
            "running": bool(fleet_row.get("running")),
            "mode": mode,
            "strategy": strategy_name,
            "assets": bot_assets,
            "settlement_only": settlement_only,
            "article_simulation": article_simulation,
            "horizons": strategy_horizons,
            "entry_order_mode": hot.get("entry_order_mode", "maker"),
            "entry_usd": entry_usd,
            "entry_min_price": entry_min,
            "entry_max_price": entry_max,
            "entry_window_secs": entry_window_secs,
            "entry_window_secs_5m": int(hot.get("lead_lag_scalp_stop_entry_5m_secs") or 90)
                if is_scalper else 0,
            "entry_window_secs_15m": int(hot.get("lead_lag_scalp_stop_entry_15m_secs") or 270)
                if is_scalper else 0,
            "entry_windows": entry_windows if settlement_only else [],
            "entry_min_secs": (entry_windows[0]["min_secs"] if settlement_only and entry_windows else 0),
            "article_target_price": 0.70 if article_simulation else None,
            "article_emergency_exit_secs": 30 if article_simulation else 0,
            "settlement_edge_pct": float(hot.get("late_window_sniper_min_net_edge_pct") or 0),
            "target_pct": target_pct,
            "hard_stop_pct": hard_stop_pct,
            "hard_stop_ticks": hard_stop_ticks,
            "reversal_exit_enabled": reversal_exit_enabled,
            "reversal_oracle_confirm_bps": float(
                hot.get("lead_lag_scalp_reversal_oracle_confirm_bps") or 0.0
            ),
            "flatten_secs": flatten_secs,
            "flatten_secs_5m": flatten_secs_5m,
            "flatten_secs_15m": flatten_secs_15m,
            "max_entries_per_window": int(hot.get("lead_lag_scalp_max_entries_per_window") or 0) if is_scalper
                else int(hot.get("late_window_sniper_max_entries_per_window") or 0),
            "cb_move_bps": float(hot.get("lead_lag_scalp_min_cb_move_bps") or 0),
            "cb_lead_bps": float(hot.get("lead_lag_scalp_min_cb_lead_bps") or 0),
            "max_window_age_ms": int(hot.get("lead_lag_scalp_max_book_age_ms") or 0),
            "max_window_usd": float(cfg.get("risk", {}).get("max_per_window_usd") or 0),
            "max_deployed_usd": float(cfg.get("risk", {}).get("max_deployed_usd") or 0),
            "session_net": float(accounting.get("net_pnl") or 0),
            "exit_24h": exit_24h,
            "realized_pnl": float(accounting.get("realized_pnl") or 0),
            "unrealized_pnl": float(accounting.get("unrealized_pnl") or 0),
            "deployed_usd": float(accounting.get("deployed_usd") or 0),
            "positions": positions.get("positions", []),
            "open_orders": open_orders,
            "order_stats": execution.get("stats", []),
            "recent_orders": recent,
            "engine_state": engine_state,
            "engine_asset": engine_match.group(1) if engine_match else None,
            "engine_horizon_seconds": int(engine_match.group(2)) if engine_match else None,
            "engine_source": engine_match.group(4) if engine_match else None,
            "engine_reason": engine_match.group(5) if engine_match else None,
            "engine_seconds_left": int(engine_match.group(6)) if engine_match and engine_match.group(6) else None,
            "entry_gate": entry_gate,
            "entry_pause": entry_pause,
            "market_feed_state": market_feed_state,
            "market_feed_frames": market_feed_frames,
            "discovered_markets": discovered_markets,
        })
    result = {"bots": bots,
              "server_ts": int(time.time() * 1000),
              "refresh_ms": 1500,
              "mode_policy": "Dedicated multi-asset strategy view. Profiles default to PAPER."}
    if len(_window_bot_desk_cache) > 32:
        _window_bot_desk_cache.clear()
    _window_bot_desk_cache[cache_key] = (time.monotonic(), result)
    return result


def api_strategy_history(inst, limit=30):
    if not re.match(r'^[\w-]+$', inst or ""):
        return {"error": "invalid instance"}
    rows = q("""SELECT s.strategy_id, s.created_at, s.llm_model, s.reason,
                       r.started_at, r.ended_at, r.fills, r.wins, r.losses,
                       ROUND(r.net_pnl::numeric, 2) AS net_pnl
                FROM vp.strategies s
                LEFT JOIN vp.strategy_runs r ON r.strategy_id = s.strategy_id
                     AND r.instance = s.instance
                WHERE s.instance = %s
                ORDER BY s.created_at DESC, r.started_at DESC NULLS LAST
                LIMIT %s""", (inst, limit))
    return {"instance": inst, "history": rows}


def _wallet_status_for(inst):
    """Return wallet config status booleans for one instance. Never exposes values."""
    path = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
    try:
        with open(path, "rb") as f:
            t = tomllib.load(f)
    except Exception:
        return None
    w = t.get("wallet", {})
    r = t.get("runtime", {})
    pk_path = (w.get("private_key_file") or "").strip()
    credentials_path = (w.get("credentials_file") or "").strip()

    def existing_path(value):
        if not value:
            return None
        path_value = value if os.path.isabs(value) else os.path.join(ROOT, value)
        return os.path.normpath(path_value) if os.path.isfile(path_value) else None

    pk_real = existing_path(pk_path)
    credentials_real = existing_path(credentials_path)
    bundle = {}
    if credentials_real:
        try:
            with open(credentials_real, encoding="utf-8") as f:
                loaded = json.load(f)
            bundle = loaded if isinstance(loaded, dict) else {}
        except Exception:
            bundle = {}
    api_key_set = bool((w.get("api_key") or "").strip() or bundle.get("apiKey"))
    api_secret_set = bool((w.get("api_secret") or "").strip() or bundle.get("secret"))
    api_passphrase_set = bool((w.get("api_passphrase") or "").strip()
                              or bundle.get("passphrase"))
    return {
        "mode":               r.get("mode", "paper"),
        "api_key_set":        api_key_set,
        "api_secret_set":     api_secret_set,
        "api_passphrase_set": api_passphrase_set,
        "credentials_file_set": bool(credentials_path),
        "credentials_file_exists": bool(credentials_real),
        "private_key_set":    bool(pk_path),
        "private_key_exists": bool(pk_real),
        "private_key_file":   pk_path,
        "credentials_ready":  bool(api_key_set and api_secret_set
                                    and api_passphrase_set and pk_real),
        "starting_balance_usd": float(w.get("starting_balance_usd") or 250.0),
    }


def api_wallet_status(instance=None):
    """Wallet configuration status for all instances or one. Never exposes values."""
    if instance:
        if not re.match(r'^[\w-]+$', instance):
            return {"error": "invalid instance"}
        s = _wallet_status_for(instance)
        return s if s else {"error": "not found"}
    all_insts = fleet_instances()
    result = {}
    for inst in all_insts:
        if os.path.exists(os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")):
            s = _wallet_status_for(inst)
            if s:
                result[inst] = s
    return {"wallets": result}


def _wallet_material_from_config(inst):
    """Read one bot's wallet material for server-side migration only.

    Secret values never leave this process; public addresses and readiness flags
    are the only fields returned by the wallet status API.
    """
    path = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
    try:
        with open(path, "rb") as f:
            cfg = tomllib.load(f)
    except Exception:
        return None
    wallet = cfg.get("wallet", {})
    polymarket = cfg.get("polymarket", {})
    bundle = {}
    credentials_path = _project_secret_path(str(wallet.get("credentials_file") or "").strip())
    if credentials_path and os.path.isfile(credentials_path):
        try:
            with open(credentials_path, encoding="utf-8") as f:
                loaded = json.load(f)
            bundle = loaded if isinstance(loaded, dict) else {}
        except Exception:
            bundle = {}
    private_key = ""
    private_key_path = _project_secret_path(str(wallet.get("private_key_file") or "").strip())
    if private_key_path and os.path.isfile(private_key_path):
        try:
            with open(private_key_path, encoding="utf-8") as f:
                private_key = f.read().strip()
        except Exception:
            private_key = ""
    signature_type = bundle.get("signatureType", wallet.get("signature_type"))
    try:
        signature_type = int(signature_type) if signature_type is not None else None
    except (TypeError, ValueError):
        signature_type = None
    funder = str(bundle.get("funderAddress") or polymarket.get("proxy_address") or "").strip()
    signer = str(bundle.get("signerAddress") or polymarket.get("signer_address") or funder).strip()
    return {
        "api_key": str(bundle.get("apiKey") or wallet.get("api_key") or "").strip(),
        "api_secret": str(bundle.get("secret") or wallet.get("api_secret") or "").strip(),
        "api_passphrase": str(bundle.get("passphrase") or wallet.get("api_passphrase") or "").strip(),
        "private_key": private_key,
        "funder_address": funder,
        "signer_address": signer,
        "signature_type": signature_type,
    }


def _admin_wallet_material():
    """Load the canonical ADMIN wallet, falling back to the legacy ADMIN bot."""
    canonical = {
        "api_key": "", "api_secret": "", "api_passphrase": "",
        "private_key": "", "funder_address": "", "signer_address": "",
        "signature_type": None, "source_instance": None,
    }
    bundle_path = _project_secret_path(ADMIN_WALLET_CREDENTIALS_FILE)
    key_path = _project_secret_path(ADMIN_WALLET_PRIVATE_KEY_FILE)
    has_canonical_material = bool((bundle_path and os.path.isfile(bundle_path))
                                  or (key_path and os.path.isfile(key_path)))
    if bundle_path and os.path.isfile(bundle_path):
        try:
            with open(bundle_path, encoding="utf-8") as f:
                bundle = json.load(f)
            if isinstance(bundle, dict):
                canonical["api_key"] = str(bundle.get("apiKey") or "").strip()
                canonical["api_secret"] = str(bundle.get("secret") or "").strip()
                canonical["api_passphrase"] = str(bundle.get("passphrase") or "").strip()
                canonical["funder_address"] = str(bundle.get("funderAddress") or "").strip()
                canonical["signer_address"] = str(bundle.get("signerAddress") or "").strip()
                sig = bundle.get("signatureType")
                canonical["signature_type"] = int(sig) if sig is not None else None
        except (OSError, ValueError, TypeError):
            pass
    if key_path and os.path.isfile(key_path):
        try:
            with open(key_path, encoding="utf-8") as f:
                canonical["private_key"] = f.read().strip()
        except OSError:
            pass
    if has_canonical_material:
        canonical["source_instance"] = "shared"

    # Before the shared bundle exists, adopt an existing ADMIN bot's wallet on
    # Save. This avoids forcing the operator to retype credentials already kept
    # on the server. Non-admin tenant bots and short-lived demos are excluded.
    if not has_canonical_material and not (all(canonical.get(k) for k in
                ("api_key", "api_secret", "api_passphrase", "private_key"))
            and canonical.get("funder_address") and canonical.get("signer_address")
            and canonical.get("signature_type") is not None):
        for inst in _admin_wallet_instances():
            legacy = _wallet_material_from_config(inst)
            if not legacy:
                continue
            for key in ("api_key", "api_secret", "api_passphrase", "private_key",
                        "funder_address", "signer_address", "signature_type"):
                if canonical.get(key) in (None, "") and legacy.get(key) is not None:
                    canonical[key] = legacy[key]
            if any(legacy.get(k) for k in ("api_key", "api_secret", "api_passphrase", "private_key")):
                canonical["source_instance"] = "shared" if canonical["source_instance"] == "shared" else inst
                if all(canonical.get(k) for k in
                       ("api_key", "api_secret", "api_passphrase", "private_key")):
                    break
    if not canonical["signer_address"]:
        canonical["signer_address"] = canonical["funder_address"]
    return canonical


def _admin_shared_wallet_config(content, material=None):
    """Attach a config to the fleet-wide ADMIN wallet without copying secrets."""
    material = material if isinstance(material, dict) else _admin_wallet_material()
    for section, key, value in (
        ("wallet", "credentials_file", ADMIN_WALLET_CREDENTIALS_FILE),
        ("wallet", "private_key_file", ADMIN_WALLET_PRIVATE_KEY_FILE),
        ("wallet", "api_key", ""),
        ("wallet", "api_secret", ""),
        ("wallet", "api_passphrase", ""),
        ("polymarket", "proxy_address", material.get("funder_address") or ""),
        ("polymarket", "signer_address", material.get("signer_address") or ""),
    ):
        content = _upsert_config_value(content, section, key, value)
    signature_type = material.get("signature_type")
    if signature_type in (0, 1, 2, 3):
        content = _upsert_config_value(content, "wallet", "signature_type", signature_type)
    return content


def api_wallet_shared_status():
    """Return write-only ADMIN wallet status and all current ADMIN-owned bots."""
    material = _admin_wallet_material()
    instances = _admin_wallet_instances()
    bundle_real = _project_secret_path(ADMIN_WALLET_CREDENTIALS_FILE)
    key_real = _project_secret_path(ADMIN_WALLET_PRIVATE_KEY_FILE)
    canonical_files = bool(bundle_real and key_real and os.path.isfile(bundle_real)
                           and os.path.isfile(key_real))
    canonical_material_present = bool((bundle_real and os.path.isfile(bundle_real))
                                      or (key_real and os.path.isfile(key_real)))
    identity_ready = bool(re.fullmatch(r"0x[0-9a-fA-F]{40}", material.get("funder_address", ""))
                          and re.fullmatch(r"0x[0-9a-fA-F]{40}", material.get("signer_address", ""))
                          and material.get("signature_type") in (0, 1, 2, 3))
    try:
        _validate_private_key_hex(material.get("private_key", ""))
        private_key_valid = True
    except ValueError:
        private_key_valid = False
    credentials_ready = bool(all(material.get(k) for k in
                                 ("api_key", "api_secret", "api_passphrase"))
                              and private_key_valid and identity_ready)
    bots = []
    for inst in instances:
        path = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
        try:
            with open(path, "rb") as f:
                cfg = tomllib.load(f)
            wallet = cfg.get("wallet", {})
            mode = str(cfg.get("runtime", {}).get("mode", "paper")).upper()
            uses_shared = (str(wallet.get("credentials_file") or "").strip()
                           == ADMIN_WALLET_CREDENTIALS_FILE
                           and str(wallet.get("private_key_file") or "").strip()
                           == ADMIN_WALLET_PRIVATE_KEY_FILE)
        except Exception:
            mode, uses_shared = "UNKNOWN", False
        bots.append({"instance": inst, "mode": mode, "running": instance_running(inst),
                     "uses_shared_credentials": uses_shared})
    return {
        "scope": "ADMIN",
        "credentials_ready": credentials_ready,
        "canonical_bundle_saved": canonical_files,
        "canonical_material_present": canonical_material_present,
        "legacy_source_instance": (material.get("source_instance")
                                   if material.get("source_instance") != "shared" else None),
        "api_key_set": bool(material.get("api_key")),
        "api_secret_set": bool(material.get("api_secret")),
        "api_passphrase_set": bool(material.get("api_passphrase")),
        "private_key_set": bool(material.get("private_key")),
        "funder_address": material.get("funder_address", ""),
        "signer_address": material.get("signer_address", ""),
        "signature_type": material.get("signature_type"),
        "shared_bot_count": sum(1 for bot in bots if bot["uses_shared_credentials"]),
        "total_bot_count": len(bots),
        "bots": bots,
    }


def _stage_atomic_file(path, data, mode):
    directory = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(prefix=".admin-wallet-", dir=directory)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
    except Exception:
        try:
            os.close(fd)
        except OSError:
            pass
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    return tmp


def _restore_wallet_file(path, original, mode):
    if original is None:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        return
    tmp = _stage_atomic_file(path, original, mode)
    os.replace(tmp, path)


def api_wallet_shared_configure(body):
    """Save one protected ADMIN credential bundle and attach it to all ADMIN bots.

    Existing per-bot modes are preserved. Only running ADMIN bots are restarted
    after every config points to the shared files. Tenant and demo bots are
    excluded by _admin_wallet_instances().
    """
    if not isinstance(body, dict):
        return (400, {"error": "request must be an object"})
    previous = _admin_wallet_material()

    def incoming(name, fallback=""):
        value = body.get(name, "")
        if value is None:
            return fallback
        if not isinstance(value, str):
            raise ValueError(f"{name} must be a string")
        return value.strip() or fallback

    try:
        api_key = incoming("api_key", previous.get("api_key", ""))
        api_secret = incoming("api_secret", previous.get("api_secret", ""))
        api_passphrase = incoming("api_passphrase", previous.get("api_passphrase", ""))
        private_key = incoming("private_key", previous.get("private_key", ""))
        funder = incoming("funder_address", previous.get("funder_address", ""))
        signer = incoming("signer_address", previous.get("signer_address", ""))
        raw_signature_type = body.get("signature_type", previous.get("signature_type"))
        if raw_signature_type is None or raw_signature_type == "":
            signature_type = None
        elif isinstance(raw_signature_type, bool):
            signature_type = None
        elif isinstance(raw_signature_type, int):
            signature_type = raw_signature_type
        elif isinstance(raw_signature_type, str) and re.fullmatch(r"[0-3]", raw_signature_type.strip()):
            signature_type = int(raw_signature_type.strip())
        else:
            raise ValueError("signature type must be 0, 1, 2 or 3")
    except (TypeError, ValueError) as e:
        return (400, {"error": str(e)})

    missing = [label for label, value in (
        ("API key", api_key), ("API secret", api_secret),
        ("API passphrase", api_passphrase), ("private key", private_key),
        ("funder/proxy address", funder), ("signer address", signer),
    ) if not value]
    if missing:
        return (400, {"error": "complete the missing credentials: " + ", ".join(missing)})
    if any("\x00" in value or len(value) > 2048 for value in
           (api_key, api_secret, api_passphrase, private_key, funder, signer)):
        return (400, {"error": "credential field contains invalid or oversized data"})
    try:
        private_key = _validate_private_key_hex(private_key)
    except ValueError as e:
        return (400, {"error": str(e)})
    if not re.fullmatch(r"0x[0-9a-fA-F]{40}", funder):
        return (400, {"error": "funder/proxy address must be a 0x-prefixed 20-byte address"})
    if not re.fullmatch(r"0x[0-9a-fA-F]{40}", signer):
        return (400, {"error": "signer address must be a 0x-prefixed 20-byte address"})
    if signature_type not in (0, 1, 2, 3):
        return (400, {"error": "signature type must be 0, 1, 2 or 3"})

    bundle_rel = ADMIN_WALLET_CREDENTIALS_FILE
    key_rel = ADMIN_WALLET_PRIVATE_KEY_FILE
    bundle_real = _project_secret_path(bundle_rel)
    key_real = _project_secret_path(key_rel)
    bundle = {
        "apiKey": api_key,
        "secret": api_secret,
        "passphrase": api_passphrase,
        "funderAddress": funder,
        "signerAddress": signer,
        "signatureType": signature_type,
    }
    bundle_bytes = (json.dumps(bundle, separators=(",", ":")) + "\n").encode()
    key_bytes = (private_key + "\n").encode()

    with _wallet_mode_lock, _config_write_lock:
        instances = _admin_wallet_instances()
        if not instances:
            return (409, {"error": "no current ADMIN-owned bot configs were found"})
        staged = []
        originals = {}
        original_modes = {}
        replaced = []
        try:
            os.makedirs(os.path.dirname(bundle_real), mode=0o700, exist_ok=True)

            def stage(path, data, default_mode):
                try:
                    st = os.stat(path)
                    with open(path, "rb") as f:
                        originals[path] = f.read()
                    original_modes[path] = stat.S_IMODE(st.st_mode)
                    mode = 0o600 if path in (bundle_real, key_real) else original_modes[path]
                except FileNotFoundError:
                    originals[path] = None
                    original_modes[path] = default_mode
                    mode = default_mode
                tmp = _stage_atomic_file(path, data, mode)
                staged.append((tmp, path))

            stage(bundle_real, bundle_bytes, 0o600)
            stage(key_real, key_bytes, 0o600)
            running = {inst for inst in instances if instance_running(inst)}
            for inst in instances:
                path = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
                with open(path, "r", encoding="utf-8") as f:
                    content = f.read()
                tomllib.loads(content)
                for section, key, value in (
                    ("wallet", "credentials_file", bundle_rel),
                    ("wallet", "private_key_file", key_rel),
                    ("wallet", "api_key", ""),
                    ("wallet", "api_secret", ""),
                    ("wallet", "api_passphrase", ""),
                    ("wallet", "signature_type", signature_type),
                    ("polymarket", "proxy_address", funder),
                    ("polymarket", "signer_address", signer),
                ):
                    content = _upsert_config_value(content, section, key, value)
                tomllib.loads(content)
                st = os.stat(path)
                stage(path, content.encode(), stat.S_IMODE(st.st_mode))

            for tmp, path in staged:
                os.replace(tmp, path)
                replaced.append(path)
        except Exception as e:
            rollback_failures = []
            for path in reversed(replaced):
                try:
                    _restore_wallet_file(path, originals[path], original_modes[path])
                except Exception:
                    rollback_failures.append(os.path.basename(path))
            detail = f"shared wallet update failed: {e}"
            if rollback_failures:
                detail += "; rollback failed for " + ", ".join(rollback_failures)
            return (500, {"error": detail})
        finally:
            for tmp, _ in staged:
                if os.path.exists(tmp):
                    os.unlink(tmp)

        restart_failures = []
        restarted = []
        for inst in sorted(running):
            try:
                result = subprocess.run(
                    ["systemctl", "--user", "--no-block", "restart",
                     f"polypulse-bot@{inst}.service"],
                    capture_output=True, text=True, timeout=5, env=_SYSTEMD_ENV)
                if result.returncode == 0:
                    restarted.append(inst)
                else:
                    restart_failures.append(inst)
            except Exception:
                restart_failures.append(inst)

    invalidate_runtime_caches()
    for inst in instances:
        _instance_running_cache.pop(inst, None)
    result = {
        "ok": not restart_failures,
        "updated_instances": instances,
        "restarted_instances": restarted,
        "restart_failures": restart_failures,
        "modes_preserved": True,
    }
    return (200 if not restart_failures else 207, result)


def api_wallet_write(instance, updates, is_admin=False):
    """Write wallet credential fields to the instance TOML (in-place regex replace)."""
    if not re.match(r'^[\w-]+$', instance or ""):
        return (400, {"error": "invalid instance"})
    if is_admin and set(updates) & {"api_key", "api_secret", "api_passphrase", "private_key_file"}:
        return (409, {"error": "ADMIN wallet credentials are fleet-wide; configure them from the Wallet page"})
    allowed = WALLET_ADMIN_FIELDS if is_admin else WALLET_USER_FIELDS
    filtered = {}
    for k, v in updates.items():
        if k not in allowed:
            continue
        if k == "starting_balance_usd":
            try:
                filtered[k] = float(v)
            except (ValueError, TypeError):
                pass
        elif isinstance(v, str) and v.strip():
            filtered[k] = v.strip()
    if not filtered:
        return (400, {"error": "no valid wallet fields provided"})
    code, resp = api_config_write(instance, filtered, allow_server_keys=True)
    if code == 200:
        if not (resp.get("changed") or []):
            return (400, {"error": "no wallet fields were written",
                          "rejected": resp.get("rejected", {})})
        _fleet_cache[0] = 0.0  # bust fleet cache so wallet status refreshes
    return code, resp


def _validate_private_key_hex(pk_hex):
    pk = pk_hex.strip()
    if pk.lower().startswith("0x"):
        pk = pk[2:]
    if not re.match(r'^[0-9a-fA-F]{64}$', pk):
        raise ValueError("private key must be 64 hex characters (with or without 0x prefix)")
    return pk


def _save_private_key(inst, pk_hex):
    """Write a hex private key to secrets/pk_{inst}.txt with mode 0600.
    Returns the relative path written, or raises OSError."""
    pk = _validate_private_key_hex(pk_hex)
    secrets_dir = os.path.join(ROOT, "secrets")
    os.makedirs(secrets_dir, exist_ok=True)
    path = os.path.join(secrets_dir, f"pk_{inst}.txt")
    tmp = f"{path}.tmp-{os.getpid()}"
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(pk + "\n")
        os.replace(tmp, path)
        os.chmod(path, 0o600)
    except Exception:
        try:
            if os.path.exists(tmp):
                os.unlink(tmp)
        except Exception:
            pass
        raise
    return f"./secrets/pk_{inst}.txt"


def _upsert_config_value(content, section, key, value):
    """Replace a TOML value or insert it into an existing table."""
    updated, found = _set_config_value(content, section, key, toml_scalar(value))
    if found:
        return updated
    lines = updated.split("\n")
    header = f"[{section}]"
    start = next((i for i, line in enumerate(lines) if line.strip() == header), None)
    if start is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines.extend([header, f"{key} = {toml_scalar(value)}"])
        return "\n".join(lines)
    insert_at = len(lines)
    for i in range(start + 1, len(lines)):
        if re.match(r'^\s*\[[^]]+\]\s*$', lines[i]):
            insert_at = i
            break
    lines.insert(insert_at, f"{key} = {toml_scalar(value)}")
    return "\n".join(lines)


def _project_secret_path(value):
    if not value:
        return None
    return os.path.normpath(value if os.path.isabs(value) else os.path.join(ROOT, value))


def _shared_wallet_values(source="cb"):
    """Return safe config references for a shared wallet, never secret contents."""
    path = os.path.join(CONFIG_DIR, f"polypulse-{source}.toml")
    try:
        with open(path, "rb") as f:
            cfg = tomllib.load(f)
    except Exception as e:
        raise ValueError(f"cannot read {source} wallet config: {e}") from e
    wallet = cfg.get("wallet", {})
    polymarket = cfg.get("polymarket", {})
    credentials_file = str(wallet.get("credentials_file") or "").strip()
    private_key_file = str(wallet.get("private_key_file") or "").strip()
    credentials_real = _project_secret_path(credentials_file)
    private_key_real = _project_secret_path(private_key_file)
    if not credentials_file or not credentials_real or not os.path.isfile(credentials_real):
        raise ValueError(f"{source} credential bundle is not configured or is missing")
    if not private_key_file or not private_key_real or not os.path.isfile(private_key_real):
        raise ValueError(f"{source} private-key file is not configured or is missing")
    try:
        with open(credentials_real, encoding="utf-8") as f:
            bundle = json.load(f)
    except Exception as e:
        raise ValueError(f"{source} credential bundle is unreadable: {e}") from e
    missing = [key for key in ("apiKey", "secret", "passphrase")
               if not str(bundle.get(key) or "").strip()]
    if missing:
        raise ValueError(f"{source} credential bundle is incomplete: {', '.join(missing)}")

    values = {
        ("wallet", "credentials_file"): credentials_file,
        ("wallet", "private_key_file"): private_key_file,
        ("wallet", "neg_risk"): bool(wallet.get("neg_risk", True)),
        ("wallet", "signature_type"): int(wallet.get("signature_type", 3)),
    }
    if wallet.get("builder_code"):
        values[("wallet", "builder_code")] = wallet["builder_code"]
    for key in ("proxy_address", "signer_address"):
        if polymarket.get(key):
            values[("polymarket", key)] = polymarket[key]
    return values


def api_set_wallet_mode(instance, mode, source=None):
    """Set one bot PAPER/LIVE; LIVE reuses the server-side shared ADMIN bundle.

    The secret values never pass through the browser and are not duplicated:
    every bot points to the same protected credential and private-key files.
    A running bot is stopped while its config is replaced, then started again.
    """
    if not re.match(r'^[\w-]+$', instance or ""):
        return (400, {"error": "invalid instance"})
    if mode not in ("paper", "live"):
        return (400, {"error": "mode must be paper or live"})
    admin_instances = _admin_wallet_instances()
    if instance not in admin_instances:
        return (403, {"error": "bot is not part of the ADMIN fleet"})
    source = source or instance
    if not re.match(r'^[\w-]+$', source or ""):
        return (400, {"error": "invalid wallet source"})
    if source != instance:
        return (400, {"error": "wallet source must be the selected ADMIN bot"})
    path = os.path.join(CONFIG_DIR, f"polypulse-{instance}.toml")
    if not os.path.isfile(path):
        return (404, {"error": "config not found"})

    try:
        shared = {}
        if mode == "live":
            wallet_status = api_wallet_shared_status()
            wallet_bot = next((bot for bot in wallet_status["bots"]
                               if bot["instance"] == instance), None)
            if not wallet_status["credentials_ready"] or not (wallet_bot or {}).get("uses_shared_credentials"):
                return (409, {"error": "complete and attach the shared ADMIN wallet before enabling LIVE"})
            shared = _shared_wallet_values(instance)
    except ValueError as e:
        return (409, {"error": str(e)})

    with _wallet_mode_lock:
        current = current_mode(instance).lower()
        if current != mode:
            if _supervisor_running(instance):
                return (409, {"error": "stop this bot's supervisor before changing its runtime mode"})
            try:
                positions = reconstructed_positions(current, accounting_since(instance, current), instance)
                if positions.get("positions"):
                    return (409, {"error": "close or settle this bot's positions before changing its runtime mode"})
                pending = q("""SELECT COUNT(*)::int AS n FROM vp.orders WHERE instance=%s
                              AND status::text IN ('submitted','acked','partial','uncertain')""",
                             (instance,), one=True)
                if pending and pending.get("n"):
                    return (409, {"error": "cancel and reconcile outstanding orders before changing runtime mode"})
            except Exception as e:
                return (503, {"error": f"could not verify the bot is safe to change modes: {e}"})
        was_running = instance_running(instance)
        service = f"polypulse-bot@{instance}.service"
        if was_running:
            stopped = subprocess.run(
                ["systemctl", "--user", "stop", service], capture_output=True,
                text=True, timeout=12, env=_SYSTEMD_ENV)
            if stopped.returncode != 0:
                return (500, {"error": "could not stop bot safely",
                              "stderr": stopped.stderr[:200]})
        try:
            with open(path, encoding="utf-8") as f:
                content = f.read()
            for (section, key), value in shared.items():
                content = _upsert_config_value(content, section, key, value)
            content = _upsert_config_value(content, "runtime", "mode", mode)
            # Parse before replacing the live config so malformed TOML can never
            # strand a running bot with a broken file.
            tomllib.loads(content)
            tmp = f"{path}.tmp-wallet-mode-{os.getpid()}"
            try:
                with open(tmp, "w", encoding="utf-8") as f:
                    f.write(content)
                    f.flush()
                    os.fsync(f.fileno())
                os.chmod(tmp, os.stat(path).st_mode & 0o777)
                os.replace(tmp, path)
            finally:
                if os.path.exists(tmp):
                    os.unlink(tmp)
        except Exception as e:
            if was_running:
                subprocess.run(["systemctl", "--user", "--no-block", "start", service],
                               capture_output=True, text=True, timeout=5, env=_SYSTEMD_ENV)
            return (500, {"error": f"config update failed: {e}"})

        restart_error = None
        if was_running:
            started = subprocess.run(
                ["systemctl", "--user", "--no-block", "start", service],
                capture_output=True, text=True, timeout=5, env=_SYSTEMD_ENV)
            if started.returncode != 0:
                restart_error = started.stderr[:200] or "systemd start failed"

    invalidate_runtime_caches()
    _instance_running_cache.pop(instance, None)
    payload = {"ok": restart_error is None, "instance": instance, "mode": mode,
               "wallet_source": source if mode == "live" else None,
               "was_running": was_running, "restarted": bool(was_running and not restart_error)}
    if restart_error:
        payload["error"] = restart_error
    return (200 if not restart_error else 500, payload)


def api_go_live(instances, private_key, api_key, api_secret, api_passphrase):
    """Configure wallet credentials and switch all given instances to live mode.
    Saves the private key to disk; restarts each bot after updating its TOML."""
    results = []
    pk = (private_key or "").strip()
    try:
        _validate_private_key_hex(pk)
    except ValueError as e:
        return (400, {"error": str(e)})

    for inst in instances:
        if not re.match(r'^[\w-]+$', inst or ""):
            results.append({"instance": inst, "ok": False, "error": "invalid name"})
            continue
        cfg_path = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
        if not os.path.exists(cfg_path):
            results.append({"instance": inst, "ok": False, "error": "config not found"})
            continue

        # Write the private key file for this instance
        try:
            rel_path = _save_private_key(inst, pk)
        except (ValueError, OSError) as e:
            results.append({"instance": inst, "ok": False, "error": str(e)})
            continue

        # Build update dict — skip empty optional creds so existing values are kept
        updates = {"private_key_file": rel_path, "mode": "live"}
        if api_key.strip():      updates["api_key"]        = api_key.strip()
        if api_secret.strip():   updates["api_secret"]     = api_secret.strip()
        if api_passphrase.strip(): updates["api_passphrase"] = api_passphrase.strip()

        code, resp = api_config_write(inst, updates, allow_server_keys=True)
        required = {"private_key_file", "mode"}
        changed = set(resp.get("changed") or []) if isinstance(resp, dict) else set()
        if code != 200 or not required.issubset(changed):
            rejected = (resp.get("rejected") if isinstance(resp, dict) else None) or {}
            missing = sorted(required - changed)
            results.append({"instance": inst, "ok": False, "error": str(resp)})
            if missing:
                results[-1]["error"] = f"required live fields not written: {missing}; rejected={rejected}"
            continue

        # Restart the bot so the new credentials take effect immediately
        try:
            subprocess.run(
                ["systemctl", "--user", "--no-block", "restart",
                 f"polypulse-bot@{inst}.service"],
                capture_output=True, timeout=5, env=_SYSTEMD_ENV)
        except Exception:
            pass

        results.append({"instance": inst, "ok": True})

    _fleet_cache[0] = 0.0
    _bot_status_cache[0] = 0.0
    _since_cache.clear()
    all_ok = all(r["ok"] for r in results)
    return (200 if all_ok else 207, {"ok": all_ok, "results": results})


def api_switch_paper(instances):
    """Switch a list of instances back to paper mode and restart their bots."""
    results = []
    for inst in instances:
        if not re.match(r'^[\w-]+$', inst or ""):
            results.append({"instance": inst, "ok": False, "error": "invalid name"})
            continue
        code, resp = api_config_write(inst, {"mode": "paper"}, allow_server_keys=True)
        changed = set(resp.get("changed") or []) if isinstance(resp, dict) else set()
        if code == 200 and "mode" in changed:
            try:
                subprocess.run(
                    ["systemctl", "--user", "--no-block", "restart",
                     f"polypulse-bot@{inst}.service"],
                    capture_output=True, timeout=5, env=_SYSTEMD_ENV)
            except Exception:
                pass
            results.append({"instance": inst, "ok": True})
        else:
            rejected = (resp.get("rejected") if isinstance(resp, dict) else None) or {}
            results.append({"instance": inst, "ok": False,
                            "error": f"paper mode not written; rejected={rejected}"})
    _fleet_cache[0] = 0.0
    _bot_status_cache[0] = 0.0
    all_ok = all(r["ok"] for r in results)
    return (200 if all_ok else 207, {"ok": all_ok, "results": results})


def api_supervisor_model(inst, model):
    """Hot-swap the OpenRouter model for an instance's supervisor."""
    if not re.match(r'^[\w-]+$', inst or ""):
        return (400, {"error": "invalid instance"})
    if not re.match(r'^[\w./:@-]+$', model or ""):
        return (400, {"error": "invalid model"})
    try:
        txt = open(MODELS_ENV).read()
    except OSError:
        txt = ""
    line = f"MODEL_{inst}={model}"
    new, n = re.subn(rf'^MODEL_{re.escape(inst)}=.*$', line, txt, flags=re.M)
    if not n:
        new = txt.rstrip("\n") + f"\n{line}\n"
    with open(MODELS_ENV, "w") as f:
        f.write(new)
    # restart supervisor if it's running so the new model takes effect
    restarted = False
    if _supervisor_running(inst):
        code, _ = api_supervisor_control("restart", inst)
        restarted = code == 200
    return (200, {"ok": True, "model": model, "restarted": restarted})


_stream_cache = [0.0, None]


def build_stream_payload():
    now = time.monotonic()
    if _stream_cache[1] and now - _stream_cache[0] < 0.5:
        return _stream_cache[1]
    try:
        oracle_rows = q("""SELECT DISTINCT ON (asset) asset, price
                           FROM vp.oracle_ticks WHERE ts > now()-interval '2 minutes'
                           ORDER BY asset, ts DESC""")
        cb_rows = q("""SELECT DISTINCT ON (asset) asset, price
                       FROM vp.coinbase_ticks WHERE ts > now()-interval '2 minutes'
                       ORDER BY asset, ts DESC""")
    except Exception:
        return _stream_cache[1] or {"prices": {}, "ts": int(time.time() * 1000)}
    om = {r["asset"]: float(r["price"]) for r in oracle_rows}
    cm = {r["asset"]: float(r["price"]) for r in cb_rows}
    prices = {}
    for a in ASSETS:
        op, cp = om.get(a), cm.get(a)
        d = round((cp - op) / op * 100, 4) if (op and cp and op > 0) else None
        prices[a] = {"oracle": op, "coinbase": cp, "delta_pct": d}
    mode = current_mode()
    since = paper_since(None)
    fills = []
    if since:
        try:
            fills = q("""SELECT f.ts, f.price, f.size_shares, o.direction, o.source,
                                o.side_label, m.asset, o.instance
                         FROM vp.fills f
                         JOIN vp.orders o ON o.client_order_id=f.client_order_id
                         LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
                         WHERE o.mode=%s AND o.status='filled'
                           AND f.ts >= %s::timestamptz
                           AND f.ts > now()-interval '2 minutes'
                         ORDER BY f.ts DESC LIMIT 10""", (mode, since))
        except Exception:
            pass
    # Live position marks (cid/side/mark/pnl) so the Positions tab moves at the
    # stream cadence instead of waiting on the 1s full refresh. Reuses the exact
    # book→synthetic→last_fill logic in reconstructed_positions (single source of
    # truth) so SSE patches never disagree with the next full render.
    pos_marks = []
    try:
        for p in (reconstructed_positions(mode, since).get("positions") or []):
            pos_marks.append({
                "cid":         p.get("cid"),
                "side":        p.get("side"),
                "instance":    p.get("instance"),
                "mark":        p.get("mark"),
                "avg":         p.get("avg"),
                "pnl_usd":     p.get("pnl_usd"),
                "pnl_pct":     p.get("pnl_pct"),
                "mark_source": p.get("mark_source"),
                "awaiting_settlement": p.get("awaiting_settlement"),
            })
    except Exception:
        pass
    payload = {"prices": prices, "recent_fills": fills,
               "positions_marks": pos_marks, "ts": int(time.time() * 1000)}
    _stream_cache[0], _stream_cache[1] = now, payload
    return payload


# ── PolyTron Chat ──────────────────────────────────────────────────────────────

_POLYTRON_SYSTEM = """\
You are PolyTron — the analytical assistant embedded in the PolySparQ++ cockpit.

PERSONALITY:
The quant who actually has fun at work. Professional enough to run a hedge fund, \
funny enough that nobody dreads your briefings. Direct, confident, with dry wit. \
Be precise about uncertainty. Use trading terms when they help and explain them \
when context demands it.

YOUR JOB:
- Explain bot behavior and trade decisions in plain English
- Interpret P&L, settlements, and win/loss patterns
- Suggest and explain config changes — always state the tradeoff
- Distinguish observed facts, estimates, and hypothetical outcomes
- Say when a decision trace or simulation is unavailable; do not invent one

WHAT YOU KNOW:
- Polymarket binary markets: YES/NO shares, price 0–1 = probability
- Strategy thresholds, fees, feed lag, and profitability vary by config and market.
- Do not assert numeric strategy parameters or a persistent trading edge without \
current evidence in the context below.
- The current context is an operational summary, not a structured decision trace.

CURRENT BOT STATE (live snapshot):
{context}

CHANGING CONFIG — you can actually push changes, not just suggest them:
- The catalog below lists the knobs supported by one-click chat proposals. Each \
shows its CURRENT value and validation range. Only catalog knobs can be proposed \
for one-click apply; some other settings require a separate admin workflow.
- When the user clearly wants a change (e.g. "raise the take-profit to 35%", \
"disable trailing stop", "set max price to 0.30", "do it"), END your reply with \
EXACTLY ONE fenced block — nothing after it:
    ```polypulse-apply
    {{"knob_name": new_value, "other_knob": new_value}}
    ```
  Strict JSON only (no comments, no trailing text). Use only catalog knob names; \
keep every value inside its range. Booleans are true/false; assets are a JSON list.
- That block becomes an APPLY button in the cockpit. The change is NOT live until \
the user clicks it, so phrase it as "proposed — hit Apply to push it to the bot", \
never "done" or "I changed it". Always explain the tradeoff in prose first, THEN \
the block.
- For questions, explanations, or when you're unsure what they want — DO NOT emit \
a block. Just talk. One concrete change set per reply, max.
{apply_scope}
KNOBS YOU CAN CHANGE:
{knobs}

Keep responses concise. Markdown renders in the chat. Bullet points and tables welcome.\
"""

_pt_sessions: dict = {}   # "username:instance" -> [{role, content}, ...]


def _pt_context(instance: str = "") -> str:
    lines = []
    try:
        live = api_live(instance if instance else None)
        acct = live.get("account", {})
        pos  = live.get("positions", [])
        mode = live.get("mode", "?")
        bot_label = f"Bot: {instance.upper()} | " if instance else ""
        lines.append(f"{bot_label}Mode: {mode} | Balance: ${float(acct.get('wallet_balance_usd') or 0):.2f}"
                     f" | Deployed: ${float(acct.get('deployed_usd') or 0):.2f}")
        lines.append(f"Realized P&L: ${float(acct.get('realized_pnl') or 0):+.2f}"
                     f" | Unrealized: ${float(acct.get('unrealized_pnl') or 0):+.2f}")
        lines.append(f"Open positions: {len(pos)}")
        for p in pos[:6]:
            lines.append(f"  {p.get('asset')} {p.get('side')} avg={p.get('avg',0):.3f}"
                         f" bid={float(p.get('mark') or 0):.3f}"
                         f" pnl={float(p.get('pnl_usd') or 0):+.2f}")
    except Exception as exc:
        lines.append(f"(live status unavailable: {exc})")
    try:
        settles = api_settles(6, instance if instance else None).get("settles", [])
        lines.append(f"Last {len(settles)} exits:")
        for s in settles:
            pnl = None
            if s.get("payout_usd") is not None and s.get("cost_basis") is not None:
                pnl = float(s["payout_usd"]) - float(s["cost_basis"])
            lines.append(f"  {s.get('asset')} {s.get('side_label')} {s.get('source')}"
                         f" entry={s.get('avg_entry','?')} exit={s.get('settle_price','?')}"
                         + (f" pnl={pnl:+.2f}" if pnl is not None else ""))
    except Exception:
        pass
    return "\n".join(lines) if lines else "No live data available."


def _pt_tunable_snapshot(instance: str) -> dict:
    """Current value of every cockpit-tunable knob present in this instance's
    config, flattened to {bare_key: value}. Lets PolyTron see what it can change
    and the UI render old→new. Empty for fleet view or an unreadable config."""
    if not instance:
        return {}
    path = os.path.join(CONFIG_DIR, f"polypulse-{instance}.toml")
    out: dict = {}
    try:
        with open(path, "rb") as f:
            data = tomllib.loads(f.read().decode())
    except Exception:
        return {}

    def walk(d):
        for k, v in d.items():
            if isinstance(v, dict):
                walk(v)
            elif k in COCKPIT_TUNABLE and k not in out:
                out[k] = v
    walk(data)
    return out


def _pt_knob_catalog(snapshot: dict) -> str:
    """LLM-readable catalog: every tunable knob actually present in this config,
    with current value + allowed range, so PolyTron only proposes valid edits."""
    lines = []
    for key in sorted(snapshot):
        spec = COCKPIT_TUNABLE.get(key)
        if not spec:
            continue
        kind = spec[0]
        if kind == "bool":
            rng = "true|false"
        elif kind in ("int", "float"):
            rng = f"{kind} {spec[1]}..{spec[2]}"
        elif kind == "assets":
            rng = "JSON list from " + "/".join(sorted(ASSET_SET))
        else:
            rng = kind
        cur = snapshot[key]
        if isinstance(cur, bool):
            cur = "true" if cur else "false"
        lines.append(f"  {key} = {cur}  [{rng}]")
    return "\n".join(lines) if lines else "  (none)"


def _pt_model(instance: str = "") -> str:
    candidates = ([f"polypulse-{instance}.toml"] if instance else []) + ["polypulse-scalp.toml", "polypulse-cb.toml"]
    for fname in candidates:
        try:
            txt = open(os.path.join(CONFIG_DIR, fname)).read()
            m = re.search(r'(?m)^\s*model\s*=\s*"([^"]+)"', txt)
            if m:
                return m.group(1)
        except Exception:
            pass
    return "deepseek/deepseek-chat-v3-0324"


def _polytron_redact(value: str) -> str:
    """Strip common credential forms before storing or sending chat context."""
    value = re.sub(r'(?im)^.*\b(?:api[_ -]?(?:key|secret|passphrase)|private[_ -]?key|'
                   r'password|seed[_ -]?phrase|mnemonic)\s*[:=].*$',
                   '[REDACTED CREDENTIAL LINE]', value)
    value = re.sub(r'(?i)\bBearer\s+\S+', 'Bearer [REDACTED]', value)
    value = re.sub(r'(?i)\b(?:api[_ -]?(?:key|secret|passphrase)|private[_ -]?key|'
                   r'password|seed[_ -]?phrase|mnemonic)\s*[:=]\s*'
                   r'(?:"[^"]*"|\x27[^\x27]*\x27|\S+)',
                   '[REDACTED CREDENTIAL]', value)
    value = re.sub(r'\b(?:0x)?[0-9a-fA-F]{64}\b', '[REDACTED KEY]', value)
    value = re.sub(r'\b(?:sk|rk|pk)_[A-Za-z0-9_-]{20,}\b', '[REDACTED KEY]', value)
    return value


def api_polytron_chat(messages: list, username: str = "anon", instance: str = "") -> tuple:
    try:
        key = open(os.path.join(ROOT, "secrets", "llm_api_key.txt")).read().strip()
    except Exception:
        return 503, {"error": "LLM API key not configured (secrets/llm_api_key.txt)"}

    sess_key = f"{username}:{instance}" if instance else username
    hist = _pt_sessions.setdefault(sess_key, [])
    for m in messages:
        if not isinstance(m, dict) or m.get("role") not in ("user", "assistant") or not isinstance(m.get("content"), str):
            return 400, {"error": "messages must have user/assistant roles and text content"}
        safe = {"role": m["role"], "content": _polytron_redact(m["content"][:12000])}
        if not hist or hist[-1] != safe:
            hist.append(safe)
    _pt_sessions[sess_key] = hist[-20:]

    snapshot = _pt_tunable_snapshot(instance)
    if instance:
        apply_scope = (f"- Changes apply to the {instance.upper()} bot's config "
                       f"(hot-reloads in ~2s).\n")
        knobs = _pt_knob_catalog(snapshot)
    else:
        apply_scope = ("- FLEET VIEW: no single bot is selected, so you CANNOT "
                       "apply changes here. Never emit a polypulse-apply block — "
                       "tell the user to open a specific bot's chat to change it.\n")
        knobs = "  (fleet view — open a specific bot to change its config)"
    system  = _POLYTRON_SYSTEM.format(context=_pt_context(instance),
                                      apply_scope=apply_scope, knobs=knobs)
    # Sanitize old in-memory sessions too, including conversations created by
    # a server process before this guard was introduced.
    safe_hist = [{"role": m.get("role", "user"),
                  "content": _polytron_redact(str(m.get("content", "")))}
                 for m in _pt_sessions[sess_key] if isinstance(m, dict)]
    _pt_sessions[sess_key] = safe_hist
    payload = json.dumps({
        "model": _pt_model(instance),
        "messages": [{"role": "system", "content": _polytron_redact(system)}] + safe_hist,
        "temperature": 0.7,
        "max_tokens": 600,
    }).encode()
    import urllib.request as _ureq
    try:
        req = _ureq.Request(
            "https://openrouter.ai/api/v1/chat/completions", data=payload,
            headers={"Authorization": f"Bearer {key}",
                     "Content-Type": "application/json",
                     "X-Title": "PolyTron"})
        with _ureq.urlopen(req, timeout=30) as r:
            reply = json.load(r)
        content = reply["choices"][0]["message"]["content"]
        _pt_sessions[sess_key].append({"role": "assistant", "content": content})
        # Return the current tunable values so the UI can render old→new for any
        # polypulse-apply block the model emitted. Applying itself goes through the
        # same guarded /api/config/write path the Strategy tab uses.
        return 200, {"reply": content, "instance": instance, "knobs": snapshot}
    except Exception as exc:
        return 500, {"error": f"LLM error: {exc}"}


# ── Push Notification Device Registration ──────────────────────────────────
PUSH_DEVICES_FILE = os.path.join(ROOT, "secrets", "push_devices.json")
PUSH_PREFS_FILE   = os.path.join(ROOT, "secrets", "push_preferences.json")
_push_lock = threading.Lock()

def _load_push_devices():
    try:
        with open(PUSH_DEVICES_FILE) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}

def _save_push_devices(data):
    os.makedirs(os.path.dirname(PUSH_DEVICES_FILE), exist_ok=True)
    with open(PUSH_DEVICES_FILE, "w") as f:
        json.dump(data, f, indent=2)

def _load_push_prefs():
    try:
        with open(PUSH_PREFS_FILE) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}

def _save_push_prefs(data):
    os.makedirs(os.path.dirname(PUSH_PREFS_FILE), exist_ok=True)
    with open(PUSH_PREFS_FILE, "w") as f:
        json.dump(data, f, indent=2)

def api_push_register(username, fcm_token, device_id):
    with _push_lock:
        devices = _load_push_devices()
        if username not in devices:
            devices[username] = []
        devices[username] = [d for d in devices[username] if d.get("device_id") != device_id]
        devices[username].append({
            "device_id": device_id,
            "fcm_token": fcm_token,
            "registered_at": datetime.now(timezone.utc).isoformat(),
        })
        _save_push_devices(devices)
    return 200, {"ok": True}

def api_push_unregister(username, device_id):
    with _push_lock:
        devices = _load_push_devices()
        if username in devices:
            devices[username] = [d for d in devices[username] if d.get("device_id") != device_id]
            _save_push_devices(devices)
    return 200, {"ok": True}

def api_push_preferences_read(username):
    prefs = _load_push_prefs()
    defaults = {"fills": True, "results": True, "bot_crash": True, "pnl_swing": True,
                "pnl_swing_usd": 5.0, "daily_summary": True}
    user_prefs = prefs.get(username, {})
    return {**defaults, **user_prefs}

def api_push_preferences_write(username, updates):
    allowed = {"fills", "results", "bot_crash", "pnl_swing", "pnl_swing_usd", "daily_summary"}
    filtered = {k: v for k, v in updates.items() if k in allowed}
    with _push_lock:
        prefs = _load_push_prefs()
        if username not in prefs:
            prefs[username] = {}
        prefs[username].update(filtered)
        _save_push_prefs(prefs)
    return 200, {"ok": True}


# ── Push Dispatcher (FCM background sender) ───────────────────────────────
_fcm_available = False
_fcm_app = None
FIREBASE_CRED_FILE = os.path.join(ROOT, "secrets", "firebase-service-account.json")

def _init_fcm():
    global _fcm_available, _fcm_app
    if not os.path.exists(FIREBASE_CRED_FILE):
        return
    try:
        import firebase_admin
        from firebase_admin import credentials
        cred = credentials.Certificate(FIREBASE_CRED_FILE)
        _fcm_app = firebase_admin.initialize_app(cred)
        _fcm_available = True
        print("  FCM push notifications enabled")
    except Exception as e:
        print(f"  FCM init failed (push disabled): {e}")

def _send_push(fcm_token, title, body, data=None):
    if not _fcm_available:
        return
    try:
        from firebase_admin import messaging
        msg = messaging.Message(
            notification=messaging.Notification(title=title, body=body),
            data=data or {},
            token=fcm_token,
        )
        messaging.send(msg)
    except Exception:
        pass

_push_last_fill_id = {}
_push_last_result_id = {}
_push_last_bot_state = {}
_push_last_pnl = {}
_push_rate_limit = {}

def _push_dispatcher_loop():
    """Background thread: detect events and send FCM pushes."""
    import time as _time
    _time.sleep(10)
    while True:
        try:
            _push_check_events()
        except Exception:
            pass
        _time.sleep(3)

def _push_check_events():
    if not _fcm_available:
        return
    devices = _load_push_devices()
    if not devices:
        return
    prefs_all = _load_push_prefs()

    for username, user_devices in devices.items():
        if not user_devices:
            continue
        user_prefs = {**{"fills": True, "results": True, "bot_crash": True, "pnl_swing": True,
                         "pnl_swing_usd": 5.0, "daily_summary": True},
                      **prefs_all.get(username, {})}

        instances = _push_instances_for_user(username)
        if instances == set():
            continue

        if user_prefs.get("fills"):
            _check_new_fills(username, user_devices, instances)

        if user_prefs.get("results"):
            _check_new_results(username, user_devices, instances)

        if user_prefs.get("bot_crash"):
            _check_bot_crashes(username, user_devices, instances)

        if user_prefs.get("pnl_swing"):
            threshold = float(user_prefs.get("pnl_swing_usd", 5.0))
            _check_pnl_swing(username, user_devices, threshold, instances)

def _rate_ok(username, event_type):
    key = f"{username}:{event_type}"
    now = time.time()
    last = _push_rate_limit.get(key, 0)
    if now - last < 30:
        return False
    _push_rate_limit[key] = now
    return True

def _push_instances_for_user(username):
    """None means admin/all; an empty set means no authorized bot."""
    users = _load_users()
    if not users:
        return None
    user = users.get(username)
    if not user or not user.get("active"):
        return set()
    if user.get("role") == "admin":
        return None
    instances = set(user.get("instances") or [])
    if user.get("instance"):
        instances.add(user["instance"])
    return instances


def _push_instance_clause(instances, alias="o"):
    if instances is None:
        return "", ()
    return f" AND {alias}.instance = ANY(%s)", (sorted(instances),)


def _check_new_fills(username, devices, instances):
    try:
        scope, args = _push_instance_clause(instances)
        rows = q("SELECT f.id, o.direction, o.side_label, f.price, m.asset, o.instance "
                    "FROM vp.fills f JOIN vp.orders o ON o.client_order_id=f.client_order_id "
                    "LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id "
                    f"WHERE 1=1 {scope} ORDER BY f.id DESC LIMIT 20", args)
        if not rows:
            return
        prev = _push_last_fill_id.get(username)
        _push_last_fill_id[username] = max(row["id"] for row in rows)
        if prev is None:
            return
        for latest in reversed([row for row in rows if row["id"] > prev]):
            asset = latest.get("asset") or "?"
            direction = (latest.get("direction") or "?").upper()
            side = latest.get("side_label") or ""
            price = float(latest.get("price") or 0)
            for d in devices:
                _send_push(d["fcm_token"], f"Fill: {direction} {asset} {side}",
                           f"{direction} @ {price:.4f}",
                           {"type": "fill", "instance": str(latest.get("instance") or "")})
    except Exception:
        pass


def _check_new_results(username, devices, instances):
    try:
        scope, args = _push_instance_clause(instances)
        rows = q(f"""
            SELECT f.id, o.instance, o.source, o.side_label, o.condition_id,
                   f.price, f.size_shares, COALESCE(f.fee_usd, 0) AS fee_usd,
                   m.asset, COALESCE(b.avg_entry, f.price) AS avg_entry
            FROM vp.fills f
            JOIN vp.orders o ON o.client_order_id=f.client_order_id
            LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
            LEFT JOIN LATERAL (
              SELECT SUM(f2.price*f2.size_shares)/NULLIF(SUM(f2.size_shares),0) AS avg_entry
              FROM vp.orders o2 JOIN vp.fills f2 ON f2.client_order_id=o2.client_order_id
              WHERE o2.condition_id=o.condition_id AND o2.instance=o.instance
                AND o2.mode=o.mode AND o2.direction='buy' AND o2.status='filled'
                AND o2.ts_created <= o.ts_created
            ) b ON true
            WHERE o.direction='sell' AND o.status='filled' {scope}
            ORDER BY f.id DESC LIMIT 20
        """, args)
        if not rows:
            return
        prev = _push_last_result_id.get(username)
        _push_last_result_id[username] = max(row["id"] for row in rows)
        if prev is None:
            return
        for result in reversed([row for row in rows if row["id"] > prev]):
            price = float(result.get("price") or 0)
            avg = float(result.get("avg_entry") or price)
            shares = float(result.get("size_shares") or 0)
            fee = float(result.get("fee_usd") or 0)
            won = price > 0.5 if result.get("source") == "settle" else price >= avg
            pnl = (price - avg) * shares - fee
            label = "Win" if won else "Loss"
            asset = result.get("asset") or "?"
            side = result.get("side_label") or ""
            for d in devices:
                _send_push(d["fcm_token"], f"{label}: {asset} {side}",
                           f"Exit @ {price:.4f} | P&L {pnl:+.2f} USD",
                           {"type": "result", "result": label.lower(),
                            "instance": str(result.get("instance") or "")})
    except Exception:
        pass

def _check_bot_crashes(username, devices, instances):
    try:
        if instances is None:
            detail = bot_status_detail()
            running = set(detail.get("instances_running") or [])
        else:
            running = {inst for inst in instances if instance_running(inst)}
        prev = _push_last_bot_state.get(username)
        _push_last_bot_state[username] = running
        if prev is None:
            return
        crashed = prev - running
        for inst in crashed:
            if not _rate_ok(username, f"crash:{inst}"):
                continue
            for d in devices:
                _send_push(d["fcm_token"], f"Bot Crashed: {inst}",
                           f"Instance '{inst}' is no longer running",
                           {"type": "bot_crash", "instance": inst})
    except Exception:
        pass

def _check_pnl_swing(username, devices, threshold, instances):
    try:
        if instances is None:
            net_pnl = float(api_status().get("net_pnl") or 0)
        else:
            net_pnl = sum(float(api_status(instance=inst).get("net_pnl") or 0)
                          for inst in instances)
        prev = _push_last_pnl.get(username)
        _push_last_pnl[username] = net_pnl
        if prev is None:
            return
        delta = abs(net_pnl - prev)
        if delta >= threshold:
            if not _rate_ok(username, "pnl_swing"):
                return
            direction = "up" if net_pnl > prev else "down"
            for d in devices:
                _send_push(d["fcm_token"], f"P&L {direction}: ${delta:.2f}",
                           f"Net P&L now ${net_pnl:.2f} (was ${prev:.2f})",
                           {"type": "pnl_swing"})
    except Exception:
        pass


def _observed_runtime_status(inst, pid):
    """Accept only a fresh status written by the currently running bot PID."""
    if not pid or not re.fullmatch(r'[\w-]+', inst):
        return None
    path = os.path.join(ROOT, "logs", f"polypulse-{inst}.runtime.json")
    try:
        with open(path) as f:
            status = json.load(f)
        age_ms = int(time.time() * 1000) - int(status["ts_ms"])
        if (status.get("instance") != inst or int(status["pid"]) != int(pid) or
                not -1000 <= age_ms <= 10_000 or
                status.get("mode") not in ("PAPER", "LIVE", "REPLAY") or
                status.get("execution_mode") not in ("taker", "maker", "hybrid")):
            return None
        return status
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
        return None


def api_v3_fleet():
    """Observed service state. A configured but stopped profile stays visible."""
    fleet = []
    for inst in fleet_instances():
        service = _systemd_bot_status(f"polypulse-bot@{inst}.service") or {}
        pid = service.get("pid")
        rss_mb = None
        if pid:
            try:
                with open(f"/proc/{pid}/status") as f:
                    match = re.search(r'^VmRSS:\s+(\d+)\s+kB', f.read(), re.M)
                rss_mb = round(int(match.group(1)) / 1024, 1) if match else None
            except (OSError, ValueError):
                pass
        observed = _observed_runtime_status(inst, pid) if service.get("running") else None
        configured_mode = current_mode(inst).upper()
        risk_state = observed.get("risk_state", "UNKNOWN") if observed else "UNVERIFIED"
        if service.get("running"):
            state = ("unverified" if not observed else
                     "degraded" if risk_state in ("ORDER_UNCERTAIN_HALT", "LOSS_LIMIT_HALT") else "running")
        else:
            state = "stopped"
        fleet.append({"instance": inst, "status": state,
                      "pid": pid, "rss_mb": rss_mb, "cpu": None, "heartbeat": None,
                      "risk_state": risk_state,
                      "autopilot": observed.get("autopilot") if observed else None,
                      "daily_kill_fired": observed.get("daily_kill_fired") if observed else None,
                      "mode": observed["mode"] if observed else configured_mode,
                      "mode_verified": bool(observed),
                      "mode_source": "runtime_status" if observed else "config_unverified",
                      "configured_mode": configured_mode,
                      "runtime_updated_ms": observed["ts_ms"] if observed else None,
                      "entry_order_mode": observed["execution_mode"] if observed else
                      load_config(inst).get("strategy", {}).get("entry_order_mode", "maker")})
    return {"fleet": fleet, "server_ts": int(time.time() * 1000)}


def api_v3_overview(instance=""):
    """Fleet snapshot with account values from the existing fill ledger."""
    positions = []
    deployed = unrealized = bankroll = 0.0
    modes = set()
    for inst in fleet_instances():
        if instance and inst != instance:
            continue
        mode = current_mode(inst)
        modes.add(mode.lower())
        since = accounting_since(inst, mode)
        snapshot = reconstructed_positions(mode, since, inst)
        acct = ledger_accounting(mode, snapshot, since, inst)
        if acct.get("equity_usd") is not None:
            bankroll += float(acct["equity_usd"])
        deployed += float(acct.get("deployed_usd") or 0)
        unrealized += float(acct.get("unrealized_pnl") or 0)
        for p in snapshot.get("positions", []):
            positions.append({"instance": inst, "mode":mode.upper(), "asset": p.get("asset") or "?",
                              "hz": f"{int(p.get('horizon_seconds') or 0) // 60}m",
                              "side": p.get("side"), "size": float(p.get("cost_basis") or 0),
                              "avg": float(p.get("avg") or 0), "mark": float(p["mark"]) if p.get("mark_is_live") else None,
                              "upl": float(p.get("pnl_usd") or 0) if p.get("mark_is_live") else None, "cid": p.get("cid"),
                              "awaiting_settlement": bool(p.get("awaiting_settlement")),
                              "mark_is_live": bool(p.get("mark_is_live"))})
    midnight = int(datetime.now(timezone.utc).replace(hour=0, minute=0, second=0, microsecond=0).timestamp()*1000)
    exits = [e for e in api_v2_analytics(1, instance)["exits"] if e["ts"] >= midnight]
    pnl = {"today_net": sum(e["realized_pnl"] for e in exits),
           "today_w": sum(e["realized_pnl"] > 0 for e in exits),
           "today_l": sum(e["realized_pnl"] < 0 for e in exits)}
    fills = q("""SELECT count(*)::int AS n FROM vp.fills f JOIN vp.orders o USING(client_order_id)
                 WHERE f.ts >= to_timestamp(%s/1000.0) AND (%s='' OR o.instance=%s)""",
              (midnight, instance, instance), one=True)
    heat = {}
    for p in positions:
        cells = heat.setdefault(p["asset"], {})
        cells[p["hz"]] = cells.get(p["hz"], 0) + p["size"]
    return {"bankroll": round(bankroll, 2) if modes == {"paper"} else None,
            "bankroll_source": "sum_of_paper_instance_equity" if modes == {"paper"} else "unavailable_for_mixed_or_live_fleet",
            "deployed": round(deployed, 2),
            "unpriced_positions":sum(not p["mark_is_live"] for p in positions),
            "unrealized": round(unrealized, 2) if all(p["mark_is_live"] for p in positions) else None, "realized_today": pnl.get("today_net"),
            "settles_today": {"w": pnl.get("today_w", 0), "l": pnl.get("today_l", 0)},
            "positions": positions, "open_positions": len(positions),
            "open_markets": len({p["cid"] for p in positions}), "fills_today": (fills or {}).get("n", 0),
            "fills_hist": [], "heatmap": [{"asset": k, "cells": v} for k, v in sorted(heat.items())],
            "scope": instance or "ALL BOTS", "server_ts": int(time.time() * 1000)}


def api_v3_events(limit=40):
    rows = q("""SELECT o.ts_created, o.instance, o.status, o.source, o.execution_mode,
                       o.reject_reason, m.asset
                FROM vp.orders o LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
                ORDER BY o.ts_created DESC LIMIT %s""", (max(1, min(200, limit)),))
    events = []
    for r in rows:
        status = str(r.get("status") or "")
        events.append({"t": datetime.fromtimestamp(r["ts_created"] / 1000, timezone.utc).isoformat(),
                       "inst": r.get("instance") or "?", "cls": "error" if status == "rejected" else "fill" if status == "filled" else "signal",
                       "text": _polytron_redact(f"{r.get('asset') or '?'} {r.get('execution_mode') or 'taker'} {status}: {r.get('reject_reason') or r.get('source') or ''}")})
    return {"events": events, "server_ts": int(time.time() * 1000)}


def api_v3_maker_fleet(instance=""):
    """Observed resting orders only. Queue, rewards and markout remain unknown."""
    rows = q("""SELECT o.client_order_id::text AS quote_id, o.exchange_order_id, o.source,
                       o.instance, o.mode, o.execution_mode, o.post_only, o.status,
                       o.condition_id, o.side_label, o.price, o.size_shares,
                       o.ts_created, o.ts_submitted, o.ts_terminal, m.asset, m.horizon_seconds,
                       m.window_close, COALESCE(f.filled,0) AS filled_shares
                FROM vp.orders o
                LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
                LEFT JOIN LATERAL (SELECT SUM(size_shares) AS filled FROM vp.fills
                                   WHERE client_order_id=o.client_order_id) f ON true
                WHERE o.execution_mode IN ('maker','hybrid') AND (%s='' OR o.instance=%s)
                  AND (o.status::text IN ('submitted','acked','partial','uncertain')
                       OR o.ts_created > now()-interval '24 hours')
                ORDER BY CASE WHEN o.status::text IN ('submitted','acked','partial','uncertain')
                              THEN 0 ELSE 1 END, o.ts_created DESC LIMIT 500""", (instance, instance))
    now_ms = int(time.time() * 1000)
    live_states = {"submitted", "acked", "partial"}
    quotes = []
    for r in rows:
        remaining = max(0.0, float(r.get("size_shares") or 0) - float(r.get("filled_shares") or 0))
        age = now_ms - r["ts_submitted"] if r.get("ts_submitted") else None
        state = {"submitted": "SUBMITTING", "acked": "RESTING", "partial": "PARTIALLY_FILLED",
                 "uncertain": "UNCERTAIN", "filled": "FILLED", "cancelled": "CANCELLED",
                 "rejected": "REJECTED"}.get(str(r.get("status")), "UNKNOWN")
        cfg = load_config(r["instance"]) if r.get("instance") else {}
        strategy=cfg.get("strategy") or {}
        max_age = strategy.get("maker_max_age_ms")
        if r.get("source") in ("RECYCLER_TARGET", "RECYCLER_PROFIT_TARGET"):
            max_age=1000*strategy.get("recycler_max_hold_secs",0)
        stale = str(r.get("status")) in live_states and age is not None and max_age and age > max_age
        if stale:
            state = "STALE"
        quotes.append({**r, "state": state, "remaining_shares": remaining,
                       "resting_notional": round(remaining * float(r.get("price") or 0), 2) if str(r.get("status")) in live_states else 0,
                       "quote_age_ms": age, "stale": bool(stale),
                       "post_only_protection": "REQUESTED" if r.get("post_only") and str(r.get("mode")) == "live" else
                                                "SIMULATED" if r.get("post_only") else "NONE",
                       "queue_ahead": None, "fill_probability": None,
                       "fair_value": None, "adverse_selection": None})
    active = [r for r in quotes if str(r.get("status")) in live_states]
    uncertain = [r for r in quotes if str(r.get("status")) == "uncertain"]
    recent = [r for r in quotes if (r.get("ts_created") or now_ms) >= now_ms - 86_400_000]
    filled_orders = sum(float(r.get("filled_shares") or 0) > 0 for r in recent)
    return {"quotes": quotes, "server_ts": now_ms, "truncated": len(rows) >= 500,
            "summary": {"resting_orders": len(active),
                        "resting_notional": round(sum(r["resting_notional"] for r in active), 2),
                        "stale_quotes": sum(r["stale"] for r in active),
                        "uncertain_orders": len(uncertain),
                        "maker_bots": len({r["instance"] for r in active}),
                        "orders_submitted_24h": len(recent),
                        "orders_with_fills_24h": filled_orders,
                        "order_fill_rate_24h": round(100 * filled_orders / len(recent), 1) if recent else None,
                        "cancelled_orders_24h": sum(r.get("status") == "cancelled" for r in recent)},
            "estimates_available": False}


def api_v4_order_events(instance="", quote_id="", limit=100):
    """Observed lifecycle callbacks, with no inferred queue or venue sequence."""
    if instance and not re.fullmatch(r'[\w-]+', instance):
        return {"error": "invalid instance"}
    if quote_id and not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', quote_id):
        return {"error": "invalid quote_id"}
    exists = q("SELECT to_regclass('vp.order_events')::text AS table_name", one=True)
    if not exists or not exists.get("table_name"):
        return {"supported": False, "reason": "Order transition ledger has not been migrated yet.",
                "events": [], "server_ts": int(time.time() * 1000)}
    rows = q("""SELECT event_id, event_ts, client_order_id::text AS quote_id,
                       instance, exchange_order_id, execution_mode, post_only,
                       status, price, size_shares, fill_delta_shares
                FROM vp.order_events
                WHERE (%s = '' OR instance = %s)
                  AND (%s = '' OR client_order_id = %s::uuid)
                ORDER BY event_id DESC LIMIT %s""",
             (instance, instance, quote_id, quote_id or None, max(1, min(500, int(limit)))))
    return {"supported": True, "events": rows, "server_ts": int(time.time() * 1000),
            "source": "vp.order_events", "estimates_available": False}


def api_v3_alerts():
    alerts = []
    try:
        uncertain = q("""SELECT client_order_id::text AS order_id, instance, mode,
                                execution_mode, condition_id, ts_created
                         FROM vp.orders WHERE status::text='uncertain'
                         ORDER BY ts_created DESC LIMIT 30""")
        for order in uncertain:
            alerts.append({"sev": "crit" if order.get("mode") == "live" else "warn",
                           "cls": "order_uncertain",
                           "text": f"{order.get('instance') or '?'} {order.get('execution_mode') or 'order'} submission outcome uncertain; reconcile venue before retry",
                           "order_id": order["order_id"]})
    except Exception:
        pass
    try:
        makers = api_v3_maker_fleet()
        for quote in makers["quotes"]:
            if quote["stale"]:
                alerts.append({"sev": "crit" if quote["mode"] == "live" else "warn",
                               "cls": "maker_quote", "text": f"{quote['instance']} {quote.get('asset') or '?'} maker quote stale ({quote['quote_age_ms']} ms)",
                               "quote_id": quote["quote_id"]})
    except Exception as e:
        alerts.append({"sev": "warn", "cls": "telemetry", "text": "Maker telemetry unavailable"})
    return {"alerts": alerts[:50], "server_ts": int(time.time() * 1000)}


def api_v3_feeds():
    feeds = []
    for source, table, keycol in (("Oracle", "oracle_ticks", "asset"),
                                  ("Coinbase", "coinbase_ticks", "asset"),
                                  ("Binance", "binance_ticks", "symbol")):
        rows = q(f"""SELECT {keycol} AS key, MAX(ts) AS last_ts, COUNT(*)::int AS n1h
                     FROM vp.{table} WHERE ts>now()-interval '1 hour' GROUP BY {keycol}""")
        for row in rows:
            age = max(0, (time.time() * 1000 - row["last_ts"]) / 1000) if row.get("last_ts") else None
            feeds.append({"source": source, "key": row["key"], "age_s": age,
                          "n1h": row["n1h"], "ok": age is not None and age < 10})
    return {"feeds": feeds, "server_ts": int(time.time() * 1000)}


def api_v3_latency():
    rows = q("""SELECT o.instance, COUNT(*)::int AS orders,
                       COUNT(*) FILTER (WHERE o.status='filled')::int AS filled,
                       COUNT(*) FILTER (WHERE o.execution_mode IN ('maker','hybrid'))::int AS maker,
                       ROUND((percentile_cont(0.5) WITHIN GROUP
                         (ORDER BY EXTRACT(EPOCH FROM (f.first_fill-o.ts_submitted))))::numeric, 3) AS p50,
                       ROUND((percentile_cont(0.9) WITHIN GROUP
                         (ORDER BY EXTRACT(EPOCH FROM (f.first_fill-o.ts_submitted))))::numeric, 3) AS p90
                FROM vp.orders o LEFT JOIN LATERAL
                    (SELECT MIN(ts) AS first_fill FROM vp.fills WHERE client_order_id=o.client_order_id) f ON true
                WHERE o.ts_created>now()-interval '24 hours' GROUP BY o.instance""")
    return {"latency": [{"instance": r["instance"] or "?", "p50": r["p50"], "p90": r["p90"],
                         "fill_rate": round(100 * r["filled"] / r["orders"], 1) if r["orders"] else None,
                         "maker_pct": round(100 * r["maker"] / r["orders"], 1) if r["orders"] else None} for r in rows]}


def api_v3_exit_attribution():
    data = api_v2_analytics(14)
    return {"rows": data["exit_reasons"], "supported": True, "method": data["method"],
            "excluded_unmatched_exits": data["excluded_unmatched_exits"], "server_ts": data["server_ts"]}


def api_v3_timetravel(ts):
    return {"bots": [], "recent": [], "supported": False, "reason": "Historical order and portfolio reconstruction is not implemented."}


def api_v3_simulate(instance, changes):
    return {"error": "Arbitrary config replay is not implemented; no simulation was run.", "supported": False}


def api_v3_snapshot(instance):
    return {"supported": False, "reason": "Frozen config/market snapshots are not implemented."}


def api_v3_capabilities():
    path = os.path.join(ROOT, "PolySparQ++_CAPABILITIES.json")
    with open(path) as f:
        return json.load(f)



class _CockpitServices:
    def __getattr__(self, name):
        return globals()[name]

_platform = None
_platform_lock = threading.Lock()


def platform_store():
    global _platform
    from cockpit_platform import PlatformStore
    with _platform_lock:
        if _platform is None or str(_platform.path.parent.parent) != ROOT:
            _platform = PlatformStore(ROOT)
        return _platform


def api_v4_platform(view, qs):
    from cockpit_platform import risk_view, system_view, signals_view, replay_view, transactions_view
    app = _CockpitServices()
    instance = qs.get("instance", [""])[0]
    if instance and not re.fullmatch(r"[\w-]+",instance):
        raise ValueError("Invalid instance")
    if view == "fleet_management":
        from cockpit_fleet import fleet_view
        return fleet_view(app)
    if view == "loss_review":
        return api_v4_loss_review(qs.get("days", ["7"])[0], instance,
                                  qs.get("mode", ["paper"])[0],
                                  qs.get("cohort_since_ms", [None])[0])
    if view == "orders":
        return api_orders(200, instance or None, all_modes=True)
    if view == "transactions":
        return transactions_view(app,qs)
    if view == "instances":
        current=api_v3_fleet()["fleet"]
        known={r["instance"] for r in current}
        historical=q("SELECT DISTINCT instance FROM vp.orders WHERE instance IS NOT NULL AND instance!='' ORDER BY instance")
        return {"instances":current+[{"instance":r["instance"],"status":"historical","pid":None,"mode":None,"mode_verified":False} for r in historical if r["instance"] not in known]}
    if view == "risk":
        return risk_view(app)
    if view == "system":
        return system_view(app)
    if view == "signals":
        return signals_view(app, instance, qs.get("search", [""])[0][:200], int(qs.get("limit", ["100"])[0]))
    if view == "closed_positions":
        from cockpit_analytics import summarize
        data = api_v2_analytics(int(qs.get("days", ["0"])[0]), instance, qs.get("mode", [""])[0])
        search = qs.get("search", [""])[0][:200].lower()
        exits = [e for e in data["exits"] if not search or search in " ".join(str(e.get(k) or "") for k in ("asset","instance","condition_id","client_order_id","source")).lower()]
        totals = summarize(exits)
        cursor = qs.get("cursor", [""])[0]
        if cursor:
            try:
                stamp, ident = cursor.split(":", 1)
                before = (int(stamp), int(ident))
            except (ValueError, TypeError):
                raise ValueError("Invalid history cursor")
            exits = [e for e in exits if (e["ts"], e["id"]) < before]
        exits = sorted(exits, key=lambda e:(e["ts"], e["id"]), reverse=True)
        limit = max(1, min(250, int(qs.get("limit", ["100"])[0])))
        page = exits[:limit]
        return {"exits":page, "totals":totals,
                "next_cursor":str(page[-1]["ts"])+":"+str(page[-1]["id"]) if len(exits)>limit else None,
                "method":data["method"], "excluded_unmatched_exits":data["excluded_unmatched_exits"],
                "server_ts":data["server_ts"]}
    if view == "analytics":
        return api_v2_analytics(float(qs.get("days", ["14"])[0]), instance, qs.get("mode", [""])[0])
    if view == "replay":
        return replay_view(app, int(qs.get("ts", [str(int(time.time()*1000))])[0]), instance)
    store = platform_store()
    if view == "audit":
        return {"events": store.events(), "server_ts": int(time.time()*1000)}
    if view == "research":
        return {"runs": store.runs(instance), "server_ts": int(time.time()*1000)}
    if view == "config_versions":
        if not instance: raise ValueError("Select an instance")
        config = api_v3_knobs(instance)
        settings = {r["key"]:r["value"] for g in config.get("groups",[]) for r in g["rows"]}
        store.config(instance,settings)
        return {"versions":store.configs(instance),"server_ts":int(time.time()*1000)}
    if view == "incidents":
        observed = []
        for alert in api_v3_alerts()["alerts"]:
            identity = alert.get("order_id") or alert.get("quote_id") or alert["cls"]
            observed.append({"fingerprint":alert["cls"]+":"+identity,
                "title":alert["text"],"severity":"CRITICAL" if alert["sev"]=="crit" else "HIGH",
                "evidence":alert})
        for bot in api_v3_fleet()["fleet"]:
            if bot["pid"] and not bot["mode_verified"]:
                observed.append({"fingerprint":"runtime:"+bot["instance"],"instance":bot["instance"],
                    "severity":"MEDIUM","title":bot["instance"]+": runtime heartbeat not verified",
                    "evidence":{"service_pid":bot["pid"],"mode_source":bot["mode_source"]}})
        for feed in api_v3_feeds()["feeds"]:
            if not feed["ok"]:
                observed.append({"fingerprint":"feed:"+feed["source"]+":"+feed["key"],
                    "severity":"HIGH","title":feed["source"]+" "+feed["key"]+": stored feed data stale",
                    "evidence":feed})
        return {"incidents":store.observe_incidents(observed),"server_ts":int(time.time()*1000)}
    raise ValueError("Unknown platform view")


def api_v4_action(body, actor):
    from cockpit_platform import run_research
    action = body.get("action")
    store = platform_store()
    if action == "fleet":
        from cockpit_fleet import action as fleet_action
        return fleet_action(_CockpitServices(), body, actor)
    if action == "acknowledge_incident":
        store.acknowledge(str(body.get("incident_id") or ""),actor)
        return {"ok":True}
    if action == "research":
        return run_research(_CockpitServices(),store,actor,body)
    raise ValueError("Unknown platform action")


ROUTES = {
    **{"/api/v4/"+view: (lambda qs, view=view: api_v4_platform(view, qs))
       for view in ("risk", "system", "signals", "analytics", "loss_review", "audit", "research", "config_versions", "incidents", "replay", "transactions", "instances", "orders", "closed_positions", "fleet_management")},
    "/api/instances": lambda qs: {"instances": bot_status_detail().get("instances_running", [])},
    "/api/live": lambda qs: api_live(qs.get("instance", [""])[0] or None),
    "/api/live_lite": lambda qs: api_live_lite(qs.get("instance", [""])[0] or None),
    "/api/activity": lambda qs: api_activity(qs.get("instance", [""])[0] or None),
    "/api/execution": lambda qs: api_execution(qs.get("instance", [""])[0] or None),
    "/api/tournament": lambda qs: api_tournament(),
    "/api/supervisor_feed": lambda qs: api_supervisor_feed(int(qs.get("limit", ["15"])[0])),
    "/api/status": lambda qs: api_status(),
    "/api/positions": lambda qs: api_positions(),
    "/api/orders": lambda qs: api_orders(int(qs.get("limit", ["60"])[0]),
                                         qs.get("instance", [""])[0] or None),
    "/api/fills": lambda qs: api_fills(int(qs.get("limit", ["60"])[0])),
    "/api/settles": lambda qs: api_settles(int(qs.get("limit", ["40"])[0]),
                                           qs.get("instance", [""])[0] or None),
    "/api/settlement_history": lambda qs: api_settlement_history(
        qs.get("instance", [""])[0] or None,
        {key: values[0] for key, values in qs.items() if values}),
    "/api/settlement_detail": lambda qs: api_settlement_detail(
        qs.get("order_id", [""])[0], qs.get("instance", [""])[0] or None),
    "/api/predictor": lambda qs: api_predictor(qs.get("instance", [""])[0] or None,
                                                int(qs.get("limit", ["150"])[0])),
    "/api/predictor_trajectory": lambda qs: api_predictor_trajectory(
                                                qs.get("condition_id", [""])[0] or None,
                                                int(qs.get("limit", ["200"])[0])),
    "/api/manual_predictions": lambda qs: api_manual_predictions(
                                                qs.get("force", ["0"])[0] == "1",
                                                qs.get("after_version", [None])[0],
                                                qs.get("wait", [0])[0]),
    "/api/pnl_history": lambda qs: api_pnl_history(int(qs.get("hours", ["24"])[0])),
    "/api/pnl_today": lambda qs: api_pnl_today(),
    "/api/v3/knobs": lambda qs: api_v3_knobs(qs.get("instance", ["cb"])[0]),
    "/api/v4/capabilities": lambda qs: api_v3_capabilities(),
    "/api/v4/maker_fleet": lambda qs: api_v3_maker_fleet(qs.get("instance", [""])[0]),
    "/api/v4/order_events": lambda qs: api_v4_order_events(
        qs.get("instance", [""])[0], qs.get("quote_id", [""])[0],
        int(qs.get("limit", ["100"])[0])),
    "/api/v3/overview": lambda qs: api_v3_overview(qs.get("instance", [""])[0]),
    "/api/v3/fleet": lambda qs: api_v3_fleet(),
    "/api/v3/events": lambda qs: api_v3_events(int(qs.get("limit", ["40"])[0])),
    "/api/v3/audit": lambda qs: api_v3_audit(qs.get("instance", [""])[0], int(qs.get("limit", ["5"])[0])),
    "/api/v3/alerts": lambda qs: api_v3_alerts(),
    "/api/v3/feeds": lambda qs: api_v3_feeds(),
    "/api/v3/latency": lambda qs: api_v3_latency(),
    "/api/v3/exit_attr": lambda qs: api_v3_exit_attribution(),
    "/api/v3/timetravel": lambda qs: api_v3_timetravel(qs.get("ts", ["0"])[0]),
    "/api/v3/simulate": lambda qs: api_v3_simulate(qs.get("instance", ["cb"])[0], []),
    "/api/v3/snapshot": lambda qs: api_v3_snapshot(qs.get("instance", ["cb"])[0]),
    "/api/v3/presets": lambda qs: {"presets": os.listdir(os.path.join(CONFIG_DIR, "presets")) if os.path.isdir(os.path.join(CONFIG_DIR, "presets")) else []},
    "/api/v2_analytics": lambda qs: api_v2_analytics(int(qs.get("days", ["14"])[0])),
    "/api/v2_live": lambda qs: api_v2_live(),
    "/api/candles": lambda qs: api_candles(qs.get("symbol", ["BTCUSDT"])[0],
                                           int(qs.get("limit", ["240"])[0])),
    "/api/prices": lambda qs: api_prices(),
    "/api/markets": lambda qs: api_markets(int(qs.get("limit", ["40"])[0])),
    "/api/mobile/markets": lambda qs: api_mobile_market_search(
        qs.get("q", [""])[0], qs.get("limit", ["30"])[0], qs.get("offset", ["0"])[0]),
    "/api/mobile/market": lambda qs: api_mobile_market_detail(qs.get("condition_id", [""])[0]),
    "/api/mobile/book": lambda qs: api_mobile_market_book(
        qs.get("condition_id", [""])[0], qs.get("outcome_index", ["0"])[0]),
    "/api/strategy": lambda qs: api_strategy(),
    "/api/orderstats": lambda qs: api_orderstats(int(qs.get("hours", ["24"])[0])),
    "/api/source_pnl": lambda qs: api_source_pnl(qs.get("instance", [""])[0] or None),
    "/api/health": lambda qs: api_health(),
    "/api/price_history": lambda qs: api_price_history(
        qs.get("cid", [""])[0], qs.get("side", ["UP"])[0],
        int(qs.get("minutes", ["30"])[0])),
    "/api/configs": lambda qs: api_configs(),
    "/api/config": lambda qs: api_config_read(qs.get("name", [""])[0]),
    "/api/cb_edge": lambda qs: api_cb_edge_live(),
    "/api/signal_history": lambda qs: api_signal_history(int(qs.get("limit", ["30"])[0])),
    "/api/perf_stats": lambda qs: api_perf_stats(),
    "/api/heatmap": lambda qs: api_heatmap(),
    "/api/supervisor_log": lambda qs: api_supervisor_log(int(qs.get("limit", ["20"])[0])),
    "/api/pnl_by_instance": lambda qs: api_pnl_by_instance(),
    "/api/log_tail": lambda qs: api_log_tail(qs.get("instance", [""])[0] or None,
                                              int(qs.get("lines", ["300"])[0])),
    "/api/session_reports": lambda qs: api_session_reports(),
    "/api/session_report": lambda qs: api_session_report_read(qs.get("file", [""])[0]),
    "/api/fleet": lambda qs: api_fleet_detail(),
    "/api/strategy_history": lambda qs: api_strategy_history(
        qs.get("instance", [""])[0], int(qs.get("limit", ["30"])[0])),
}


# ── Force-sell (the ONE write action) ───────────────────────────────────────
# The cockpit never touches the bot directly; it drops a small JSON command file
# into control/ that the running bot polls and acts on (source=HUMAN_OVERRIDE).
# Token-gated because the port may be internet-exposed.
COMMANDS_DIR = os.path.join(ROOT, "control")
TOKEN_FILE = os.path.join(ROOT, "secrets", "cockpit_token.txt")

def cockpit_token():
    try:
        with open(TOKEN_FILE) as f:
            t = f.read().strip()
            if t:
                return t
    except Exception:
        pass
    t = _secrets.token_urlsafe(18)
    os.makedirs(os.path.dirname(TOKEN_FILE), exist_ok=True)
    with open(TOKEN_FILE, "w") as f:
        f.write(t)
    os.chmod(TOKEN_FILE, 0o600)
    return t

def _write_control_atomic(path, payload):
    fd,tmp = tempfile.mkstemp(prefix=".command-",dir=COMMANDS_DIR)
    try:
        with os.fdopen(fd,"w") as stream:
            json.dump(payload,stream,separators=(",",":"),allow_nan=False)
            stream.flush();os.fsync(stream.fileno())
        os.replace(tmp,path)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)


def _queue_force_sell(cid, side, instance):
    os.makedirs(COMMANDS_DIR, exist_ok=True)
    safe = "".join(ch for ch in cid if ch.isalnum())[:24]
    with _force_sell_queue_lock:
        ts = int(time.time() * 1000)
        prefix = f"force_sell_{instance}_{safe}_{side}_"
        for filename in os.listdir(COMMANDS_DIR):
            if not filename.startswith(prefix) or not filename.endswith(".json"):
                continue
            try:
                with open(os.path.join(COMMANDS_DIR, filename), encoding="utf-8") as queued_file:
                    queued = json.load(queued_file)
                age_ms = ts - int(queued.get("ts") or 0)
                if queued.get("cmd") == "force_sell" and 0 <= age_ms <= 30_000:
                    return None
            except (OSError, ValueError, TypeError):
                continue
        path = os.path.join(COMMANDS_DIR, f"{prefix}{ts}.json")
        _write_control_atomic(path,{"cmd":"force_sell","instance":instance,"condition_id":cid,
                                   "side":side,"ts":ts,"expected_mode":current_mode(instance).upper()})
    return os.path.basename(path)


def _queue_cancel_open_orders(instance):
    if not instance or not re.match(r'^[\w-]+$', instance):
        raise ValueError("valid instance required")
    os.makedirs(COMMANDS_DIR, exist_ok=True)
    ts = int(time.time() * 1000)
    path = os.path.join(COMMANDS_DIR, f"cancel_open_orders_{instance}_{ts}.json")
    _write_control_atomic(path,{"op":"cancel_open_orders","instance":instance,"ts":ts})
    return os.path.basename(path)


def _fleet_open_positions():
    out = []
    for inst in fleet_instances():
        mode = current_mode(inst)
        out.extend(reconstructed_positions(mode, accounting_since(inst, mode), inst).get("positions") or [])
    return out


def api_force_sell(cid, side, token, instance=None):
    if token != cockpit_token():
        return (403, {"error": "bad token"})
    if not cid or side not in ("UP", "DOWN"):
        return (400, {"error": "need cid and side=UP|DOWN"})
    if instance and instance not in fleet_instances():
        return (400, {"error": "unknown instance"})
    matches = [p for p in _fleet_open_positions()
               if p.get("cid") == cid and p.get("side") == side and
               (not instance or p.get("instance") == instance) and (p.get("shares") or 0) > 0.01]
    if not matches:
        return (404, {"error": "no matching open position"})
    owners = {p["instance"] for p in matches}
    if len(owners) != 1:
        return (409, {"error": "multiple instances hold this market; specify instance"})
    owner = next(iter(owners))
    if current_mode(owner).lower() not in ("paper", "live"):
        return (409, {"error": "manual close is unavailable for this bot mode"})
    queued = _queue_force_sell(cid, side, owner)
    if not queued:
        return (409, {"error": "a close request is already waiting for this bot"})
    return (200, {"ok": True, "instance": owner,
                  "queued": queued})


def api_force_sell_all(token):
    if token != cockpit_token():
        return (403, {"error": "bad token"})
    positions = _fleet_open_positions()
    controls = []
    for inst in fleet_instances():
        if instance_running(inst):
            for op, enabled in (("autopilot", False), ("cancel_open_orders", None)):
                command = {"instance": inst, "op": op}
                if enabled is not None:
                    command["enabled"] = enabled
                code, result = api_v3_command(command)
                if code == 200:
                    controls.append(result["queued"])
    queued = []
    for p in positions:
        cid = p.get("cid")
        side = p.get("side")
        if (cid and side in ("UP", "DOWN") and (p.get("shares") or 0) > 0.01
                and current_mode(p["instance"]).lower() in ("paper", "live")):
            command = _queue_force_sell(cid, side, p["instance"])
            if command:
                queued.append(command)
    return (200, {"ok": True, "queued": queued, "count": len(queued),
                  "controls_queued": controls,
                  "note": "Commands are queued; venue cancellation and exits require acknowledgement/reconciliation."})


# ── User / auth store ────────────────────────────────────────────────────────

def _load_users():
    """Return users dict from USERS_FILE (mtime-cached). {} on missing file."""
    try:
        mtime = os.stat(USERS_FILE).st_mtime
    except OSError:
        return {}
    with _users_lock:
        if _users_cache[1] is not None and _users_cache[0] == mtime:
            return _users_cache[1]
    try:
        with open(USERS_FILE) as f:
            users = json.load(f)
    except Exception:
        users = {}
    with _users_lock:
        _users_cache[0], _users_cache[1] = mtime, users
    return users


def _save_users(users):
    os.makedirs(os.path.dirname(USERS_FILE), exist_ok=True)
    tmp = USERS_FILE + ".tmp"
    with _users_lock:
        with open(tmp, "w") as f:
            json.dump(users, f, indent=2)
        os.replace(tmp, USERS_FILE)
        _users_cache[0], _users_cache[1] = 0.0, None  # invalidate


def _load_tokens():
    try:
        mtime = os.stat(TOKENS_FILE).st_mtime
    except OSError:
        return {}
    with _tokens_lock:
        if _tokens_cache[1] is not None and _tokens_cache[0] == mtime:
            return _tokens_cache[1]
    try:
        with open(TOKENS_FILE) as f:
            tokens = json.load(f)
    except Exception:
        tokens = {}
    with _tokens_lock:
        _tokens_cache[0], _tokens_cache[1] = mtime, tokens
    return tokens


def _save_tokens(tokens):
    os.makedirs(os.path.dirname(TOKENS_FILE), exist_ok=True)
    tmp = TOKENS_FILE + ".tmp"
    with _tokens_lock:
        with open(tmp, "w") as f:
            json.dump(tokens, f, indent=2)
        os.replace(tmp, TOKENS_FILE)
        _tokens_cache[0], _tokens_cache[1] = 0.0, None


def _migrate_from_legacy():
    """On first startup, convert the single password-hash file to cockpit_users.json."""
    if os.path.exists(USERS_FILE):
        return
    if not COCKPIT_PW_HASH:
        return
    users = {
        "admin": {
            "role": "admin", "instance": None,
            "pw_hash": COCKPIT_PW_HASH, "active": True,
            "email": "", "created_at": int(time.time()),
        }
    }
    _save_users(users)
    print("[cockpit] Migrated legacy password hash → cockpit_users.json (admin account created)")


def _filter_user_config_updates(name, updates):
    """Defense-in-depth pre-filter for non-admins: drop anything whose (bare) key
    is not in the cockpit allow-list before it reaches api_config_write. Keys may
    be section-qualified ("risk.max_per_window_usd")."""
    return {k: v for k, v in updates.items()
            if k.rpartition('.')[2] in COCKPIT_TUNABLE}


def _email_to_username(email):
    local = email.split("@")[0]
    name = re.sub(r'[^a-z0-9-]', '-', local.lower())
    return re.sub(r'-+', '-', name).strip('-')[:32] or "user"


def _create_set_password_token(username):
    token = _secrets.token_urlsafe(32)
    tokens = _load_tokens()
    tokens[token] = {"username": username, "expires": int(time.time()) + 86400}
    _save_tokens(tokens)
    return token


def _normalize_plan(plan):
    """Map a BTCPay itemDesc / loose label to a canonical plan key."""
    p = (plan or "").lower()
    if "pro" in p:
        return "pro"
    if "standard" in p or "std" in p:
        return "standard"
    if p in ("pro", "standard", "demo", "admin"):
        return p
    return "standard"


def _sanitize_provisioned_config(path, stake=250.0):
    """Make a freshly-copied config safe for a new tenant: paper mode, no wallet
    credentials, default stake. The user adds their own keys later to go live."""
    try:
        with open(path) as f:
            c = f.read()
    except OSError:
        return

    def setkey(content, key, value):
        new, _ = re.subn(
            rf'^(\s*{re.escape(key)}\s*=\s*)([^\n#]+?)([ \t]*(?:#[^\n]*)?)$',
            lambda m, _v=value: f"{m.group(1)}{_v}{m.group(3)}",
            content, count=1, flags=re.MULTILINE)
        return new

    c = setkey(c, "mode", '"paper"')
    c = setkey(c, "api_key", '""')
    c = setkey(c, "api_secret", '""')
    c = setkey(c, "api_passphrase", '""')
    c = setkey(c, "credentials_file", '""')
    # An explicit empty string disables Config's default shared-bundle path.
    c = _upsert_config_value(c, "wallet", "credentials_file", "")
    c = setkey(c, "private_key_file", '""')
    c = setkey(c, "starting_balance_usd", f"{stake:.2f}")
    c = setkey(c, "proxy_address", '""')
    c = setkey(c, "signer_address", '""')
    with open(path, "w") as f:
        f.write(c)


def _provision_user(username, email, plan=None):
    """Provision a tenant for their plan: one sanitized paper config per plan bot
    (STD → 1 CB copy; PRO → cb/maniac/conv copies), seed each bot's supervisor
    model, enable the services, and record instances + plan. Returns a set-password
    token. Idempotent: safe to re-run (refreshes plan/instances, re-issues token)."""
    if not re.match(r'^[a-z0-9][a-z0-9-]{0,31}$', username):
        raise ValueError(f"invalid username: {username!r}")
    import shutil
    plan = _normalize_plan(plan)
    pdef = plan_def(plan)
    base_map = plan_bot_base()
    bots = pdef.get("bots")
    if bots == "*" or not bots:
        bots = ["cb"]
    max_bots = pdef.get("max_bots")
    if isinstance(max_bots, int):
        bots = bots[:max_bots]
    default_model = pdef.get("default_model")

    instances = []
    for i, bot in enumerate(bots):
        inst = username if i == 0 else f"{username}-{bot}"
        if not re.match(r'^[a-z0-9][a-z0-9-]{0,40}$', inst):
            continue
        dest_cfg = os.path.join(CONFIG_DIR, f"polypulse-{inst}.toml")
        if not os.path.exists(dest_cfg):
            base_name = base_map.get(bot, bot)
            base_path = os.path.join(CONFIG_DIR, f"polypulse-{base_name}.toml")
            src = base_path if os.path.exists(base_path) else TEMPLATE_TOML
            if not os.path.exists(src):
                print(f"[cockpit] WARNING: no base config for bot '{bot}' — {inst} skipped")
                continue
            shutil.copy2(src, dest_cfg)
            _sanitize_provisioned_config(dest_cfg)
            print(f"[cockpit] provisioned config: {dest_cfg} (from {os.path.basename(src)})")
        instances.append(inst)
        # Seed this bot's supervisor model (writes MODEL_<inst>; bot isn't running
        # yet so nothing restarts).
        if default_model:
            try:
                api_supervisor_model(inst, default_model)
            except Exception:
                pass
        # Enable the service (don't start — user activates on set-password)
        try:
            subprocess.run(
                ["systemctl", "--user", "enable", f"polypulse-bot@{inst}.service"],
                capture_output=True, timeout=5.0, env=_SYSTEMD_ENV)
        except Exception:
            pass

    primary = instances[0] if instances else username
    users = _load_users()
    if username not in users:
        users[username] = {
            "role": "user", "instance": primary, "instances": instances,
            "pw_hash": "", "active": False,
            "email": email, "created_at": int(time.time()),
            "plan": plan,
        }
        _save_users(users)
        print(f"[cockpit] provisioned user: {username} ({email}) plan={plan} bots={instances}")
    else:
        u = users[username]
        changed = False
        if u.get("plan") != plan:
            u["plan"] = plan; changed = True
        if u.get("instances") != instances and instances:
            u["instances"] = instances; changed = True
        if not u.get("instance"):
            u["instance"] = primary; changed = True
        if changed:
            _save_users(users)
        print(f"[cockpit] user {username} exists — refreshed plan={plan} bots={instances}; re-issuing token")

    return _create_set_password_token(username)


def _demo_valid_token(token):
    """Return (valid: bool, instance: str|None) for a demo session token."""
    if not token:
        return False, None
    with _demo_lock:
        sess = _demo_sessions.get(token)
    if not sess or time.monotonic() >= sess["expires_mono"]:
        return False, None
    return True, sess["instance"]


def _demo_instance_name():
    return "demo-" + _secrets.token_hex(4)   # e.g. demo-a1b2c3d4


def _demo_config_path(instance):
    return os.path.join(ROOT, "config", f"polypulse-{instance}.toml")


def _demo_expire_session(token, sess):
    """Stop the bot, delete its config, and purge the token."""
    instance = sess["instance"]
    try:
        subprocess.run(["systemctl", "--user", "stop",
                        f"polypulse-bot@{instance}.service"],
                       capture_output=True, timeout=5.0, env=_SYSTEMD_ENV)
    except Exception:
        pass
    cfg_path = _demo_config_path(instance)
    try:
        if os.path.exists(cfg_path):
            os.unlink(cfg_path)
    except Exception:
        pass
    with _demo_lock:
        _demo_sessions.pop(token, None)
    print(f"[cockpit] demo: expired — stopped {instance}")


def _demo_cleanup_loop():
    """Daemon thread: expire demo sessions that have passed their deadline."""
    while True:
        time.sleep(60)
        now = time.monotonic()
        with _demo_lock:
            expired = [(tok, sess) for tok, sess in _demo_sessions.items()
                       if sess["expires_mono"] < now]
        for tok, sess in expired:
            _demo_expire_session(tok, sess)


def _demo_startup_cleanup():
    """On cockpit restart, stop any orphaned demo-* bots and remove their configs."""
    try:
        result = subprocess.run(
            ["systemctl", "--user", "list-units", "--state=active,activating,running",
             "--plain", "--no-legend"],
            capture_output=True, text=True, timeout=5.0, env=_SYSTEMD_ENV)
        for line in result.stdout.splitlines():
            parts = line.split()
            if not parts:
                continue
            svc = parts[0]
            if svc.startswith("polypulse-bot@demo-") and svc.endswith(".service"):
                instance = svc[len("polypulse-bot@"):-len(".service")]
                subprocess.run(["systemctl", "--user", "stop", svc],
                               capture_output=True, timeout=5.0, env=_SYSTEMD_ENV)
                cfg = _demo_config_path(instance)
                if os.path.exists(cfg):
                    os.unlink(cfg)
                print(f"[cockpit] demo: startup cleanup — stopped orphaned {instance}")
    except Exception as e:
        print(f"[cockpit] demo: startup cleanup error: {e}")


def _demo_start(client_ip=None):
    """Start a fresh per-session demo bot and return (token, expires_epoch, instance)."""
    import shutil

    # Rate limit per IP
    if client_ip:
        now = time.monotonic()
        with _demo_lock:
            last = _demo_rate.get(client_ip, 0)
        if now - last < DEMO_RATE_COOLDOWN:
            raise Exception(f"Too many signups from this IP — please wait {DEMO_RATE_COOLDOWN}s")

    # Cap concurrent sessions
    with _demo_lock:
        active = sum(1 for s in _demo_sessions.values()
                     if s["expires_mono"] > time.monotonic())
    if active >= DEMO_MAX_CONCURRENT:
        raise Exception("All demo slots are busy — please try again in a few minutes")

    # Unique instance and config
    instance = _demo_instance_name()
    cfg_path = _demo_config_path(instance)
    if os.path.exists(DEMO_BASELINE_TOML):
        shutil.copy2(DEMO_BASELINE_TOML, cfg_path)
        print(f"[cockpit] demo: config created for {instance}")

    # Start fresh bot
    time.sleep(0.2)
    try:
        subprocess.run(["systemctl", "--user", "start",
                        f"polypulse-bot@{instance}.service"],
                       capture_output=True, timeout=5.0, env=_SYSTEMD_ENV)
        print(f"[cockpit] demo: bot {instance} started")
    except Exception as e:
        print(f"[cockpit] demo: bot start failed: {e}")

    # Issue token
    token = _secrets.token_urlsafe(32)
    deadline_mono  = time.monotonic() + DEMO_SESSION_SECS
    deadline_epoch = int(time.time() + DEMO_SESSION_SECS)
    with _demo_lock:
        _demo_sessions[token] = {"instance": instance, "expires_mono": deadline_mono}
        if client_ip:
            _demo_rate[client_ip] = time.monotonic()
    return token, deadline_epoch, instance


def _send_set_password_email(email, username, token):
    try:
        cfg = json.load(open(SMTP_FILE))
    except Exception as e:
        print(f"[cockpit] SMTP config missing ({SMTP_FILE}): {e}")
        return
    base_url = cfg.get("base_url", "http://localhost:8888").rstrip("/")
    link = f"{base_url}/set_password?token={token}"
    body = (
        f"Hi {username},\n\n"
        f"Your PolySparQ++ subscription is ready.\n\n"
        f"Set your password (link expires in 24h):\n{link}\n\n"
        f"After setting your password you can log in at:\n{base_url}/\n\n"
        f"— PolySparQ++\n"
    )
    msg = MIMEText(body)
    msg["Subject"] = "Set your PolySparQ++ password"
    msg["From"] = cfg.get("from", cfg["user"])
    msg["To"] = email
    try:
        port = int(cfg.get("port", 587))
        if port == 465:
            with smtplib.SMTP_SSL(cfg["host"], port) as s:
                s.login(cfg["user"], cfg["password"])
                s.sendmail(cfg["user"], [email], msg.as_string())
        else:
            with smtplib.SMTP(cfg["host"], port) as s:
                s.starttls()
                s.login(cfg["user"], cfg["password"])
                s.sendmail(cfg["user"], [email], msg.as_string())
        print(f"[cockpit] set-password email sent to {email}")
    except Exception as e:
        print(f"[cockpit] email send failed: {e}")


SET_PASSWORD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PolySparQ++ — Set Password</title>
<style>
*{{box-sizing:border-box}}body{{background:#0d0d0f;color:#e0e0e0;font-family:monospace;
display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}}
.card{{background:#16161a;border:1px solid #2a2a30;border-radius:8px;padding:32px;width:360px}}
h2{{margin:0 0 20px;color:#a8c7ff;font-size:18px}}
label{{display:block;font-size:12px;color:#888;margin-bottom:4px}}
input[type=password]{{width:100%;padding:8px 10px;background:#0d0d0f;border:1px solid #333;
border-radius:4px;color:#e0e0e0;font-family:monospace;font-size:14px;margin-bottom:14px}}
input[type=password]:focus{{outline:none;border-color:#4a9eff}}
.btn{{display:block;width:100%;padding:10px;background:#4a9eff;color:#000;border:none;
border-radius:4px;cursor:pointer;font-family:monospace;font-size:14px;font-weight:bold}}
.btn:hover{{background:#6ab5ff}}.msg{{padding:10px;border-radius:4px;margin-bottom:16px;
font-size:13px}}.err{{background:#2a1010;border:1px solid #662222;color:#ff9090}}
.ok{{background:#102a10;border:1px solid #226622;color:#90ff90}}
a{{color:#4a9eff}}
</style>
</head>
<body>
<div class="card">
<h2>PolySparQ++ — Activate Account</h2>
{message}
<form method="POST" action="/set_password">
<input type="hidden" name="token" value="{token}">
<label>New password (min 10 chars)</label>
<input type="password" name="password" required minlength="10" autocomplete="new-password">
<label>Confirm password</label>
<input type="password" name="confirm" required minlength="10" autocomplete="new-password">
<button class="btn" type="submit">Set password &amp; activate bot</button>
</form>
</div>
</body>
</html>"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass  # quiet

    def _require_auth(self):
        self._username = None
        self._user = {}

        # Demo token auth: X-Demo-Token header (sent by the SPA in demo mode)
        demo_token = self.headers.get("X-Demo-Token", "")
        if demo_token:
            valid, demo_inst = _demo_valid_token(demo_token)
            if valid:
                self._username = "demo"
                self._user = {"role": "demo", "instance": demo_inst, "active": True}
                return True

        users = _load_users()

        # Dev mode: no users file AND no legacy hash
        if not users and not COCKPIT_PW_HASH:
            self._user = {"role": "admin", "instance": None, "active": True}
            return True

        auth = self.headers.get("Authorization", "")
        if auth.startswith("Basic "):
            try:
                decoded = base64.b64decode(auth[6:]).decode("utf-8", errors="replace")
                username, password = decoded.split(":", 1)
                candidate = hashlib.sha256(password.encode()).hexdigest()
            except Exception:
                username, candidate = "", ""

            if users:
                u = users.get(username)
                if u and u.get("active") and u.get("pw_hash") and \
                        hmac.compare_digest(candidate, u["pw_hash"]):
                    self._username = username
                    self._user = u
                    return True
            elif COCKPIT_PW_HASH:
                # Legacy: no users file, check password-only against old hash
                if hmac.compare_digest(candidate, COCKPIT_PW_HASH):
                    self._user = {"role": "admin", "instance": None, "active": True}
                    return True

        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="PolySparQ++ Cockpit"')
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()
        return False

    def _is_admin(self):
        return self._user.get("role") == "admin"

    def _is_demo(self):
        return self._user.get("role") == "demo"

    def _plan_name(self):
        if self._is_admin():
            return "admin"
        if self._is_demo():
            return "demo"
        return self._user.get("plan", "standard")

    def _allowed_models(self):
        return plan_allowed_models(self._plan_name())

    def _user_instances(self, user=None):
        """The set of bot instances a (non-admin) user owns. Supports the new
        multi-bot `instances` list (PRO) and the legacy single `instance` (STD),
        so a STD user keeps one bot and a PRO user owns up to three."""
        u = user if user is not None else self._user
        insts = u.get("instances")
        if isinstance(insts, list):
            owned = {i for i in insts if i}
        else:
            owned = set()
        primary = u.get("instance")
        if primary:
            owned.add(primary)
        return owned

    def _enforce_instance(self, requested):
        if self._is_admin():
            return requested
        owned = self._user_instances()
        if requested and requested in owned:
            return requested            # honour the user's own bot selection
        return self._user.get("instance") or (next(iter(owned)) if owned else None)

    def _allowed_instances(self):
        if self._is_admin():
            return None  # None = all
        return self._user_instances()

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, default=str).encode()
        elif isinstance(body, str):
            body = body.encode()
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return True
        except (BrokenPipeError, ConnectionResetError, OSError):
            # Browser navigation/refresh commonly closes an in-flight request.
            # The response is already irrelevant; do not let do_GET attempt a
            # second 500 response on the same dead socket or emit a traceback.
            return False

    def _serve_sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("X-Accel-Buffering", "no")  # disable nginx proxy buffering
        self.end_headers()
        is_admin = self._is_admin()
        user_inst = None if is_admin else self._user.get("instance")
        deadline = time.monotonic() + 86400
        try:
            while time.monotonic() < deadline:
                payload = build_stream_payload()
                if not is_admin:
                    own = self._user_instances(self._user)
                    payload = dict(payload)
                    payload["recent_fills"] = [
                        f for f in payload.get("recent_fills", [])
                        if f.get("instance") in own
                    ]
                    payload["positions_marks"] = [
                        p for p in payload.get("positions_marks", [])
                        if p.get("instance") in own
                    ]
                self.wfile.write(
                    f"data: {json.dumps(payload, default=str)}\n\n".encode())
                self.wfile.flush()
                time.sleep(0.5)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _api_fleet_for_user(self):
        result = dict(api_fleet_detail())   # copy: don't mutate the shared cache
        plan_name = self._plan_name()
        result["plan"] = plan_name
        result["allowed_models"] = self._allowed_models()
        result["allow_custom_model"] = self._is_admin()
        result["max_bots"] = None if self._is_admin() else plan_def(plan_name).get("max_bots")
        if self._is_admin():
            return result
        own = self._user_instances()
        filtered = {**result, "fleet": [f for f in result.get("fleet", []) if f["instance"] in own]}
        # Demo: hide available_configs and recent_strategies to prevent config browsing
        if self._is_demo():
            filtered["available_configs"] = []
            for card in filtered["fleet"]:
                card.pop("recent_strategies", None)
        # Strip admin-only wallet field (private key path) for non-admins
        for card in filtered["fleet"]:
            w = card.get("wallet")
            if isinstance(w, dict):
                card["wallet"] = {k: v for k, v in w.items() if k != "private_key_file"}
        return filtered

    def _api_wallet_for_user(self, instance):
        if not self._is_admin():
            instance = self._user.get("instance")
        result = api_wallet_status(instance)
        if not self._is_admin():
            # Strip private_key_file from non-admin responses
            if isinstance(result, dict):
                if "wallets" in result:
                    for v in result["wallets"].values():
                        v.pop("private_key_file", None)
                else:
                    result.pop("private_key_file", None)
        return result

    def _api_pnl_by_instance_for_user(self):
        result = api_pnl_by_instance()
        if self._is_admin():
            return result
        own = self._user_instances()
        insts = result.get("instances", {})
        return {**result, "instances": {k: v for k, v in insts.items() if k in own}}

    def _serve_set_password_form(self, qs):
        token = (qs.get("token", [""])[0] or "").strip()
        if not token:
            html = SET_PASSWORD_HTML.format(
                token="",
                message='<div class="msg err">Missing token. Check your email link.</div>'
            )
            return self._send(400, html, "text/html; charset=utf-8")
        tokens = _load_tokens()
        entry = tokens.get(token)
        if not entry or entry.get("expires", 0) < time.time():
            html = SET_PASSWORD_HTML.format(
                token="",
                message='<div class="msg err">This link has expired or is invalid. '
                        'Contact support for a new one.</div>'
            )
            return self._send(400, html, "text/html; charset=utf-8")
        html = SET_PASSWORD_HTML.format(token=token, message="")
        return self._send(200, html, "text/html; charset=utf-8")

    def _handle_set_password_post(self):
        from urllib.parse import parse_qs as _pqs
        try:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length).decode("utf-8", errors="replace")
            fields = _pqs(raw)
        except Exception:
            fields = {}
        token    = (fields.get("token",    [""])[0] or "").strip()
        password = fields.get("password", [""])[0] or ""
        confirm  = fields.get("confirm",  [""])[0] or ""

        def _err(msg):
            html = SET_PASSWORD_HTML.format(
                token=token,
                message=f'<div class="msg err">{msg}</div>'
            )
            self._send(400, html, "text/html; charset=utf-8")

        tokens = _load_tokens()
        entry = tokens.get(token)
        if not entry or entry.get("expires", 0) < time.time():
            return _err("Token expired or invalid.")
        if len(password) < 10:
            return _err("Password must be at least 10 characters.")
        if password != confirm:
            return _err("Passwords do not match.")

        username = entry["username"]
        pw_hash = hashlib.sha256(password.encode()).hexdigest()
        users = _load_users()
        if username not in users:
            return _err("User not found. Contact support.")
        users[username]["pw_hash"] = pw_hash
        users[username]["active"] = True
        _save_users(users)

        # Remove used token
        del tokens[token]
        _save_tokens(tokens)

        # Start every bot service this tenant owns (PRO = up to 3).
        owned = users[username].get("instances") or [users[username].get("instance") or username]
        for inst in owned:
            try:
                subprocess.run(
                    ["systemctl", "--user", "--no-block", "start",
                     f"polypulse-bot@{inst}.service"],
                    capture_output=True, timeout=5.0, env=_SYSTEMD_ENV)
            except Exception:
                pass

        print(f"[cockpit] user {username} activated, bots started: {owned}")
        html = SET_PASSWORD_HTML.format(
            token="",
            message=(
                '<div class="msg ok">Password set! Your bot is starting.<br>'
                'You can now <a href="/">log in to the cockpit</a>.</div>'
                '<form></form>'  # replace the form with nothing
            )
        ).replace(
            '<form method="POST" action="/set_password">',
            '<div style="display:none">'
        ).replace(
            '<button class="btn" type="submit">Set password &amp; activate bot</button>\n</form>',
            '</div>'
        )
        self._send(200, html, "text/html; charset=utf-8")

    def _handle_btcpay_pay(self, qs):
        """Create a BTCPay invoice for a plan and 302-redirect to the payment page."""
        import urllib.request as _ureq
        if not (BTCPAY_STORE_ID and BTCPAY_BASE_URL and BTCPAY_PUBLIC_URL and BTCPAY_PLANS):
            return self._send(503, {"error": "commercial checkout is not configured on this server"})
        plan_key = (qs.get("plan", [""])[0] or "").lower()
        plan = BTCPAY_PLANS.get(plan_key)
        if not plan:
            return self._send(400, {"error": f"unknown plan: {plan_key!r}; valid: {list(BTCPAY_PLANS)}"})
        try:
            api_key = open(BTCPAY_API_KEY_FILE).read().strip()
        except Exception:
            return self._send(503, {"error": "BTCPay API key not configured on server; contact support"})
        payload = json.dumps({
            "amount": plan["amount"],
            "currency": plan["currency"],
            "metadata": {"itemDesc": plan["itemDesc"]},
            "checkout": {
                "redirectURL": f"{BTCPAY_PUBLIC_URL}/?subscribed=1",
                "redirectAutomatically": True,
                "requiresRefundEmail": True,
            },
        }).encode()
        req = _ureq.Request(
            f"{BTCPAY_BASE_URL}/api/v1/stores/{BTCPAY_STORE_ID}/invoices",
            data=payload,
            headers={"Content-Type": "application/json", "Authorization": f"token {api_key}"},
            method="POST",
        )
        try:
            with _ureq.urlopen(req, timeout=12) as r:
                data = json.loads(r.read())
        except Exception as e:
            print(f"[cockpit] btcpay/pay: invoice creation failed: {e}")
            return self._send(502, {"error": f"BTCPay invoice creation failed: {e}"})
        checkout_url = data.get("checkoutLink") or (data.get("checkout") or {}).get("link")
        if not checkout_url:
            return self._send(502, {"error": f"no checkoutLink in BTCPay response; keys={list(data)}"})
        self.send_response(302)
        self.send_header("Location", checkout_url)
        self.send_header("Content-Length", "0")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def _handle_btcpay_webhook(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
            raw_body = self.rfile.read(length)
        except Exception:
            return self._send(400, {"error": "bad request"})

        # Verify HMAC signature
        try:
            secret = open(BTCPAY_SECRET_FILE).read().strip().encode()
        except Exception:
            print("[cockpit] BTCPay webhook: secret file missing")
            return self._send(500, {"error": "webhook not configured"})

        sig_header = self.headers.get("BTCPAY-SIG", "")
        if sig_header.startswith("sha256="):
            expected = hmac.new(secret, raw_body, "sha256").hexdigest()
            if not hmac.compare_digest(sig_header[7:], expected):
                return self._send(401, {"error": "invalid signature"})
        else:
            return self._send(401, {"error": "missing signature"})

        try:
            data = json.loads(raw_body)
        except Exception:
            return self._send(400, {"error": "invalid JSON"})

        event_type = data.get("type", "")
        if event_type != "InvoiceSettled":
            return self._send(200, {"ok": True, "skipped": event_type})

        meta = data.get("metadata") or {}
        # BTCPay places the checkout-collected email at root level as "buyerEmail"
        # and also in metadata; check both.
        email = (data.get("buyerEmail") or data.get("receipt", {}).get("Email")
                 or meta.get("buyerEmail") or meta.get("email") or "")
        plan  = meta.get("itemDesc") or meta.get("item") or "standard"

        if not email:
            all_keys = {**data, "metadata_keys": list(meta.keys())}
            print(f"[cockpit] BTCPay webhook: InvoiceSettled but no buyerEmail; top-level keys={list(data.keys())}")
            return self._send(200, {"ok": True, "warning": "no email"})

        username = _email_to_username(email)
        print(f"[cockpit] BTCPay InvoiceSettled: email={email} → username={username} plan={plan}")
        try:
            token = _provision_user(username, email, plan)
            _send_set_password_email(email, username, token)
            return self._send(200, {"ok": True, "username": username})
        except Exception as e:
            print(f"[cockpit] provision error: {e}")
            return self._send(500, {"error": str(e)})

    def _handle_demo_launch(self):
        """GET /demo_launch — no auth required.
        Starts a fresh per-session demo bot, issues 60-min token, redirects to cockpit."""
        try:
            client_ip = self.client_address[0]
            token, expires_epoch, _inst = _demo_start(client_ip)
            redirect_url = f"/?demo_token={token}&demo_expires={expires_epoch}"
            self.send_response(302)
            self.send_header("Location", redirect_url)
            self.send_header("Content-Length", "0")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
        except Exception as e:
            self._send(500, f"<h1>Demo launch failed</h1><pre>{e}</pre>", "text/html")

    def _handle_demo_start(self):
        """POST /api/demo_start — no auth required.
        Returns JSON {token, expires_at, cockpit_url} for AJAX-based demo launch."""
        try:
            client_ip = self.client_address[0]
            token, expires_at, _inst = _demo_start(client_ip)
            return self._send(200, {
                "token": token,
                "expires_at": expires_at,
                "seconds": DEMO_SESSION_SECS,
                "cockpit_url": f"/?demo_token={token}&demo_expires={expires_at}",
            })
        except Exception as e:
            return self._send(500, {"error": str(e)})

    ADMIN_ONLY_ROUTES = {
        "/api/tournament", "/api/supervisor_feed", "/api/supervisor_log", "/api/instances",
    }
    INSTANCE_PARAM_ROUTES = {
        "/api/live", "/api/live_lite", "/api/activity",
        "/api/orders", "/api/fills", "/api/log_tail",
        "/api/source_pnl", "/api/strategy_history", "/api/settles",
        "/api/settlement_history", "/api/settlement_detail",
        "/api/predictor",
        "/api/execution",
    }

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers",
                         "Content-Type, Authorization, X-Demo-Token")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        self._username = None
        self._user = {}
        u = urlparse(self.path)
        path = u.path
        qs = parse_qs(u.query)

        # Pre-auth: set-password page and demo launch
        if path == "/btcpay/pay":
            return self._handle_btcpay_pay(qs)
        if path == "/set_password":
            return self._serve_set_password_form(qs)
        if path == "/demo_launch":
            return self._handle_demo_launch()

        if path.startswith("/assets_v3/") or path.startswith("/assets_v4/") or path.startswith("/assets_v5/"):
            version = "v5" if path.startswith("/assets_v5/") else "v4" if path.startswith("/assets_v4/") else "v3"
            rel = path[len(f"/assets_{version}/"):]
            if not re.fullmatch(r"[A-Za-z0-9_./-]+", rel) or ".." in rel.split("/") or rel.startswith("/"):
                return self._send(404, "not found", "text/plain")
            asset_root = os.path.realpath(os.path.join(os.path.dirname(UI_FILE), f"assets_{version}"))
            fp = os.path.realpath(os.path.join(asset_root, rel))
            if not fp.startswith(asset_root + os.sep):
                return self._send(404, "not found", "text/plain")
            if not os.path.isfile(fp):
                return self._send(404, "not found", "text/plain")
            ext = os.path.splitext(fp)[1].lower()
            mime = {".png": "image/png", ".gif": "image/gif", ".jpg": "image/jpeg",
                    ".svg": "image/svg+xml", ".webp": "image/webp", ".webmanifest": "application/manifest+json",
                    ".js": "text/javascript", ".css": "text/css; charset=utf-8"}.get(ext, "application/octet-stream")
            try:
                data = open(fp, "rb").read()
                self.send_response(200)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "public, max-age=3600")
                self.end_headers()
                self.wfile.write(data)
            except Exception:
                pass
            return

        if not self._require_auth():
            return

        # V3/V4/V5 currently aggregate the entire trading fleet. Restrict these
        # surfaces until every query is tenant scoped.
        if (path in ("/v3", "/v3/", "/v4", "/v4/", "/v5", "/v5/") or path.startswith("/api/v3/") or
                path.startswith("/api/v4/")) and not self._is_admin():
            return self._send(403, {"error": "admin required"})


        if path in ("/v3", "/v3/", "/v4", "/v4/"):
            try:
                with open(os.path.join(os.path.dirname(UI_FILE), "cockpit_v3.html"), "r", encoding="utf-8") as f:
                    return self._send(200, _inject_public_site_config(f.read()), "text/html; charset=utf-8")
            except Exception as e:
                return self._send(500, f"<h1>cockpit_v3.html missing</h1><pre>{e}</pre>", "text/html")

        if path in ("/v5", "/v5/"):
            try:
                with open(os.path.join(os.path.dirname(UI_FILE), "cockpit_v5.html"), "r", encoding="utf-8") as f:
                    return self._send(200, _inject_public_site_config(f.read()), "text/html; charset=utf-8")
            except Exception as e:
                return self._send(500, f"<h1>cockpit_v5.html missing</h1><pre>{e}</pre>", "text/html")

        if path == "/v2" or path == "/v2/":
            try:
                with open(os.path.join(os.path.dirname(UI_FILE), "cockpit_v2.html"), "r", encoding="utf-8") as f:
                    return self._send(200, _inject_public_site_config(f.read()), "text/html; charset=utf-8")
            except Exception as e:
                return self._send(500, f"<h1>cockpit_v2.html missing</h1><pre>{e}</pre>", "text/html")

        if path == "/" or path == "/index.html":
            try:
                with open(UI_FILE, "r", encoding="utf-8") as f:
                    return self._send(200, _inject_public_site_config(f.read()), "text/html; charset=utf-8")
            except Exception as e:
                return self._send(500, f"<h1>cockpit_ui.html missing</h1><pre>{e}</pre>",
                                  "text/html")

        if path == "/api/stream":
            # SSE can't set custom headers; demo token comes as query param
            if self._is_demo() or (not self._user and qs.get("demo_token", [""])[0]):
                qt = qs.get("demo_token", [""])[0]
                if qt:
                    valid, demo_inst = _demo_valid_token(qt)
                    if valid:
                        self._user = {"role": "demo", "instance": demo_inst, "active": True}
                        self._username = "demo"
            return self._serve_sse()

        if path == "/api/whoami":
            plan = "admin" if self._is_admin() else self._user.get("plan", "standard")
            resp = {
                "username": self._username,
                "role": self._user.get("role", "user"),
                "instance": self._user.get("instance"),
                "instances": sorted(self._user_instances()) if not self._is_admin() else None,
                "plan": plan,
            }
            if self._is_demo():
                # Tell the UI when this demo session expires
                demo_token = self.headers.get("X-Demo-Token", "")
                with _demo_lock:
                    sess = _demo_sessions.get(demo_token)
                if sess:
                    resp["demo_expires_in"] = max(0, int(sess["expires_mono"] - time.monotonic()))
                    resp["demo_total_secs"] = DEMO_SESSION_SECS
            return self._send(200, resp)

        if path == "/api/fleet":
            return self._send(200, self._api_fleet_for_user())

        if path in ("/api/window_bots", "/api/scalper_desk"):
            allowed = None if self._is_admin() else self._user_instances()
            return self._send(200, api_window_bot_desk(allowed))

        if path == "/api/pnl_by_instance":
            return self._send(200, self._api_pnl_by_instance_for_user())

        if path == "/api/wallet":
            inst = qs.get("instance", [""])[0] or None
            return self._send(200, self._api_wallet_for_user(inst))

        if path == "/api/wallet/shared":
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            return self._send(200, api_wallet_shared_status())

        if path == "/api/push/preferences":
            return self._send(200, api_push_preferences_read(self._username or "anon"))

        if path in self.ADMIN_ONLY_ROUTES and not self._is_admin():
            return self._send(403, {"error": "admin required"})

        # Demo: also block supervisor routes and config write access
        DEMO_BLOCKED_ROUTES = {"/api/supervisor_feed", "/api/supervisor_log",
                               "/api/configs", "/api/config"}
        if self._is_demo() and path in DEMO_BLOCKED_ROUTES:
            return self._send(403, {"error": "not available in demo"})

        if path in self.INSTANCE_PARAM_ROUTES:
            enforced = self._enforce_instance(qs.get("instance", [""])[0] or None)
            qs = dict(qs)
            qs["instance"] = [enforced or ""]

        # Per-user config isolation: non-admins see only their own config(s)
        if path == "/api/configs" and not self._is_admin():
            cfgs = []
            for own in sorted(self._user_instances()):
                own_file = f"polypulse-{own}.toml"
                cfg_path = os.path.join(CONFIG_DIR, own_file)
                if os.path.exists(cfg_path):
                    cfgs.append({"name": own, "file": own_file,
                                 "mtime": int(os.path.getmtime(cfg_path) * 1000)})
            return self._send(200, {"configs": cfgs})

        if path == "/api/config" and not self._is_admin():
            requested = qs.get("name", [""])[0]
            if requested not in self._user_instances():
                return self._send(403, {"error": "can only read your own config"})
            result = api_config_read(requested, full=False)
            result.pop("raw", None)  # never expose raw TOML to non-admins
            return self._send(200, result)

        if path in ROUTES:
            try:
                result = ROUTES[path](qs)
                if path in ("/api/orders", "/api/activity") and not self._is_admin():
                    order_rows = result.get("orders", []) if isinstance(result, dict) else []
                    for row in order_rows:
                        if isinstance(row, dict):
                            row.pop("exchange_order_id", None)
                if path == "/api/settlement_detail" and not self._is_admin() and isinstance(result, dict):
                    for row in result.get("fills", []):
                        if isinstance(row, dict):
                            row.pop("exchange_order_id", None)
                    if isinstance(result.get("trade"), dict):
                        result["trade"].pop("exchange_order_id", None)
                if isinstance(result, tuple) and len(result) == 2 and isinstance(result[0], int):
                    return self._send(result[0], result[1])
                return self._send(200, result)
            except Exception as e:
                return self._send(500, {"error": str(e), "where": path})
        self._send(404, {"error": "not found", "path": path})

    def do_POST(self):
        self._username = None
        self._user = {}
        u = urlparse(self.path)
        qs = parse_qs(u.query)

        # Pre-auth routes: webhook, set-password, demo
        if u.path == "/btcpay/webhook":
            return self._handle_btcpay_webhook()
        if u.path == "/set_password":
            return self._handle_set_password_post()
        if u.path == "/api/demo_start":
            return self._handle_demo_start()

        if not self._require_auth():
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length) or "{}") if length else {}
        except Exception:
            body = {}

        # Demo accounts are fully read-only
        if self._is_demo():
            return self._send(403, {"error": "demo account — read-only"})

        if u.path in ("/api/force_sell", "/api/force_sell_all"):
            token = body.get("token") or qs.get("token", [""])[0]
            # Authenticated admins may use first-party clients without copying the
            # separate cockpit token onto every device. The token remains required
            # for any non-admin caller and for direct/internal API usage.
            if not token and self._is_admin():
                token = cockpit_token()
            if u.path == "/api/force_sell_all":
                if not self._is_admin():
                    return self._send(403, {"error": "admin required"})
                try:
                    code, resp = api_force_sell_all(token)
                    return self._send(code, resp)
                except Exception as e:
                    return self._send(500, {"error": str(e)})
            cid = body.get("cid") or qs.get("cid", [""])[0]
            side = body.get("side") or qs.get("side", ["UP"])[0]
            instance = body.get("instance") or qs.get("instance", [""])[0] or None
            if not self._is_admin():
                instance = self._enforce_instance(instance)
                if not instance:
                    return self._send(403, {"error": "instance access denied"})
            try:
                code, resp = api_force_sell(cid, side, token, instance)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/orders/cancel_open":
            inst = body.get("instance") or qs.get("instance", [""])[0]
            inst = self._enforce_instance(inst)
            if not inst:
                return self._send(400, {"error": "select one bot instance"})
            try:
                queued = _queue_cancel_open_orders(inst)
                return self._send(200, {"ok": True, "instance": inst, "queued": queued})
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/orders/cancel":
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            instance = str(body.get("instance") or "")
            if instance not in fleet_instances():
                return self._send(400, {"error": "select a valid bot instance"})
            try:
                code, response = _queue_cancel_order(
                    instance, body.get("client_order_id"), body.get("exchange_order_id"))
                return self._send(code, response)
            except Exception:
                return self._send(500, {"error": "could not queue the cancellation request"})

        if u.path == "/api/config/write":
            name = body.get("name") or qs.get("name", [""])[0]
            # Strip "polypulse-" prefix if present so users reference by instance name
            if name.startswith("polypulse-"):
                name = name[len("polypulse-"):]
            updates = body.get("updates") or {}
            if not isinstance(updates, dict):
                return self._send(400, {"error": "updates must be object"})
            if not self._is_admin():
                if name not in self._user_instances():
                    return self._send(403, {"error": "can only edit your own config"})
                updates = _filter_user_config_updates(name, updates)
            try:
                # Generic config writes are limited to validated trading knobs.
                # Live mode and wallet changes have dedicated explicit workflows.
                code, resp = api_config_write(name, updates)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/v4/action":
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            try:
                return self._send(200, api_v4_action(body, self._username or "admin"))
            except (ValueError, TypeError, KeyError) as e:
                return self._send(422, {"error": str(e)})
            except Exception:
                return self._send(500, {"error": "Platform action failed; inspect server logs"})

        if u.path == "/api/v3/config_write":
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            try:
                body["_actor"] = self._username or "admin"
                code, resp = api_v3_config_write(body)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})
        if u.path == "/api/v3/fleet_control":
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            try:
                code, resp = api_v3_fleet_control(body)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/v3/command":
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            try:
                code, resp = api_v3_command(body, self._username or "admin")
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/v3/simulate":
            try:
                resp = api_v3_simulate(body.get("instance", "cb"), body.get("changes", []))
                return self._send(200, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/v3/preset_save":
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            code, resp = api_v3_preset_save(body)
            return self._send(code, resp)

        if u.path == "/api/bot/control":
            action      = body.get("action") or qs.get("action", [""])[0]
            inst        = body.get("instance") or qs.get("instance", [""])[0]
            strategy_id = body.get("strategy_id") or qs.get("strategy_id", [""])[0] or None
            config_name = body.get("config_name") or qs.get("config_name", [""])[0] or None
            if not self._is_admin():
                if action in ("swap_config", "delete"):
                    return self._send(403, {"error": "admin required"})
                inst = self._enforce_instance(inst)
            try:
                code, resp = api_bot_control(action, inst, strategy_id, config_name,
                                             self._is_admin(), body.get("confirm_live") is True)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/supervisor/control":
            action = body.get("action") or qs.get("action", [""])[0]
            inst   = body.get("instance") or qs.get("instance", [""])[0]
            inst   = self._enforce_instance(inst)
            try:
                code, resp = api_supervisor_control(action, inst)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/supervisor/model":
            inst  = body.get("instance") or qs.get("instance", [""])[0]
            model = body.get("model") or qs.get("model", [""])[0]
            # Non-admins may set the model only for a bot they own, and only to a
            # model their plan allows. Admins are unrestricted.
            if not self._is_admin():
                if inst not in self._user_instances():
                    return self._send(403, {"error": "can only set your own bot's model"})
                if model not in self._allowed_models():
                    return self._send(403, {"error": "model not available on your plan"})
            try:
                code, resp = api_supervisor_model(inst, model)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/wallet/write":
            instance = body.get("instance") or qs.get("instance", [""])[0]
            updates  = body.get("updates") or {}
            if not isinstance(updates, dict):
                return self._send(400, {"error": "updates must be object"})
            if not self._is_admin():
                instance = self._enforce_instance(instance)
                updates  = {k: v for k, v in updates.items() if k in WALLET_USER_FIELDS}
            try:
                code, resp = api_wallet_write(instance, updates, self._is_admin())
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/wallet/shared":
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            try:
                code, resp = api_wallet_shared_configure(body)
                if code in (200, 207):
                    try:
                        platform_store().record(
                            self._username or "admin", "wallet.shared.configure", "ADMIN",
                            {"updated_instances": resp.get("updated_instances", []),
                             "restarted_instances": resp.get("restarted_instances", []),
                             "restart_failures": resp.get("restart_failures", []),
                             "modes_preserved": True})
                    except Exception:
                        pass
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/wallet/mode":
            # One-click shared-wallet switching is intentionally admin-only.
            # Customer accounts retain the explicit credential flow for their
            # own isolated wallet.
            if not self._is_admin():
                return self._send(403, {"error": "admin required"})
            instance = body.get("instance", "")
            mode = str(body.get("mode", "")).lower()
            if mode == "live" and body.get("confirm_live") is not True:
                return self._send(400, {"error": "explicit confirm_live=true required for LIVE mode"})
            try:
                code, resp = api_set_wallet_mode(instance, mode, source=instance)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/wallet/go_live":
            if body.get("confirm_live") is not True:
                return self._send(400, {"error": "explicit confirm_live=true required for LIVE mode"})
            if self._is_admin():
                return self._send(409, {"error": "ADMIN credentials are shared across all current ADMIN bots; configure them from the Wallet page"})
            private_key    = body.get("private_key", "")
            api_key        = body.get("api_key", "")
            api_secret     = body.get("api_secret", "")
            api_passphrase = body.get("api_passphrase", "")
            # Determine which instances to configure
            raw = body.get("instances")
            if not isinstance(raw, list) or not raw:
                return self._send(400, {"error": "select explicit instances for LIVE mode"})
            if self._is_admin():
                instances = [i for i in raw if isinstance(i, str) and re.fullmatch(r'[\w-]+', i)]
            else:
                owned = self._user_instances()
                instances = [i for i in raw if i in owned]
            if not instances or len(instances) != len(raw) or len(set(instances)) != len(instances):
                return self._send(403, {"error": "one or more selected instances are unavailable"})
            if not private_key.strip():
                return self._send(400, {"error": "private_key is required"})
            try:
                code, resp = api_go_live(instances, private_key, api_key, api_secret, api_passphrase)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/wallet/paper":
            raw = body.get("instances")
            if not isinstance(raw, list) or not raw:
                return self._send(400, {"error": "select explicit instances for PAPER mode"})
            if not self._is_admin():
                owned = self._user_instances()
                instances = [i for i in raw if i in owned]
            else:
                instances = [i for i in raw if isinstance(i, str) and re.fullmatch(r'[\w-]+', i)]
            if not instances or len(instances) != len(raw) or len(set(instances)) != len(instances):
                return self._send(403, {"error": "one or more selected instances are unavailable"})
            try:
                code, resp = api_switch_paper(instances)
                return self._send(code, resp)
            except Exception as e:
                return self._send(500, {"error": str(e)})

        if u.path == "/api/polytron/chat":
            messages = body.get("messages", [])
            instance = body.get("instance", "")
            if not isinstance(messages, list) or not messages:
                return self._send(400, {"error": "messages required"})
            code, resp = api_polytron_chat(messages, self._username or "anon", instance)
            return self._send(code, resp)

        if u.path == "/api/push/register":
            fcm_token = body.get("fcm_token", "")
            device_id = body.get("device_id", "")
            if not fcm_token or not device_id:
                return self._send(400, {"error": "fcm_token and device_id required"})
            code, resp = api_push_register(self._username or "anon", fcm_token, device_id)
            return self._send(code, resp)

        if u.path == "/api/push/preferences":
            code, resp = api_push_preferences_write(self._username or "anon", body)
            return self._send(code, resp)

        self._send(404, {"error": "not found", "path": u.path})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("COCKPIT_PORT", "8888")))
    ap.add_argument("--host", default=os.environ.get("COCKPIT_HOST", "127.0.0.1"))
    args = ap.parse_args()
    _migrate_from_legacy()
    get_pool()  # fail fast if DB unreachable
    api_manual_predictions()  # collect forecasts even with no connected viewers
    _demo_startup_cleanup()
    _init_fcm()
    threading.Thread(target=_demo_cleanup_loop, daemon=True, name="demo-cleanup").start()
    if _fcm_available:
        threading.Thread(target=_push_dispatcher_loop, daemon=True, name="push-dispatcher").start()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"PolySparQ++ Cockpit → http://{args.host}:{args.port}/  (Ctrl-C to stop)")
    print(f"  DB={DSN}  UI={UI_FILE}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
