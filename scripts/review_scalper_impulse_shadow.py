#!/usr/bin/env python3
"""Summarize PAPER-only lead-scalper impulse shadow observations.

This report measures quote-path touches and markouts. It cannot infer maker
queue fills or realized strategy P&L.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import json

import cockpit


EVENT_TYPES = [
    "scalper_impulse_sample",
    "scalper_impulse_candidate",
    "scalper_impulse_markout",
    "scalper_impulse_outcome",
]


def recent_event_id_floor(hours: float) -> int:
    """Find a conservative ID boundary for recent events using the PK index.

    ``window_events`` is append-only and has no timestamp index. Filtering the
    1M+ row table by ``ts`` alone forces a sequential scan, which can exceed the
    cockpit's statement timeout. IDs and insertion timestamps are monotonic in
    normal operation; the lookback cushion protects rows whose transactions
    committed slightly out of order. The final event query still checks ``ts``.
    """
    cutoff = cockpit.q(
        "SELECT now() - %s::interval AS cutoff", (f"{hours} hours",), one=True
    )["cutoff"]
    latest = cockpit.q(
        "SELECT id, ts FROM vp.window_events ORDER BY id DESC LIMIT 1", one=True
    )
    if not latest or latest["ts"] < cutoff:
        return 0

    low, high = 1, int(latest["id"])
    while low < high:
        middle = (low + high) // 2
        row = cockpit.q(
            "SELECT id, ts FROM vp.window_events WHERE id >= %s "
            "ORDER BY id LIMIT 1",
            (middle,), one=True,
        )
        if not row:
            break
        row_id = int(row["id"])
        if row["ts"] < cutoff:
            low = row_id + 1
        else:
            high = row_id

    return max(0, low - 10_000)


def payload_dict(value: object) -> dict[str, object]:
    if isinstance(value, dict):
        return value
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
            return parsed if isinstance(parsed, dict) else {}
        except json.JSONDecodeError:
            return {}
    return {}


def event_ms(row: dict[str, object], payload: dict[str, object]) -> int:
    try:
        return int(payload.get("signal_ts_ms") or payload.get("sample_ts_ms") or
                   payload.get("created_ms") or
                   int(row["ts"].timestamp() * 1000))
    except (KeyError, TypeError, ValueError, OverflowError):
        return 0


def summarize_group(rows: list[dict[str, object]]) -> dict[str, object]:
    candidates = [x for x in rows if x["eligible"]]
    outcomes = [x for x in candidates if x["outcome"]]
    classifiable = [x for x in outcomes if is_classifiable(x)]
    censored = len(outcomes) - len(classifiable)
    wins = sum(x["outcome"].get("first_barrier") == "TARGET" for x in classifiable)
    stops = sum(x["outcome"].get("first_barrier") == "STOP" for x in classifiable)
    time_eligible = [
        x for x in candidates
        if x["time_eligibility_known"] and x["entry_time_eligible"] is True
    ]
    time_classifiable = [x for x in time_eligible if is_classifiable(x)]
    time_wins = sum(
        x["outcome"].get("first_barrier") == "TARGET" for x in time_classifiable
    )
    time_stops = sum(
        x["outcome"].get("first_barrier") == "STOP" for x in time_classifiable
    )
    markout_counts = {}
    late_markout_counts = {}
    observed_markout_counts = {}
    markouts = {}
    for horizon in (250, 500, 1000, 2000):
        values = []
        late = 0
        observed = 0
        for x in candidates:
            mark = x["markouts"].get(horizon)
            if not mark or mark.get("best_bid") is None:
                continue
            observed += 1
            try:
                observed_after_ms = float(mark.get("observed_after_ms"))
                markout = float(mark.get("markout_pct_vs_quote"))
            except (TypeError, ValueError):
                late += 1
                continue
            # BBO updates are event-driven, so the nearest quote can arrive
            # after a milestone. Keep the original event for audit, but do not
            # present a multi-second-late quote as a subsecond markout.
            tolerance_ms = max(100.0, horizon * 0.10)
            if observed_after_ms > horizon + tolerance_ms:
                late += 1
                continue
            values.append(markout)
        markouts[horizon] = (
            round(sum(values) / len(values), 4) if values else None
        )
        markout_counts[horizon] = len(values)
        late_markout_counts[horizon] = late
        observed_markout_counts[horizon] = observed
    return {
        "candidate_count": len(rows),
        "eligible_candidates": len(candidates),
        "time_checked_candidates": sum(
            x["time_eligibility_known"] for x in rows
        ),
        "time_eligible_candidates": sum(
            x["eligible"] and x["entry_time_eligible"] is True for x in rows
        ),
        "legacy_quote_eligible": sum(
            x["eligible"] and not x["time_eligibility_known"] for x in rows
        ),
        "classifiable": len(classifiable),
        "censored": censored,
        "target_first": wins,
        "stop_first": stops,
        "no_barrier": len(classifiable) - wins - stops,
        "target_first_rate": round(wins / len(classifiable), 4) if classifiable else None,
        "time_eligible_classifiable": len(time_classifiable),
        "time_eligible_target_first": time_wins,
        "time_eligible_stop_first": time_stops,
        "time_eligible_target_first_rate": (
            round(time_wins / len(time_classifiable), 4)
            if time_classifiable else None
        ),
        "avg_bid_markout_pct": markouts,
        "markout_counts": markout_counts,
        "late_markout_counts": late_markout_counts,
        "observed_markout_counts": observed_markout_counts,
    }


def is_classifiable(candidate: dict[str, object]) -> bool:
    outcome = candidate.get("outcome")
    if not isinstance(outcome, dict):
        return False
    if outcome.get("first_barrier") in {"TARGET", "STOP"}:
        return True
    return outcome.get("completion_reason") == "five_second_horizon"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hours", type=float, default=24.0,
                        help="lookback in hours (default: 24)")
    parser.add_argument("--instance", default="",
                        help="show observations for one bot instance")
    parser.add_argument("--session", default="",
                        help="show observations for one bot session UUID")
    parser.add_argument("--latest-session", action="store_true",
                        help="with --instance, select that bot's most recently active session")
    parser.add_argument("--compact", action="store_true",
                        help="aggregate sessions into one row per bot, asset, and horizon")
    args = parser.parse_args()
    if not (0.1 <= args.hours <= 24 * 90):
        parser.error("--hours must be between 0.1 and 2160")
    if args.latest_session and args.session:
        parser.error("--latest-session and --session cannot be used together")
    if args.latest_session and not args.instance:
        parser.error("--latest-session requires --instance")

    id_floor = recent_event_id_floor(args.hours)

    if args.latest_session:
        latest = cockpit.q(
            """SELECT payload->>'session_id' AS session_id
               FROM vp.window_events
               WHERE id >= %s AND ts >= now() - %s::interval
                 AND event_type = ANY(%s)
                 AND payload->>'instance' = %s
                 AND NULLIF(payload->>'session_id', '') IS NOT NULL
               ORDER BY ts DESC, id DESC
               LIMIT 1""",
            (id_floor, f"{args.hours} hours", EVENT_TYPES, args.instance),
            one=True,
        )
        args.session = str(latest["session_id"]) if latest else ""

    event_sql = """SELECT condition_id, ts, event_type, payload
                   FROM vp.window_events
                   WHERE id >= %s AND ts >= now() - %s::interval
                     AND event_type = ANY(%s)"""
    event_args: list[object] = [id_floor, f"{args.hours} hours", EVENT_TYPES]
    if args.instance:
        event_sql += " AND payload->>'instance' = %s"
        event_args.append(args.instance)
    if args.session:
        event_sql += " AND payload->>'session_id' = %s"
        event_args.append(args.session)
    event_sql += " ORDER BY ts, id"
    records = cockpit.q(event_sql, tuple(event_args))
    sample_counts: dict[tuple[str, str, str, int], int] = defaultdict(int)
    candidates: dict[str, dict[str, object]] = {}
    outcomes: dict[str, dict[str, object]] = {}
    markouts: dict[str, dict[int, dict[str, object]]] = defaultdict(dict)

    for raw in records:
        row = dict(raw)
        payload = payload_dict(row.get("payload"))
        if args.instance and payload.get("instance") != args.instance:
            continue
        if args.session and payload.get("session_id") != args.session:
            continue
        event_type = str(row.get("event_type") or "")
        if event_type == "scalper_impulse_sample":
            key = (str(payload.get("instance") or "legacy"),
                   str(payload.get("session_id") or "legacy"),
                   str(payload.get("asset") or "?"),
                   int(payload.get("horizon_s") or 0))
            sample_counts[key] += 1
        elif event_type == "scalper_impulse_candidate":
            shadow_id = str(payload.get("shadow_id") or "")
            if shadow_id:
                candidates[shadow_id] = {
                    "asset": str(payload.get("asset") or "?"),
                    "instance": str(payload.get("instance") or "legacy"),
                    "session_id": str(payload.get("session_id") or "legacy"),
                    "condition_id": str(row.get("condition_id") or ""),
                    "horizon": int(payload.get("horizon_s") or 0),
                    "eligible": bool(payload.get("eligible_by_config")),
                    "time_eligibility_known": "eligible_by_time" in payload,
                    "entry_time_eligible": (
                        bool(payload.get("eligible_by_time"))
                        if "eligible_by_time" in payload else None
                    ),
                    "ts_ms": event_ms(row, payload),
                    "markouts": {},
                    "outcome": None,
                }
        elif event_type == "scalper_impulse_markout":
            shadow_id = str(payload.get("shadow_id") or "")
            try:
                milestone = int(payload.get("milestone_ms") or 0)
            except (TypeError, ValueError):
                milestone = 0
            if shadow_id and milestone:
                markouts[shadow_id][milestone] = payload
        elif event_type == "scalper_impulse_outcome":
            shadow_id = str(payload.get("shadow_id") or "")
            if shadow_id:
                outcomes[shadow_id] = payload

    for shadow_id, candidate in candidates.items():
        candidate["markouts"] = markouts.get(shadow_id, {})
        candidate["outcome"] = outcomes.get(shadow_id)

    grouped: dict[tuple[str, str, str, int], list[dict[str, object]]] = defaultdict(list)
    for candidate in candidates.values():
        key = (str(candidate["instance"]), str(candidate["session_id"]),
               str(candidate["asset"]), int(candidate["horizon"]))
        grouped[key].append(candidate)

    print("SCALPER IMPULSE SHADOW · quote-path research only")
    print(f"Lookback: {args.hours:g} hours · sampled feature rows: {sum(sample_counts.values())}")
    if args.instance:
        print(f"Instance filter: {args.instance}")
    if args.latest_session:
        if args.session:
            print(f"Latest session: {args.session}")
        else:
            print("Latest-session filter: no session found in this lookback")
    elif args.session:
        print(f"Session filter: {args.session}")
    print("Eligibility is payload-versioned: older rows test target/price/spread only; newer rows also record the entry-time cutoff. Capacity, model, oracle, book, and fill gates remain separate.")
    print("Maker fills are UNKNOWN; bid touches are not fills or realized P&L.")
    print("Markouts exclude quotes arriving > max(100ms, 10%) after their milestone; stale barrier paths are censored.")
    if not candidates:
        print("No impulse candidates recorded in this interval.")
        return

    if args.compact:
        cohorts: dict[tuple[str, str, int], list[dict[str, object]]] = defaultdict(list)
        for candidate in candidates.values():
            key = (str(candidate["instance"]), str(candidate["asset"]),
                   int(candidate["horizon"]))
            cohorts[key].append(candidate)
        print("\nBOT / ASSET / WINDOW  SAMPLES  CANDIDATES  QUOTE-ELIGIBLE  TIME-CHECKED  "
              "QUOTE+TIME  LEGACY-QUOTE  CLASSIF  CENSORED  TARGET  STOP  "
              "QUOTE RATE  TIME CLASSIF  TIME TARGET/STOP  TIME QUOTE RATE  "
              "TIMELY BID MARKOUT % (250/500/1000/2000ms; n)")
        for key in sorted(cohorts):
            rows = cohorts[key]
            summary = summarize_group(rows)
            sample_count = sum(
                count for (instance, _session, asset, horizon), count in sample_counts.items()
                if (instance, asset, horizon) == key
            )
            markouts = summary["avg_bid_markout_pct"]
            markout_counts = summary["markout_counts"]
            formatted = "/".join(
                ("—" if markouts[h] is None else f"{markouts[h]:+.2f}")
                + f"({markout_counts[h]})"
                for h in (250, 500, 1000, 2000)
            )
            rate = ("—" if summary["target_first_rate"] is None
                    else f"{100 * summary['target_first_rate']:.1f}%")
            time_rate = ("—" if summary["time_eligible_target_first_rate"] is None
                         else f"{100 * summary['time_eligible_target_first_rate']:.1f}%")
            print(f"{key[0]} / {key[1]} / {key[2] // 60:2d}m  "
                  f"{sample_count:>7}  {summary['candidate_count']:>10}  "
                  f"{summary['eligible_candidates']:>14}  "
                  f"{summary['time_checked_candidates']:>12}  "
                  f"{summary['time_eligible_candidates']:>10}  "
                  f"{summary['legacy_quote_eligible']:>12}  "
                  f"{summary['classifiable']:>7}  {summary['censored']:>8}  "
                  f"{summary['target_first']:>6}  {summary['stop_first']:>4}  "
                  f"{rate:>10s}  {summary['time_eligible_classifiable']:>11}  "
                  f"{summary['time_eligible_target_first']:>4}/"
                  f"{summary['time_eligible_stop_first']:<4}  "
                  f"{time_rate:>15s}     {formatted}")
        print("\nQuote-path evidence only: it cannot establish maker fills, realized P&L, or profitability.")
        return

    print("\nINSTANCE / SESSION / ASSET / WINDOW  SAMPLES  CANDIDATES  QUOTE-ELIGIBLE  TIME-CHECKED  QUOTE+TIME  LEGACY-QUOTE  CLASSIF  CENSORED  TARGET  STOP  QUOTE RATE  TIME CLASSIF  TIME TARGET/STOP  TIME QUOTE RATE  AVG BID MARKOUT % (250/500/1000/2000ms; timely/observed)")
    for key in sorted(grouped):
        rows = grouped[key]
        s = summarize_group(rows)
        mo = s["avg_bid_markout_pct"]
        formatted = "/".join(
            ("—" if mo[h] is None else f"{mo[h]:+.3f}") +
            f"({s['markout_counts'][h]}/{s['observed_markout_counts'][h]})"
            for h in (250, 500, 1000, 2000))
        rate = "—" if s["target_first_rate"] is None else f"{100*s['target_first_rate']:.1f}%"
        time_rate = ("—" if s["time_eligible_target_first_rate"] is None
                     else f"{100*s['time_eligible_target_first_rate']:.1f}%")
        short_session = key[1][:8] if key[1] != "legacy" else "legacy"
        sampled = sample_counts.get(key, 0)
        print(f"{key[0]} / {short_session} / {key[2]} / {key[3]//60:2d}m  "
              f"{sampled:>7}  {s['candidate_count']:>10}  "
              f"{s['eligible_candidates']:>14}  {s['time_checked_candidates']:>12}  "
              f"{s['time_eligible_candidates']:>10}  {s['legacy_quote_eligible']:>12}  "
              f"{s['classifiable']:>7}  {s['censored']:>8}  "
              f"{s['target_first']:>6}  {s['stop_first']:>4}  "
              f"{rate:>10s}  {s['time_eligible_classifiable']:>11}  "
              f"{s['time_eligible_target_first']:>4}/"
              f"{s['time_eligible_stop_first']:<4}  {time_rate:>15s}     {formatted}")

    print("\nCHRONOLOGICAL CHECK (distinct market windows stay in one split)")
    for key in sorted(grouped):
        rows = grouped[key]
        by_window: dict[str, list[dict[str, object]]] = defaultdict(list)
        for candidate in rows:
            by_window[str(candidate["condition_id"])].append(candidate)
        windows = sorted(by_window, key=lambda cid: min(
            int(x["ts_ms"]) for x in by_window[cid]))
        split = max(1, int(len(windows) * 0.70))
        chunks = (("first 70%", windows[:split]), ("latest 30%", windows[split:]))
        for label, window_ids in chunks:
            chunk = [x for cid in window_ids for x in by_window[cid]]
            eligible = [x for x in chunk if x["eligible"] and is_classifiable(x)]
            censored = sum(1 for x in chunk if x["eligible"] and x["outcome"] and
                           not is_classifiable(x))
            wins = sum(x["outcome"].get("first_barrier") == "TARGET" for x in eligible)
            stops = sum(x["outcome"].get("first_barrier") == "STOP" for x in eligible)
            rate = f"{100*wins/len(eligible):.1f}%" if eligible else "—"
            session = key[1][:8] if key[1] != "legacy" else "legacy"
            print(f"{key[0]} / {session} / {key[2]} {key[3]//60}m / {label}: "
                  f"windows={len(window_ids):4d}, classifiable={len(eligible):4d}, censored={censored:4d}, "
                  f"target-first={wins}, stop-first={stops}, rate={rate}")
    print("\nA later holdout is descriptive only. Repeated candidates within a market"
          " window stay together; maker fills and realized P&L remain unknown.")


if __name__ == "__main__":
    main()
