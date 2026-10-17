#!/usr/bin/env python3
"""Run the conformance suite (fixtures) against one or all implementations.

Usage:
  python3 benchmark/runner.py --language go
  python3 benchmark/runner.py --all
  python3 benchmark/runner.py --language go --category references
  python3 benchmark/runner.py --all --public          # skip benchmark/holdout/
  python3 benchmark/runner.py --all --round round-1

Fixtures: benchmark/fixtures/<category>/*.json  (+ benchmark/holdout/ when present)
Each fixture: {name, input, expected_exit_code, expected_output?, expected_error_code?, stdin?}
Results: results/<round>/<language>/test-results.json
"""
import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from common import (HERE, ROOT, RESULTS, default_round, load_adapters, load_round,
                    project_ready, resolve_project, task_paths)

def project_ready_any(adapter, round_cfg):
    return project_ready(adapter, resolve_project(adapter, round_cfg))


def load_fixtures(tp, category=None, include_private=True):
    """Public fixtures always; holdout ones only if present and requested."""
    public, private = tp["fixtures"], tp["holdout"]
    roots = [public] + ([private] if include_private and Path(private).is_dir() else [])
    out = []
    for root in roots:
        for f in sorted(root.glob("*/*.json")):
            if category and f.parent.name != category:
                continue
            d = json.loads(f.read_text())
            d["_file"] = str(f.relative_to(ROOT))
            d["_private"] = root is private
            out.append(d)
    return out


def run_fixture(adapter, fixture, project, timeout):
    lang = adapter["language"]
    with tempfile.TemporaryDirectory(prefix=f"wf-{lang}-") as td:
        t0 = time.monotonic()
        if fixture.get("stdin"):
            cmd = [c.replace("{input}", "-") for c in adapter.get("run_stdin", adapter["run"])]
            payload = json.dumps(fixture["input"])
        else:
            inp = Path(td) / "workflow.json"
            inp.write_text(json.dumps(fixture["input"]))
            cmd = [c.replace("{input}", str(inp)) for c in adapter["run"]]
            payload = None
        try:
            p = subprocess.run(cmd, cwd=project, input=payload,
                               capture_output=True, text=True, timeout=timeout)
            elapsed = time.monotonic() - t0
            try:
                parsed = json.loads(p.stdout.strip())
                valid_json = True
            except Exception:
                parsed, valid_json = None, False
            return {
                "exit_code": p.returncode, "stdout_json": parsed,
                "stdout_valid_json": valid_json, "stderr_tail": p.stderr[-2000:],
                "seconds": round(elapsed, 3), "timeout": False,
            }
        except subprocess.TimeoutExpired:
            return {"exit_code": None, "stdout_json": None, "stdout_valid_json": False,
                    "stderr_tail": "TIMEOUT", "seconds": timeout, "timeout": True}
        except FileNotFoundError as e:
            return {"exit_code": None, "stdout_json": None, "stdout_valid_json": False,
                    "stderr_tail": f"MISSING EXECUTABLE: {e}", "seconds": 0, "timeout": False}


RESPONSE_FIELDS = ("status", "results", "execution_order", "skipped", "error")


def shape_errors(resp):
    if not isinstance(resp, dict):
        return ["response is not a JSON object"]
    problems = [f"missing field `{k}`" for k in RESPONSE_FIELDS if k not in resp]
    extra = [k for k in resp if k not in RESPONSE_FIELDS]
    if extra:
        problems.append(f"unexpected fields: {extra}")
    if resp.get("status") not in ("completed", "failed"):
        problems.append(f"bad status {resp.get('status')!r}")
    if not isinstance(resp.get("results"), dict):
        problems.append("results is not an object")
    for field in ("execution_order", "skipped"):
        v = resp.get(field)
        if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
            problems.append(f"{field} is not an array of strings")
    err = resp.get("error")
    if err is not None:
        if not isinstance(err, dict) or not {"code", "step", "message"} <= set(err):
            problems.append("error is not {code, step, message}")
    elif resp.get("status") == "failed":
        problems.append("failed response has no error object")
    return problems


def comparable(x):
    """Recursively drop free-form "message" fields before comparing."""
    if isinstance(x, dict):
        return {k: comparable(v) for k, v in x.items() if k != "message"}
    if isinstance(x, list):
        return [comparable(v) for v in x]
    return x


