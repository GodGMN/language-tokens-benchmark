#!/usr/bin/env python3
"""Self-test driver for the workflow CLI.

Runs every selftest/NN_*.json workflow through `workflow.py run -` and compares
the parsed response with selftest/expected/NN_*.json. Exit code 0 means all
comparisons matched.
"""
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLI = os.path.join(ROOT, "workflow.py")


def main():
    inputs = sorted(glob.glob(os.path.join(HERE, "[0-9][0-9]_*.json")))
    failures = 0
    for path in inputs:
        name = os.path.basename(path)[:-5]
        expected_path = os.path.join(HERE, "expected", name + ".json")
        with open(path, "r", encoding="utf-8") as fh:
            doc = fh.read()
        proc = subprocess.run(
            [sys.executable, CLI, "run", "-"],
            input=doc.encode("utf-8"),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            got = json.loads(proc.stdout.decode("utf-8"))
        except ValueError:
            print("FAIL %s: stdout is not valid JSON: %r" % (name, proc.stdout))
            failures += 1
            continue
        with open(expected_path, "r", encoding="utf-8") as fh:
            want = json.load(fh)
        if got != want:
            print("FAIL %s" % name)
            print("  expected: %s" % json.dumps(want, sort_keys=True))
            print("  got:      %s" % json.dumps(got, sort_keys=True))
            failures += 1
        else:
            print("ok   %s (exit %d)" % (name, proc.returncode))
    if failures:
        print("%d self-test(s) failed" % failures)
        return 1
    print("all %d self-tests passed" % len(inputs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
