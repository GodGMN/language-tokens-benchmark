#!/usr/bin/env python3
"""Generate the comparative report and a website-ready summary.

Usage:
  python3 benchmark/report.py --round round-1
  python3 benchmark/report.py --latest

Outputs (under results/<round>/):
  report.md     human-readable report with leaderboard
  rows.json     full per-language metric rows
  summary.json  compact, stable schema for a future website
"""
import argparse
import json
import sys
from pathlib import Path

import common

LANGUAGES = ["python", "javascript", "go", "rust", "c"]
SRC_EXT = {".py", ".js", ".mjs", ".go", ".rs", ".c", ".h", ".cc", ".hpp"}
SKIP_DIRS = {".git", "target", "node_modules", "__pycache__", "vendor", "build", ".venv"}


def code_metrics(project: Path):
    if not project.is_dir():
        return None
    files = [f for f in project.rglob("*")
             if f.is_file() and not (set(f.parts) & SKIP_DIRS)]
    src = [f for f in files if f.suffix in SRC_EXT]
    lines = 0
    for f in src:
        try:
            lines += len(f.read_text(errors="ignore").splitlines())
        except OSError:
            pass
    return {"source_files": len(src), "source_lines": lines,
            "source_bytes": sum(f.stat().st_size for f in src)}


