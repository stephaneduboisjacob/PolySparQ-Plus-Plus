#!/usr/bin/env python3
"""Read-only timing study for lead entries near their configured flatten gate.

The report groups completed PAPER market-side exposures by how close the first
fill was to that profile's horizon-specific flatten threshold. It uses recorded
fills and fees only; it does not invent executable prices or exit counterfactuals.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from datetime import datetime
import json
from pathlib import Path
import sys
import tomllib

import cockpit
from cockpit_review import completed_exposures


DEFAULT_INSTANCES = ("btc-5m-lead-scalper", "btc-15m-lead-scalper")
ELIGIBLE_ASSETS = {"BTC", "SOL"}
ENTRY_SOURCE = "CB_LEAD_SCALP_ENTRY"


def timestamp_ms(value: object) -> int:
    if isinstance(value, datetime):
        return int(value.timestamp() * 1000)
    if isinstance(value, (int, float)):
        return int(value if value > 100_000_000_000 else value * 1000)
    return int(datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp() * 1000)


def load_paper_flatten_thresholds(instances: tuple[str, ...]) -> dict[tuple[str, int], int]:
    root = Path(__file__).resolve().parents[1]
    thresholds: dict[tuple[str, int], int] = {}
    for instance in instances:
        path = root / "config" / f"polypulse-{instance}.toml"
        data = tomllib.loads(path.read_text())
        if str(data.get("runtime", {}).get("mode", "")).lower() != "paper":
            raise ValueError(f"{instance} config is not PAPER")
        hot = data.get("strategy", {}).get("hot", {})
        fallback = int(hot.get("lead_lag_scalp_flatten_secs", 0))
        for horizon, suffix in ((300, "5m"), (900, "15m")):
            value = int(hot.get(f"lead_lag_scalp_flatten_{suffix}_secs", fallback))
            if value <= 0:
                raise ValueError(f"missing positive {suffix} flatten threshold for {instance}")
            thresholds[(instance, horizon)] = value
    return thresholds


def summarize(days: int, boundary_seconds: float, instances: tuple[str, ...],
              include_details: bool = False) -> dict[str, object]:
    thresholds = load_paper_flatten_thresholds(instances)
    fills = cockpit.q(
        """SELECT f.id, f.ts, f.price, f.size_shares, f.fee_usd,
                  o.client_order_id::text AS client_order_id, o.mode::text AS mode,
                  o.instance, o.session_id::text AS session_id, o.condition_id,
                  o.token_id, o.side_label::text AS side_label,
                  o.direction::text AS direction, o.source, o.execution_mode,
                  m.asset, m.horizon_seconds, m.window_close
           FROM vp.fills f
           JOIN vp.orders o USING (client_order_id)
           LEFT JOIN vp.pm_markets m ON m.condition_id=o.condition_id
           WHERE o.mode='paper' AND o.instance=ANY(%s)
             AND f.ts >= now() - %s::interval
           ORDER BY f.ts, f.id""",
        (list(instances), f"{days} days"),
    )
    fills = [dict(row) for row in fills]
    window_close_by_key = {
        (row["instance"], row.get("session_id"), row.get("condition_id"), row.get("token_id")):
            timestamp_ms(row["window_close"])
        for row in fills if row.get("window_close") is not None
    }
    completed, excluded = completed_exposures(fills)

    rows = []
    for exposure in completed:
        if exposure.get("entry_source") != ENTRY_SOURCE:
            continue
        asset = str(exposure.get("asset") or "").upper()
        horizon = int(exposure.get("horizon_seconds") or 0)
        if asset not in ELIGIBLE_ASSETS or horizon not in (300, 900):
            continue
        key = (exposure["instance"], exposure.get("session_id"),
               exposure.get("condition_id"), exposure.get("token_id"))
        close_ms = window_close_by_key.get(key)
        threshold = thresholds.get((exposure["instance"], horizon))
        if close_ms is None or threshold is None:
            continue

        entry_remaining = (close_ms - exposure["first_entry_ms"]) / 1000.0
        exit_remaining = (close_ms - exposure["exit_ms"]) / 1000.0
        seconds_before_gate = entry_remaining - threshold
        if seconds_before_gate < -0.5:
            bucket = "entry_at_or_after_flatten_gate"
        elif seconds_before_gate <= boundary_seconds:
            bucket = "within_boundary_buffer"
        else:
            bucket = "earlier_than_boundary_buffer"

        rows.append({
            "instance": exposure["instance"],
            "asset": asset,
            "horizon_seconds": horizon,
            "side": exposure.get("side"),
            "entry_remaining_seconds": round(entry_remaining, 3),
            "flatten_threshold_seconds": threshold,
            "seconds_before_flatten_gate": round(seconds_before_gate, 3),
            "exit_remaining_seconds": round(exit_remaining, 3),
            "hold_seconds_after_last_entry_fill": round(
                (exposure["exit_ms"] - exposure["last_entry_ms"]) / 1000.0, 3
            ),
            "shares": round(float(exposure["shares"]), 4),
            "entry_vwap": round(float(exposure["entry_price"]), 4),
            "recorded_fees_usd": round(
                float(exposure["entry_fees"]) + float(exposure["exit_fees"]), 6
            ),
            "realized_net_usd": round(float(exposure["net"]), 6),
            "_net_exact": float(exposure["net"]),
            "_fees_exact": float(exposure["entry_fees"]) + float(exposure["exit_fees"]),
            "entry_bucket": bucket,
            "exit_within_2s_of_flatten_clock": abs(exit_remaining - threshold) <= 2.0,
            "exit_uses_flatten_source": "CB_LEAD_SCALP_FLAT" in exposure.get("exit_sources", []),
            "exit_sources": exposure.get("exit_sources", []),
            "_condition_id": exposure.get("condition_id"),
        })

    groups: dict[str, list[dict[str, object]]] = defaultdict(list)
    for row in rows:
        groups[str(row["entry_bucket"])].append(row)

    def metrics(cohort: list[dict[str, object]]) -> dict[str, object]:
        values = [float(row["_net_exact"]) for row in cohort]
        return {
            "profile_exposures": len(cohort),
            "unique_market_windows": len({row["_condition_id"] for row in cohort}),
            "wins": sum(value > 0 for value in values),
            "losses": sum(value < 0 for value in values),
            "realized_net_usd": round(sum(values), 6),
            "recorded_fees_usd": round(sum(float(row["_fees_exact"]) for row in cohort), 6),
            "exits_within_2s_of_flatten_clock": sum(
                bool(row["exit_within_2s_of_flatten_clock"]) for row in cohort
            ),
            "flatten_source_exits_within_2s_of_clock": sum(
                bool(row["exit_within_2s_of_flatten_clock"] and row["exit_uses_flatten_source"])
                for row in cohort
            ),
        }

    asset_horizon_side: dict[tuple[str, int, str], list[dict[str, object]]] = defaultdict(list)
    for row in rows:
        asset_horizon_side[(str(row["asset"]), int(row["horizon_seconds"]),
                            str(row["side"]))].append(row)
    stratified = []
    for (asset, horizon, side), cohort in sorted(asset_horizon_side.items()):
        windows: dict[str, list[dict[str, object]]] = defaultdict(list)
        for row in cohort:
            windows[str(row.get("_condition_id") or "unknown")].append(row)
        window_nets = [sum(float(row["_net_exact"]) for row in market_rows)
                       for market_rows in windows.values()]
        window_fees = [sum(float(row["_fees_exact"]) for row in market_rows)
                       for market_rows in windows.values()]
        stratified.append({
            "asset": asset,
            "horizon_seconds": horizon,
            "side": side,
            "profile_metrics": metrics(cohort),
            "market_window_metrics": {
                "unique_market_windows": len(windows),
                "wins": sum(value > 0 for value in window_nets),
                "losses": sum(value < 0 for value in window_nets),
                "realized_net_usd": round(sum(window_nets), 6),
                "recorded_fees_usd": round(sum(window_fees), 6),
                "mirrored_windows": sum(len(market_rows) > 1
                                         for market_rows in windows.values()),
            },
        })

    exit_timing = []
    for (asset, horizon, side), cohort in sorted(asset_horizon_side.items()):
        flatten_source = [row for row in cohort if row["exit_uses_flatten_source"]]
        other_sources = [row for row in cohort if not row["exit_uses_flatten_source"]]
        near_clock = [row for row in cohort if row["exit_within_2s_of_flatten_clock"]]
        away_from_clock = [row for row in cohort if not row["exit_within_2s_of_flatten_clock"]]
        exit_timing.append({
            "asset": asset,
            "horizon_seconds": horizon,
            "side": side,
            "all_exits": metrics(cohort),
            "flatten_source_exits": metrics(flatten_source),
            "other_source_exits": metrics(other_sources),
            "within_2s_of_flatten_clock": metrics(near_clock),
            "outside_2s_of_flatten_clock": metrics(away_from_clock),
        })

    result = {
        "scope": {"mode": "paper", "instances": list(instances), "assets": sorted(ELIGIBLE_ASSETS),
                  "lookback_days": days, "entry_boundary_buffer_seconds": boundary_seconds,
                  "flatten_thresholds_seconds": {
                      f"{instance}:{horizon}s": threshold
                      for (instance, horizon), threshold in sorted(thresholds.items())
                  }},
        "sample_unit": "completed instance/session/market/side exposure; mirrored market exposure is correlated",
        "summary_by_entry_bucket": {
            bucket: metrics(groups.get(bucket, []))
            for bucket in ("within_boundary_buffer", "earlier_than_boundary_buffer",
                           "entry_at_or_after_flatten_gate")
        },
        "summary_by_asset_horizon_side": stratified,
        "summary_by_exit_timing_asset_horizon_side": exit_timing,
        "overall": metrics(rows),
        "completed_profile_exposures": len(rows),
        "excluded_incomplete_exposures": excluded,
        "limitations": [
            "Uses first and last recorded fill timestamps and final exit fill timestamps.",
            "A near-gate loss does not establish that the entry or flatten rule caused it.",
            "No alternative executable bid, queue position, or post-exit book mark is synthesized.",
            "Paper fills and recorded fees are simulator evidence, not live execution evidence.",
            "Repeated or mirrored fills within a market are not independent samples.",
        ],
    }
    if include_details:
        result["exposure_details"] = [
            {key: value for key, value in row.items() if not key.startswith("_")}
            for row in rows
        ]
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--days", type=int, default=30, help="PAPER ledger lookback (1-30 days)")
    parser.add_argument("--boundary-seconds", type=float, default=5.0,
                        help="entry buffer before the flatten gate (1-30 seconds)")
    parser.add_argument("--instances", nargs="+", default=list(DEFAULT_INSTANCES),
                        choices=list(DEFAULT_INSTANCES))
    parser.add_argument("--details", action="store_true",
                        help="include individual exposure rows (market identifiers remain omitted)")
    args = parser.parse_args()
    if not 1 <= args.days <= 30:
        parser.error("--days must be between 1 and 30")
    if not 1 <= args.boundary_seconds <= 30:
        parser.error("--boundary-seconds must be between 1 and 30")
    try:
        print(json.dumps(summarize(args.days, args.boundary_seconds, tuple(args.instances), args.details),
                         indent=2, sort_keys=True))
    except Exception as exc:
        print(json.dumps({"error": type(exc).__name__, "detail": "read-only boundary review failed"}),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
