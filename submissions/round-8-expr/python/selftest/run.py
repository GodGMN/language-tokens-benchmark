#!/usr/bin/env python3
"""Self-test runner for expr.py.

Each case is run through the real CLI (stdin or a temporary request file) and
its JSON response, exit code and error position are checked.  The `message`
field is free-form, so it is not compared.
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXPR = os.path.join(ROOT, "expr.py")
CASES = os.path.join(HERE, "cases.json")


def run_cli(payload, use_file):
    if use_file:
        fd, path = tempfile.mkstemp(suffix=".json")
        try:
            with os.fdopen(fd, "wb") as handle:
                handle.write(payload)
            command = [sys.executable, EXPR, "eval", path]
            result = subprocess.run(
                command, cwd=ROOT, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        finally:
            os.unlink(path)
    else:
        command = [sys.executable, EXPR, "eval", "-"]
        result = subprocess.run(
            command, cwd=ROOT, input=payload,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    return result


def compare(case, result):
    problems = []
    if result.returncode != case["exit"]:
        problems.append(
            "exit %r != %r" % (result.returncode, case["exit"])
        )

    try:
        actual = json.loads(result.stdout.decode("utf-8"))
    except Exception as exc:
        problems.append("stdout is not valid UTF-8 JSON: %s (%r)" % (
            exc, result.stdout[:200]))
        return problems

    expected = case["expected"]
    if actual.get("status") != expected["status"]:
        problems.append("status %r != %r" % (
            actual.get("status"), expected["status"]))
        return problems

    if expected["status"] == "ok":
        if actual.get("value") != expected["value"]:
            problems.append("value %r != %r" % (
                actual.get("value"), expected["value"]))
        if set(actual) != {"status", "value"}:
            problems.append("unexpected response fields: %r" % sorted(actual))
    else:
        if actual.get("code") != expected["code"]:
            problems.append("code %r != %r" % (
                actual.get("code"), expected["code"]))
        if actual.get("position") != expected["position"]:
            problems.append("position %r != %r" % (
                actual.get("position"), expected["position"]))
        if "message" not in actual or not isinstance(actual["message"], str):
            problems.append("missing string message")
        if set(actual) != {"status", "code", "position", "message"}:
            problems.append("unexpected response fields: %r" % sorted(actual))

    return problems


def main():
    with open(CASES, encoding="utf-8") as handle:
        cases = json.load(handle)["cases"]

    failures = 0
    for index, case in enumerate(cases):
        if "raw" in case:
            payload = case["raw"].encode("utf-8")
        else:
            payload = json.dumps(
                case["request"], ensure_ascii=False, separators=(",", ":")
            ).encode("utf-8")

        # Exercise both CLI input modes; results must be identical.
        use_file = index % 7 == 0
        result = run_cli(payload, use_file)
        problems = compare(case, result)

        if problems:
            failures += 1
            print("FAIL %s (%s)" % (case["name"], "file" if use_file else "stdin"))
            for problem in problems:
                print("     " + problem)
            print("     stdout: %r" % result.stdout.decode("utf-8", "replace"))
            print("     stderr: %r" % result.stderr.decode("utf-8", "replace"))
        elif "-v" in sys.argv:
            print("ok   %s: %s" % (case["name"], result.stdout.decode().strip()))

    print("%d/%d self-tests passed" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
