#!/usr/bin/env python3
"""Compare current-session PAPER exits for matched lead-scalper exposures.

Pairs are keyed by market and outcome token. The report includes all common
exposures and separately marks near-synchronous entries; neither paper fills
nor these correlated pairs establish live profitability.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from collections import defaultdict
import subprocess

import cockpit
from cockpit_review import completed_exposures, timestamp_ms


PAIRS = (
    ("5m · isolated stop10/tp20", "btc-5m-lead-scalper",
     "btc-5m-lead-scalper-stop10-tp20-isolated-test"),
    ("5m · Coinbase lead only vs mixed entry paths",
     "btc-5m-lead-scalper",
     "btc-5m-lead-scalper-cb-only-isolated-test"),
    ("5m · maker reprice 1s vs 250ms",
     "btc-5m-lead-scalper",
     "btc-5m-lead-scalper-reprice1s-isolated-test"),
    ("5m · isolated stop5/tp20 vs stop10",
     "btc-5m-lead-scalper-stop10-tp20-isolated-test",
     "btc-5m-lead-scalper-stop5-tp20-isolated-test"),
    ("5m · 90s opening delay only",
     "btc-5m-lead-scalper-stop10-tp20-isolated-test",
     "btc-5m-lead-scalper-delay90s-isolated-test"),
    ("5m · stop10/tp20 lead vs OFI impulse",
     "btc-5m-lead-scalper-stop10-tp20-isolated-test",
     "btc-5m-lead-scalper-stop10-tp20-impulse-isolated-test"),
    ("5m · isolated stop25/tp50", "btc-5m-lead-scalper",
     "btc-5m-lead-scalper-tp50-isolated-test"),
    ("5m · isolated stop25/tp30", "btc-5m-lead-scalper",
     "btc-5m-lead-scalper-tp30-isolated-test"),
    ("5m · model confidence default vs 0.50", "btc-5m-lead-scalper",
     "btc-5m-lead-scalper-conf50-isolated-test"),
    ("5m · stop10/tp50 bundle (not isolated)", "btc-5m-lead-scalper",
     "btc-5m-lead-scalper-stop10-tp50-matched-test"),
    ("15m · isolated stop10/tp20", "btc-15m-lead-scalper",
     "btc-15m-lead-scalper-stop10-tp20-isolated-test"),
    ("15m · isolated stop5/tp20", "btc-15m-lead-scalper",
     "btc-15m-lead-scalper-stop5-tp20-isolated-test"),
    ("15m · maker improve 1 tick vs 2 ticks", "btc-15m-lead-scalper",
     "btc-15m-lead-scalper-improve2-isolated-test"),
    ("15m · retry cap 5 vs 10", "btc-15m-lead-scalper",
     "btc-15m-lead-scalper-retry10-isolated-test"),
    ("15m · model confidence 0.08 vs 0.50", "btc-15m-lead-scalper",
     "btc-15m-lead-scalper-conf50-isolated-test"),
    ("15m · delayed-open only (first 5m skipped)", "btc-15m-lead-scalper",
     "btc-15m-lead-scalper-delay5m-isolated-test"),
)


def running_bot_instances() -> set[str]:
    result = subprocess.run(
        ["systemctl", "--user", "list-units", "--type=service",
         "--state=running", "polypulse-bot@*.service", "--no-legend",
         "--no-pager"],
        check=True, capture_output=True, text=True, timeout=5,
    )
    names = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        unit = fields[0] if fields else ""
        if unit.startswith("polypulse-bot@") and unit.endswith(".service"):
            names.add(unit[len("polypulse-bot@"): -len(".service")])
    return names


def fmt_money(value: float) -> str:
    return f"{'+' if value >= 0 else '-'}${abs(value):.2f}"


def fmt_roi(rows: list[dict[str, object]]) -> str:
    capital = sum(float(row["entry_cost"]) + float(row["entry_fees"])
                  for row in rows)
    net = sum(float(row["net"]) for row in rows)
    return f"{100.0 * net / capital:+.2f}%" if capital > 0 else "—"


def summarize(hours: float, lookback_days: int, detail_limit: int) -> None:
    clock = cockpit.q("SELECT now() AS server_now", one=True)
    now_ms = timestamp_ms(clock["server_now"])
    since_ms = now_ms - int(hours * 3_600_000)
    instances = sorted({name for _, base, test in PAIRS for name in (base, test)})

    running = running_bot_instances()
    inactive = [name for name in instances if name not in running]
    if inactive:
        print("Skipping inactive bot sessions: " + ", ".join(inactive))

    session_starts: dict[str, int] = {}
    for instance in instances:
        if instance not in running:
            continue
        value = cockpit.accounting_since(instance, "paper")
        if isinstance(value, datetime):
            parsed = value
        else:
            normalized = cockpit.parse_systemd_wall_ts(str(value))
            parsed = datetime.fromisoformat(str(normalized).replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            # Bot logs and this cockpit host use UTC for naive timestamps.
            parsed = parsed.replace(tzinfo=timezone.utc)
        session_starts[instance] = int(parsed.timestamp() * 1000)

    fill_rows = cockpit.q(
        """SELECT f.id, f.ts, f.price, f.size_shares, f.fee_usd,
                  o.client_order_id::text AS client_order_id,
                  o.mode::text AS mode, o.instance,
                  o.session_id::text AS session_id, o.condition_id, o.token_id,
                  o.side_label::text AS side_label, o.direction::text AS direction,
                  o.source, o.execution_mode, m.asset, m.horizon_seconds
           FROM vp.fills f
           JOIN vp.orders o USING (client_order_id)
           LEFT JOIN vp.pm_markets m ON m.condition_id = o.condition_id
           WHERE o.mode = 'paper' AND o.instance = ANY(%s)
             AND f.ts >= now() - %s::interval
           ORDER BY f.ts, f.id""",
        (instances, f"{lookback_days} days"),
    )
    exposures, excluded = completed_exposures(
        [dict(row) for row in fill_rows if timestamp_ms(row["ts"]) <= now_ms]
    )

    print("LEAD SCALPER EXIT PAIRS · PAPER ONLY")
    print(f"Exit window: last {hours:g}h · fill reconstruction: {lookback_days}d")
    print("Pairs require both bots running now and both current-session exposures to be complete.")
    for label, baseline, challenger in PAIRS:
        if baseline not in session_starts or challenger not in session_starts:
            continue
        pair_start = max(session_starts[baseline], session_starts[challenger])
        left = {
            (row["condition_id"], row["token_id"]): row
            for row in exposures
            if row["instance"] == baseline
            and row["first_entry_ms"] >= pair_start
            and since_ms <= row["exit_ms"] <= now_ms
        }
        right = {
            (row["condition_id"], row["token_id"]): row
            for row in exposures
            if row["instance"] == challenger
            and row["first_entry_ms"] >= pair_start
            and since_ms <= row["exit_ms"] <= now_ms
        }
        pairs = [(left[key], right[key]) for key in left.keys() & right.keys()]
        pairs.sort(key=lambda pair: max(pair[0]["exit_ms"], pair[1]["exit_ms"]))
        close_entries = [
            pair for pair in pairs
            if abs(pair[0]["first_entry_ms"] - pair[1]["first_entry_ms"]) <= 5_000
            and abs(pair[0]["entry_price"] - pair[1]["entry_price"]) <= 0.03
        ]
        base_net = sum(float(pair[0]["net"]) for pair in pairs)
        test_net = sum(float(pair[1]["net"]) for pair in pairs)
        close_base_net = sum(float(pair[0]["net"]) for pair in close_entries)
        close_test_net = sum(float(pair[1]["net"]) for pair in close_entries)
        print(
            f"\n{label}: {baseline} vs {challenger} · same-market/token={len(pairs)} "
            f"near-synchronous={len(close_entries)}"
        )
        print(
            f"  all shared markets (descriptive only): baseline={fmt_money(base_net)} "
            f"({fmt_roi([pair[0] for pair in pairs])} ROI) "
            f"challenger={fmt_money(test_net)} "
            f"({fmt_roi([pair[1] for pair in pairs])} ROI) "
            f"delta={fmt_money(test_net - base_net)}"
        )
        if close_entries:
            print(
                f"  near-synchronous subset: baseline={fmt_money(close_base_net)} "
                f"({fmt_roi([pair[0] for pair in close_entries])} ROI) "
                f"challenger={fmt_money(close_test_net)} "
                f"({fmt_roi([pair[1] for pair in close_entries])} ROI) "
                f"delta={fmt_money(close_test_net - close_base_net)}"
            )
        else:
            print("  No near-synchronous completed exposure pairs yet.")
        if not pairs:
            print("  No shared completed market/token exposures in these current sessions yet.")
            continue
        if detail_limit:
            for base, test in pairs[-detail_limit:]:
                delta_ms = abs(base["first_entry_ms"] - test["first_entry_ms"])
                delta = float(test["net"]) - float(base["net"])
                print(
                    f"  {base.get('asset') or '?'} {int(base.get('horizon_seconds') or 0)//60}m "
                    f"{base.get('side') or '?'} · entry Δ={delta_ms}ms "
                    f"price Δ={base['entry_price']-test['entry_price']:+.3f} · "
                    f"near-sync={'yes' if (abs(base['first_entry_ms']-test['first_entry_ms']) <= 5_000 and abs(base['entry_price']-test['entry_price']) <= 0.03) else 'no'} · "
                    f"base {fmt_money(float(base['net']))} "
                    f"test {fmt_money(float(test['net']))} · Δ {fmt_money(delta)} · "
                    f"exits {','.join(base['exit_sources'])} / "
                    f"{','.join(test['exit_sources'])}"
                )
    print(
        "\nInterpretation limits: same-market pairs are correlated; near-synchronous "
        "entries can still have different fills. Paper queue priority and live fills "
        "are unverified."
    )
    if excluded:
        print(f"Incomplete or unmatched exposure groups excluded: {sum(excluded.values())}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hours", type=float, default=6.0)
    parser.add_argument("--lookback-days", type=int, default=7)
    parser.add_argument("--detail-limit", type=int, default=8)
    args = parser.parse_args()
    if not 0.1 <= args.hours <= 24 * 30:
        parser.error("--hours must be between 0.1 and 720")
    if not 1 <= args.lookback_days <= 30:
        parser.error("--lookback-days must be between 1 and 30")
    if not 0 <= args.detail_limit <= 100:
        parser.error("--detail-limit must be between 0 and 100")
    try:
        summarize(args.hours, args.lookback_days, args.detail_limit)
    except Exception as exc:
        print(f"report unavailable: {type(exc).__name__}: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
