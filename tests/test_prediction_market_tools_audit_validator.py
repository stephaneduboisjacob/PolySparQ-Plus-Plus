"""Offline regression checks for the public-list audit coverage validator."""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "research"))
import verify_awesome_prediction_market_tools_audit as audit


class AuditValidatorTests(unittest.TestCase):
    def test_source_items_include_featured_entry_and_skip_contents(self):
        readme = """# Example

## ⭐ Featured: Oddpool
**[Oddpool](https://oddpool.example/)** — Featured description.

## Contents
- [AI agents](#ai-agents)

## 🧠 AI Agents
- [Alpha](https://alpha.example/tool/?utm_source=list) — Product.

## 📚 APIs
- [Beta](https://beta.example/) — API.
"""

        self.assertEqual(
            audit.source_items(readme),
            [
                ("Oddpool", "https://oddpool.example"),
                ("Alpha", "https://alpha.example/tool"),
                ("Beta", "https://beta.example"),
            ],
        )

    def test_report_rows_accept_complete_fields_once_and_in_order(self):
        report = (
            "1. **[Oddpool](https://oddpool.example)** — "
            "**Listed:** Feature. **Checked:** HTTP 200. **Assessment:** No integration.\n"
        )

        rows, issues = audit.report_items(report)

        self.assertEqual(rows, [(1, "Oddpool", "https://oddpool.example")])
        self.assertEqual(issues, [])

    def test_report_rows_reject_missing_and_duplicate_fields(self):
        missing = (
            "1. **[Oddpool](https://oddpool.example)** — "
            "**Listed:** Feature. **Checked:** HTTP 200. **Assessmnt:** No integration.\n"
        )
        duplicate = (
            "1. **[Oddpool](https://oddpool.example)** — "
            "**Listed:** Feature. **Listed:** duplicate. **Checked:** HTTP 200. "
            "**Assessment:** No integration.\n"
        )

        _, missing_issues = audit.report_items(missing)
        _, duplicate_issues = audit.report_items(duplicate)

        self.assertIn("entry 1: **Assessment:** appears 0 times (expected exactly once)", missing_issues)
        self.assertIn("entry 1: **Listed:** appears 2 times (expected exactly once)", duplicate_issues)

    def test_report_rows_reject_empty_values_and_out_of_order_fields(self):
        empty = (
            "1. **[Oddpool](https://oddpool.example)** — "
            "**Listed:** Feature. **Checked:** **Assessment:** No integration.\n"
        )
        out_of_order = (
            "1. **[Oddpool](https://oddpool.example)** — "
            "**Assessment:** No integration. **Listed:** Feature. **Checked:** HTTP 200.\n"
        )

        _, empty_issues = audit.report_items(empty)
        _, order_issues = audit.report_items(out_of_order)

        self.assertIn("entry 1: **Checked:** value is empty", empty_issues)
        self.assertIn("entry 1: required fields are out of order", order_issues)

    def test_tracking_query_parameters_do_not_change_source_identity(self):
        self.assertEqual(
            audit.normalize_url("https://EXAMPLE.com/tool/?utm_source=list&ref=directory"),
            audit.normalize_url("https://example.com/tool?ref=directory"),
        )


if __name__ == "__main__":
    unittest.main()
