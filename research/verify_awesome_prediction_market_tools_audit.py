#!/usr/bin/env python3
"""Check that the item-by-item report still covers the upstream tool list.

Run from any directory with:
    python3 research/verify_awesome_prediction_market_tools_audit.py

The network read is limited to the public upstream README. Use --readme-file
to validate against a previously saved upstream copy without network access.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import sys
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit
from urllib.request import Request, urlopen


UPSTREAM_README = (
    "https://raw.githubusercontent.com/aarora4/"
    "Awesome-Prediction-Market-Tools/main/README.md"
)
DEFAULT_REPORT = Path(__file__).with_name("awesome-prediction-market-tools-audit.md")
SOURCE_ITEM = re.compile(r"^\s*- \[([^\]]+)\]\(([^)]+)\)")
FEATURED_ITEM = re.compile(r"^\*\*\[([^\]]+)\]\(([^)]+)\)\*\*", re.MULTILINE)
REPORT_ITEM = re.compile(r"^(\d+)\. \*\*\[([^\]]+)\]\(([^)]+)\)\*\*")
REQUIRED_REPORT_FIELDS = ("**Listed:**", "**Checked:**", "**Assessment:**")


def normalize_url(url: str) -> str:
    """Ignore tracking parameters while preserving meaningful URL identity."""
    parts = urlsplit(url.strip())
    query = sorted(
        (key, value)
        for key, value in parse_qsl(parts.query, keep_blank_values=True)
        if not key.lower().startswith("utm_")
    )
    return urlunsplit(
        (
            parts.scheme.lower(),
            parts.netloc.lower(),
            parts.path.rstrip("/"),
            urlencode(query),
            "",
        )
    )


def source_items(readme: str) -> list[tuple[str, str]]:
    """Extract the featured project and category listings, excluding Contents."""
    items: list[tuple[str, str]] = []
    section = ""
    for line in readme.splitlines():
        if line.startswith("## "):
            section = line[3:].strip().casefold()
        if not section or section.startswith("contents") or section.startswith("⭐ featured"):
            continue
        match = SOURCE_ITEM.match(line)
        if match:
            items.append((match.group(1).strip(), normalize_url(match.group(2))))

    featured = FEATURED_ITEM.search(readme)
    if featured:
        items.insert(0, (featured.group(1).strip(), normalize_url(featured.group(2))))
    return items


def report_items(report: str) -> tuple[list[tuple[int, str, str]], list[str]]:
    rows: list[tuple[int, str, str]] = []
    incomplete: list[str] = []
    for line in report.splitlines():
        match = REPORT_ITEM.match(line)
        if not match:
            continue
        number = int(match.group(1))
        rows.append((number, match.group(2).strip(), normalize_url(match.group(3))))
        positions = [line.find(field) for field in REQUIRED_REPORT_FIELDS]
        counts = [line.count(field) for field in REQUIRED_REPORT_FIELDS]
        for field, count in zip(REQUIRED_REPORT_FIELDS, counts):
            if count != 1:
                incomplete.append(
                    f"entry {number}: {field} appears {count} times (expected exactly once)"
                )
        if all(position >= 0 for position in positions) and positions != sorted(positions):
            incomplete.append(f"entry {number}: required fields are out of order")
        if all(count == 1 for count in counts) and positions == sorted(positions):
            values = [
                line[positions[index] + len(REQUIRED_REPORT_FIELDS[index]):positions[index + 1]]
                for index in range(len(REQUIRED_REPORT_FIELDS) - 1)
            ]
            values.append(line[positions[-1] + len(REQUIRED_REPORT_FIELDS[-1]):])
            for field, value in zip(REQUIRED_REPORT_FIELDS, values):
                if not value.strip():
                    incomplete.append(f"entry {number}: {field} value is empty")
    return rows, incomplete


def fetch_readme(url: str) -> bytes:
    request = Request(url, headers={"User-Agent": "PolySparQ-Audit-Coverage-Check/1.0"})
    with urlopen(request, timeout=30) as response:
        if response.status != 200:
            raise OSError(f"upstream README returned HTTP {response.status}")
        return response.read()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--readme-url", default=UPSTREAM_README)
    parser.add_argument(
        "--readme-file",
        type=Path,
        help="validate against a saved README instead of fetching upstream",
    )
    args = parser.parse_args()

    try:
        raw = args.readme_file.read_bytes() if args.readme_file else fetch_readme(args.readme_url)
        readme = raw.decode("utf-8-sig")
        report = args.report.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    source = source_items(readme)
    rows, incomplete = report_items(report)
    report_pairs = [(name, url) for _, name, url in rows]
    sequential = [number for number, _, _ in rows] == list(range(1, len(rows) + 1))
    matches = source == report_pairs
    unique = len(set(source)) == len(source)
    passed = len(source) == 179 and sequential and unique and matches and not incomplete

    print(f"README bytes: {len(raw)}")
    print(f"README SHA-256: {hashlib.sha256(raw).hexdigest()}")
    print(f"Source projects: {len(source)}; report entries: {len(rows)}")
    print(f"Source projects unique: {'yes' if unique else 'NO'}")
    print(f"Report numbering sequential: {'yes' if sequential else 'NO'}")
    print(f"Names and normalized URLs match in order: {'yes' if matches else 'NO'}")
    print(f"Every entry has Listed/Checked/Assessment: {'yes' if not incomplete else 'NO'}")

    if not matches:
        for index, (source_item, report_item) in enumerate(zip(source, report_pairs), start=1):
            if source_item != report_item:
                print(f"First mismatch at entry {index}: source={source_item!r}; report={report_item!r}")
                break
        if len(source) != len(report_pairs):
            print(f"Entry-count mismatch: source={len(source)}; report={len(report_pairs)}")
    for issue in incomplete:
        print(issue)
    if not passed:
        return 1
    print("PASS: report covers the current upstream inventory.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
