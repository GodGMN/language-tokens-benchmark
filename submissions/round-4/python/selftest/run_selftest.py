#!/usr/bin/env python3
"""Run the bundled self-tests against ../workflow.py.

The comparison deliberately ignores the free-form error message, mirroring the
external contract (only error.code and error.step are specified).
"""

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CASES = HERE / "cases"
EXPECTED = HERE / "expected"


def compare(name, actual, expected, exit_code):
    problems = []

    if exit_code != expected["exit_code"]:
        problems.append("exit code %r != %r" % (exit_code, expected["exit_code"]))

    for key in ("status", "results", "execution_order", "skipped"):
        if actual.get(key) != expected.get(key):
            problems.append("%s mismatch:\n  actual:   %r\n  expected: %r"
                            % (key, actual.get(key), expected.get(key)))

    exp_error = expected.get("error")
    act_error = actual.get("error")
    if exp_error is None:
        if act_error is not None:
            problems.append("expected no error, got %r" % (act_error,))
    else:
        if not isinstance(act_error, dict):
            problems.append("expected an error object, got %r" % (act_error,))
        else:
            if act_error.get("code") != exp_error.get("code"):
                problems.append("error.code %r != %r"
                                % (act_error.get("code"), exp_error.get("code")))
            if act_error.get("step") != exp_error.get("step"):
                problems.append("error.step %r != %r"
                                % (act_error.get("step"), exp_error.get("step")))

    return problems


def main():
    failures = 0
    case_files = sorted(CASES.glob("*.json"))
    if not case_files:
        print("No self-test cases found in %s" % CASES)
        return 1

    for case_file in case_files:
        name = case_file.stem
        expected_file = EXPECTED / (name + ".json")
        if not expected_file.exists():
            print("FAIL %-22s missing expected file" % name)
            failures += 1
            continue

        expected = json.loads(expected_file.read_text(encoding="utf-8"))
        proc = subprocess.run(
            [sys.executable, str(ROOT / "workflow.py"), "run", str(case_file)],
            cwd=str(ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            actual = json.loads(proc.stdout)
        except Exception as exc:
            print("FAIL %-22s stdout is not one JSON document: %s" % (name, exc))
            print("  stdout: %r" % proc.stdout)
            failures += 1
            continue

        problems = compare(name, actual, expected, proc.returncode)
        if problems:
            print("FAIL %-22s" % name)
            for problem in problems:
                print("  " + problem.replace("\n", "\n  "))
            failures += 1
        else:
            print("PASS %-22s" % name)

    total = len(case_files)
    print("\n%d/%d self-tests passed" % (total - failures, total))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
