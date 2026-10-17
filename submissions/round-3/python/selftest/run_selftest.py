#!/usr/bin/env python3
"""Self-test harness for the workflow CLI.

Runs every fixture under selftest/cases/ through `workflow.py run <file>`
and compares the response (ignoring free-form error messages) and exit code.
Also exercises the `run -` stdin path.

Run from the project root:
    python3 selftest/run_selftest.py
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CASES = os.path.join(HERE, "cases")


def completed(results, order, skipped=None):
    return {
        "status": "completed",
        "results": results,
        "execution_order": order,
        "skipped": skipped or [],
        "error": None,
    }


def failed(code, step, results=None, order=None, skipped=None):
    return {
        "status": "failed",
        "results": results or {},
        "execution_order": order or [],
        "skipped": skipped or [],
        "error": {"code": code, "step": step},
    }


# (fixture, expected exit code, expected response)
EXPECTED = [
    (
        "01_math_pipeline.json",
        0,
        completed(
            {
                "r": [1, 2, 3, 4, 5],
                "add": 5,
                "sub": 9,
                "mul": -12,
                "div": -3,
                "mod": -1,
                "total": 15,
                "lo": 1,
                "hi": 5,
                "red": 15,
                "redsub": 4,
                "redmul": 6,
                "empty_sum": 0,
                "rev_range": [],
            },
            ["r", "add", "sub", "mul", "div", "mod", "total", "lo", "hi",
             "red", "redsub", "redmul", "empty_sum", "rev_range"],
        ),
    ),
    (
        "02_strings.json",
        0,
        completed(
            {
                "s": "  Hello, World!  ",
                "up": "  HELLO, WORLD!  ",
                "low": "  hello, world!  ",
                "tr": "Hello, World!",
                "sp": ["a", "b", "", "c"],
                "sp0": ["h", "é", "l", "l", "o"],
                "jn": "a-b-c",
                "jn0": "",
                "rp": "baNANA",
                "ln": 5,
                "cc": "foobar",
            },
            ["s", "up", "low", "tr", "sp", "sp0", "jn", "jn0", "rp", "ln",
             "cc"],
        ),
    ),
    (
        "03_arrays.json",
        0,
        completed(
            {
                "nums": [3, 1, 2, 1, 3],
                "ordered": [1, 1, 2, 3, 3],
                "rev": [3, 1, 2, 1, 3],
                "uniq": [3, 1, 2],
                "mapped": [13, 11, 12, 11, 13],
                "mapop": [6, 2, 4, 2, 6],
                "evens": [2],
                "big": [3, 2, 3],
                "five": [0, 1, 2, 3, 4],
                "sliced": [1, 2, 3],
                "neg": [3, 4],
                "flat": [1, 2, 3, 4],
                "mixed": [1, "a", "b", False, True, None, [1], {"a": 1}],
                "types": ["number", "string", "null", "boolean"],
                "stringified": ["1", '"a"', "true", "null"],
            },
            ["nums", "ordered", "rev", "uniq", "mapped", "mapop", "evens",
             "big", "five", "sliced", "neg", "flat", "mixed", "types",
             "stringified"],
        ),
    ),
    (
        "04_objects.json",
        0,
        completed(
            {
                "obj": {"b": 2, "a": 1, "c": 3},
                "ks": ["a", "b", "c"],
                "vs": [1, 2, 3],
                "got": 1,
                "gotmiss": None,
                "st": {"b": 2, "a": 1, "c": 3, "d": 4},
                "mrg": {"a": 1, "b": 20, "c": 30},
                "pk": {"a": 1, "c": 3, "missing": None},
                "om": {"a": 1, "c": 3},
            },
            ["obj", "ks", "vs", "got", "gotmiss", "st", "mrg", "pk", "om"],
        ),
    ),
    (
        "05_utilities.json",
        0,
        completed(
            {
                "t1": "number",
                "t2": "boolean",
                "t3": "null",
                "eq1": True,
                "eq2": True,
                "eq3": False,
                "cmp1": True,
                "cmp2": True,
                "a": False,
                "o": False,
                "n": True,
                "c": {"nested": [1, {"x": None}]},
            },
            ["t1", "t2", "t3", "eq1", "eq2", "eq3", "cmp1", "cmp2", "a", "o",
             "n", "c"],
        ),
    ),
    (
        "06_conditions.json",
        0,
        completed(
            {"x": 15, "y": 1, "z": None, "w": 3, "v": None, "u": 5},
            ["x", "y", "z", "w", "v", "u"],
            ["z", "v"],
        ),
    ),
    (
        "07_retry_success.json",
        0,
        completed({"flaky": "ok"}, ["flaky"]),
    ),
    (
        "08_retry_exhausted.json",
        1,
        failed("STEP_FAILED", "flaky", {"first": 1}, ["first"]),
    ),
    (
        "09_nested_refs.json",
        0,
        completed(
            {
                "n": {"user": {"name": "Ada", "tags": ["x", "y"]},
                      "items": [5, 6]},
                "nm": "Ada",
                "it": 11,
                "eq": True,
                "lit": {"$ref": "n", "other": "Ada"},
                "mid": 1,
                "trans": 21,
            },
            ["n", "nm", "it", "eq", "lit", "mid", "trans"],
        ),
    ),
    (
        "10_dag_order.json",
        0,
        completed({"final": 42, "base": 41, "other": "z"},
                  ["base", "final", "other"]),
    ),
    (
        "11_errors.json",
        1,
        failed("INVALID_REFERENCE", "bad_path", {"base": {"a": 1}}, ["base"]),
    ),
    (
        "12_skipped_ref.json",
        1,
        failed("SKIPPED_REFERENCE", "user", {"base": 1, "sk": None},
               ["base", "sk"], ["sk"]),
    ),
    (
        "13_invalid_arg.json",
        1,
        failed("INVALID_ARG", "a"),
    ),
]


# Inline stdin checks for error-code coverage.
EXTRA = [
    (
        "overflow",
        {"version": 1, "steps": [
            {"id": "a", "op": "add",
             "args": {"a": 9223372036854775807, "b": 1}}]},
        1,
        failed("ARITHMETIC_OVERFLOW", "a"),
    ),
    (
        "division by zero",
        {"version": 1, "steps": [
            {"id": "a", "op": "divide", "args": {"a": 1, "b": 0}}]},
        1,
        failed("DIVISION_BY_ZERO", "a"),
    ),
    (
        "empty array",
        {"version": 1, "steps": [
            {"id": "a", "op": "min", "args": {"input": []}}]},
        1,
        failed("EMPTY_ARRAY", "a"),
    ),
    (
        "non-transient error is not retried",
        {"version": 1, "steps": [
            {"id": "a", "op": "mod", "args": {"a": 1, "b": 0},
             "retry": {"max_attempts": 5}}]},
        1,
        failed("DIVISION_BY_ZERO", "a"),
    ),
    (
        "cycle",
        {"version": 1, "steps": [
            {"id": "a", "op": "constant", "args": {"value": 1},
             "depends_on": ["b"]},
            {"id": "b", "op": "constant", "args": {"value": 2},
             "depends_on": ["a"]}]},
        2,
        failed("CYCLIC_DEPENDENCY", None),
    ),
    (
        "unknown dependency",
        {"version": 1, "steps": [
            {"id": "a", "op": "constant", "args": {"value": 1},
             "depends_on": ["zzz"]}]},
        2,
        failed("UNKNOWN_DEPENDENCY", "a"),
    ),
    (
        "unknown top-level field",
        {"version": 1, "steps": [], "extra": True},
        2,
        failed("INVALID_WORKFLOW", None),
    ),
    (
        "float version",
        {"version": 1.0, "steps": []},
        2,
        failed("INVALID_WORKFLOW", None),
    ),
    (
        "bad condition shape",
        {"version": 1, "steps": [
            {"id": "a", "op": "constant", "args": {"value": 1},
             "when": {"op": "bogus", "left": 1, "right": 2}}]},
        2,
        failed("INVALID_WORKFLOW", "a"),
    ),
]


def normalize(response):
    error = response.get("error")
    if error is None:
        return response
    return {
        "status": response.get("status"),
        "results": response.get("results"),
        "execution_order": response.get("execution_order"),
        "skipped": response.get("skipped"),
        "error": {"code": error.get("code"), "step": error.get("step")},
    }


def run(argv, stdin_data=None):
    proc = subprocess.run(
        [sys.executable, os.path.join(ROOT, "workflow.py")] + argv,
        cwd=ROOT,
        input=stdin_data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return proc


def main():
    failures = 0
    for name, want_exit, want_response in EXPECTED:
        path = os.path.join(CASES, name)
        proc = run(["run", path])
        try:
            got = json.loads(proc.stdout.decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            print("FAIL %s: invalid JSON on stdout (%s): %r" %
                  (name, exc, proc.stdout))
            failures += 1
            continue
        problems = []
        if proc.returncode != want_exit:
            problems.append("exit %r != %r" % (proc.returncode, want_exit))
        if normalize(got) != want_response:
            problems.append("response mismatch:\n  got:  %r\n  want: %r" %
                            (normalize(got), want_response))
        if problems:
            failures += 1
            print("FAIL %s" % name)
            for p in problems:
                print("  " + p)
        else:
            print("ok   %s" % name)

    # stdin path: feed the retry-success fixture through `run -`
    with open(os.path.join(CASES, "07_retry_success.json"), "rb") as fh:
        data = fh.read()
    proc = run(["run", "-"], stdin_data=data)
    try:
        got = normalize(json.loads(proc.stdout.decode("utf-8")))
    except Exception:  # noqa: BLE001
        got = None
    if proc.returncode == 0 and got == completed({"flaky": "ok"}, ["flaky"]):
        print("ok   stdin (run -)")
    else:
        failures += 1
        print("FAIL stdin (run -): exit=%r response=%r" %
              (proc.returncode, got))

    for name, workflow, want_exit, want_response in EXTRA:
        proc = run(["run", "-"],
                   stdin_data=json.dumps(workflow).encode("utf-8"))
        try:
            got = normalize(json.loads(proc.stdout.decode("utf-8")))
        except Exception:  # noqa: BLE001
            got = None
        if proc.returncode == want_exit and got == want_response:
            print("ok   stdin check: %s" % name)
        else:
            failures += 1
            print("FAIL stdin check: %s\n  exit=%r response=%r" %
                  (name, proc.returncode, got))

    total = len(EXPECTED) + 1 + len(EXTRA)
    print("\n%d/%d checks passed" % (total - failures, total))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
