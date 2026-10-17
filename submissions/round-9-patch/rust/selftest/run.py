#!/usr/bin/env python3
"""Run the JSON Patch self-test suite against target/release/patch."""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BINARY = os.path.join(ROOT, "target", "release", "patch")
CASES = os.path.join(HERE, "cases.json")


def expected_exit(expected):
    if expected["status"] == "ok":
        return 0
    if expected["code"] == "INVALID_REQUEST":
        return 2
    return 1


def normalized(response):
    """Messages are free-form, so they are not compared."""
    if response.get("status") == "ok":
        return {"status": "ok", "document": response.get("document")}
    return {
        "status": "error",
        "code": response.get("code"),
        "index": response.get("index"),
    }


def run_case(case, use_file):
    request_text = json.dumps(case["request"])
    if use_file:
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(request_text)
        try:
            completed = subprocess.run(
                [BINARY, "apply", path],
                capture_output=True,
                text=True,
            )
        finally:
            os.unlink(path)
    else:
        completed = subprocess.run(
            [BINARY, "apply", "-"],
            input=request_text,
            capture_output=True,
            text=True,
        )

    expected = case["expected"]
    wanted_exit = expected_exit(expected)
    problems = []
    if completed.returncode != wanted_exit:
        problems.append(f"exit {completed.returncode}, wanted {wanted_exit}")
    if completed.stderr:
        problems.append(f"stderr was not empty: {completed.stderr!r}")
    try:
        actual = json.loads(completed.stdout)
    except Exception as error:  # noqa: BLE001 - report parse failure verbosely
        problems.append(f"stdout is not one JSON document ({error}): {completed.stdout!r}")
        return problems
    if normalized(actual) != expected:
        problems.append(f"response {normalized(actual)!r}, wanted {expected!r}")
    return problems


def main():
    if not os.path.exists(BINARY):
        print(f"missing {BINARY}; run `cargo build --release` first", file=sys.stderr)
        return 1

    with open(CASES, encoding="utf-8") as handle:
        cases = json.load(handle)

    failures = 0
    for i, case in enumerate(cases):
        for use_file in (False, True):
            problems = run_case(case, use_file)
            if problems:
                failures += 1
                mode = "file" if use_file else "stdin"
                print(f"FAIL [{i}] {case['name']} ({mode})", file=sys.stderr)
                for problem in problems:
                    print(f"  - {problem}", file=sys.stderr)
                break

    if failures:
        print(f"{failures} of {len(cases)} cases failed", file=sys.stderr)
        return 1
    print(f"all {len(cases)} cases passed (stdin and file modes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
