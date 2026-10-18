#!/usr/bin/env python3
"""Audit PAPER maker fills against public Polymarket trade prints.

This is a plausibility check, not fill replay: public prints omit queue
position, aggressor is inferred from the local BBO, and feed/timestamp gaps
can hide executions.
"""

from __future__ import annotations

import argparse
import subprocess
import sys

import cockpit


DEFAULT_INSTANCES = [
    "btc-5m-lead-scalper",
    "btc-15m-lead-scalper",
    "btc-5m-ofi-impulse-paper-test",
    "btc-5m-lead-scalper-stop15-test",
    "btc-15m-lead-scalper-stop10-tp50-test",
    "btc-5m-article-sim-test",
    "cb",
]


def running_bot_instances() -> set[str]:
    result = subprocess.run(
        ["systemctl", "--user", "list-units", "--type=service",
         "--state=running", "polypulse-bot@*.service", "--no-legend",
         "--no-pager"],
        check=True, capture_output=True, text=True, timeout=5,
    )
    names = set()
    for line in result.stdout.splitlines():
        unit = line.split(maxsplit=1)[0] if line.split() else ""
        if unit.startswith("polypulse-bot@") and unit.endswith(".service"):
            names.add(unit[len("polypulse-bot@"): -len(".service")])
    return names


