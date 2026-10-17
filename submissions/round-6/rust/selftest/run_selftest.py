#!/usr/bin/env python3
"""Run the hand-written workflow fixtures and compare against expected results.

Usage (from the project root):
    python3 selftest/run_selftest.py
    python3 selftest/run_selftest.py --stdin
"""
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "target", "release", "workflow")
SELFTEST = os.path.join(ROOT, "selftest")


def compare(name, expected, actual, exit_code):
    problems = []
    if exit_code != expected["exit_code"]:
        problems.append(f"exit code {exit_code} != {expected['exit_code']}")
    for key in ("status", "results", "execution_order", "skipped"):
        if actual.get(key) != expected.get(key):
            problems.append(f"{key}: {actual.get(key)!r} != {expected.get(key)!r}")
    exp_err = expected.get("error")
    act_err = actual.get("error")
    if exp_err is None:
        if act_err is not None:
            problems.append(f"error: {act_err!r} != None")
    else:
        if act_err is None:
            problems.append(f"error: None != {exp_err!r}")
        else:
            if act_err.get("code") != exp_err.get("code"):
                problems.append(f"error.code: {act_err.get('code')!r} != {exp_err.get('code')!r}")
            if act_err.get("step") != exp_err.get("step"):
                problems.append(f"error.step: {act_err.get('step')!r} != {exp_err.get('step')!r}")
    if problems:
        print(f"FAIL {name}")
        for problem in problems:
            print(f"     {problem}")
        return False
    print(f"ok   {name}")
    return True


def main():
    if not os.path.exists(BINARY):
        print(f"missing binary: {BINARY}")
        print("build it with: cargo build --release")
        return 2

    use_stdin = "--stdin" in sys.argv[1:]
    fixtures = sorted(
        f for f in glob.glob(os.path.join(SELFTEST, "*.json"))
        if not f.endswith(".expected.json")
    )
    if not fixtures:
        print("no fixtures found")
        return 2

    failures = 0
    for path in fixtures:
        name = os.path.basename(path)[:-5]
        expected_path = os.path.join(SELFTEST, name + ".expected.json")
        with open(expected_path, "r", encoding="utf-8") as fh:
            expected = json.load(fh)
        if use_stdin:
            with open(path, "r", encoding="utf-8") as fh:
                payload = fh.read()
            cmd = [BINARY, "run", "-"]
            proc = subprocess.run(cmd, input=payload, capture_output=True, text=True)
        else:
            cmd = [BINARY, "run", path]
            proc = subprocess.run(cmd, capture_output=True, text=True)
        try:
            actual = json.loads(proc.stdout)
        except json.JSONDecodeError:
            print(f"FAIL {name}: stdout is not JSON: {proc.stdout!r}")
            failures += 1
            continue
        if not compare(name, expected, actual, proc.returncode):
            failures += 1
            if proc.stderr.strip():
                print(f"     stderr: {proc.stderr.strip()}")

    print()
    total = len(fixtures)
    print(f"{total - failures}/{total} self-tests passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
