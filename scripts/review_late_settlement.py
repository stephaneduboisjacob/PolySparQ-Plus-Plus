#!/usr/bin/env python3
"""Read-only paper review for the late-settlement quote-band strategy.

This summarizes recorded entry orders and completed settlement exposures. It
does not estimate queue fills, invent quotes, change strategy settings, or write
to PostgreSQL.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from datetime import datetime, timezone
import json
import sys

import cockpit
from cockpit_review import completed_exposures


INSTANCE = "btc-late-settlement-sniper"
ENTRY_SOURCE = "LATE_SETTLEMENT_ENTRY"
PRICE_BANDS = ((0.75, 0.80), (0.80, 0.85), (0.85, 0.90), (0.90, 0.95))


def timestamp_ms(value: object) -> int:
    if isinstance(value, datetime):
        return int(value.timestamp() * 1000)
    if isinstance(value, (int, float)):
        return int(value if value > 100_000_000_000 else value * 1000)
    return int(datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp() * 1000)


def money(value: float) -> float:
    return round(value, 6)


def utc_iso(timestamp: int) -> str:
    return datetime.fromtimestamp(timestamp / 1000.0, timezone.utc).isoformat(timespec="milliseconds")


def summarize(days: int, include_details: bool = False) -> dict[str, object]:
    interval = f"{days} days"
    fills = cockpit.q(
        """SELECT f.id, f.ts, f.price, f.size_shares, f.fee_usd,
                  o.client_order_id::text AS client_order_id, o.mode::text AS mode,
                  o.instance, o.session_id::text AS session_id, o.condition_id,
                  o.token_id, o.side_label::text AS side_label,
                  o.direction::text AS direction, o.source, o.execution_mode,
                  o.post_only, o.ts_created, o.price AS order_price,
                  o.notional_usd, o.status, m.asset, m.horizon_seconds,
                  m.window_close
           FROM vp.fills f
           JOIN vp.orders o USING (client_order_id)
           LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
           WHERE o.mode='paper' AND o.instance=%s
             AND f.ts >= now() - %s::interval
           ORDER BY f.ts, f.id""",
        (INSTANCE, interval),
    )

    normalized = []
    for row in fills:
        item = dict(row)
        item["ts"] = timestamp_ms(item["ts"])
        normalized.append(item)

    closed, excluded = completed_exposures(normalized)
    settled = [
        row for row in closed
        if row.get("entry_source") == ENTRY_SOURCE
        and "settle" in row.get("exit_sources", [])
        and row.get("settled_outcome") in (0, 1)
    ]

    entry_orders = cockpit.q(
        """SELECT o.client_order_id::text AS client_order_id,
                  o.condition_id, o.ts_created, o.price, o.notional_usd,
                  o.size_shares, o.status, o.direction, o.side_label::text AS side_label,
                  o.source, o.execution_mode,
                  m.asset, m.horizon_seconds, m.window_close,
                  COALESCE(SUM(f.size_shares), 0) AS filled_shares,
                  COALESCE(SUM(f.price * f.size_shares), 0) AS filled_notional,
                  COALESCE(SUM(f.fee_usd), 0) AS fees
           FROM vp.orders o
           LEFT JOIN vp.fills f USING (client_order_id)
           LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
           WHERE o.mode='paper' AND o.instance=%s AND o.source=%s
             AND o.direction='buy' AND o.ts_created >= now() - %s::interval
           GROUP BY o.client_order_id, m.asset, m.horizon_seconds, m.window_close
           ORDER BY o.ts_created""",
        (INSTANCE, ENTRY_SOURCE, interval),
    )

    grouped: dict[tuple[str, int], list[dict[str, object]]] = defaultdict(list)
    windows: dict[str, list[dict[str, object]]] = defaultdict(list)
    for order in entry_orders:
        close = order.get("window_close")
        created = order.get("ts_created")
        horizon = int(order.get("horizon_seconds") or 0)
        remaining = ((timestamp_ms(close) - timestamp_ms(created)) / 1000.0
                     if close is not None and created is not None else None)
        order["seconds_to_close_at_submit"] = remaining
        order["time_window_ok"] = bool(
            remaining is not None and 0 < remaining <= (30 if horizon == 300 else 90 if horizon == 900 else 0)
        )
        requested = float(order.get("notional_usd") or 0)
        filled_notional = float(order.get("filled_notional") or 0)
        order["fill_notional_ratio"] = filled_notional / requested if requested > 0 else None
        limit_price = float(order.get("price") or 0)
        order["limit_price_in_spec_band"] = 0.75 < limit_price < 0.95
        order["order_cap_ok"] = float(order.get("notional_usd") or 0) <= 25.000001
        if order.get("condition_id"):
            windows[str(order["condition_id"])].append(order)
        key = (str(order.get("asset") or "unknown"), horizon)
        grouped[key].append(order)

    for orders in windows.values():
        one_order = len(orders) == 1
        for order in orders:
            order["one_order_in_window"] = one_order
    time_window_violations = [
        {
            "asset": order.get("asset"),
            "horizon_seconds": order.get("horizon_seconds"),
            "seconds_to_close_at_submit": round(float(order["seconds_to_close_at_submit"]), 3)
                if order.get("seconds_to_close_at_submit") is not None else None,
            "limit_price": float(order.get("price") or 0),
            "status": order.get("status"),
        }
        for order in entry_orders if not order["time_window_ok"]
    ]
    terminal_partial_taker_fills = []
    other_terminal_underfills = []
    for order in entry_orders:
        if (order.get("status") != "filled" or order.get("fill_notional_ratio") is None
                or order["fill_notional_ratio"] >= 0.90):
            continue
        requested = float(order.get("notional_usd") or 0)
        filled = float(order.get("filled_notional") or 0)
        shares = float(order.get("filled_shares") or 0)
        price = float(order.get("price") or 0)
        requested_shares_at_limit = requested / price if requested > 0 and price > 0 else None
        diagnostic = {
            "asset": order.get("asset"),
            "horizon_seconds": order.get("horizon_seconds"),
            "status": order.get("status"),
            "execution_mode": order.get("execution_mode"),
            "requested_notional_usd": round(requested, 4),
            "filled_notional_usd": round(filled, 4),
            "unfilled_request_usd": round(max(0.0, requested - filled), 4),
            "filled_shares": round(shares, 4),
            "requested_shares_at_limit": round(requested_shares_at_limit, 4)
                if requested_shares_at_limit is not None else None,
            "share_fill_ratio": round(shares / requested_shares_at_limit, 4)
                if requested_shares_at_limit else None,
            "limit_price": price,
        }
        if order.get("source") == ENTRY_SOURCE and order.get("execution_mode") == "taker":
            diagnostic["paper_execution_semantics"] = "top_of_book_FAK_partial; unfilled remainder is terminal"
            terminal_partial_taker_fills.append(diagnostic)
        else:
            other_terminal_underfills.append(diagnostic)

    band_rows = []
    for lower, upper in PRICE_BANDS:
        cohort = [r for r in settled if lower <= float(r.get("entry_price") or 0) < upper]
        unique = {str(r.get("condition_id")) for r in cohort if r.get("condition_id")}
        total_fees = sum(float(r.get("entry_fees") or 0) + float(r.get("exit_fees") or 0) for r in cohort)
        band_rows.append({
            "entry_price_band": f"{lower:.2f}-{upper:.2f}",
            "profile_exposures": len(cohort),
            "unique_market_windows": len(unique),
            "settlement_wins": sum(r["settled_outcome"] == 1 for r in cohort),
            "settlement_losses": sum(r["settled_outcome"] == 0 for r in cohort),
            "net_after_recorded_fees_usd": money(sum(float(r["net"]) for r in cohort)),
            "recorded_fees_usd": money(total_fees),
        })

    total_fees = sum(float(r.get("entry_fees") or 0) + float(r.get("exit_fees") or 0) for r in settled)
    order_count = len(entry_orders)
    close_by_market = {
        str(order["condition_id"]): timestamp_ms(order["window_close"])
        for order in entry_orders
        if order.get("condition_id") and order.get("window_close") is not None
    }
    settled_details = []
    for row in sorted(settled, key=lambda item: item["exit_ms"]):
        close_ms = close_by_market.get(str(row.get("condition_id")))
        settled_details.append({
            "asset": row.get("asset"),
            "horizon_seconds": row.get("horizon_seconds"),
            "side": row.get("side"),
            "entry_source": row.get("entry_source"),
            "entry_execution": row.get("entry_execution"),
            "entry_utc": utc_iso(row["first_entry_ms"]),
            "settlement_utc": utc_iso(row["exit_ms"]),
            "seconds_to_close_at_entry": round((close_ms - row["first_entry_ms"]) / 1000.0, 3)
                if close_ms is not None else None,
            "shares": round(float(row["shares"]), 4),
            "entry_vwap": round(float(row["entry_price"]), 4),
            "settlement_price": int(row["settled_outcome"]),
            "entry_orders": int(row["entry_orders"]),
            "exit_orders": int(row["exit_orders"]),
            "entry_fees_usd": money(float(row["entry_fees"])),
            "exit_fees_usd": money(float(row["exit_fees"])),
            "total_fees_usd": money(float(row["entry_fees"]) + float(row["exit_fees"])),
            "net_after_recorded_fees_usd": money(float(row["net"])),
            "exit_sources": row.get("exit_sources", []),
        })
    session_start = cockpit.accounting_since(INSTANCE, "paper")
    current_positions = cockpit.reconstructed_positions("paper", session_start, INSTANCE)
    pending_orders = cockpit.api_execution(INSTANCE).get("open_orders", [])
    result = {
        "scope": {"mode": "paper", "instance": INSTANCE, "entry_source": ENTRY_SOURCE,
                  "fill_lookback_days": days},
        "entry_orders": {
            "count": order_count,
            "time_window_ok": sum(bool(o["time_window_ok"]) for o in entry_orders),
            "limit_price_strictly_inside_0_75_0_95": sum(bool(o["limit_price_in_spec_band"]) for o in entry_orders),
            "order_cap_at_most_25_usd": sum(bool(o["order_cap_ok"]) for o in entry_orders),
            "single_order_per_market_window": sum(bool(o.get("one_order_in_window")) for o in entry_orders),
            "time_window_violations": time_window_violations,
            "terminal_fill_underfill_diagnostics": {
                "paper_taker_fak_partials": terminal_partial_taker_fills,
                "other_underfilled_filled_status_orders": other_terminal_underfills,
            },
            "by_asset_horizon": [
                {"asset": asset, "horizon_seconds": horizon, "orders": len(orders)}
                for (asset, horizon), orders in sorted(grouped.items())
            ],
        },
        "settled_exposures": {
            "profile_exposures": len(settled),
            "unique_market_windows": len({str(r.get("condition_id")) for r in settled if r.get("condition_id")}),
            "wins": sum(r["settled_outcome"] == 1 for r in settled),
            "losses": sum(r["settled_outcome"] == 0 for r in settled),
            "net_after_recorded_fees_usd": money(sum(float(r["net"]) for r in settled)),
            "recorded_fees_usd": money(total_fees),
            "entry_price_bands": band_rows,
        },
        "unresolved_or_non_settlement_exposures": sum(
            1 for r in closed if r.get("entry_source") == ENTRY_SOURCE and
            ("settle" not in r.get("exit_sources", []) or r.get("settled_outcome") not in (0, 1))
        ),
        "current_session_open_positions": len(current_positions.get("positions", [])),
        "pending_paper_orders_across_sessions": len(pending_orders),
        "unfilled_entry_orders": sum(float(o.get("filled_shares") or 0) <= 0 for o in entry_orders),
        "limitations": [
            "Limit price is used as a proxy for the qualifying ask; the order table does not retain the decision-time ask.",
            "Paper fills do not model live queue priority or establish executable depth.",
            "The paper taker-buy path caps fills to visible best-ask size and terminalizes the remainder (FAK); historical best-ask-size snapshots are not retained, so individual underfill depth cannot be independently replayed.",
            "Outcome counts use settled token fills; mirrored exposure in one market is reported separately from unique market windows.",
            "A short or single-window sample is not evidence of a durable edge.",
        ],
    }
    if include_details:
        result["settled_exposure_details"] = settled_details
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--days", type=int, default=7, help="paper ledger lookback (1-30 days; default: 7)")
    parser.add_argument("--details", action="store_true",
                        help="include settlement outcome rows without market identifiers")
    args = parser.parse_args()
    if not 1 <= args.days <= 30:
        parser.error("--days must be between 1 and 30")
    try:
        print(json.dumps(summarize(args.days, args.details), indent=2, sort_keys=True))
    except Exception as exc:
        print(json.dumps({"error": type(exc).__name__, "detail": "read-only review failed"}), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
