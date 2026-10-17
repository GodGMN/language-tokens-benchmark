#!/usr/bin/env python3
"""Self-test runner for the Python workflow CLI.

Runs every workflow in this folder through the CLI and compares the parsed
JSON response (and process exit code) with the expected values below.

    python3 selftest/run_selftest.py
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORKFLOW = os.path.join(ROOT, "workflow.py")


EXPECTED = {
    "01_math_pipeline.json": (
        0,
        {
            "status": "completed",
            "results": {
                "nums": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
                "evens": [2, 4, 6, 8, 10],
                "total": 30,
                "doubled": 60,
                "final": 59,
            },
            "execution_order": ["nums", "evens", "total", "doubled", "final"],
            "skipped": [],
            "error": None,
        },
    ),
    "02_string_pipeline.json": (
        0,
        {
            "status": "completed",
            "results": {
                "raw": "  Hello,World  ",
                "trimmed": "Hello,World",
                "upper": "HELLO,WORLD",
                "parts": ["HELLO", "WORLD"],
                "joined": "HELLO-WORLD",
                "replaced": "HELL0-W0RLD",
                "size": 11,
                "lower": "hell0-w0rld",
            },
            "execution_order": [
                "raw", "trimmed", "upper", "parts", "joined", "replaced",
                "size", "lower",
            ],
            "skipped": [],
            "error": None,
        },
    ),
    "03_object_ops.json": (
        0,
        {
            "status": "completed",
            "results": {
                "base": {"name": "Ada", "age": 36, "tags": ["math", "code"]},
                "withCity": {
                    "name": "Ada",
                    "age": 36,
                    "tags": ["math", "code"],
                    "city": "London",
                },
                "merged": {
                    "name": "Ada",
                    "age": 37,
                    "tags": ["math", "code"],
                    "city": "London",
                    "lang": "py",
                },
                "keys": ["age", "city", "lang", "name", "tags"],
                "values": [37, "London", "py", "Ada", ["math", "code"]],
                "picked": {"missing": None, "name": "Ada"},
                "omitted": {
                    "name": "Ada",
                    "age": 37,
                    "city": "London",
                    "lang": "py",
                },
                "got": 37,
            },
            "execution_order": [
                "base", "withCity", "merged", "keys", "values", "picked",
                "omitted", "got",
            ],
            "skipped": [],
            "error": None,
        },
    ),
    "04_skipped_step.json": (
        0,
        {
            "status": "completed",
            "results": {"x": 3, "y": None, "z": "done"},
            "execution_order": ["x", "y", "z"],
            "skipped": ["y"],
            "error": None,
        },
    ),
    "05_retry_success.json": (
        0,
        {
            "status": "completed",
            "results": {"flaky": "ok"},
            "execution_order": ["flaky"],
            "skipped": [],
            "error": None,
        },
    ),
    "06_retry_exhausted.json": (
        1,
        {
            "status": "failed",
            "results": {},
            "execution_order": [],
            "skipped": [],
            "error": {"code": "STEP_FAILED", "step": "flaky"},
        },
    ),
    "07_array_ops.json": (
        0,
        {
            "status": "completed",
            "results": {
                "nums": [5, 3, 3, 1, 2],
                "sorted": [1, 2, 3, 3, 5],
                "uniq": [1, 2, 3, 5],
                "reversed": [5, 3, 2, 1],
                "sliced": [3, 2],
                "mapped": [2, 1],
                "mapped2": [-2, -1],
                "filtered": [5, 3, 3, 1],
                "reduced": 22,
                "flattened": [5, 3, 3, 1, 2, 9, [10]],
            },
            "execution_order": [
                "nums", "sorted", "uniq", "reversed", "sliced", "mapped",
                "mapped2", "filtered", "reduced", "flattened",
            ],
            "skipped": [],
            "error": None,
        },
    ),
    "08_references_nested.json": (
        0,
        {
            "status": "completed",
            "results": {
                "obj": {
                    "user": {"name": "Ada", "roles": ["admin", "dev"]},
                    "items": [10, 20, 30],
                },
                "user": {"name": "Ada", "roles": ["admin", "dev"]},
                "name": "Ada",
                "same": "Ada",
                "role": "dev",
                "final": "AdaAda",
                "sumItems": 60,
            },
            "execution_order": [
                "obj", "user", "name", "same", "role", "final", "sumItems",
            ],
            "skipped": [],
            "error": None,
        },
    ),
    "09_arithmetic_overflow.json": (
        1,
        {
            "status": "failed",
            "results": {"max": 9223372036854775807},
            "execution_order": ["max"],
            "skipped": [],
            "error": {"code": "ARITHMETIC_OVERFLOW", "step": "over"},
        },
    ),
    "10_conditions_compound.json": (
        0,
        {
            "status": "completed",
            "results": {"a": 5, "b": 10, "big": "yes", "small": None},
            "execution_order": ["a", "b", "big", "small"],
            "skipped": ["small"],
            "error": None,
        },
    ),
}


def normalise(value):
    """Drop free-form error messages before comparing."""
    if isinstance(value, dict):
        return {
            k: normalise(v)
            for k, v in value.items()
            if k != "message"
        }
    if isinstance(value, list):
        return [normalise(v) for v in value]
    return value


def invoke(args, stdin_data=None):
    return subprocess.run(
        [sys.executable, WORKFLOW] + args,
        cwd=ROOT,
        input=stdin_data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def one_check(label, args, stdin_data, expected_exit, expected_doc):
    proc = invoke(args, stdin_data)
    problems = []
    if proc.returncode != expected_exit:
        problems.append("exit %r != %r" % (proc.returncode, expected_exit))
    try:
        doc = json.loads(proc.stdout.decode("utf-8"))
    except Exception as exc:  # noqa: BLE001
        problems.append("stdout is not one JSON document: %s" % exc)
        doc = None
    if doc is not None:
        got = normalise(doc)
        exp = normalise(expected_doc)
        if got != exp:
            problems.append("response mismatch")
            problems.append("  got: " + json.dumps(got, sort_keys=True))
            problems.append("  exp: " + json.dumps(exp, sort_keys=True))
    if problems:
        print("FAIL %s" % label)
        for p in problems:
            print("  " + p)
        if proc.stderr:
            print("  stderr: " + proc.stderr.decode("utf-8", "replace").strip())
        return False
    print("PASS %s" % label)
    return True


# Document, expected exit code, expected error code, expected error.step.
ERROR_CASES = [
    (
        "INVALID_WORKFLOW (version)",
        {"steps": []},
        2,
        "INVALID_WORKFLOW",
        None,
    ),
    (
        "INVALID_WORKFLOW (step field)",
        {
            "version": 1,
            "steps": [
                {"id": "a", "op": "constant", "args": {"value": 1}, "x": 1}
            ],
        },
        2,
        "INVALID_WORKFLOW",
        "a",
    ),
    (
        "UNKNOWN_DEPENDENCY",
        {
            "version": 1,
            "steps": [
                {"id": "a", "op": "constant", "args": {"value": 1},
                 "depends_on": ["missing"]}
            ],
        },
        2,
        "UNKNOWN_DEPENDENCY",
        "a",
    ),
    (
        "CYCLIC_DEPENDENCY",
        {
            "version": 1,
            "steps": [
                {"id": "a", "op": "constant", "args": {"value": 1},
                 "depends_on": ["b"]},
                {"id": "b", "op": "constant", "args": {"value": 1},
                 "depends_on": ["a"]},
            ],
        },
        2,
        "CYCLIC_DEPENDENCY",
        None,
    ),
    (
        "INVALID_REFERENCE",
        {
            "version": 1,
            "steps": [
                {"id": "a", "op": "constant", "args": {"value": 1}},
                {"id": "b", "op": "constant",
                 "args": {"value": {"$ref": "a"}}},
            ],
        },
        1,
        "INVALID_REFERENCE",
        "b",
    ),
    (
        "SKIPPED_REFERENCE",
        {
            "version": 1,
            "steps": [
                {"id": "a", "op": "constant", "args": {"value": 1},
                 "when": {"op": "eq", "left": 1, "right": 2}},
                {"id": "b", "op": "constant",
                 "args": {"value": {"$ref": "a"}}, "depends_on": ["a"]},
            ],
        },
        1,
        "SKIPPED_REFERENCE",
        "b",
    ),
    (
        "INVALID_ARG",
        {
            "version": 1,
            "steps": [{"id": "a", "op": "add", "args": {"a": 1}}],
        },
        1,
        "INVALID_ARG",
        "a",
    ),
    (
        "INVALID_CONDITION",
        {
            "version": 1,
            "steps": [
                {"id": "a", "op": "constant", "args": {"value": 5}},
                {"id": "b", "op": "constant", "args": {"value": 1},
                 "depends_on": ["a"], "when": {"$ref": "a"}},
            ],
        },
        1,
        "INVALID_CONDITION",
        "b",
    ),
    (
        "DIVISION_BY_ZERO",
        {
            "version": 1,
            "steps": [{"id": "a", "op": "divide",
                        "args": {"a": 1, "b": 0}}],
        },
        1,
        "DIVISION_BY_ZERO",
        "a",
    ),
    (
        "EMPTY_ARRAY",
        {
            "version": 1,
            "steps": [{"id": "a", "op": "min", "args": {"input": []}}],
        },
        1,
        "EMPTY_ARRAY",
        "a",
    ),
    (
        "STEP_FAILED",
        {
            "version": 1,
            "steps": [{"id": "f", "op": "fail_until",
                        "args": {"failures": 2, "value": "ok"}}],
        },
        1,
        "STEP_FAILED",
        "f",
    ),
]


def error_check(label, doc, expected_exit, expected_code, expected_step):
    proc = invoke(["run", "-"], json.dumps(doc).encode("utf-8"))
    problems = []
    if proc.returncode != expected_exit:
        problems.append("exit %r != %r" % (proc.returncode, expected_exit))
    try:
        response_doc = json.loads(proc.stdout.decode("utf-8"))
    except Exception as exc:  # noqa: BLE001
        problems.append("stdout is not one JSON document: %s" % exc)
        response_doc = None
    if response_doc is not None:
        error = response_doc.get("error") or {}
        if response_doc.get("status") != "failed":
            problems.append("status is not failed")
        if error.get("code") != expected_code:
            problems.append("code %r != %r" % (error.get("code"), expected_code))
        if error.get("step") != expected_step:
            problems.append("step %r != %r" % (error.get("step"), expected_step))
    if problems:
        print("FAIL %s" % label)
        for p in problems:
            print("  " + p)
        return False
    print("PASS %s" % label)
    return True


def main():
    ok = True
    names = sorted(EXPECTED)
    for name in names:
        path = os.path.join(HERE, name)
        exit_code, expected_doc = EXPECTED[name]
        # Canonical invocation used by the benchmark adapter.
        ok &= one_check(
            "%s (run <file>)" % name,
            ["run", path],
            None,
            exit_code,
            expected_doc,
        )
        # Bare-file invocation documented in the README.
        ok &= one_check(
            "%s (<file>)" % name,
            [path],
            None,
            exit_code,
            expected_doc,
        )

    # stdin invocation
    name = "01_math_pipeline.json"
    with open(os.path.join(HERE, name), "rb") as handle:
        data = handle.read()
    exit_code, expected_doc = EXPECTED[name]
    ok &= one_check(
        "%s (run - stdin)" % name,
        ["run", "-"],
        data,
        exit_code,
        expected_doc,
    )

    for label, doc, exit_code, code, step in ERROR_CASES:
        ok &= error_check(label, doc, exit_code, code, step)

    print()
    if ok:
        print(
            "All self-tests passed (%d workflows, %d error cases)."
            % (len(names), len(ERROR_CASES))
        )
        return 0
    print("Self-tests FAILED.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
