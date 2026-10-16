#!/usr/bin/env python3
"""Read-only realized PAPER PnL by bot for a recent exit-time interval.

The interval is selected by the final fill in a complete market-side exposure.
Open positions are reported separately and never counted as realized PnL.
Only aggregate trade details are emitted; market and order identifiers are
omitted so this output is suitable for an operator report.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from datetime import datetime, timezone
import json
import sys

import cockpit
from cockpit_review import completed_exposures, timestamp_ms


def utc_text(value_ms: int) -> str:
    return datetime.fromtimestamp(value_ms / 1000, timezone.utc).isoformat()


def exposure_metrics(rows: list[dict[str, object]]) -> dict[str, object]:
    nets = [float(row["net"]) for row in rows]
    return {
        "closed_exposures": len(rows),
        "unique_market_windows": len({row.get("condition_id") for row in rows}),
        "wins": sum(value > 0 for value in nets),
        "losses": sum(value < 0 for value in nets),
        "breakeven": sum(value == 0 for value in nets),
        "realized_net_after_fees_usd": round(sum(nets), 6),
        "recorded_fees_usd": round(sum(
            float(row["entry_fees"]) + float(row["exit_fees"]) for row in rows
        ), 6),
    }


def summarize(minutes: int, lookback_days: int) -> dict[str, object]:
    clock_rows = cockpit.q(
        """SELECT now() AS server_now,
                  (SELECT max(id) FROM vp.fills) AS fill_id,
                  (SELECT max(ts) FROM vp.fills) AS fill_ts"""
    )
    if not clock_rows:
        raise RuntimeError("database clock query returned no row")
    server_now = clock_rows[0]["server_now"]
    now_ms = timestamp_ms(server_now)
    interval_start_ms = now_ms - minutes * 60_000

    fill_rows = cockpit.q(
        """SELECT f.id, f.ts, f.price, f.size_shares, f.fee_usd,
                  o.client_order_id::text AS client_order_id,
                  o.mode::text AS mode, o.instance, o.session_id::text AS session_id,
                  o.condition_id, o.token_id, o.side_label::text AS side_label,
                  o.direction::text AS direction, o.source, o.execution_mode,
                  m.asset, m.horizon_seconds
           FROM vp.fills f
           JOIN vp.orders o USING (client_order_id)
           LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
           WHERE o.mode='paper' AND f.ts >= now() - %s::interval
           ORDER BY f.ts, f.id""",
        (f"{lookback_days} days",),
    )
    fills = [dict(row) for row in fill_rows]
    completed, excluded = completed_exposures(
        [row for row in fills if timestamp_ms(row["ts"]) <= now_ms]
    )
    interval_rows = [row for row in completed
                     if interval_start_ms <= row["exit_ms"] <= now_ms]

    by_instance: dict[str, list[dict[str, object]]] = defaultdict(list)
    for row in interval_rows:
        by_instance[str(row.get("instance") or "unknown")].append(row)
    summaries = {
        instance: exposure_metrics(rows)
        for instance, rows in sorted(by_instance.items())
    }
    summaries["ALL PAPER"] = exposure_metrics(interval_rows)

    reject_rows = cockpit.q(
        """SELECT instance, coalesce(source, '(none)') AS source,
                  coalesce(reject_reason, '(none)') AS reason, count(*) AS rejected
           FROM vp.orders
           WHERE mode='paper' AND status::text='rejected'
             AND ts_created >= now() - %s::interval
           GROUP BY 1,2,3 ORDER BY rejected DESC, instance, source, reason""",
        (f"{minutes} minutes",),
    )
    rejections = [{
        "instance": row["instance"], "source": row["source"],
        "reason": row["reason"], "orders": int(row["rejected"]),
    } for row in reject_rows]

    # Use the same current-session reconstruction as the cockpit. A simple
    # multi-day fill balance would incorrectly label positions from old paper
    # sessions as current after a safe session boundary.
    current_positions: dict[str, object] = {}
    for bot in cockpit.api_fleet_detail().get("fleet", []):
        if not bot.get("running"):
            continue
        instance = str(bot.get("instance") or "")
        execution = cockpit.api_execution(instance)
        if str(execution.get("mode", "")).lower() != "paper":
            continue
        session_start = cockpit.accounting_since(instance, "paper")
        account = cockpit.reconstructed_positions("paper", session_start, instance)
        positions = account.get("positions", [])
        current_positions[instance] = {
            "positions": len(positions),
            "assets": sorted({str(row["asset"]) for row in positions if row.get("asset")}),
            "deployed_usd": round(float(account.get("deployed_usd") or 0), 6),
            "pending_orders": len(execution.get("open_orders", [])),
            "accounting_errors": len(account.get("accounting_errors", [])),
        }

    details = [{
        "instance": row.get("instance"),
        "asset": row.get("asset"),
        "horizon_seconds": row.get("horizon_seconds"),
        "side": row.get("side"),
        "entry_source": row.get("entry_source"),
        "entry_execution": row.get("entry_execution"),
        "exit_sources": row.get("exit_sources"),
        "entry_utc": utc_text(int(row["first_entry_ms"])),
        "exit_utc": utc_text(int(row["exit_ms"])),
        "shares": round(float(row["shares"]), 6),
        "entry_vwap": round(float(row["entry_price"]), 6),
        "entry_fees_usd": round(float(row["entry_fees"]), 6),
        "exit_fees_usd": round(float(row["exit_fees"]), 6),
        "realized_net_after_fees_usd": round(float(row["net"]), 6),
        "settled_outcome": row.get("settled_outcome"),
    } for row in sorted(interval_rows, key=lambda item: item["exit_ms"])]

    cursor_ts = clock_rows[0].get("fill_ts")
    return {
        "scope": {
            "mode": "paper",
            "interval_minutes": minutes,
            "exit_time_window_utc": {
                "from": utc_text(interval_start_ms),
                "through": utc_text(now_ms),
            },
            "fill_reconstruction_lookback_days": lookback_days,
            "sample_unit": "completed instance/session/market/token exposure",
        },
        "by_instance": summaries,
        "closed_exposure_details": details,
        "current_session_positions_excluded_from_pnl": current_positions,
        "rejected_orders_in_interval": rejections,
        "excluded_incomplete_exposures": excluded,
        "fill_cursor": {
            "id": clock_rows[0].get("fill_id"),
            "ts_utc": utc_text(timestamp_ms(cursor_ts)) if cursor_ts is not None else None,
        },
        "limitations": [
            "Only fills and recorded fees are used; unsettled positions are not marked to market.",
            "The lookback must exceed the longest market horizon to reconstruct exits reliably.",
            "Paper fills do not establish live queue priority or executable depth.",
            "No decision-time signal or alternate executable price is inferred.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--minutes", type=int, default=15,
                        help="exit-time interval in minutes (1-1440)")
    parser.add_argument("--lookback-days", type=int, default=7,
                        help="fill reconstruction history (1-30 days)")
    args = parser.parse_args()
    if not 1 <= args.minutes <= 1440:
        parser.error("--minutes must be between 1 and 1440")
    if not 1 <= args.lookback_days <= 30:
        parser.error("--lookback-days must be between 1 and 30")
    try:
        print(json.dumps(summarize(args.minutes, args.lookback_days), indent=2, sort_keys=True))
    except Exception as exc:
        print(json.dumps({"error": type(exc).__name__, "detail": "read-only interval review failed"}),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
