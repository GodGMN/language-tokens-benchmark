#!/usr/bin/env python3
"""Run the conformance suite (fixtures) against one or all implementations.

Usage:
  python3 benchmark/runner.py --language go
  python3 benchmark/runner.py --all
  python3 benchmark/runner.py --language go --category references
  python3 benchmark/runner.py --all --timeout 10

Fixtures: benchmark/fixtures/<category>/*.json
Each fixture: {name, input, expected_exit_code, expected_output?, expected_error_code?}
Results: results/<round>/<language>/test-results.json
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
ADAPTERS = HERE / "adapters"
FIXTURES = HERE / "fixtures"
RESULTS = ROOT / "results"
ROUND = "round-1"


def load_adapters():
    return {p.stem: json.loads(p.read_text()) for p in sorted(ADAPTERS.glob("*.json"))}


def project_ready(adapter):
    proj = ROOT / adapter["project_dir"]
    return all((proj / r).exists() for r in adapter.get("requires", []))


def load_fixtures(category=None, include_private=True):
    """Load fixtures from fixtures/ (public) and, if present, fixtures-private/.

    The private set is gitignored and is meant to hold the evaluation suite
    that implementing agents must not see during their session.
    """
    roots = [FIXTURES]
    private = HERE / "fixtures-private"
    if include_private and private.is_dir():
        roots.append(private)

    out = []
    for root in roots:
        for f in sorted(root.glob("*/*.json")):
            if category and f.parent.name != category:
                continue
            d = json.loads(f.read_text())
            d["_file"] = str(f.relative_to(HERE))
            out.append(d)
    return out


def run_fixture(adapter, fixture, timeout):
    import tempfile

    lang = adapter["language"]
    proj = ROOT / adapter["project_dir"]
    with tempfile.TemporaryDirectory(prefix=f"wf-{lang}-") as td:
        inp = Path(td) / "workflow.json"
        inp.write_text(json.dumps(fixture["input"]))
        cmd = [c.replace("{input}", str(inp)) for c in adapter["run"]]
        t0 = time.monotonic()
        try:
            p = subprocess.run(
                cmd, cwd=proj, input=None,
                capture_output=True, text=True, timeout=timeout,
            )
            elapsed = time.monotonic() - t0
            stdout = p.stdout.strip()
            try:
                parsed = json.loads(stdout)
                valid_json = True
            except Exception:
                parsed, valid_json = None, False
            return {
                "exit_code": p.returncode,
                "stdout_json": parsed,
                "stdout_valid_json": valid_json,
                "stderr_tail": p.stderr[-2000:],
                "seconds": round(elapsed, 3),
                "timeout": False,
            }
        except subprocess.TimeoutExpired:
            return {"exit_code": None, "stdout_json": None, "stdout_valid_json": False,
                    "stderr_tail": "TIMEOUT", "seconds": timeout, "timeout": True}
        except FileNotFoundError as e:
            return {"exit_code": None, "stdout_json": None, "stdout_valid_json": False,
                    "stderr_tail": f"MISSING EXECUTABLE: {e}", "seconds": 0, "timeout": False}


RESPONSE_FIELDS = ("status", "results", "execution_order", "skipped", "error")


def shape_errors(resp):
    """Validate the response envelope required by SPEC §5.3/5.4."""
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


def comparable(resp):
    """Strip the free-form error.message before comparing (SPEC §5.4)."""
    if not isinstance(resp, dict):
        return resp
    resp = dict(resp)
    if isinstance(resp.get("error"), dict):
        resp["error"] = {k: v for k, v in resp["error"].items() if k != "message"}
    return resp


def check(fixture, result):
    reasons = []
    exp_exit = fixture.get("expected_exit_code")
    if result["exit_code"] != exp_exit:
        reasons.append(f"exit {result['exit_code']} != expected {exp_exit}")

    if not result["stdout_valid_json"]:
        reasons.append("stdout is not valid JSON")
        return reasons

    actual = result["stdout_json"]
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


def run_language(adapter, fixtures, timeout, verbose=True):
    results = []
    passed = 0
    for fx in fixtures:
        r = run_fixture(adapter, fx, timeout)
        reasons = check(fx, r)
        ok = not reasons
        passed += ok
        if verbose:
            mark = "PASS" if ok else "FAIL"
            print(f"  [{mark}] {fx['_file']}  (exit={r['exit_code']}, {r['seconds']}s)")
            for why in reasons[:6]:
                if verbose and not ok:
                    print(f"         {why}")
        results.append({"name": fx["name"], "file": fx["_file"], "passed": ok,
                        "reasons": reasons, "exit_code": r["exit_code"],
                        "seconds": r["seconds"], "detail": r})
    return passed, len(fixtures), results


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--language")
    g.add_argument("--all", action="store_true")
    ap.add_argument("--category")
    ap.add_argument("--public", action="store_true",
                    help="ignore fixtures-private/ (public suite only)")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--round", default=ROUND)
    args = ap.parse_args()

    adapters = load_adapters()
    fixtures = load_fixtures(args.category, include_private=not args.public)
    if not fixtures:
        print(f"no fixtures found{f' for category {args.category}' if args.category else ''}", file=sys.stderr)
        sys.exit(1)

    names = list(adapters) if args.all else [args.language]
    summary = {}
    all_ok = True
    for n in names:
        adapter = adapters[n]
        if not project_ready(adapter):
            print(f"\n== {n}: SKIP (not implemented yet) ==")
            summary[n] = {"passed": 0, "total": 0, "pct": 0.0, "skipped": True}
            continue
        print(f"\n== {n} ({adapter['project_dir']}) — {len(fixtures)} fixtures ==")
        passed, total, results = run_language(adapter, fixtures, args.timeout)
        summary[n] = {"passed": passed, "total": total, "pct": round(100 * passed / total, 2)}
        all_ok &= passed == total
        outdir = RESULTS / args.round / n
        outdir.mkdir(parents=True, exist_ok=True)
        (outdir / "test-results.json").write_text(json.dumps(
            {"language": n, "passed": passed, "total": total, "results": results}, indent=2))
        print(f"  -> {passed}/{total} passed  [{outdir / 'test-results.json'}]")

    print("\n== summary ==")
    for n, s in summary.items():
        if s.get("skipped"):
            print(f"  {n:12s} not implemented")
        else:
            print(f"  {n:12s} {s['passed']:3d}/{s['total']:<3d}  {s['pct']}%")
    ran = [s for s in summary.values() if not s.get("skipped")]
    sys.exit(0 if all_ok and ran else 1)


if __name__ == "__main__":
    main()
