"""Root-cockpit feed freshness is based on stored tick timestamps only."""
from pathlib import Path
import sys
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class RootFeedPulseTests(unittest.TestCase):
    def test_returns_fresh_stale_unknown_and_clock_ahead_sources(self):
        now_ms = 2_000_000_000_000
        rows = [
            {"source": "Oracle", "asset_key": "BTC", "samples_1h": 240,
             "last_ts": now_ms - 1_200, "last_price": 67_123.45,
             "oracle_source": "chainlink"},
            {"source": "Coinbase", "asset_key": "BTC", "samples_1h": 90,
             "last_ts": now_ms - 12_000, "last_price": 67_120.00,
             "oracle_source": None},
            {"source": "Binance", "asset_key": "BTCUSDT", "samples_1h": 1_500,
             "last_ts": now_ms + 7_000, "last_price": 67_125.10,
             "oracle_source": None},
        ]
        with patch.object(cockpit, "_root_feed_pulse_source_cache", (None, False, ())), \
                patch.object(cockpit.time, "monotonic", return_value=100.0), \
                patch.object(cockpit.time, "time", return_value=now_ms / 1000), \
                patch.object(cockpit, "q", return_value=rows) as query, \
                patch.object(cockpit, "_root_persistence_queue_snapshot",
                             return_value={"rows": [], "sampled_ms": now_ms,
                                           "available": False}):
            result = cockpit.api_root_feed_pulse()

        self.assertEqual(result["sampled_ms"], now_ms)
        self.assertEqual(result["fresh_threshold_s"], 10)
        self.assertEqual(result["sample_window_s"], 3600)
        by_source = {source["source"]: {item["asset"]: item for item in source["items"]}
                     for source in result["sources"]}
        self.assertEqual(by_source["Oracle"]["BTC"]["state"], "fresh")
        self.assertEqual(by_source["Oracle"]["BTC"]["age_s"], 1.2)
        self.assertEqual(by_source["Oracle"]["BTC"]["oracle_source"], "chainlink")
        self.assertEqual(by_source["Coinbase"]["BTC"]["state"], "stale")
        self.assertEqual(by_source["Coinbase"]["BTC"]["age_s"], 12.0)
        self.assertEqual(by_source["Binance"]["BTCUSDT"]["state"], "clock_ahead")
        self.assertEqual(by_source["Oracle"]["XRP"]["state"], "unknown")
        self.assertEqual(by_source["Oracle"]["XRP"]["samples_1h"], 0)
        self.assertEqual(result["persistence_queue"], [])
        self.assertFalse(result["persistence_queue_available"])
        self.assertTrue(result["available"])
        self.assertEqual(query.call_count, 1)
        self.assertIn("vp.oracle_ticks", query.call_args.args[0])
        self.assertIn("vp.coinbase_ticks", query.call_args.args[0])
        self.assertIn("vp.binance_ticks", query.call_args.args[0])

    def test_source_query_failure_keeps_queue_telemetry_and_marks_feed_unknown(self):
        now_ms = 2_000_000_000_000
        queue_rows = [{"instance": "cb", "state": "current",
                       "runtime_updated_ms": now_ms,
                       "queue": {"pending": 4, "capacity": 256,
                                 "rejected_by_source": {}}}]
        with patch.object(cockpit, "_root_feed_pulse_source_cache", (None, False, ())), \
                patch.object(cockpit.time, "monotonic", return_value=100.0), \
                patch.object(cockpit.time, "time", return_value=now_ms / 1000), \
                patch.object(cockpit, "q", side_effect=RuntimeError("database read failed")), \
                patch.object(cockpit, "_root_persistence_queue_snapshot",
                             return_value={"rows": queue_rows, "sampled_ms": now_ms,
                                           "available": True}):
            result = cockpit.api_root_feed_pulse({"cb"})

        self.assertTrue(result["available"])
        self.assertFalse(result["feed_available"])
        self.assertEqual([source["source"] for source in result["sources"]],
                         ["Oracle", "Coinbase", "Binance"])
        self.assertTrue(all(
            item["state"] == "unknown" and item["last_ts"] is None
            for source in result["sources"] for item in source["items"]))
        self.assertEqual(result["persistence_queue"], queue_rows)
        self.assertTrue(result["persistence_queue_available"])

    def test_repeated_source_pollers_share_a_one_second_aggregate_cache(self):
        now_ms = 2_000_000_000_000
        with patch.object(cockpit, "_root_feed_pulse_source_cache", (None, False, ())), \
                patch.object(cockpit.time, "monotonic",
                             side_effect=[100.0, 100.1, 100.5, 101.2, 101.3]), \
                patch.object(cockpit.time, "time", return_value=now_ms / 1000), \
                patch.object(cockpit, "q", return_value=[]) as query, \
                patch.object(cockpit, "_root_persistence_queue_snapshot",
                             return_value={"rows": [], "sampled_ms": now_ms,
                                           "available": True}):
            first = cockpit.api_root_feed_pulse()
            second = cockpit.api_root_feed_pulse()
            third = cockpit.api_root_feed_pulse()

        self.assertTrue(first["feed_available"])
        self.assertTrue(second["feed_available"])
        self.assertTrue(third["feed_available"])
        self.assertEqual(query.call_count, 2)

    def test_simultaneous_source_pollers_run_only_one_aggregate(self):
        now_ms = 2_000_000_000_000
        barrier = threading.Barrier(2)

        def query(_sql):
            time.sleep(0.05)
            return []

        def poll():
            barrier.wait(timeout=2)
            return cockpit.api_root_feed_pulse()

        with patch.object(cockpit, "_root_feed_pulse_source_cache", (None, False, ())), \
                patch.object(cockpit.time, "time", return_value=now_ms / 1000), \
                patch.object(cockpit, "q", side_effect=query) as source_query, \
                patch.object(cockpit, "_root_persistence_queue_snapshot",
                             return_value={"rows": [], "sampled_ms": now_ms,
                                           "available": True}):
            with ThreadPoolExecutor(max_workers=2) as pool:
                results = list(pool.map(lambda _index: poll(), range(2)))

        self.assertTrue(all(result["feed_available"] for result in results))
        self.assertEqual(source_query.call_count, 1)

    def test_root_route_exposes_only_the_feed_pulse_payload(self):
        expected = {"available": True, "sources": [], "sampled_ms": 123}
        with patch.object(cockpit, "api_root_feed_pulse", return_value=expected):
            result = cockpit.ROUTES["/api/feed_pulse"]({})
        self.assertEqual(result, expected)

    def test_root_persistence_queue_is_sanitized_and_instance_scoped(self):
        fleet = [
            {"instance": "cb", "status": "running", "mode_verified": True,
             "runtime_updated_ms": 1_999_999_999_900,
             "persistence_queue": {"pending": 3, "capacity": 256,
                                   "rejected_by_source": {"oracle_tick": 2}},
             "pid": 12345, "rss_mb": 99.0, "config_source_sha256": "private"},
            {"instance": "other", "status": "running", "mode_verified": False,
             "runtime_updated_ms": None, "persistence_queue": None, "pid": 98765},
            {"instance": "stopped", "status": "stopped", "mode_verified": False,
             "runtime_updated_ms": None, "persistence_queue": None, "pid": None},
            {"instance": "outside_scope", "status": "running", "mode_verified": True,
             "runtime_updated_ms": 1_999_999_999_900,
             "persistence_queue": {"pending": 0, "capacity": 10,
                                   "rejected_by_source": {}}, "pid": 111},
        ]
        with patch.object(cockpit, "_fleet_runtime_snapshot", return_value={"fleet": fleet}), \
                patch.object(cockpit, "_root_persist_queue_cache", {}), \
                patch.object(cockpit.time, "time", return_value=2_000_000_000):
            result = cockpit._root_persistence_queue_snapshot({"cb", "other", "stopped"})

        by_instance = {row["instance"]: row for row in result["rows"]}
        self.assertEqual(set(by_instance), {"cb", "other", "stopped"})
        self.assertEqual(by_instance["cb"]["state"], "current")
        self.assertEqual(by_instance["cb"]["queue"], {
            "pending": 3, "capacity": 256, "rejected_by_source": {"oracle_tick": 2}})
        self.assertEqual(by_instance["other"]["state"], "unknown")
        self.assertIsNone(by_instance["other"]["queue"])
        self.assertEqual(by_instance["stopped"]["state"], "stopped")
        self.assertTrue(result["available"])
        self.assertNotIn("outside_scope", by_instance)
        self.assertFalse({"pid", "rss_mb", "config_source_sha256"} & set(by_instance["cb"]))

    def test_runtime_snapshot_failure_marks_queue_unavailable_without_raising(self):
        with patch.object(cockpit, "_fleet_runtime_snapshot",
                          side_effect=RuntimeError("runtime source unavailable")), \
                patch.object(cockpit, "_root_persist_queue_cache", {}), \
                patch.object(cockpit.time, "time", return_value=2_000_000_000):
            result = cockpit._root_persistence_queue_snapshot({"cb"})

        self.assertEqual(result["rows"], [])
        self.assertFalse(result["available"])
        self.assertEqual(result["sampled_ms"], 2_000_000_000_000)

    def test_feed_pulse_scope_is_admin_owned_instances_or_empty_for_demo(self):
        handler = object.__new__(cockpit.Handler)
        with patch.object(cockpit, "api_root_feed_pulse", return_value={"ok": True}) as api, \
                patch.object(cockpit.Handler, "_is_admin", return_value=False), \
                patch.object(cockpit.Handler, "_is_demo", return_value=False), \
                patch.object(cockpit.Handler, "_user_instances", return_value={"cb"}):
            self.assertEqual(handler._api_root_feed_pulse_for_user(), {"ok": True})
            api.assert_called_once_with({"cb"})

        with patch.object(cockpit, "api_root_feed_pulse", return_value={"ok": True}) as api, \
                patch.object(cockpit.Handler, "_is_admin", return_value=False), \
                patch.object(cockpit.Handler, "_is_demo", return_value=True):
            self.assertEqual(handler._api_root_feed_pulse_for_user(), {"ok": True})
            api.assert_called_once_with(set())

        with patch.object(cockpit, "api_root_feed_pulse", return_value={"ok": True}) as api, \
                patch.object(cockpit.Handler, "_is_admin", return_value=True):
            self.assertEqual(handler._api_root_feed_pulse_for_user(), {"ok": True})
            api.assert_called_once_with(None)

    def test_live_lite_queue_overlay_uses_user_scope_and_preserves_shared_cache(self):
        handler = object.__new__(cockpit.Handler)
        base = {"radar": [], "server_ts": 123}
        queue_rows = [{"instance": "cb", "state": "current", "queue": {"pending": 0}}]
        handler._user = {"role": "user", "instance": "cb", "instances": ["cb", "other"]}
        with patch.object(cockpit, "api_live_lite", return_value=base) as live, \
                patch.object(cockpit, "_root_persistence_queue_snapshot",
                             return_value={"rows": queue_rows, "sampled_ms": 456,
                                           "available": True}) as queue:
            result = handler._api_live_lite_for_user("cb")

        live.assert_called_once_with("cb")
        queue.assert_called_once_with({"cb"})
        self.assertEqual(result["persistence_queue"], queue_rows)
        self.assertTrue(result["persistence_queue_available"])
        self.assertEqual(result["persistence_queue_sampled_ms"], 456)
        self.assertNotIn("persistence_queue", base)

        with patch.object(cockpit, "api_live_lite", return_value=base), \
                patch.object(cockpit, "_root_persistence_queue_snapshot",
                             return_value={"rows": [], "sampled_ms": 789,
                                           "available": True}) as queue:
            handler._user = {"role": "demo", "instance": "cb"}
            demo = handler._api_live_lite_for_user("cb")
        queue.assert_called_once_with(set())
        self.assertEqual(demo["persistence_queue"], [])

        with patch.object(cockpit, "api_live_lite", return_value=base), \
                patch.object(cockpit, "_root_persistence_queue_snapshot",
                             return_value={"rows": queue_rows, "sampled_ms": 999,
                                           "available": True}) as queue:
            handler._user = {"role": "admin"}
            handler._api_live_lite_for_user(None)
        queue.assert_called_once_with(None)

    def test_root_overview_contains_read_only_queue_card(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("LOCAL DATABASE WRITE QUEUE", page)
        self.assertIn("rejected writes are cumulative since each process started", page)
        self.assertIn("function renderPersistenceQueue(payload)", page)
        self.assertIn("SOURCE DATA UNAVAILABLE", page)
        self.assertIn("source query unavailable", page)
        self.assertIn("persistence_queue_available:false", page)
        self.assertIn("Bot queue telemetry is temporarily unavailable. Queue health is unknown.", page)
        self.assertNotIn("The live feed remains available; queue health is unknown.", page)
        self.assertIn("function tickPersistenceQueue()", page)
        self.assertIn("const _persistQueueTrendByInstance=new Map()", page)
        self.assertIn("function observePersistenceQueue(rows,sampledMs)", page)
        self.assertIn("function renderPersistenceQueueTrend(instance)", page)
        self.assertIn("const receivedAt=performance.now()", page)
        self.assertIn("const heartbeatAge=heartbeat==null?null:sampledMs-heartbeat", page)
        self.assertIn("point.at-history[index-1].at>15000", page)
        self.assertIn("up to five minutes in this browser tab", page)
        self.assertIn("80% high / 95% critical", page)
        self.assertIn(".pq-trend-head span,.pq-trend-head small,.pq-trend .pq-trend-foot{font-size:9px", page)
        self.assertIn("payload?.persistence_queue_sampled_ms??payload?.sampled_ms", page)
        self.assertIn("if(_feedPulsePayload)renderPersistenceQueue(_feedPulsePayload)", page)
        self.assertIn("id=\"persistQueueDetails\"", page)
        self.assertIn("_persistQueueDetails.open=false", page)
        self.assertIn("if(inst){const details=$('persistQueueDetails');if(details)details.open=true;}", page)
        self.assertIn("age>10", page)
        self.assertIn("function persistenceQueueRadarAlerts(rows,sampledMs,now=Date.now(),available=true)", page)
        self.assertIn("WRITE QUEUE DROPPED DATA", page)
        self.assertIn("persistenceQueueRadarAlerts(d.persistence_queue,d.persistence_queue_sampled_ms,now,d.persistence_queue_available)", page)
        self.assertIn("WRITE QUEUE TELEMETRY UNAVAILABLE", page)
        self.assertIn("openPersistenceQueueAlert(${jsStr(a.instance)})", page)
        self.assertIn("function openPersistenceQueueAlert(instance)", page)
        queue_ui = page[page.index("function renderPersistenceQueue(payload)"):
                         page.index("async function loadFeedPulse(force=false)")]
        self.assertNotIn("apiPost(", queue_ui)
        self.assertNotIn("apiFetch(", queue_ui)


if __name__ == "__main__":
    unittest.main()