def summarize(hours: float, instances: list[str] | None,
              current_sessions: bool = False) -> None:
    default_scope = instances is None
    if instances is None and not current_sessions:
        instances = DEFAULT_INSTANCES
    if current_sessions:
        # ActiveEnterTimestamp survives after a unit stops. Only call these
        # "current process sessions" for bots that are running now.
        running = running_bot_instances()
        instances = sorted(running if default_scope else
                           running.intersection(instances or []))
        if not instances:
            print("No requested PAPER bot is currently running.")
            return

    session_cte = ""
    session_join = ""
    session_filter = ""
    if current_sessions:
        session_values = ", ".join(["(%s::text, %s::timestamptz)"] * len(instances))
        session_args = []
        for instance in instances:
            session_args.extend((instance, cockpit.accounting_since(instance, "paper")))
        session_cte = f"paper_sessions(instance, started_at) AS (VALUES {session_values}),\n"
        session_join = "JOIN paper_sessions ps ON ps.instance = o.instance"
        session_filter = "AND ps.started_at IS NOT NULL AND f.ts >= ps.started_at"
    else:
        session_args = []

    sql = f"""
    WITH
    {session_cte}maker_fills AS (
        SELECT f.id, f.ts, f.price, f.size_shares,
               o.instance, o.condition_id, o.token_id,
               o.direction::text AS direction, o.source
        FROM vp.fills f
        JOIN vp.orders o USING (client_order_id)
        {session_join}
        WHERE f.ts >= now() - %s::interval
          {session_filter}
          AND o.mode = 'paper'
          AND o.execution_mode = 'maker'
          AND o.instance = ANY(%s)
    ), evidence AS (
        SELECT f.*,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 0.1
               ) AS prints_100ms,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 0.5
               ) AS prints_500ms,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 2
               ) AS prints_2s,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 0.1
                     AND ((f.direction = 'buy' AND t.price <= f.price + 0.000001)
                       OR (f.direction = 'sell' AND t.price >= f.price - 0.000001))
               ) AS crosses_100ms,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 0.5
                     AND ((f.direction = 'buy' AND t.price <= f.price + 0.000001)
                       OR (f.direction = 'sell' AND t.price >= f.price - 0.000001))
               ) AS crosses_500ms,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 2
                     AND ((f.direction = 'buy' AND t.price <= f.price + 0.000001)
                       OR (f.direction = 'sell' AND t.price >= f.price - 0.000001))
               ) AS crosses_2s,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 10
                     AND ((f.direction = 'buy' AND t.price <= f.price + 0.000001)
                       OR (f.direction = 'sell' AND t.price >= f.price - 0.000001))
               ) AS crosses_10s,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 30
                     AND ((f.direction = 'buy' AND t.price <= f.price + 0.000001)
                       OR (f.direction = 'sell' AND t.price >= f.price - 0.000001))
               ) AS crosses_30s,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 2
                     AND t.aggressor IS NOT NULL
               ) AS classified_2s,
               count(t.id) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 2
                     AND ((f.direction = 'buy' AND t.price <= f.price + 0.000001
                           AND t.aggressor = 'sell')
                       OR (f.direction = 'sell' AND t.price >= f.price - 0.000001
                           AND t.aggressor = 'buy'))
               ) AS side_confirms_2s,
               coalesce(sum(t.size) FILTER (
                   WHERE abs(extract(epoch FROM (t.ts - f.ts))) <= 2
                     AND ((f.direction = 'buy' AND t.price <= f.price + 0.000001)
                      OR (f.direction = 'sell' AND t.price >= f.price - 0.000001)
                     )
               ), 0) AS crossing_volume_2s
        FROM maker_fills f
        LEFT JOIN vp.pm_trades t
          ON t.condition_id = f.condition_id
         AND t.token_id = f.token_id
         AND t.ts BETWEEN f.ts - interval '30 seconds'
                      AND f.ts + interval '30 seconds'
        GROUP BY f.id, f.ts, f.price, f.size_shares, f.instance,
                 f.condition_id, f.token_id, f.direction, f.source
    )
    SELECT instance, source, count(*) AS fills,
           count(*) FILTER (WHERE prints_100ms > 0) AS any_print_100ms,
           count(*) FILTER (WHERE prints_500ms > 0) AS any_print_500ms,
           count(*) FILTER (WHERE crosses_100ms > 0) AS price_cross_100ms,
           count(*) FILTER (WHERE crosses_500ms > 0) AS price_cross_500ms,
           count(*) FILTER (WHERE crosses_2s > 0) AS price_cross_2s,
           count(*) FILTER (WHERE crosses_10s > 0) AS price_cross_10s,
           count(*) FILTER (WHERE crosses_30s > 0) AS price_cross_30s,
           count(*) FILTER (WHERE classified_2s > 0) AS fills_with_classified_print,
           count(*) FILTER (WHERE side_confirms_2s > 0) AS side_confirms_2s,
           round(avg(crossing_volume_2s)::numeric, 2) AS avg_crossing_volume_2s,
           round(sum(size_shares)::numeric, 2) AS paper_filled_shares
    FROM evidence
    GROUP BY instance, source
    ORDER BY instance, source
    """
    rows = cockpit.q(sql, (*session_args, f"{hours:g} hours", instances))
    print("PAPER MAKER FILL / PUBLIC PRINT AUDIT")
    print(f"Lookback: {hours:g}h · bots: {', '.join(instances)}")
    if current_sessions:
        print("Scope: fills since each bot's current PAPER process session began.")
    print("A crossing print is a same-token public trade at/beyond the paper fill price")
    print("in the order's direction, within the listed time tolerance.")
    print("The ±10s/±30s windows are timestamp context, not proof of a fill at that time.")
    if not rows:
        print("No matching PAPER maker fills.")
        return
    for row in rows:
        d = dict(row)
        n = int(d["fills"])
        rate = lambda key: 100 * int(d[key]) / n if n else 0.0
        classified = int(d["fills_with_classified_print"])
        side_rate = (
            100 * int(d["side_confirms_2s"]) / classified
            if classified else None
        )
        print(
            f"{d['instance']} · {d['source']}: fills={n}, "
            f"any print ±100ms={rate('any_print_100ms'):.1f}%, "
            f"±500ms={rate('any_print_500ms'):.1f}%, "
            f"crossing print ±100ms={rate('price_cross_100ms'):.1f}%, "
            f"±500ms={rate('price_cross_500ms'):.1f}%, "
            f"±2/10/30s={rate('price_cross_2s'):.1f}/"
            f"{rate('price_cross_10s'):.1f}/{rate('price_cross_30s'):.1f}%, "
            f"aggressor coverage ±2s={rate('fills_with_classified_print'):.1f}%, "
            f"right aggressor among classified="
            f"{f'{side_rate:.1f}%' if side_rate is not None else 'n/a'} "
            f"(n={classified}), "
            f"mean crossing volume ±2s={float(d['avg_crossing_volume_2s']):.2f}, "
            f"shares={float(d['paper_filled_shares']):.2f}"
        )
    print(
        "\nLimits: no trade print near a fill does not prove it was impossible; "
        "the print feed can lag or be incomplete. A matching print does not prove "
        "queue priority or a fill. Aggressor classification is inferred from the "
        "local BBO; it can be unknown when a print lands inside the spread."
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hours", type=float, default=6.0)
    parser.add_argument(
        "--current-sessions", action="store_true",
        help="exclude fills from before each bot's current PAPER process session",
    )
    parser.add_argument("--instance", action="append", dest="instances",
                        help="restrict to an instance; may be repeated")
    args = parser.parse_args()
    if not 0.1 <= args.hours <= 24 * 30:
        parser.error("--hours must be between 0.1 and 720")
    try:
        summarize(args.hours, args.instances,
                  current_sessions=args.current_sessions)
    except Exception as exc:
        print(f"report unavailable: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
