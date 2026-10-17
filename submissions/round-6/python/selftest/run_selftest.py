#!/usr/bin/env python3
"""Self-test runner for the workflow CLI.

Runs every file in selftest/cases through the CLI (both via a file path and,
for one case, via stdin) and compares the JSON response and the exit code
against the hand-written expectations in selftest/expected.

Usage:
    python3 selftest/run_selftest.py
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLI = os.path.join(ROOT, "workflow.py")
CASES = os.path.join(HERE, "cases")
EXPECTED = os.path.join(HERE, "expected")


def strip_message(value):
    """Drop error.message, which the SPEC says is never compared."""
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            if k == "message":
                continue
            out[k] = strip_message(v)
        return out
    if isinstance(value, list):
        return [strip_message(v) for v in value]
    return value


def run_cli(args, stdin_data=None):
    proc = subprocess.run(
        [sys.executable, CLI] + args,
        input=stdin_data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    raw = proc.stdout.decode("utf-8")
    try:
        parsed = json.loads(raw)
    except Exception:
        parsed = {"__unparsable_stdout__": raw}
    return proc.returncode, parsed, proc.stderr.decode("utf-8")


def main():
    names = sorted(
        n for n in os.listdir(CASES) if n.endswith(".json")
    )
    failures = 0
    for name in names:
        case_path = os.path.join(CASES, name)
        exp_path = os.path.join(EXPECTED, name)
        with open(exp_path, "r", encoding="utf-8") as fh:
            expected = json.load(fh)

        code, output, stderr = run_cli(["run", case_path])
        got = strip_message(output)
        want = strip_message(expected["output"])

        ok = True
        if code != expected["exit"]:
            ok = False
            print("FAIL %-32s exit %r != %r" % (name, code, expected["exit"]))
        if got != want:
            ok = False
            print("FAIL %-32s output mismatch" % name)
            print("  expected: %s" % json.dumps(want, sort_keys=True, ensure_ascii=False))
            print("  actual:   %s" % json.dumps(got, sort_keys=True, ensure_ascii=False))
        if ok:
            print("ok   %s" % name)
        else:
            failures += 1
            if stderr:
                print("  stderr: %s" % stderr.strip())

    # stdin mode check with the first case
    if names:
        first = os.path.join(CASES, names[0])
        with open(first, "rb") as fh:
            data = fh.read()
        code, output, _ = run_cli(["run", "-"], stdin_data=data)
        with open(os.path.join(EXPECTED, names[0]), "r", encoding="utf-8") as fh:
            expected = json.load(fh)
        if code == expected["exit"] and strip_message(output) == strip_message(
            expected["output"]
        ):
            print("ok   stdin mode (%s)" % names[0])
        else:
            failures += 1
            print("FAIL stdin mode (%s)" % names[0])

    # malformed CLI usage must fail with exit 2 and a JSON response
    code, output, _ = run_cli([])
    if code == 2 and output.get("status") == "failed":
        print("ok   invalid CLI usage")
    else:
        failures += 1
        print("FAIL invalid CLI usage: exit=%r output=%r" % (code, output))

    total = len(names) + 2
    print("\n%d/%d self-tests passed" % (total - failures, total))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
