#!/usr/bin/env python3
"""Self-test runner for the Rust workflow CLI.

Runs every JSON document in selftest/cases through the release binary and
compares the parsed stdout with the matching document in selftest/expected.
The free-form error.message field is ignored (SPEC says it is not checked).
"""
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
BIN = ROOT / "target" / "release" / "workflow"
CASES = HERE / "cases"
EXPECTED = HERE / "expected"

VALIDATION_CODES = {"INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY"}


def strip_messages(value):
    if isinstance(value, dict):
        return {k: strip_messages(v) for k, v in value.items() if k != "message"}
    if isinstance(value, list):
        return [strip_messages(v) for v in value]
    return value


def expected_exit(expected):
    if expected.get("status") == "completed":
        return 0
    code = (expected.get("error") or {}).get("code")
    return 2 if code in VALIDATION_CODES else 1


def run(args):
    proc = subprocess.run(
        [str(BIN), "run", *args],
        input=None,
        capture_output=True,
        text=True,
    )
    return proc


def main():
    if not BIN.exists():
        print(f"missing binary: {BIN}\nrun: cargo build --release", file=sys.stderr)
        return 1

    failures = 0
    cases = sorted(CASES.glob("*.json"))
    for case in cases:
        expected = json.loads((EXPECTED / case.name).read_text())
        proc = run([str(case)])
        label = case.name
        try:
            actual = json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            print(f"FAIL {label}: stdout is not one JSON document ({exc})")
            print(proc.stdout)
            failures += 1
            continue

        problems = []
        if proc.returncode != expected_exit(expected):
            problems.append(
                f"exit {proc.returncode}, expected {expected_exit(expected)}"
            )
        if strip_messages(actual) != strip_messages(expected):
            problems.append("response differs")
        if problems:
            failures += 1
            print(f"FAIL {label}: {', '.join(problems)}")
            print("  expected:", json.dumps(expected, sort_keys=True))
            print("  actual:  ", json.dumps(actual, sort_keys=True))
        else:
            print(f"ok   {label}")

    # Exercise the stdin form too.
    if cases:
        text = cases[0].read_text()
        proc = subprocess.run(
            [str(BIN), "run", "-"], input=text, capture_output=True, text=True
        )
        expected = json.loads((EXPECTED / cases[0].name).read_text())
        if proc.returncode == expected_exit(expected) and strip_messages(
            json.loads(proc.stdout)
        ) == strip_messages(expected):
            print("ok   run - (stdin)")
        else:
            failures += 1
            print("FAIL run - (stdin)")

    if failures:
        print(f"\n{failures} self-test(s) failed")
        return 1
    print(f"\nall {len(cases)} self-tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
