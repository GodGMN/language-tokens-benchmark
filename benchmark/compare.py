#!/usr/bin/env python3
"""Cross-round comparison.

Reads every results/<round>/summary.json and writes results/compare.md:
  - one table row per (round, language)
  - a stability section showing how output tokens vary across rounds

Usage: python3 benchmark/compare.py
"""
import json
import sys

import common

LANGS = ["python", "javascript", "go", "rust", "c"]


def load_summaries():
    out = []
    for p in sorted(common.RESULTS.glob("*/summary.json")):
        out.append(json.loads(p.read_text()))
    return out


def main():
    rounds = load_summaries()
    if not rounds:
        sys.exit("no results/*/summary.json found")

    lines = ["# Cross-round comparison", ""]
    lines.append("| round | spec | language | scored | output tok | total tok | cost $ | conforming |")
    lines.append("|---|---|---|---|---|---|---|---|")
    stability = {l: [] for l in LANGS}
    for r in rounds:
        by_lang = {row["language"]: row for row in r.get("all", [])}
        for lang in LANGS:
            row = by_lang.get(lang)
            if not row or row.get("total") is None:
                continue
            conf = row.get("conforming")
            lines.append("| " + " | ".join([
                r.get("round", "?"),
                f'v{r.get("spec_version", 1)}',
                lang,
                f'{row["passed"]}/{row["total"]}',
                f'{row.get("output", 0):,}' if row.get("output") is not None else "—",
                f'{row.get("total_tokens", 0):,}' if row.get("total_tokens") is not None else "—",
                f'{row["cost"]:.4f}' if row.get("cost") is not None else "—",
                "yes" if conf else "no" if conf is not None else "—",
            ]) + " |")
            if conf and row.get("output"):
                stability[lang].append((r.get("round"), row["output"]))

    lines += ["", "## Output-token stability across rounds", "",
              "Output tokens are the cleaner measure: they are not inflated by cache reads.",
              ""]
    lines.append("| language | rounds | output tokens | spread |")
    lines.append("|---|---|---|---|")
    for lang in LANGS:
        vals = stability[lang]
        if len(vals) < 2:
            continue
        nums = [v for _, v in vals]
        lo, hi = min(nums), max(nums)
        spread = f"{(hi / lo - 1) * 100:.0f}%"
        detail = ", ".join(f'{r}:{v:,}' for r, v in vals)
        lines.append(f"| {lang} | {len(vals)} | {detail} | {spread} |")

    lines += ["", "## Reading this", "",
              "- A single round is one sample. Rankings by **total** tokens move a lot",
              "  between runs because session length (and therefore cache reads) varies.",
              "- **Output** tokens are more stable and are the better signal for",
              "  'how much did the model have to write'.",
              "- Correctness is the gate: only conforming implementations are ranked.",
              ""]

    out = common.RESULTS / "compare.md"
    out.write_text("\n".join(lines))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
