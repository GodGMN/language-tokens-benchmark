#!/usr/bin/env python3
"""Self-test runner for the JavaScript workflow engine.

Runs every selftest/*.json (except *.expected.json) through the CLI and
compares the parsed response against the sibling *.expected.json file.
`error.message` is intentionally not compared (the spec says it is free-form).
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORKFLOW = os.path.join(ROOT, "workflow.js")
VALIDATION_CODES = {"INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY"}


def normalize(resp):
    out = {
        "status": resp.get("status"),
        "results": resp.get("results"),
        "execution_order": resp.get("execution_order"),
        "skipped": resp.get("skipped"),
        "error": None,
    }
    err = resp.get("error")
    if err is not None:
        out["error"] = {"code": err.get("code"), "step": err.get("step")}
    return out


def expected_exit(expected):
    if expected["status"] == "completed":
        return 0
    if expected.get("error") and expected["error"].get("code") in VALIDATION_CODES:
        return 2
    return 1


def main():
    failures = 0
    cases = sorted(
        name[:-5]
        for name in os.listdir(HERE)
        if name.endswith(".json") and not name.endswith(".expected.json")
    )
    for case in cases:
        input_path = os.path.join(HERE, case + ".json")
        expected_path = os.path.join(HERE, case + ".expected.json")
        with open(expected_path, "r", encoding="utf-8") as fh:
            expected = json.load(fh)

        proc = subprocess.run(
            ["node", WORKFLOW, "run", input_path],
            capture_output=True,
            text=True,
            cwd=ROOT,
        )

        problems = []
        try:
            actual = json.loads(proc.stdout)
        except Exception as exc:  # noqa: BLE001
            actual = None
            problems.append("stdout is not one JSON document: %s (%r)" % (exc, proc.stdout))

        want_exit = expected_exit(expected)
        if proc.returncode != want_exit:
            problems.append("exit %d, expected %d" % (proc.returncode, want_exit))

        if actual is not None:
            got = normalize(actual)
            want = normalize(expected)
            if got != want:
                problems.append("response mismatch\n  got:  %s\n  want: %s" % (
                    json.dumps(got, ensure_ascii=False, sort_keys=True),
                    json.dumps(want, ensure_ascii=False, sort_keys=True),
                ))

        if problems:
            failures += 1
            print("FAIL %s" % case)
            for p in problems:
                print("     " + p)
        else:
            print("ok   %s" % case)

    print("\n%d/%d self-test cases passed" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
