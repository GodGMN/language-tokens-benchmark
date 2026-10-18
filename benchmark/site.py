#!/usr/bin/env python3
"""Build results/index.json — a compact index a website can consume.

Aggregates every results/<round>/summary.json into one file:

    python3 benchmark/site.py

The schema is intentionally small and stable:

    {
      "benchmark": "language-tokens-benchmark",
      "rounds": [
        { "round": "...", "label": "...", "model": {...},
          "tests": {...}, "leaderboard": [ {...}, ... ] }
      ]
    }
"""
import json
import sys

import common


def main():
    rounds = []
    for summary_path in sorted(common.RESULTS.glob("*/summary.json")):
        try:
            rounds.append(json.loads(summary_path.read_text()))
        except json.JSONDecodeError as e:
            print(f"skipping {summary_path}: {e}", file=sys.stderr)

    if not rounds:
        sys.exit("no results/*/summary.json found (run benchmark/report.py first)")

    index = {
        "benchmark": "language-tokens-benchmark",
        "rounds": [
            {
                "round": r.get("round"),
                "label": r.get("label"),
                "model": r.get("model"),
                "tests": r.get("tests"),
                "task": r.get("task"),
                "evaluation": r.get("evaluation"),
                "leaderboard": r.get("leaderboard", []),
            }
            for r in rounds
        ],
    }
    out = common.RESULTS / "index.json"
    out.write_text(json.dumps(index, indent=2))
    print(f"wrote {out} ({len(rounds)} rounds)")


if __name__ == "__main__":
    main()
