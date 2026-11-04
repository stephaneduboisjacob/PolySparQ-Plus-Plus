"""Root prediction-history checkpoint metrics stay resolved-only and explicit."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cockpit


class PredictionHistoryScorecardTests(unittest.TestCase):
    def test_api_returns_all_checkpoint_scores_and_paired_comparisons(self):
        score_row = {
            "checkpoint_key": "elapsed_60pct",
            "model_samples": 8, "model_correct": 5,
            "confidence_eligible": 8, "confidence_samples": 5, "confidence_correct": 4,
            "paired_directional_samples": 6,
            "paired_model_correct": 4, "paired_market_correct": 3,
            "paired_brier_samples": 7,
            "model_brier": 0.21, "market_brier": 0.24,
            "model_log_loss": 0.58, "market_log_loss": 0.61,
        }
        calibration_row = {
            "source": "MODEL", "probability_bin": 6, "samples": 4,
            "observed_up_count": 3,
            "mean_forecast_up": 0.64, "observed_up": 0.75,
        }
        with patch.object(cockpit, "_prediction_history_ensure_schema"), \
             patch.object(cockpit, "q", side_effect=[
                 {"total": 12}, [], [score_row],
                 [{"model_version": "model-a"}, {"model_version": "model-b"}],
                 [calibration_row], None
             ]) as query, \
             patch.object(cockpit.os.path, "isfile", return_value=False):
            result = cockpit.api_prediction_history("btc", 300, 50, 0, 5, "model-a")

        self.assertEqual(len(result["scorecards"]), 6)
        sixty = next(item for item in result["scorecards"] if item["key"] == "elapsed_60pct")
        self.assertEqual(sixty["model_samples"], 8)
        self.assertEqual(sixty["model_correct"], 5)
        self.assertEqual(sixty["model_accuracy"], 5/8)
        self.assertEqual(sixty["confidence_eligible"], 8)
        self.assertEqual(sixty["confidence_samples"], 5)
        self.assertEqual(sixty["confidence_correct"], 4)
        self.assertEqual(sixty["confidence_accuracy"], 4/5)
        confidence_ci = cockpit._wilson_interval(4, 5)
        self.assertEqual(sixty["confidence_accuracy_ci_low"], confidence_ci[0])
        self.assertEqual(sixty["confidence_accuracy_ci_high"], confidence_ci[1])
        self.assertEqual(sixty["confidence_coverage"], 5/8)
        self.assertEqual(sixty["confidence_threshold_pct"], 60)
        self.assertEqual(sixty["paired_directional_samples"], 6)
        self.assertEqual(sixty["paired_model_accuracy"], 4/6)
        self.assertEqual(sixty["paired_market_accuracy"], 3/6)
        self.assertEqual(sixty["paired_brier_samples"], 7)
        self.assertEqual(sixty["model_brier"], 0.21)
        self.assertEqual(sixty["market_brier"], 0.24)
        self.assertEqual(sixty["model_log_loss"], 0.58)
        self.assertEqual(sixty["market_log_loss"], 0.61)
        no_sample = next(item for item in result["scorecards"] if item["key"] == "open_0pct")
        self.assertIsNone(no_sample["model_accuracy"])
        self.assertIsNone(no_sample["confidence_accuracy_ci_low"])
        self.assertIsNone(no_sample["confidence_accuracy_ci_high"])
        self.assertEqual(result["score_days"], 5)
        self.assertEqual(result["score_model"], "model-a")
        self.assertEqual(result["score_min_confidence_pct"], 60)
        self.assertEqual(result["score_models"], ["model-a", "model-b"])
        self.assertEqual(result["score_checkpoint"], "elapsed_60pct")
        calibration = result["calibration"][6]["model"]
        self.assertEqual(calibration["samples"], 4)
        self.assertEqual(calibration["mean_forecast_up"], 0.64)
        self.assertEqual(calibration["observed_up"], 0.75)
        self.assertAlmostEqual(calibration["observed_up_ci_low"], 0.3006, places=4)
        self.assertAlmostEqual(calibration["observed_up_ci_high"], 0.9544, places=4)
        self.assertIsNone(result["calibration"][6]["clob"])
        self.assertEqual(query.call_args_list[2].args[1], (0.6, 0.6, "BTC", 300, 5, 5, "model-a", "model-a"))
        score_sql = query.call_args_list[2].args[0]
        self.assertIn("w.outcome IS NOT NULL", score_sql)
        self.assertIn("c.capture_status='captured'", score_sql)
        self.assertIn("c.market_up_probability<>0.5", score_sql)
        self.assertIn("interval '1 day'", score_sql)
        self.assertIn("COALESCE(NULLIF(c.model_version,''),'__legacy_unknown__')", score_sql)
        self.assertIn("AS model_log_loss", score_sql)
        self.assertIn("AS market_log_loss", score_sql)
        self.assertIn("c.side_probability >= %s", score_sql)
        self.assertIn("AS confidence_eligible", score_sql)
        self.assertIn("AS confidence_samples", score_sql)
        self.assertIn("1e-12::double precision", score_sql)
        calibration_sql = query.call_args_list[4].args[0]
        self.assertIn("c.checkpoint_key=%s", calibration_sql)
        self.assertIn("c.market_up_probability IS NOT NULL", calibration_sql)
        self.assertIn("FROM resolved WHERE market_up_probability IS NOT NULL", calibration_sql)
        self.assertIn("sum(outcome_up)::int AS observed_up_count", calibration_sql)
        self.assertEqual(query.call_args_list[4].args[1],
                         ("BTC", 300, "elapsed_60pct", 5, 5, "model-a", "model-a"))

    def test_wilson_interval_handles_small_samples_and_invalid_counts(self):
        lower, upper = cockpit._wilson_interval(0, 1)
        self.assertAlmostEqual(lower, 0.0, places=8)
        self.assertAlmostEqual(upper, 0.7935, places=4)
        self.assertEqual(cockpit._wilson_interval(0, 0), None)
        self.assertEqual(cockpit._wilson_interval(-1, 4), None)
        self.assertEqual(cockpit._wilson_interval(5, 4), None)

    def test_all_history_range_is_allowed_and_invalid_ranges_are_rejected(self):
        with patch.object(cockpit, "_prediction_history_ensure_schema"), \
             patch.object(cockpit, "q", side_effect=[{"total": 0}, [], [], [], [], None]), \
             patch.object(cockpit.os.path, "isfile", return_value=False):
            result = cockpit.api_prediction_history("ETH", 900, score_days=0)
        self.assertEqual(result["score_days"], 0)

        with patch.object(cockpit, "_prediction_history_ensure_schema"), \
             patch.object(cockpit, "q") as query:
            result = cockpit.api_prediction_history("ETH", 900, score_days=7)
        self.assertIn("score_days", result["error"])
        query.assert_not_called()

        with patch.object(cockpit, "_prediction_history_ensure_schema"), \
             patch.object(cockpit, "q") as query:
            result = cockpit.api_prediction_history("ETH", 900, score_model="bad/version")
        self.assertIn("score_model", result["error"])
        query.assert_not_called()

        with patch.object(cockpit, "_prediction_history_ensure_schema"), \
             patch.object(cockpit, "q") as query:
            result = cockpit.api_prediction_history("ETH", 900, score_checkpoint="elapsed_999pct")
        self.assertIn("score_checkpoint", result["error"])
        query.assert_not_called()

        with patch.object(cockpit, "_prediction_history_ensure_schema"), \
             patch.object(cockpit, "q") as query:
            result = cockpit.api_prediction_history("ETH", 900, score_min_confidence_pct=62)
        self.assertIn("score_min_confidence_pct", result["error"])
        query.assert_not_called()

    def test_root_history_route_forwards_selected_lookback(self):
        expected = {"score_days": 30}
        with patch.object(cockpit, "api_prediction_history", return_value=expected) as endpoint:
            actual = cockpit.ROUTES["/api/prediction_history"]({
                "asset": ["SOL"], "horizon_seconds": ["300"],
                "limit": ["25"], "offset": ["50"], "score_days": ["30"],
                "score_model": ["model-a"], "score_checkpoint": ["elapsed_40pct"],
                "score_min_confidence_pct": ["65"],
            })
        self.assertEqual(actual, expected)
        endpoint.assert_called_once_with("SOL", "300", "25", "50", "30", "model-a", "elapsed_40pct", "65")

    def test_root_history_modal_explains_and_renders_scorecard(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn('id="predictionHistoryScorecards"', page)
        self.assertIn('id="predictionHistoryModelFilter"', page)
        self.assertIn('id="predictionHistoryCheckpointFilter"', page)
        self.assertIn('id="predictionHistoryConfidenceFilter"', page)
        self.assertIn("function setPredictionHistoryConfidence(value)", page)
        self.assertIn("function scrollPredictionHistoryScorecardToCheckpoint()", page)
        self.assertIn("data-score-checkpoint=\"${esc(key)}\"", page)
        self.assertIn("confidence_coverage", page)
        self.assertIn("confidence_accuracy_ci_low", page)
        self.assertIn("confidence_accuracy_ci_high", page)
        self.assertIn("NOMINAL 95% WILSON INTERVAL", page)
        self.assertIn("SIDE CONFIDENCE · HIT RATE", page)
        self.assertIn('id="predictionHistoryCalibrationChart"', page)
        self.assertIn("function historyScoreWilsonInterval(item)", page)
        self.assertIn("observed_up_ci_low", page)
        self.assertIn("95% WILSON INTERVAL", page)
        self.assertIn('id="predictionHistoryExport"', page)
        self.assertIn("function predictionHistoryCsv(rows)", page)
        self.assertIn("exportPredictionHistoryCsv()", page)
        self.assertIn("setPredictionHistoryRange(5)", page)
        self.assertIn("setPredictionHistoryModel(this.value)", page)
        self.assertIn("setPredictionHistoryCheckpoint(this.value)", page)
        self.assertIn("PAIRED BRIER", page)
        self.assertIn("PAIRED LOG LOSS", page)
        self.assertIn("function historyScoreLogLoss(value)", page)
        self.assertIn("function historyScoreBrierSkill(model,clob,samples)", page)
        self.assertIn("CLOB-RELATIVE BRIER SKILL", page)
        self.assertIn("1 − model Brier / CLOB Brier", page)
        self.assertIn("paired_model_accuracy", page)
        self.assertIn("paired_market_accuracy", page)
        self.assertIn("lower Brier is better", page)
        self.assertIn("/api/prediction_history?${qs}", page)
        self.assertNotIn("/api/v3/prediction_history", page)


if __name__ == "__main__":
    unittest.main()
