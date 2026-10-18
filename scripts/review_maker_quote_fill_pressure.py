#!/usr/bin/env python3
"""Measure recorded public trade pressure against PAPER maker buy quotes.

This checks whether a quote had a recorded trade at its limit with sell
aggressor or a trade through its limit while resting. It does not infer queue
priority, fills, profitability, or live execution.
"""

from __future__ import annotations

import argparse
import subprocess
from collections import defaultdict

import cockpit
from cockpit_review import timestamp_ms


DEFAULT_INSTANCES = (
    "btc-5m-lead-scalper",
    "btc-5m-lead-scalper-tp30-isolated-test",
    "btc-5m-lead-scalper-tp50-isolated-test",
    "btc-15m-lead-scalper",
    "btc-15m-lead-scalper-retry10-isolated-test",
)
TICK_SIZE = 0.01


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


def review(hours: float, instances: list[str], current_sessions: bool = False) -> None:
    session_values = ""
    session_args: list[object] = []
    session_join = ""
    session_filter = ""
    if current_sessions:
        running = running_bot_instances()
        instances = sorted(running.intersection(instances))
        if not instances:
            print("No requested PAPER bot is currently running.")
            return
        starts = {instance: cockpit.accounting_since(instance, "paper")
                  for instance in instances}
        instances = [instance for instance in instances if starts[instance] is not None]
        if not instances:
            print("Could not determine current PAPER session start times.")
            return
        session_values = ", ".join(["(%s::text, %s::timestamptz)"] * len(instances))
        for instance in instances:
            session_args.extend((instance, starts[instance]))
        session_join = "JOIN bot_sessions bs ON bs.instance = o.instance"
        session_filter = "AND o.ts_created >= bs.started_at"

    cte = (f"WITH bot_sessions(instance, started_at) AS (VALUES {session_values})"
           if current_sessions else "")
    orders = [dict(row) for row in cockpit.q(
        f"""{cte}
           SELECT o.client_order_id::text AS order_id, o.instance,
                  o.condition_id, o.token_id, o.side_label::text AS side,
                  o.ts_created, o.ts_terminal, o.price, o.size_shares,
                  o.status::text AS status, o.execution_mode::text AS execution_mode,
                  m.asset, m.horizon_seconds
           FROM vp.orders o
           {session_join}
           LEFT JOIN vp.pm_markets m ON m.condition_id = o.condition_id
           WHERE o.mode = 'paper' AND o.instance = ANY(%s)
             AND o.source = 'CB_LEAD_SCALP_ENTRY' AND o.direction = 'buy'
             AND o.ts_created >= now() - %s::interval
             {session_filter}
           ORDER BY o.ts_created""",
        (*session_args, instances, f"{hours} hours"),
    )]
    if not orders:
        print("No PAPER maker-entry orders in the selected window.")
        return
    now_ms = timestamp_ms(cockpit.q("SELECT now() AS t", one=True)["t"])

    order_ids = [row["order_id"] for row in orders]
    condition_ids = sorted({row["condition_id"] for row in orders})
    events = [dict(row) for row in cockpit.q(
        """SELECT client_order_id::text AS order_id, event_ts,
                  status, price, size_shares
           FROM vp.order_events
           WHERE client_order_id::text = ANY(%s)
           ORDER BY event_ts""",
        (order_ids,),
    )]
    trade_time_filter = "ts >= %s" if current_sessions else "ts >= now() - %s::interval"
    trade_cutoff = (min(starts[i] for i in instances) if current_sessions
                    else f"{hours} hours")
    trades = [dict(row) for row in cockpit.q(
        """SELECT condition_id, token_id, side_label::text AS side,
                  price, size, aggressor::text AS aggressor, ts
           FROM vp.pm_trades
           WHERE condition_id = ANY(%s)
             AND """ + trade_time_filter + """
           ORDER BY condition_id, ts""",
        (condition_ids, trade_cutoff),
    )]

    events_by_order: dict[str, list[dict]] = defaultdict(list)
    for event in events:
        events_by_order[event["order_id"]].append(event)
    tape_by_token: dict[tuple[str, str, str], list[dict]] = defaultdict(list)
    for trade in trades:
        key = (trade["condition_id"], trade["token_id"], trade["side"])
        tape_by_token[key].append(trade)

    stats = defaultdict(lambda: {
        "orders": 0, "filled_orders": 0, "cancelled_orders": 0,
        "active_quotes": 0, "quotes_with_sell_at_limit": 0,
        "sell_prints_at_limit": 0, "quotes_with_trade_through": 0,
        "trade_through_prints": 0,
    })
    for order in orders:
        row = stats[order["instance"]]
        row["orders"] += 1
        row["filled_orders"] += order["status"] == "filled"
        row["cancelled_orders"] += order["status"] == "cancelled"
        segments = events_by_order[order["order_id"]]
        tape = tape_by_token[(order["condition_id"], order["token_id"], order["side"])]
        saw_sell_at_limit = False
        saw_trade_through = False
        for index, event in enumerate(segments):
            if event["status"] not in ("acked", "repriced"):
                continue
            start_ms = timestamp_ms(event["event_ts"])
            if index + 1 < len(segments):
                end_ms = timestamp_ms(segments[index + 1]["event_ts"])
            elif order.get("ts_terminal") is not None:
                end_ms = timestamp_ms(order["ts_terminal"])
            else:
                end_ms = now_ms
            limit = float(event["price"] or order["price"])
            for trade in tape:
                trade_ms = timestamp_ms(trade["ts"])
                if not start_ms < trade_ms <= end_ms:
                    continue
                price = float(trade["price"])
                tolerance = TICK_SIZE * 0.25
                if price < limit - tolerance:
                    saw_trade_through = True
                    row["trade_through_prints"] += 1
                elif abs(price - limit) <= tolerance and (trade.get("aggressor") or "").lower() == "sell":
                    saw_sell_at_limit = True
                    row["sell_prints_at_limit"] += 1
        if order["status"] == "acked":
            row["active_quotes"] += 1
        if saw_sell_at_limit:
            row["quotes_with_sell_at_limit"] += 1
        if saw_trade_through:
            row["quotes_with_trade_through"] += 1

    print("PAPER MAKER QUOTE / PUBLIC TRADE PRESSURE")
    print(f"Window: {hours:g}h · instances: {', '.join(instances)}")
    if current_sessions:
        print("Scope: orders since each bot's current PAPER process session began.")
    print("Outcome is quote-by-quote. A sell print at the limit can still lose queue; a through print is stronger pressure evidence.")
    for instance in instances:
        row = stats[instance]
        print(
            f"{instance}: quotes={row['orders']} fills={row['filled_orders']} "
            f"cancels={row['cancelled_orders']} active={row['active_quotes']} "
            f"quotes with sell-at-limit={row['quotes_with_sell_at_limit']} "
            f"({row['sell_prints_at_limit']} prints) "
            f"quotes with trade-through={row['quotes_with_trade_through']} "
            f"({row['trade_through_prints']} prints)"
        )
    print("PAPER tape lacks a verified live queue position and does not establish profitability.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hours", type=float, default=6.0)
    parser.add_argument("--instance", action="append", dest="instances")
    parser.add_argument(
        "--current-sessions", action="store_true",
        help="exclude orders from before each bot's current PAPER process session",
    )
    args = parser.parse_args()
    if not 0.1 <= args.hours <= 168:
        parser.error("--hours must be between 0.1 and 168")
    try:
        review(args.hours, args.instances or list(DEFAULT_INSTANCES),
               current_sessions=args.current_sessions)
    except Exception as exc:
        print(f"review unavailable: {type(exc).__name__}: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
