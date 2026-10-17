#!/usr/bin/env python3
"""Aggregate multiple rounds that used the same model group.

A single round is one sample; rankings by total tokens are noisy because total
tokens are dominated by cache reads, which scale with the number of turns.

This groups rounds by `group` (from round.json) and reports, per language:
  - conformance rate across samples
  - median / min / max for output and total tokens (conforming samples only)
  - median turns and tool calls

Usage:
  python3 benchmark/aggregate.py
"""
import json
import statistics
import sys

import common

LANGS = ["python", "javascript", "go", "rust", "c"]


def load_all():
    out = []
    for sp in sorted(common.RESULTS.glob("*/summary.json")):
        s = json.loads(sp.read_text())
        rid = s.get("round")
        try:
            cfg = common.load_round(rid)
        except FileNotFoundError:
            cfg = {}
        s["_group"] = cfg.get("group") or (s.get("model") or {}).get("name") or "ungrouped"
        s["_spec"] = s.get("spec_version", cfg.get("spec_version", 1))
        s["_task"] = cfg.get("task", "workflow")
        s["_version"] = (cfg.get("model") or {}).get("version") or "unspecified"
        s["_by_lang"] = {r["language"]: r for r in s.get("all", [])}
        out.append(s)
    return out


def median(vals):
    return statistics.median(vals) if vals else None


def fmt(v):
    return "—" if v is None else f"{v:,}"


def main():
    rounds = load_all()
    if not rounds:
        sys.exit("no results/*/summary.json found")

    groups = {}
    for r in rounds:
        # Group by model group AND spec version: different specs are different tasks.
        groups.setdefault((r["_group"], r["_task"], r["_spec"]), []).append(r)

    lines = ["# Aggregated results", "",
             "Samples are grouped by **model group, task and spec version**; rounds that "
             "differ in any of them are not pooled.", ""]
    for (group, task, spec), rs in sorted(groups.items()):
        versions = sorted({r["_version"] for r in rs})
        lines += [f"## {task} · {group} · spec v{spec}", "",
                  f"Samples: {len(rs)} round(s) — {', '.join(r['round'] for r in rs)}  ",
                  f"Version(s): {', '.join(versions)}", ""]
        if len(rs) < 3:
            lines += ["> Fewer than 3 samples: treat these numbers as indicative only.", ""]

        lines.append("| language | samples | conforming | output tok (median [min–max]) | total tok (median [min–max]) | turns (median) |")
        lines.append("|---|---|---|---|---|---|")
        for lang in LANGS:
            rows = [r["_by_lang"].get(lang) for r in rs]
            rows = [x for x in rows if x and x.get("total") is not None]
            if not rows:
                continue
            conf = [x for x in rows if x.get("conforming")]
            out = [x["output"] for x in conf if x.get("output")]
            tot = [x["total_tokens"] for x in conf if x.get("total_tokens")]
            turns = [x["turns"] for x in rows if x.get("turns")]

            def rng(v):
                return "—" if not v else f"{fmt(int(median(v)))} [{fmt(min(v))}–{fmt(max(v))}]"

            lines.append("| " + " | ".join([
                lang,
                str(len(rows)),
                f"{len(conf)}/{len(rows)}",
                rng(out), rng(tot),
                fmt(int(median(turns))) if turns else "—",
            ]) + " |")
        lines.append("")

    lines += [
        "## Method notes",
        "",
        "- Only **conforming** samples contribute to the token medians; a sample that",
        "  fails the suite is recorded in the conformance column but never averaged in.",
        "- Report the median with the min–max range while the sample count is small.",
        "- **Do not pool rounds that differ in model version or spec version.** Both are",
        "  different tasks; group by both. If the provider exposes no version, record it",
        "  manually per round.",
        "- Total tokens ≈ turns × context size, so it is far noisier than output",
        "  tokens. Prefer output tokens for 'effort' and total tokens for 'cost'.",
        "",
    ]

    out = common.RESULTS / "aggregate.md"
    out.write_text("\n".join(lines))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