def check(fixture, result, envelope="workflow"):
    reasons = []
    exp_exit = fixture.get("expected_exit_code")
    if result["exit_code"] != exp_exit:
        reasons.append(f"exit {result['exit_code']} != expected {exp_exit}")
    if not result["stdout_valid_json"]:
        reasons.append("stdout is not valid JSON")
        return reasons
    actual = result["stdout_json"]
    if envelope == "workflow":
        reasons.extend(shape_errors(actual))

    exp = fixture.get("expected_output")
    if exp is not None and comparable(exp) != comparable(actual):
        reasons.append("output mismatch")
        for k in RESPONSE_FIELDS:
            a, b = comparable(exp).get(k, "<missing>"), comparable(actual).get(k, "<missing>")
            if a != b:
                reasons.append(f"  field `{k}`: got {json.dumps(b)} want {json.dumps(a)}")

    exp_code = fixture.get("expected_error_code")
    if exp_code is not None:
        got = (actual.get("error") or {}).get("code")
        if got != exp_code:
            reasons.append(f"error.code {got!r} != expected {exp_code!r}")
    return reasons


def run_language(adapter, fixtures, project, timeout, spec_version, envelope="workflow", verbose=True):
    """Run fixtures, bucketing into public / scored-holdout / future-spec.

    A holdout fixture tagged `"since": N` only counts for rounds whose
    round.json has `spec_version >= N`; otherwise it is reported separately so
    an implementation is never penalised for a rule that did not exist yet.
    """
    results = []
    buckets = {"public": [0, 0], "holdout": [0, 0], "future": [0, 0]}
    for fx in fixtures:
        r = run_fixture(adapter, fx, project, timeout)
        reasons = check(fx, r, envelope)
        ok = not reasons
        since = fx.get("since", 1)
        if not fx["_private"]:
            bucket = "public"
        elif since <= spec_version:
            bucket = "holdout"
        else:
            bucket = "future"
        buckets[bucket][0] += int(ok)
        buckets[bucket][1] += 1
        if verbose:
            print(f"  [{'PASS' if ok else 'FAIL'}] ({bucket}) {fx['_file']}  (exit={r['exit_code']}, {r['seconds']}s)")
            for why in (reasons[:8] if not ok else []):
                print(f"         {why}")
        results.append({"name": fx["name"], "file": fx["_file"], "private": fx["_private"],
                        "since": since, "bucket": bucket, "passed": ok, "reasons": reasons,
                        "exit_code": r["exit_code"], "seconds": r["seconds"]})
    scored = (buckets["public"][0] + buckets["holdout"][0],
              buckets["public"][1] + buckets["holdout"][1])
    return scored[0], scored[1], buckets, results


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--language")
    g.add_argument("--all", action="store_true")
    ap.add_argument("--category")
    ap.add_argument("--public", action="store_true", help="ignore the holdout suite")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--round", default=None)
    args = ap.parse_args()

    round_cfg = load_round(args.round or default_round())
    round_id = round_cfg["id"]
    tp = task_paths(round_cfg)
    adapters = load_adapters(tp["adapters"])
    fixtures = load_fixtures(tp, args.category, include_private=not args.public)
    if not fixtures:
        print("no fixtures found", file=sys.stderr)
        sys.exit(1)

    spec_version = round_cfg.get("spec_version", 1)
    names = list(adapters) if args.all else [args.language]
    summary, ok_all = {}, True
    for n in names:
        adapter = adapters[n]
        project = resolve_project(adapter, round_cfg)
        if not project_ready(adapter, project):
            print(f"\n== {n}: SKIP (not implemented yet) ==")
            summary[n] = {"skipped": True}
            continue
        print(f"\n== {n} ({project.relative_to(HERE.parent)}) — {len(fixtures)} fixtures ==")
        passed, total, buckets, results = run_language(
            adapter, fixtures, project, args.timeout, spec_version, tp["envelope"])
        summary[n] = {"passed": passed, "total": total, "buckets": buckets}
        ok_all &= passed == total
        outdir = RESULTS / round_id / n
        outdir.mkdir(parents=True, exist_ok=True)
        (outdir / "test-results.json").write_text(json.dumps({
            "language": n, "passed": passed, "total": total, "spec_version": spec_version,
            "buckets": buckets, "results": results}, indent=2))
        fut = buckets["future"]
        fut_note = f"; future-spec {fut[0]}/{fut[1]} not scored" if fut[1] else ""
        print(f"  -> {passed}/{total} scored (public {buckets['public'][0]}/{buckets['public'][1]}, "
              f"holdout {buckets['holdout'][0]}/{buckets['holdout'][1]}{fut_note})")

    print("\n== summary — task %s, spec v%s ==" % (tp["id"], spec_version))
    for n, s in summary.items():
        if s.get("skipped"):
            print(f"  {n:12s} not implemented")
        else:
            pct = round(100 * s["passed"] / s["total"], 1) if s["total"] else 0
            fut = s["buckets"]["future"]
            fut_note = f"   (future-spec {fut[0]}/{fut[1]})" if fut[1] else ""
            print(f"  {n:12s} {s['passed']:3d}/{s['total']:<3d}  {pct}%{fut_note}")
    ran = [s for s in summary.values() if not s.get("skipped")]
    sys.exit(0 if ok_all and ran else 1)


if __name__ == "__main__":
    main()
