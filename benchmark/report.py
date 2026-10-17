#!/usr/bin/env python3
"""Generate a comparative report from metrics.json + per-language test-results.json.

Usage:
  python3 benchmark/report.py --round round-1
  python3 benchmark/report.py --latest
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RESULTS = ROOT / "results"

LANGS = ["python", "javascript", "go", "rust", "c"]


def gather(round_id):
    base = RESULTS / round_id
    metrics = json.loads((base / "metrics.json").read_text()) if (base / "metrics.json").exists() else None
    tests = {}
    for f in sorted(base.glob("*/test-results.json")):
        tests[f.parent.name] = json.loads(f.read_text())
    return metrics, tests


def code_metrics(lang_dir: Path):
    if not lang_dir.is_dir():
        return None
    files = [f for f in lang_dir.rglob("*")
             if f.is_file() and not any(part in {".git", "target", "node_modules", "__pycache__", "vendor"} for part in f.parts)]
    src_ext = {".py", ".js", ".mjs", ".go", ".rs", ".c", ".h", ".cc", ".hpp"}
    src = [f for f in files if f.suffix in src_ext]
    lines = 0
    for f in src:
        try:
            lines += len(f.read_text().splitlines())
        except UnicodeDecodeError:
            pass
    return {"source_files": len(src), "total_files": len(files), "source_lines": lines,
            "source_bytes": sum(f.stat().st_size for f in src)}


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--round")
    g.add_argument("--latest", action="store_true")
    args = ap.parse_args()

    if args.latest:
        dirs = [d for d in RESULTS.iterdir() if d.is_dir()]
        if not dirs:
            sys.exit("no rounds in results/")
        round_id = sorted(d.name for d in dirs)[-1]
    else:
        round_id = args.round

    metrics, tests = gather(round_id)
    rows = []
    extra = [l for l in tests if l not in LANGS]  # e.g. oracle
    for lang in LANGS + extra:
        m = (metrics or {}).get("languages", {}).get(lang)
        t = tests.get(lang)
        c = code_metrics(ROOT / f"cli-{lang}" if lang in LANGS else ROOT / "benchmark" / lang)
        passed = t["passed"] if t else None
        total = t["total"] if t else None
        gen_eff = round(passed / m["output"] * 1000, 2) if m and passed and m["output"] else None
        tot_eff = round(passed / m["totalTokens"] * 1000, 2) if m and passed and m["totalTokens"] else None
        rows.append({
            "language": lang,
            "reference": lang not in LANGS,
            "passed": passed, "total": total,
            "pct": round(100 * passed / total, 1) if passed is not None else None,
            "conforming": passed == total if passed is not None else None,
            "in": m["input"] if m else None, "out": m["output"] if m else None,
            "reasoning": m["reasoning"] if m else None,
            "cacheR": m["cacheRead"] if m else None, "cacheW": m["cacheWrite"] if m else None,
            "total_tokens": m["totalTokens"] if m else None,
            "cost": round(m["cost"]["total"], 4) if m else None,
            "turns": m["user_messages"] if m else None,
            "tool_calls": m["tool_results"] if m else None,
            "compactions": m["compactions"] if m else None,
            "models": ", ".join(m["models"]) if m else None,
            "src_files": c["source_files"] if c else None,
            "src_lines": c["source_lines"] if c else None,
            "src_bytes": c["source_bytes"] if c else None,
            "gen_eff_k": gen_eff, "tot_eff_k": tot_eff,
        })

    if not any(r["passed"] is not None for r in rows):
        sys.exit(f"no test data for round {round_id} (run benchmark/runner.py first)")

    conforming = [r for r in rows if r["conforming"] and r["language"] in LANGS]
    if conforming:
        min_total = min(r["total_tokens"] for r in conforming)
        for r in rows:
            r["rel_cost"] = round(r["total_tokens"] / min_total, 2) if r["total_tokens"] else None

    def fmt(v, suffix=""):
        return "—" if v is None else f"{v}{suffix}"

    lines = [
        f"# Benchmark report — {round_id}", "",
        f"Model: {((metrics or {}).get('model')) or '(see metrics.json)'}", "",
        "| language | status | tests | % | conforming | in | out | total | reasoning | cache R/W | cost $ | turns | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        if r["passed"] is None:
            status = "not run"
        elif r["reference"]:
            status = "reference"
        else:
            status = "ok" if r["conforming"] else "failing"
        lines.append("| " + " | ".join([
            r["language"],
            status,
            f'{r["passed"]}/{r["total"]}' if r["passed"] is not None else "—",
            fmt(r["pct"]),
            "yes" if r["conforming"] else ("no" if r["conforming"] is not None else "—"),
            fmt(r["in"]), fmt(r["out"]), fmt(r["total_tokens"]), fmt(r["reasoning"]),
            f'{fmt(r["cacheR"])}/{fmt(r["cacheW"])}',
            f'{r["cost"]:.4f}' if r["cost"] is not None else "—",
            fmt(r["turns"]), fmt(r["tool_calls"]),
            fmt(r["src_files"]), fmt(r["src_lines"]),
            fmt(r["gen_eff_k"]), fmt(r["tot_eff_k"]), fmt(r.get("rel_cost")),
        ]) + " |")

    lines += ["",
              "**Metrics** — `in/out/total/reasoning/cache`: provider-reported tokens summed from the pi session "
              "(`message.usage` on every assistant turn). `gen eff (k)` = passed_tests / output_tokens * 1000. "
              "`tot eff (k)` = passed_tests / total_tokens * 1000. "
              "`rel cost` = total_tokens / min(total_tokens of conforming implementations).",
              ""]
    if conforming:
        lines.append("**Ranking** (conforming implementations, by lowest total tokens):")
        for i, r in enumerate(sorted(conforming, key=lambda x: x["total_tokens"]), 1):
            lines.append(f"{i}. **{r['language']}** — {r['total_tokens']} total tokens, "
                         f"{r['turns']} turns, {r['tool_calls']} tool calls")
    else:
        lines.append("_No conforming implementations yet._")
    nonconf = [r for r in rows if not r["reference"] and r["conforming"] is False]
    if nonconf:
        lines += ["", "**Non-conforming** (by % tests passed):"]
        for i, r in enumerate(sorted(nonconf, key=lambda x: -(x["pct"] or 0)), 1):
            lines.append(f"{i}. **{r['language']}** — {r['pct']}% ({r['passed']}/{r['total']})")
    not_run = [r["language"] for r in rows if r["passed"] is None]
    if not_run:
        lines += ["", f"_Not run yet: {', '.join(not_run)}._"]
    lines.append("")

    out = RESULTS / round_id / "report.md"
    out.write_text("\n".join(lines) + "\n")
    (RESULTS / round_id / "rows.json").write_text(json.dumps(rows, indent=2))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
