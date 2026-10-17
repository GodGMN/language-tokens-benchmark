#!/usr/bin/env python3
"""Export every round into tidy analysis tables.

Writes (under results/):
  dataset.csv         one row per (round, language) sample
  dataset_tests.csv   one row per (round, language, test)
  dataset.json        the same sample rows as JSON, plus model metadata

Usage: python3 benchmark/export.py
"""
import csv
import json
import sys

import common

SAMPLE_COLS = ["round", "task", "spec_version", "model", "model_version", "language",
               "passed", "total", "conforming", "output_tokens", "input_tokens",
               "cache_read", "cache_write", "reasoning", "total_tokens", "cost_usd",
               "turns", "tool_calls", "src_files", "src_lines"]
TEST_COLS = ["round", "task", "language", "test", "bucket", "private", "since", "passed", "seconds"]


def main():
    samples, tests = [], []
    for mf in sorted(common.RESULTS.glob("*/metrics.json")):
        rid = mf.parent.name
        try:
            cfg = common.load_round(rid)
        except FileNotFoundError:
            cfg = {}
        task = cfg.get("task", "workflow")
        model = cfg.get("model") or {}
        metrics = json.loads(mf.read_text())
        summary_p = common.RESULTS / rid / "summary.json"
        summary = json.loads(summary_p.read_text()) if summary_p.exists() else {}
        rows = {r["language"]: r for r in summary.get("all", [])}
        for lang, m in sorted(metrics.get("languages", {}).items()):
            r = rows.get(lang, {})
            code = r.get("code") or {}
            samples.append({
                "round": rid, "task": task, "spec_version": cfg.get("spec_version", 1),
                "model": model.get("name"), "model_version": model.get("version"),
                "language": lang,
                "passed": r.get("passed"), "total": r.get("total"),
                "conforming": r.get("conforming"),
                "output_tokens": m.get("output"), "input_tokens": m.get("input"),
                "cache_read": m.get("cacheRead"), "cache_write": m.get("cacheWrite"),
                "reasoning": m.get("reasoning"), "total_tokens": m.get("totalTokens"),
                "cost_usd": round((m.get("cost") or {}).get("total", 0.0), 6),
                "turns": m.get("assistant_messages"), "tool_calls": m.get("tool_results"),
                "src_files": code.get("source_files"), "src_lines": code.get("source_lines"),
            })
        tr = common.RESULTS / rid
        for tf in sorted(tr.glob("*/test-results.json")):
            d = json.loads(tf.read_text())
            for t in d.get("results", []):
                tests.append({
                    "round": rid, "task": task, "language": d["language"],
                    "test": t.get("name"), "bucket": t.get("bucket"),
                    "private": t.get("private"), "since": t.get("since"),
                    "passed": t.get("passed"), "seconds": t.get("seconds"),
                })

    if not samples:
        sys.exit("no results found")

    with open(common.RESULTS / "dataset.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=SAMPLE_COLS); w.writeheader(); w.writerows(samples)
    with open(common.RESULTS / "dataset_tests.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=TEST_COLS); w.writeheader(); w.writerows(tests)
    (common.RESULTS / "dataset.json").write_text(json.dumps(
        {"samples": samples, "tests": len(tests)}, indent=2))
    print(f"wrote results/dataset.csv ({len(samples)} samples), "
          f"results/dataset_tests.csv ({len(tests)} test outcomes)")


if __name__ == "__main__":
    main()