def gather(round_id):
    base = common.RESULTS / round_id
    metrics = json.loads((base / "metrics.json").read_text()) if (base / "metrics.json").exists() else {}
    tests = {}
    for f in sorted(base.glob("*/test-results.json")):
        tests[f.parent.name] = json.loads(f.read_text())
    return metrics, tests


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--round")
    g.add_argument("--latest", action="store_true")
    args = ap.parse_args()

    if args.latest:
        dirs = [d for d in common.RESULTS.iterdir() if d.is_dir()]
        if not dirs:
            sys.exit("no rounds in results/")
        round_id = sorted(dirs, key=lambda d: d.stat().st_mtime)[-1].name
    else:
        round_id = args.round

    try:
        round_cfg = common.load_round(round_id)
    except FileNotFoundError:
        round_cfg = {"id": round_id, "label": round_id, "model": None, "languages": {}}

    metrics, tests = gather(round_id)
    model = metrics.get("model") or round_cfg.get("model") or {}

    rows = []
    extras = [l for l in tests if l not in LANGUAGES]  # e.g. oracle
    for lang in LANGUAGES + extras:
        m = metrics.get("languages", {}).get(lang)
        t = tests.get(lang)
        adapter = common.load_adapters().get(lang, {"language": lang})
        project = common.resolve_project(adapter, round_cfg)
        code = code_metrics(project)

        passed = t["passed"] if t else None
        total = t["total"] if t else None
        b = (t or {}).get("buckets", {})
        pub = b.get("public", [0, 0])
        hold = b.get("holdout", [0, 0])
        fut = b.get("future", [0, 0])

        out_tok = m["output"] if m else None
        tot_tok = m["totalTokens"] if m else None
        rows.append({
            "language": lang,
            "reference": lang not in LANGUAGES,
            "passed": passed, "total": total,
            "pct": round(100 * passed / total, 1) if passed is not None and total else None,
            "conforming": passed == total if passed is not None and total else None,
            "public": pub, "holdout": hold, "future": fut,
            "input": m["input"] if m else None, "output": out_tok,
            "reasoning": m["reasoning"] if m else None,
            "cache_read": m["cacheRead"] if m else None, "cache_write": m["cacheWrite"] if m else None,
            "total_tokens": tot_tok, "cost": round(m["cost"]["total"], 4) if m else None,
            "turns": m["user_messages"] if m else None,
            "tool_calls": m["tool_results"] if m else None,
            "code": code,
            "gen_eff_k": round(passed / out_tok * 1000, 2) if passed and out_tok else None,
            "tot_eff_k": round(passed / tot_tok * 1000, 2) if passed and tot_tok else None,
        })

    if not any(r["passed"] is not None for r in rows):
        sys.exit(f"no test data for round {round_id} (run benchmark/runner.py first)")

    conforming = [r for r in rows if r["conforming"] and not r["reference"]]
    if conforming:
        min_total = min(r["total_tokens"] for r in conforming)
        for r in rows:
            r["rel_cost"] = round(r["total_tokens"] / min_total, 2) if r["total_tokens"] else None
        for rank, r in enumerate(sorted(conforming, key=lambda x: x["total_tokens"]), 1):
            r["rank"] = rank

    def fmt(v):
        return "—" if v is None else str(v)

    def fnum(v):
        return "—" if v is None else f"{v:,}"

    lines = [
        f"# Benchmark report — {round_id}", "",
        f"**{round_cfg.get('label', round_id)}**",
        "",
        f"Model: **{model.get('display') or model.get('name') or 'unknown'}** "
        f"(`{model.get('provider','?')}/{model.get('name','?')}`)"
        + (f" · {model['params']}" if model.get("params") else "")
        + (f" · thinking={model['thinking']}" if model.get("thinking") else ""),
        "",
        (lambda rep: (
            f"Tests per implementation: {rep['public'][1]} public + {rep['holdout'][1]} holdout "
            f"= {rep['total']} scored"
            + (f" (+{rep['future'][1]} future-spec, not scored)" if rep['future'][1] else "")
        ) if rep else "Tests: —")(next((r for r in rows if not r["reference"] and r["total"]), None)),
        "",
        "| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        if r["passed"] is None:
            status = "not run"
        elif r["reference"]:
            status = "reference"
        else:
            status = "ok" if r["conforming"] else "failing"
        lines.append("| " + " | ".join([
            r["language"], status,
            f'{r["passed"]}/{r["total"]}' if r["passed"] is not None else "—",
            f'{r["public"][0]}/{r["public"][1]}',
            f'{r["holdout"][0]}/{r["holdout"][1]}' if r["holdout"][1] else "—",
            f'{r["future"][0]}/{r["future"][1]}' if r["future"][1] else "—",
            fnum(r["output"]), fnum(r["total_tokens"]),
            f'{r["cost"]:.4f}' if r["cost"] is not None else "—",
            fmt(r["tool_calls"]),
            fmt(r["code"]["source_files"] if r["code"] else None),
            fmt(r["code"]["source_lines"] if r["code"] else None),
            fmt(r["gen_eff_k"]), fmt(r["tot_eff_k"]), fmt(r.get("rel_cost")),
        ]) + " |")

    lines += ["",
              "**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. "
              "`gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. "
              "`rel cost` = total_tokens ÷ best conforming total. "
              "`holdout` = edge-case suite scored for this round's spec version. "
              "`future-spec` = holdout cases for rules added after this round (informational).", ""]

    if conforming:
        lines.append("## Leaderboard (conforming, by total tokens)")
        lines.append("")
        for r in sorted(conforming, key=lambda x: x["rank"]):
            lines.append(f"{r['rank']}. **{r['language']}** — {r['total_tokens']:,} tokens, "
                         f"{r['output']:,} output, {r['tool_calls']} tool calls, ${r['cost']:.4f}")
    else:
        lines += ["_No conforming implementations yet._"]

    nonconf = [r for r in rows if not r["reference"] and r["conforming"] is False]
    if nonconf:
        lines += ["", "## Non-conforming (by pass rate)", ""]
        for i, r in enumerate(sorted(nonconf, key=lambda x: -(x["pct"] or 0)), 1):
            lines.append(f"{i}. **{r['language']}** — {r['pct']}% ({r['passed']}/{r['total']})")

    not_run = [r["language"] for r in rows if r["passed"] is None]
    if not_run:
        lines += ["", f"_Not run yet: {', '.join(not_run)}._"]
    lines.append("")

    outdir = common.RESULTS / round_id
    (outdir / "report.md").write_text("\n".join(lines))

    # Website-ready compact summary (stable schema).
    summary = {
        "round": round_id,
        "label": round_cfg.get("label", round_id),
        "spec_version": round_cfg.get("spec_version", 1),
        "model": model,
        "tests": {
            "total": (next((r for r in rows if not r["reference"] and r["total"]), {}) or {}).get("total"),
            "public": (next((r for r in rows if not r["reference"] and r["total"]), {}) or {}).get("public", [0, 0])[1],
            "holdout": (next((r for r in rows if not r["reference"] and r["total"]), {}) or {}).get("holdout", [0, 0])[1],
            "future_spec": (next((r for r in rows if not r["reference"] and r["total"]), {}) or {}).get("future", [0, 0])[1],
        },
        "leaderboard": [
            {k: r.get(k) for k in ("rank", "language", "passed", "total", "pct",
                                   "output", "total_tokens", "cost", "tool_calls",
                                   "gen_eff_k", "tot_eff_k", "rel_cost")}
            | {"code": r["code"]}
            for r in sorted(conforming, key=lambda x: x["rank"])
        ],
        "all": rows,
    }
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2))
    (outdir / "rows.json").write_text(json.dumps(rows, indent=2))
    print(f"wrote {outdir / 'report.md'}")
    print(f"wrote {outdir / 'summary.json'}")


if __name__ == "__main__":
    main()
